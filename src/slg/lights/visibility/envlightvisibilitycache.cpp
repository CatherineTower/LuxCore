/***************************************************************************
 * Copyright 1998-2018 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagebuf.h>

#include <memory>
#if defined(_OPENMP)
#include <omp.h>
#endif

#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/chrono.hpp>

#include "luxrays/core/bvh/bvhbuild.h"
#include "luxrays/utils/atomic.h"
#include "luxrays/utils/safesave.h"

#include "slg/samplers/metropolis.h"
#include "slg/lights/visibility/envlightvisibilitycache.h"
#include "slg/film/imagepipeline/plugins/gaussianblur3x3.h"
#include "slg/utils/film2sceneradius.h"
#include "slg/utils/scenevisibility.h"

using namespace std;
using namespace luxrays;
using namespace slg;
OIIO_NAMESPACE_USING

//------------------------------------------------------------------------------
// ELVCOctree
//------------------------------------------------------------------------------

ELVCOctree::ELVCOctree(const vector<ELVCVisibilityParticle> &entries,
		const BBox &bbox, const float r, const float n, const u_int md) :
	IndexOctree(entries, bbox, r, n, md) {
}

ELVCOctree::~ELVCOctree() {
}

u_int ELVCOctree::GetNearestEntry(const Point &p, const Normal &n, const bool isVolume) const {
	u_int nearestEntryIndex = NULL_INDEX;
	float nearestDistance2 = entryRadius2;

	GetNearestEntryImpl(&root, worldBBox, p, n, isVolume,
			nearestEntryIndex, nearestDistance2);
	
	return nearestEntryIndex;
}

void ELVCOctree::GetNearestEntryImpl(const IndexOctreeNode *node, const BBox &nodeBBox,
		const Point &p, const Normal &n, const bool isVolume,
		u_int &nearestEntryIndex, float &nearestDistance2) const {
	// Check if I'm inside the node bounding box
	if (!nodeBBox.Inside(p))
		return;

	// Check every entry in this node
	for (auto const &entryIndex : node->entriesIndex) {
		const ELVCVisibilityParticle &entry = allEntries[entryIndex];

		const BSDF &bsdf = entry.bsdfList[0];
		const Normal landingNormal = bsdf.hitPoint.GetLandingShadeN();
		const float distance2 = DistanceSquared(p, bsdf.hitPoint.p);
		if ((distance2 < nearestDistance2) && (isVolume == bsdf.IsVolume()) &&
				(bsdf.IsVolume() || (Dot(n, landingNormal) >= entryNormalCosAngle))) {
			// I have found a valid nearer entry
			nearestEntryIndex = entryIndex;
			nearestDistance2 = distance2;
		}
	}

	// Check the children too
	const Point pMid = .5 * (nodeBBox.pMin + nodeBBox.pMax);
	for (u_int child = 0; child < 8; ++child) {
		if (node->children[child]) {
			const BBox childBBox = ChildNodeBBox(child, nodeBBox, pMid);

			GetNearestEntryImpl(node->children[child], childBBox, p, n, isVolume,
					nearestEntryIndex, nearestDistance2);
		}
	}
}

//------------------------------------------------------------------------------
// Env. light visibility cache builder
//------------------------------------------------------------------------------

EnvLightVisibilityCache::EnvLightVisibilityCache(const Scene *scn, const EnvLightSource *envl,
		ImageMap *li, const ELVCParams &p) :
		scene(scn), envLight(envl), luminanceMapImage(li), params(p),
		cacheEntriesBVH(nullptr) {
}

EnvLightVisibilityCache::~EnvLightVisibilityCache() {
	delete cacheEntriesBVH;
}

bool EnvLightVisibilityCache::IsCacheEnabled(const BSDF &bsdf) const {
	return !bsdf.IsDelta();
}

//------------------------------------------------------------------------------
// EvaluateBestRadius
//------------------------------------------------------------------------------

namespace slg {

class ELVCFilm2SceneRadiusValidator : public Film2SceneRadiusValidator {
public:
	ELVCFilm2SceneRadiusValidator(const EnvLightVisibilityCache &c) : elvc(c) { }
	virtual ~ELVCFilm2SceneRadiusValidator() { }
	
	virtual bool IsValid(const BSDF &bsdf) const {
		return elvc.IsCacheEnabled(bsdf);
	}

private:
	const EnvLightVisibilityCache &elvc;
};

}

float EnvLightVisibilityCache::EvaluateBestRadius() {
	SLG_LOG("EnvLightVisibilityCache evaluating best radius");

	// The percentage of image plane to cover with the radius
	const float imagePlaneRadius = .1f;

	// The old default radius: 15cm
	const float defaultRadius = .15f;
	
	ELVCFilm2SceneRadiusValidator validator(*this);

	return Film2SceneRadius(scene, imagePlaneRadius, defaultRadius,
			params.visibility.maxPathDepth,
			0.f, 1.f,
			&validator);
}

//------------------------------------------------------------------------------
// Scene visibility
//------------------------------------------------------------------------------

namespace slg {

class ELVCSceneVisibility : public SceneVisibility<ELVCVisibilityParticle> {
public:
	ELVCSceneVisibility(EnvLightVisibilityCache &cache) :
		SceneVisibility(cache.scene, cache.visibilityParticles,
				cache.params.visibility.maxPathDepth, cache.params.visibility.maxSampleCount,
				cache.params.visibility.targetHitRate,
				cache.params.visibility.lookUpRadius, cache.params.visibility.lookUpNormalAngle,
				0.f, 1.f),
		elvc(cache) {
	}
	virtual ~ELVCSceneVisibility() { }
	
protected:
	virtual IndexOctree<ELVCVisibilityParticle> *AllocOctree() const {
		return new ELVCOctree(visibilityParticles, scene->dataSet->GetBBox(),
				lookUpRadius, lookUpNormalAngle);
	}

	virtual bool ProcessHitPoint(const BSDF &bsdf, const PathVolumeInfo &volInfo,
			vector<ELVCVisibilityParticle> &visibilityParticles) const {
		if (elvc.IsCacheEnabled(bsdf))
			visibilityParticles.push_back(ELVCVisibilityParticle(bsdf, volInfo));

		return true;
	}

	virtual bool ProcessVisibilityParticle(const ELVCVisibilityParticle &vp,
			vector<ELVCVisibilityParticle> &visibilityParticles,
			IndexOctree<ELVCVisibilityParticle> *octree, const float maxDistance2) const {
		ELVCOctree *particlesOctree = (ELVCOctree *)octree;

		// Check if a cache entry is available for this point
		const u_int entryIndex = particlesOctree->GetNearestEntry(vp.bsdfList[0].hitPoint.p,
				vp.bsdfList[0].hitPoint.GetLandingShadeN(), vp.bsdfList[0].IsVolume());

		if (entryIndex == NULL_INDEX) {
			// Add as a new entry
			visibilityParticles.push_back(vp);
			particlesOctree->Add(visibilityParticles.size() - 1);

			return false;
		} else {
			ELVCVisibilityParticle &entry = visibilityParticles[entryIndex];
			const float distance2 = DistanceSquared(vp.bsdfList[0].hitPoint.p, entry.bsdfList[0].hitPoint.p);

			if (distance2 > maxDistance2) {
				// Add as a new entry
				visibilityParticles.push_back(vp);
				particlesOctree->Add(visibilityParticles.size() - 1);
				
				return false;
			} else {
				entry.Add(vp);
				
				return true;
			}
		}
	}
	
	EnvLightVisibilityCache &elvc;
};

}

void EnvLightVisibilityCache::TraceVisibilityParticles() {
	ELVCSceneVisibility elvcVisibility(*this);
	
	elvcVisibility.Build();

	if (visibilityParticles.size() == 0) {
		// Something wrong, nothing in the scene is visible and/or cache enabled
		return;
	}
}

//------------------------------------------------------------------------------
// Build cache entries
//------------------------------------------------------------------------------

void EnvLightVisibilityCache::BuildCacheEntry(const u_int entryIndex) {
	//const double t1 = WallClockTime();

	const ELVCVisibilityParticle &visibilityParticle = visibilityParticles[entryIndex];
	ELVCacheEntry &cacheEntry = cacheEntries[entryIndex];

	// Set up the default cache entry
	const BSDF &firstBsdf = visibilityParticle.bsdfList[0];
	cacheEntry.p = firstBsdf.hitPoint.p;
	cacheEntry.n = firstBsdf.hitPoint.GetLandingShadeN();
	cacheEntry.isVolume = firstBsdf.IsVolume();
	cacheEntry.visibilityMap = nullptr;
	
	// Allocate the map storage
	unique_ptr<ImageMap> visibilityMapImage(ImageMap::AllocImageMap<float>(1.f, 1,
			params.map.width, params.map.height, ImageMapStorage::REPEAT));
	float *visibilityMap = (float *)visibilityMapImage->GetStorage()->GetPixelsData();
	fill(visibilityMap, visibilityMap + params.map.width * params.map.height, 0.f);
	vector<u_int> sampleCount(params.map.width * params.map.height, 0);

	// Trace all shadow rays
	const u_int totSamples = params.map.width * params.map.height * params.map.sampleCount;
	for (u_int pass = 1; pass <= totSamples; ++pass) {
		// Using pass + 1 to avoid 0.0 value
		const float u0 = RadicalInverse(pass, 3);
		const float u1 = RadicalInverse(pass, 5);
		const float u2 = RadicalInverse(pass, 7);
		const float u3 = RadicalInverse(pass, 11);
		const float u4 = RadicalInverse(pass, 13);

		// Pick a sampling point index
		const u_int pointIndex = Min<u_int>(Floor2UInt(u0 * visibilityParticle.bsdfList.size()), visibilityParticle.bsdfList.size() - 1);

		// Pick a sampling point
		const BSDF &bsdf = visibilityParticle.bsdfList[pointIndex];

		// Build local sampling direction
		Vector localSamplingDir = bsdf.IsVolume() ?
			UniformSampleSphere(u1, u2) : UniformSampleHemisphere(u1, u2);
		// I need to flip Z if I'm coming from the back. The BSDF Frame is
		// expressed in term of "coming from the front".
		if (!bsdf.hitPoint.intoObject)
			localSamplingDir.z *= -1.f;

		// Transform local sampling direction to global;
		const Frame &frame = bsdf.GetFrame();
		const Vector globalSamplingDir = frame.ToWorld(localSamplingDir);

		// Get the map pixel x/y
		const Vector localLightDir = Normalize(Inverse(envLight->lightToWorld) * globalSamplingDir);

		float u, v, latLongMappingPdf;
		EnvLightSource::ToLatLongMapping(localLightDir, &u, &v, &latLongMappingPdf);
		if (latLongMappingPdf == 0.f)
			continue;

		const float s = u * params.map.width - .5f;
		const float t = v * params.map.height - .5f;

		const int s0 = Floor2Int(s);
		const int t0 = Floor2Int(t);

		const u_int x = static_cast<u_int>(Mod<int>(s0, params.map.width));
		const u_int y = static_cast<u_int>(Mod<int>(t0, params.map.height));
		const u_int pixelIndex = x + y * params.map.width;

		assert (x < params.map.width);
		assert (y < params.map.height);
		
		// Set up the shadow ray
		Ray shadowRay(bsdf.GetRayOrigin(globalSamplingDir), globalSamplingDir);
		shadowRay.time = u3;

		// Check if the light source is visible
		RayHit shadowRayHit;
		BSDF shadowBsdf;
		Spectrum connectionThroughput;

		PathVolumeInfo volInfo = visibilityParticle.volInfoList[pointIndex];
		if (!scene->Intersect(nullptr, false, false, &volInfo, u4, &shadowRay,
				&shadowRayHit, &shadowBsdf, &connectionThroughput)) {
			// Nothing was hit, the light source is visible

			visibilityMap[pixelIndex] += connectionThroughput.Y();
		}
		
		sampleCount[pixelIndex] += 1;
	}
	
	//const double t2 = WallClockTime();

	for (u_int y = 0; y < params.map.height; ++y) {
		for (u_int x = 0; x < params.map.width; ++x) {
			const u_int pixelIndex = x + y * params.map.width;
			if (sampleCount[pixelIndex] > 0)
				visibilityMap[pixelIndex] /= sampleCount[pixelIndex];
		}
	}

	// Filter the map
	const u_int mapPixelCount = params.map.width * params.map.height;
	vector<float> tmpBuffer(mapPixelCount);
	GaussianBlur3x3FilterPlugin::ApplyBlurFilter(params.map.width, params.map.height,
				&visibilityMap[0], &tmpBuffer[0],
				.5f, 1.f, .5f);

	// Check if I have set the lower hemisphere to 0.0
	if (params.map.sampleUpperHemisphereOnly) {
		for (u_int y = params.map.height / 2 + 1; y < params.map.height; ++y)
			for (u_int x = 0; x < params.map.width; ++x)
				visibilityMap[x + y * params.map.width] = 0.f;
	}

	// Normalize and multiply for normalized image luminance
	float visibilityMaxVal = 0.f;
	for (u_int i = 0; i < mapPixelCount; ++i)
		visibilityMaxVal = Max(visibilityMaxVal, visibilityMap[i]);

	if (visibilityMaxVal == 0.f) {
		// In this case I will use the normal map
		return;
	}

	// For some debug, save the map to a file
	/*if (entryIndex % 100 == 0) {
		ImageSpec spec(params.map.width, params.map.height, 3, TypeDesc::FLOAT);
		ImageBuf buffer(spec);
		float maxVal = -INFINITY;
		float minVal = INFINITY;
		for (ImageBuf::ConstIterator<float> it(buffer); !it.done(); ++it) {
			u_int x = it.x();
			u_int y = it.y();
			float *pixel = (float *)buffer.pixeladdr(x, y, 0);
			const float v = visibilityMap[x + y * params.map.width];			
			
			maxVal = Max(v, maxVal);
			minVal = Min(v, minVal);

			pixel[0] = v;
			pixel[1] = v;
			pixel[2] = v;
		}
		buffer.write("visibiliy-" + ToString(entryIndex) + ".exr");

		SLG_LOG("Visibility " << entryIndex << " Max=" << maxVal << " Min=" << minVal);
	}*/

	const float invVisibilityMaxVal = 1.f / visibilityMaxVal;
	for (u_int i = 0; i < mapPixelCount; ++i) {
		const float normalizedVisVal = visibilityMap[i] * invVisibilityMaxVal;

		visibilityMap[i] = normalizedVisVal;
	}

	if (luminanceMapImage) {
		const ImageMapStorage *luminanceMapStorage = luminanceMapImage->GetStorage();

		// For some debug, save the map to a file
		/*if (entryIndex % 100 == 0) {
			ImageSpec spec(params.map.width, params.map.height, 3, TypeDesc::FLOAT);
			ImageBuf buffer(spec);
			float maxVal = -INFINITY;
			float minVal = INFINITY;
			for (ImageBuf::ConstIterator<float> it(buffer); !it.done(); ++it) {
				u_int x = it.x();
				u_int y = it.y();
				float *pixel = (float *)buffer.pixeladdr(x, y, 0);
				const float v = luminanceMapStorage->GetFloat(x + y * params.map.width);
				
				maxVal = Max(v, maxVal);
				minVal = Min(v, minVal);
			
				pixel[0] = v;
				pixel[1] = v;
				pixel[2] = v;
			}
			buffer.write("luminance-" + ToString(entryIndex) + ".exr");

			SLG_LOG("Luminance " << entryIndex << " Max=" << maxVal << " Min=" << minVal);
		}*/

		float luminanceMaxVal = 0.f;
		for (u_int i = 0; i < mapPixelCount; ++i)
			luminanceMaxVal = Max(visibilityMaxVal, luminanceMapStorage->GetFloat(i));

		const float invLuminanceMaxVal = 1.f / luminanceMaxVal;
		for (u_int i = 0; i < mapPixelCount; ++i) {
			const float normalizedLumiVal = luminanceMapStorage->GetFloat(i) * invLuminanceMaxVal;

			visibilityMap[i] *= normalizedLumiVal;
		}
	}

	// For some debug, save the map to a file
	/*if (entryIndex % 100 == 0) {
		ImageSpec spec(params.map.width, params.map.height, 3, TypeDesc::FLOAT);
		ImageBuf buffer(spec);
		float maxVal = -INFINITY;
		float minVal = INFINITY;
		for (ImageBuf::ConstIterator<float> it(buffer); !it.done(); ++it) {
			u_int x = it.x();
			u_int y = it.y();
			float *pixel = (float *)buffer.pixeladdr(x, y, 0);
			const float v = visibilityMap[x + y * params.map.width];

			maxVal = Max(v, maxVal);
			minVal = Min(v, minVal);

			pixel[0] = v;
			pixel[1] = v;
			pixel[2] = v;
		}
		buffer.write("map-" + ToString(entryIndex) + ".exr");
		
		SLG_LOG("Map " << entryIndex << " Max=" << maxVal << " Min=" << minVal);
	}*/

	// Avoid to have cells with too low probability (i.e. to avoid fireflies)
	for (u_int y = 0; y < params.map.height; ++y) {
		for (u_int x = 0; x < params.map.width; ++x) {
			const u_int pixelIndex = x + y * params.map.width;
			if (visibilityMap[pixelIndex] > 0.f)
				visibilityMap[pixelIndex] = Max(visibilityMap[pixelIndex], 0.05f);
		}
	}

	cacheEntry.visibilityMap = new Distribution2D(&visibilityMap[0], params.map.width, params.map.height);
	
	//const double t3 = WallClockTime();
	//SLG_LOG("Visibility map rendering times: " << int((t2 - t1) * 1000.0) << "ms + " << int((t3 - t2) * 1000.0) << "ms");
}

void EnvLightVisibilityCache::BuildCacheEntries() {
	SLG_LOG("EnvLightVisibilityCache building cache entries: " << visibilityParticles.size());

	const double startTime = WallClockTime();
	double lastPrintTime = WallClockTime();
	atomic<u_int> counter(0);

	cacheEntries.resize(visibilityParticles.size());
	#pragma omp parallel for
	for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int i = 0; i < visibilityParticles.size(); ++i) {
		const int tid =
#if defined(_OPENMP)
			omp_get_thread_num()
#else
			0
#endif
			;

		if (tid == 0) {
			const double now = WallClockTime();
			if (now - lastPrintTime > 2.0) {
				SLG_LOG("EnvLightVisibilityCache initializing distributions: " << counter << "/" << visibilityParticles.size() <<" (" <<
						(boost::format("%.2f entries/sec, ") % (counter / (now - startTime))) <<
						(u_int)((100.0 * counter) / visibilityParticles.size()) << "%)");
				lastPrintTime = now;
			}
		}

		BuildCacheEntry(i);

		++counter;
	}
}

//------------------------------------------------------------------------------
// ELVCBvh
//------------------------------------------------------------------------------

ELVCBvh::ELVCBvh(const vector<ELVCacheEntry> *entries, const float radius, const float normalAngle) :
			IndexBvh(entries, radius), normalCosAngle(cosf(Radians(normalAngle))) {
}

ELVCBvh::~ELVCBvh() {
}

const ELVCacheEntry *ELVCBvh::GetNearestEntry(const Point &p, const Normal &n, const bool isVolume) const {
	const ELVCacheEntry *nearestEntry = nullptr;
	float nearestDistance2 = entryRadius2;

	u_int currentNode = 0; // Root Node
	const u_int stopNode = BVHNodeData_GetSkipIndex(arrayNodes[0].nodeData); // Non-existent

	while (currentNode < stopNode) {
		const slg::ocl::IndexBVHArrayNode &node = arrayNodes[currentNode];

		const u_int nodeData = node.nodeData;
		if (BVHNodeData_IsLeaf(nodeData)) {
			// It is a leaf, check the entry
			const ELVCacheEntry *entry = &((*allEntries)[node.entryLeaf.entryIndex]);

			const float distance2 = DistanceSquared(p, entry->p);
			if ((distance2 < nearestDistance2) && (isVolume == entry->isVolume) &&
					(isVolume || (Dot(n, entry->n) > normalCosAngle))) {
				// I have found a valid nearer entry
				nearestEntry = entry;
				nearestDistance2 = distance2;
			}

			++currentNode;
		} else {
			// It is a node, check the bounding box
			if (p.x >= node.bvhNode.bboxMin[0] && p.x <= node.bvhNode.bboxMax[0] &&
					p.y >= node.bvhNode.bboxMin[1] && p.y <= node.bvhNode.bboxMax[1] &&
					p.z >= node.bvhNode.bboxMin[2] && p.z <= node.bvhNode.bboxMax[2])
				++currentNode;
			else {
				// I don't need to use IndexBVHNodeData_GetSkipIndex() here because
				// I already know the leaf flag is 0
				currentNode = nodeData;
			}
		}
	}

	return nearestEntry;
}

//------------------------------------------------------------------------------
// Build
//------------------------------------------------------------------------------

void EnvLightVisibilityCache::Build() {
	//--------------------------------------------------------------------------
	// Load the persistent cache file if required
	//--------------------------------------------------------------------------

	if (params.persistent.fileName != "") {
		// Check if the file already exist
		if (boost::filesystem::exists(params.persistent.fileName)) {
			// Load the cache from the file
			LoadPersistentCache(params.persistent.fileName);

			return;
		}
		
		// The file doesn't exist so I have to go trough normal pre-processing
	}

	//--------------------------------------------------------------------------
	// Evaluate best radius if required
	//--------------------------------------------------------------------------

	if (params.visibility.lookUpRadius == 0.f) {
		params.visibility.lookUpRadius = EvaluateBestRadius();
		SLG_LOG("EnvLightVisibilityCache best cache radius: " << params.visibility.lookUpRadius);
	}
	
	//--------------------------------------------------------------------------
	// Build the list of visible points (i.e. the cache points)
	//--------------------------------------------------------------------------
	
	TraceVisibilityParticles();
	
	//--------------------------------------------------------------------------
	// Build cache entries
	//--------------------------------------------------------------------------

	BuildCacheEntries();

	//--------------------------------------------------------------------------
	// Free memory
	//--------------------------------------------------------------------------

	visibilityParticles.clear();
	visibilityParticles.shrink_to_fit();

	//--------------------------------------------------------------------------
	// Build cache entries BVH
	//--------------------------------------------------------------------------

	if (cacheEntries.size() > 0) {
		SLG_LOG("EnvLightVisibilityCache building cache entries BVH");
		cacheEntriesBVH = new ELVCBvh(&cacheEntries, params.visibility.lookUpRadius,
				params.visibility.lookUpNormalAngle);
	} else
		SLG_LOG("WARNING: EnvLightVisibilityCache has an empty cache");

	//--------------------------------------------------------------------------
	// Check if I have to save the persistent cache
	//--------------------------------------------------------------------------

	if (params.persistent.fileName != "")
		SavePersistentCache(params.persistent.fileName);
}

//------------------------------------------------------------------------------
// GetVisibilityMap
//------------------------------------------------------------------------------

const Distribution2D *EnvLightVisibilityCache::GetVisibilityMap(const BSDF &bsdf) const {
	if (cacheEntriesBVH) {
		const ELVCacheEntry *entry = cacheEntriesBVH->GetNearestEntry(bsdf.hitPoint.p,
				bsdf.hitPoint.GetLandingShadeN(), bsdf.IsVolume());
		if (entry)
			return entry->visibilityMap;
	}

	return nullptr;
}

//------------------------------------------------------------------------------
// Properties2Params
//------------------------------------------------------------------------------

ELVCParams EnvLightVisibilityCache::Properties2Params(const string &prefix, const Properties props) {
	ELVCParams params;

	params.map.width = Max(16u, props.Get(Property(prefix + ".visibilitymapcache.map.width")(512)).Get<u_int>());
	params.map.height = Max(8u, props.Get(Property(prefix + ".visibilitymapcache.map.height")(256)).Get<u_int>());
	params.map.sampleCount = Max(1u, props.Get(Property(prefix + ".visibilitymapcache.map.samplecount")(1)).Get<u_int>());
	params.map.sampleUpperHemisphereOnly = props.Get(Property(prefix + ".visibilitymapcache.map.sampleupperhemisphereonly")(false)).Get<bool>();

	params.visibility.maxSampleCount = Max(1u, props.Get(Property(prefix + ".visibilitymapcache.visibility.maxsamplecount")(1024 * 1024)).Get<u_int>());
	params.visibility.maxPathDepth = Max(1u, props.Get(Property(prefix + ".visibilitymapcache.visibility.maxdepth")(4)).Get<u_int>());
	params.visibility.targetHitRate = Max(0.f, props.Get(Property(prefix + ".visibilitymapcache.visibility.targethitrate")(.99f)).Get<float>());
	params.visibility.lookUpRadius = Max(0.f, props.Get(Property(prefix + ".visibilitymapcache.visibility.radius")(0.f)).Get<float>());
	params.visibility.lookUpNormalAngle = Max(0.f, props.Get(Property(prefix + ".visibilitymapcache.visibility.normalangle")(25.f)).Get<float>());

	params.persistent.fileName = props.Get(Property(prefix + ".visibilitymapcache.persistent.file")("")).Get<string>();
	params.persistent.safeSave = props.Get(Property(prefix + ".visibilitymapcache.persistent.safesave")(true)).Get<bool>();

	return params;
}

//------------------------------------------------------------------------------
// Params2Props
//------------------------------------------------------------------------------

Properties EnvLightVisibilityCache::Params2Props(const string &prefix, const ELVCParams &params) {
	Properties props;
	
	props <<
			Property(prefix + ".visibilitymapcache.map.width")(params.map.width) <<
			Property(prefix + ".visibilitymapcache.map.height")(params.map.height) <<
			Property(prefix + ".visibilitymapcache.map.samplecount")(params.map.sampleCount) <<
			Property(prefix + ".visibilitymapcache.map.sampleupperhemisphereonly")(params.map.sampleUpperHemisphereOnly) <<
			Property(prefix + ".visibilitymapcache.visibility.maxsamplecount")(params.visibility.maxSampleCount) <<
			Property(prefix + ".visibilitymapcache.visibility.maxdepth")(params.visibility.maxPathDepth) <<
			Property(prefix + ".visibilitymapcache.visibility.targethitrate")(params.visibility.targetHitRate) <<
			Property(prefix + ".visibilitymapcache.visibility.radius")(params.visibility.lookUpRadius) <<
			Property(prefix + ".visibilitymapcache.visibility.normalangle")(params.visibility.lookUpNormalAngle) <<
			Property(prefix + ".visibilitymapcache.persistent.file")(params.persistent.fileName) <<
			Property(prefix + ".visibilitymapcache.persistent.safesave")(params.persistent.safeSave);
	
	return props;
}

//------------------------------------------------------------------------------
// Serialization
//------------------------------------------------------------------------------

void EnvLightVisibilityCache::LoadPersistentCache(const std::string &fileName) {
	SLG_LOG("Loading persistent EnvLightVisibility cache: " + fileName);

	SerializationInputFile sif(fileName);

	sif.GetArchive() >> params;

	sif.GetArchive() >> cacheEntries;
	sif.GetArchive() >> cacheEntriesBVH;

	visibilityParticles.clear();
	visibilityParticles.shrink_to_fit();
	
	if (!sif.IsGood())
		throw runtime_error("Error while loading EnvLightVisibility persistent cache: " + fileName);
}

void EnvLightVisibilityCache::SavePersistentCache(const std::string &fileName) {
	SLG_LOG("Saving persistent EnvLightVisibility cache: " + fileName);

	SafeSave safeSave(fileName);
	{
		SerializationOutputFile sof(params.persistent.safeSave ? safeSave.GetSaveFileName() : fileName);

		sof.GetArchive() << params;

		sof.GetArchive() << cacheEntries;
		sof.GetArchive() << cacheEntriesBVH;

		visibilityParticles.clear();
		visibilityParticles.shrink_to_fit();

		if (!sof.IsGood())
			throw runtime_error("Error while saving EnvLightVisibility persistent cache: " + fileName);

		sof.Flush();

		SLG_LOG("EnvLightVisibility persistent cache saved: " << (sof.GetPosition() / 1024) << " Kbytes");
	}
	// Now sof is closed and I can call safeSave.Process()
	
	if (params.persistent.safeSave)
		safeSave.Process();
}

BOOST_CLASS_EXPORT_IMPLEMENT(slg::ELVCacheEntry)
BOOST_CLASS_EXPORT_IMPLEMENT(slg::ELVCBvh)
BOOST_CLASS_EXPORT_IMPLEMENT(slg::ELVCParams)
