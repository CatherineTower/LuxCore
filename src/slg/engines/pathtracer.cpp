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

#include "slg/engines/pathtracer.h"
#include "slg/engines/caches/photongi/photongicache.h"

using namespace std;
using namespace luxrays;
using namespace slg;

PathTracer::PathTracer() : pixelFilterDistribution(nullptr), photonGICache(nullptr) {
}

PathTracer::~PathTracer() {
	delete pixelFilterDistribution;
}

void PathTracer::InitPixelFilterDistribution(const Filter *pixelFilter) {
	// Compile sample distribution
	delete pixelFilterDistribution;
	pixelFilterDistribution = new FilterDistribution(pixelFilter, 64);
}

void  PathTracer::DeletePixelFilterDistribution() {
	delete pixelFilterDistribution;
	pixelFilterDistribution = NULL;
}

void PathTracer::InitEyeSampleResults(const Film *film, vector<SampleResult> &sampleResults) const {
	SampleResult &sampleResult = sampleResults[0];

	sampleResult.Init(Film::RADIANCE_PER_PIXEL_NORMALIZED | Film::ALPHA | Film::DEPTH |
			Film::POSITION | Film::GEOMETRY_NORMAL | Film::SHADING_NORMAL | Film::MATERIAL_ID |
			Film::DIRECT_DIFFUSE | Film::DIRECT_GLOSSY | Film::EMISSION | Film::INDIRECT_DIFFUSE |
			Film::INDIRECT_GLOSSY | Film::INDIRECT_SPECULAR | Film::DIRECT_SHADOW_MASK |
			Film::INDIRECT_SHADOW_MASK | Film::UV | Film::RAYCOUNT | Film::IRRADIANCE |
			Film::OBJECT_ID | Film::SAMPLECOUNT | Film::CONVERGENCE | Film::MATERIAL_ID_COLOR |
			Film::ALBEDO | Film::AVG_SHADING_NORMAL | Film::NOISE,
			film->GetRadianceGroupCount());
	sampleResult.useFilmSplat = false;
}

//------------------------------------------------------------------------------
// RenderEyeSample methods
//------------------------------------------------------------------------------

PathTracer::DirectLightResult PathTracer::DirectLightSampling(
		luxrays::IntersectionDevice *device, const Scene *scene,
		const float time,
		const float u0, const float u1, const float u2,
		const float u3, const float u4,
		const PathDepthInfo &depthInfo,
		const Spectrum &pathThroughput, const BSDF &bsdf,
		PathVolumeInfo volInfo,
		SampleResult *sampleResult) const {
	if (!bsdf.IsDelta()) {
		// Select the light strategy to use
		const LightStrategy *lightStrategy;
		if (bsdf.IsShadowCatcherOnlyInfiniteLights())
			lightStrategy = scene->lightDefs.GetInfiniteLightStrategy();
		else
			lightStrategy = scene->lightDefs.GetIlluminateLightStrategy();
		
		// Pick a light source to sample
		const Normal landingNormal = bsdf.hitPoint.intoObject ? bsdf.hitPoint.shadeN : -bsdf.hitPoint.shadeN;
		float lightPickPdf;
		const LightSource *light = lightStrategy->SampleLights(u0,
				bsdf.hitPoint.p, landingNormal, bsdf.IsVolume(), &lightPickPdf);

		if (light) {
			Vector lightRayDir;
			float distance, directPdfW;
			Spectrum lightRadiance = light->Illuminate(*scene, bsdf,
					u1, u2, u3, &lightRayDir, &distance, &directPdfW);
			assert (!lightRadiance.IsNaN() && !lightRadiance.IsInf());

			if (!lightRadiance.Black()) {
				assert (!isnan(directPdfW) && !isinf(directPdfW));

				BSDFEvent event;
				float bsdfPdfW;
				Spectrum bsdfEval = bsdf.Evaluate(lightRayDir, &event, &bsdfPdfW);
				assert (!bsdfEval.IsNaN() && !bsdfEval.IsInf());

				if (!bsdfEval.Black() &&
						(!hybridBackForwardEnable || (depthInfo.depth == 0) ||
							!IsStillSpecularGlossyCausticPath(sampleResult->specularGlossyCausticPath,
								bsdf, event, depthInfo))) {
					assert (!isnan(bsdfPdfW) && !isinf(bsdfPdfW));
					
					// Create a new DepthInfo for the path to the light source
					PathDepthInfo directLightDepthInfo = depthInfo;
					directLightDepthInfo.IncDepths(event);
					
					Ray shadowRay(bsdf.GetRayOrigin(lightRayDir), lightRayDir,
							0.f,
							distance,
							time);
					shadowRay.UpdateMinMaxWithEpsilon();
					RayHit shadowRayHit;
					BSDF shadowBsdf;
					Spectrum connectionThroughput;
					// Check if the light source is visible
					if (!scene->Intersect(device, false, false, &volInfo, u4, &shadowRay,
							&shadowRayHit, &shadowBsdf, &connectionThroughput, nullptr,
							nullptr, true)) {
						// Add the light contribution only if it is not a shadow catcher
						// (because, if the light is visible, the material will be
						// transparent in the case of a shadow catcher).

						if (!bsdf.IsShadowCatcher()) {
							// I'm ignoring volume emission because it is not sampled in
							// direct light step.
							const float directLightSamplingPdfW = directPdfW * lightPickPdf;
							const float factor = 1.f / directLightSamplingPdfW;

							if (directLightDepthInfo.GetRRDepth() >= rrDepth) {
								// Russian Roulette
								bsdfPdfW *= RenderEngine::RussianRouletteProb(bsdfEval, rrImportanceCap);
							}

							// Account for material transparency
							bsdfPdfW *= light->GetAvgPassThroughTransparency();

							// MIS between direct light sampling and BSDF sampling
							//
							// Note: I have to avoid MIS on the last path vertex
							const bool misEnabled = !sampleResult->lastPathVertex &&
								(light->IsEnvironmental() || light->IsIntersectable()) &&
								CheckDirectHitVisibilityFlags(light, directLightDepthInfo, event);

							const float weight = misEnabled ? PowerHeuristic(directLightSamplingPdfW, bsdfPdfW) : 1.f;
							const Spectrum incomingRadiance = bsdfEval * (weight * factor) * connectionThroughput * lightRadiance;

							sampleResult->AddDirectLight(light->GetID(), event, pathThroughput, incomingRadiance, 1.f);

							// The first path vertex is not handled by AddDirectLight(). This is valid
							// for irradiance AOV only if it is not a SPECULAR material.
							//
							// Note: irradiance samples the light sources only here (i.e. no
							// direct hit, no MIS, it would be useless)
							//
							// Note: RR is ignored here because it can not happen on first path vertex
							if ((sampleResult->firstPathVertex) && !(bsdf.GetEventTypes() & SPECULAR))
								sampleResult->irradiance =
										(INV_PI * fabsf(Dot(bsdf.hitPoint.shadeN, shadowRay.d)) *
										factor) * connectionThroughput * lightRadiance;
						}

						return ILLUMINATED;
					} else
						return SHADOWED; 
				}
			}
		}
	}

	return NOT_VISIBLE;
}

bool PathTracer::CheckDirectHitVisibilityFlags(const LightSource *lightSource, const PathDepthInfo &depthInfo,
		const BSDFEvent lastBSDFEvent) const {
	if (depthInfo.depth == 0)
		return true;

	if ((lastBSDFEvent & DIFFUSE) && lightSource->IsVisibleIndirectDiffuse())
		return true;
	if ((lastBSDFEvent & GLOSSY) && lightSource->IsVisibleIndirectGlossy())
		return true;
	if ((lastBSDFEvent & SPECULAR) && lightSource->IsVisibleIndirectSpecular())
		return true;

	return false;
}

void PathTracer::DirectHitFiniteLight(const Scene *scene,  const PathDepthInfo &depthInfo,
		const BSDFEvent lastBSDFEvent, const Spectrum &pathThroughput,
		const Ray &ray, const Normal &rayNormal, const bool rayFromVolume,
		const float distance, const BSDF &bsdf,
		const float lastPdfW, SampleResult *sampleResult) const {
	const LightSource *lightSource = bsdf.GetLightSource();

	// Check if the light source is visible according the settings
	if (!CheckDirectHitVisibilityFlags(lightSource, depthInfo, lastBSDFEvent))
		return;
	
	float directPdfA;
	const Spectrum emittedRadiance = bsdf.GetEmittedRadiance(&directPdfA);

	if (!emittedRadiance.Black()) {
		float weight;
		if (!(lastBSDFEvent & SPECULAR)) {
			const LightStrategy *lightStrategy = scene->lightDefs.GetIlluminateLightStrategy();
			const float lightPickProb = lightStrategy->SampleLightPdf(lightSource, ray.o, rayNormal, rayFromVolume);

			// This is a specific check to avoid fireflies with DLSC
			if ((lightPickProb == 0.f) && lightSource->IsDirectLightSamplingEnabled() &&
					(lightStrategy->GetType() == TYPE_DLS_CACHE))
				return;

			const float directPdfW = PdfAtoW(directPdfA, distance,
					AbsDot(bsdf.hitPoint.fixedDir, bsdf.hitPoint.shadeN));

			// MIS between BSDF sampling and direct light sampling
			weight = PowerHeuristic(lastPdfW * lightSource->GetAvgPassThroughTransparency(), directPdfW * lightPickProb);
		} else
			weight = 1.f;

		sampleResult->AddEmission(bsdf.GetLightID(), pathThroughput, weight * emittedRadiance);
	}
}

void PathTracer::DirectHitInfiniteLight(const Scene *scene,  const PathDepthInfo &depthInfo,
		const BSDFEvent lastBSDFEvent, const Spectrum &pathThroughput,
		const BSDF *bsdf, const Ray &ray, const Normal &rayNormal, const bool rayFromVolume,
		const float lastPdfW, SampleResult *sampleResult) const {
	BOOST_FOREACH(EnvLightSource *envLight, scene->lightDefs.GetEnvLightSources()) {
		// Check if the light source is visible according the settings
		if (!CheckDirectHitVisibilityFlags(envLight, depthInfo, lastBSDFEvent))
			continue;

		float directPdfW;
		const Spectrum envRadiance = envLight->GetRadiance(*scene, bsdf, -ray.d, &directPdfW);
		if (!envRadiance.Black()) {
			float weight;
			if (!(lastBSDFEvent & SPECULAR)) {
				const float lightPickProb = scene->lightDefs.GetIlluminateLightStrategy()->
						SampleLightPdf(envLight, ray.o, rayNormal, rayFromVolume);

				// MIS between BSDF sampling and direct light sampling
				weight = PowerHeuristic(lastPdfW, directPdfW * lightPickProb);
			} else
				weight = 1.f;

			sampleResult->AddEmission(envLight->GetID(), pathThroughput, weight * envRadiance);
		}
	}	
}

void PathTracer::GenerateEyeRay(const Camera *camera, const Film *film, Ray &eyeRay,
		PathVolumeInfo &volInfo, Sampler *sampler, SampleResult &sampleResult) const {
	const float filmX = sampler->GetSample(0);
	const float filmY = sampler->GetSample(1);

	// Use fast pixel filtering, like the one used in TILEPATH.

	const u_int *subRegion = film->GetSubRegion();
	sampleResult.pixelX = Min(Floor2UInt(filmX), subRegion[1]);
	sampleResult.pixelY = Min(Floor2UInt(filmY), subRegion[3]);
	assert (sampleResult.pixelX >= subRegion[0]);
	assert (sampleResult.pixelX <= subRegion[1]);
	assert (sampleResult.pixelY >= subRegion[2]);
	assert (sampleResult.pixelY <= subRegion[3]);

	const float uSubPixelX = filmX - sampleResult.pixelX;
	const float uSubPixelY = filmY - sampleResult.pixelY;

	// Sample according the pixel filter distribution
	float distX, distY;
	pixelFilterDistribution->SampleContinuous(uSubPixelX, uSubPixelY, &distX, &distY);

	sampleResult.filmX = sampleResult.pixelX + .5f + distX;
	sampleResult.filmY = sampleResult.pixelY + .5f + distY;

	const float timeSample = sampler->GetSample(4);
	const float time = camera->GenerateRayTime(timeSample);

	camera->GenerateRay(time, sampleResult.filmX, sampleResult.filmY, &eyeRay, &volInfo,
		sampler->GetSample(2), sampler->GetSample(3));
}

//------------------------------------------------------------------------------
// RenderEyeSample
//------------------------------------------------------------------------------

bool PathTracer::IsStillSpecularGlossyCausticPath(const bool isSpecularGlossyCausticPath,
		const BSDF &bsdf, const BSDFEvent lastBSDFEvent, const PathDepthInfo &depthInfo) const {
	// First bounce condition
	if (depthInfo.depth == 0)
		return (lastBSDFEvent & DIFFUSE) ||
				((lastBSDFEvent & GLOSSY) && (bsdf.GetGlossiness() > hybridBackForwardGlossinessThreshold));

	// All other bounce conditions
	return isSpecularGlossyCausticPath && ((lastBSDFEvent & SPECULAR) ||
			((lastBSDFEvent & GLOSSY) && (bsdf.GetGlossiness() <= hybridBackForwardGlossinessThreshold)));
}

void PathTracer::RenderEyeSample(IntersectionDevice *device, const Scene *scene, const Film *film,
		Sampler *sampler, vector<SampleResult> &sampleResults) const {
	SampleResult &sampleResult = sampleResults[0];

	// Set to 0.0 all result colors
	sampleResult.emission = Spectrum();
	for (u_int i = 0; i < sampleResult.radiance.size(); ++i)
		sampleResult.radiance[i] = Spectrum();
	sampleResult.directDiffuse = Spectrum();
	sampleResult.directGlossy = Spectrum();
	sampleResult.indirectDiffuse = Spectrum();
	sampleResult.indirectGlossy = Spectrum();
	sampleResult.indirectSpecular = Spectrum();
	sampleResult.directShadowMask = 1.f;
	sampleResult.indirectShadowMask = 1.f;
	sampleResult.irradiance = Spectrum();
	sampleResult.albedo = Spectrum();
	sampleResult.passThroughPath = true;
	sampleResult.specularGlossyCausticPath = true;

	// To keep track of the number of rays traced
	const double deviceRayCount = device->GetTotalRaysCount();

	Ray eyeRay;
	PathVolumeInfo volInfo;
	GenerateEyeRay(scene->camera, film, eyeRay, volInfo, sampler, sampleResult);
	// This is used by light strategy
	Normal lastNormal(eyeRay.d);
	bool lastFromVolume = false;

	bool photonGIShowIndirectPathMixUsed = false;
	bool photonGICausticCacheAlreadyUsed = false;
	bool photonGICacheEnabledOnLastHit = false;
	bool albedoToDo = true;
	BSDFEvent lastBSDFEvent = SPECULAR; // SPECULAR is required to avoid MIS
	float lastPdfW = 1.f;
	float lastGlossiness = 0.f;
	Spectrum pathThroughput(1.f);
	PathDepthInfo depthInfo;
	BSDF bsdf;
	for (;;) {
		sampleResult.firstPathVertex = (depthInfo.depth == 0);
		const u_int sampleOffset = eyeSampleBootSize + depthInfo.depth * eyeSampleStepSize;

		RayHit eyeRayHit;
		Spectrum connectionThroughput;
		const float passThrough = sampler->GetSample(sampleOffset);
		const bool hit = scene->Intersect(device, false, sampleResult.firstPathVertex,
				&volInfo, passThrough,
				&eyeRay, &eyeRayHit, &bsdf, &connectionThroughput,
				&pathThroughput, &sampleResult);
		pathThroughput *= connectionThroughput;
		// Note: pass-through check is done inside Scene::Intersect()

		if (!hit) {
			// Nothing was hit, look for env. lights
			if ((!forceBlackBackground || !sampleResult.passThroughPath) &&
					(!hybridBackForwardEnable || (depthInfo.depth <= 1) ||
						!sampleResult.specularGlossyCausticPath) &&
					(!photonGICache ||
						photonGICache->IsDirectLightHitVisible(photonGICausticCacheAlreadyUsed,
							lastBSDFEvent, depthInfo))) {
				DirectHitInfiniteLight(scene, depthInfo, lastBSDFEvent, pathThroughput,
						sampleResult.firstPathVertex ? nullptr : &bsdf,
						eyeRay, lastNormal, lastFromVolume,
						lastPdfW, &sampleResult);
			}

			if (sampleResult.firstPathVertex) {
				sampleResult.alpha = 0.f;
				sampleResult.depth = numeric_limits<float>::infinity();
				sampleResult.position = Point(
						numeric_limits<float>::infinity(),
						numeric_limits<float>::infinity(),
						numeric_limits<float>::infinity());
				sampleResult.geometryNormal = Normal();
				sampleResult.shadingNormal = Normal();
				sampleResult.materialID = numeric_limits<u_int>::max();
				sampleResult.objectID = numeric_limits<u_int>::max();
				sampleResult.uv = UV(numeric_limits<float>::infinity(),
						numeric_limits<float>::infinity());
			}
			break;
		}

		// Something was hit

		if (albedoToDo && bsdf.IsAlbedoEndPoint()) {
			sampleResult.albedo = pathThroughput * bsdf.Albedo();
			albedoToDo = false;
		}

		if (sampleResult.firstPathVertex) {
			// The alpha value can be changed if the material is a shadow catcher (see below)
			sampleResult.alpha = 1.f;
			sampleResult.depth = eyeRayHit.t;
			sampleResult.position = bsdf.hitPoint.p;
			sampleResult.geometryNormal = bsdf.hitPoint.geometryN;
			sampleResult.shadingNormal = bsdf.hitPoint.shadeN;
			sampleResult.materialID = bsdf.GetMaterialID();
			sampleResult.objectID = bsdf.GetObjectID();
			sampleResult.uv = bsdf.hitPoint.uv;
		}
		sampleResult.lastPathVertex = depthInfo.IsLastPathVertex(maxPathDepth, bsdf.GetEventTypes());
		
		//----------------------------------------------------------------------
		// Check if it is a light source and I have to add light emission
		//----------------------------------------------------------------------

		if (bsdf.IsLightSource() &&
				(!hybridBackForwardEnable || (depthInfo.depth <= 1) ||
					!sampleResult.specularGlossyCausticPath) &&
				(!photonGICache ||
					photonGICache->IsDirectLightHitVisible(photonGICausticCacheAlreadyUsed,
						lastBSDFEvent, depthInfo))) {
			DirectHitFiniteLight(scene, depthInfo, lastBSDFEvent, pathThroughput,
					eyeRay, lastNormal, lastFromVolume,
					eyeRayHit.t, bsdf, lastPdfW, &sampleResult);
		}

		//----------------------------------------------------------------------
		// Check if I can use the photon cache
		//----------------------------------------------------------------------

		if (photonGICache) {
			const bool isPhotonGIEnabled = photonGICache->IsPhotonGIEnabled(bsdf);

			// Check if one of the debug modes is enabled
			if (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECT) {
				if (isPhotonGIEnabled)
					sampleResult.radiance[0] += photonGICache->GetIndirectRadiance(bsdf);
				break;
			} else if (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWCAUSTIC) {
				if (isPhotonGIEnabled)
					sampleResult.radiance[0] += photonGICache->GetCausticRadiance(bsdf);
				break;
			} else if (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECTPATHMIX) {
				if (isPhotonGIEnabled && photonGICacheEnabledOnLastHit &&
						(eyeRayHit.t > photonGICache->GetIndirectUsageThreshold(lastBSDFEvent,
							lastGlossiness,
							passThrough))) {
					sampleResult.radiance[0] = Spectrum(0.f, 0.f, 1.f);
					photonGIShowIndirectPathMixUsed = true;
					break;
				}
			}

			// Check if the cache is enabled for this material
			if (isPhotonGIEnabled) {
				// TODO: add support for AOVs (possible ?)
				// TODO: support for radiance groups (possible ?)

				if (photonGICache->IsIndirectEnabled() && photonGICacheEnabledOnLastHit &&
						(eyeRayHit.t > photonGICache->GetIndirectUsageThreshold(lastBSDFEvent,
							lastGlossiness,
							// I hope to not introduce strange sample correlations
							// by using passThrough here
							passThrough))) {
					sampleResult.radiance[0] += pathThroughput * photonGICache->GetIndirectRadiance(bsdf);
					// I can terminate the path, all done
					break;
				}

				if (photonGICache->IsCausticEnabled() && !photonGICausticCacheAlreadyUsed)
					sampleResult.radiance[0] += pathThroughput * photonGICache->GetCausticRadiance(bsdf);

				photonGICausticCacheAlreadyUsed = true;
				photonGICacheEnabledOnLastHit = true;
			} else
				photonGICacheEnabledOnLastHit = false;
		}

		//------------------------------------------------------------------
		// Direct light sampling
		//------------------------------------------------------------------

		// I avoid to do DL on the last vertex otherwise it introduces a lot of
		// noise because I can not use MIS.
		// I handle as a special case when the path vertex is both the first
		// and the last: I do direct light sampling without MIS.
		if (sampleResult.lastPathVertex && !sampleResult.firstPathVertex)
			break;

		const DirectLightResult directLightResult = DirectLightSampling(
				device, scene,
				eyeRay.time,
				sampler->GetSample(sampleOffset + 1),
				sampler->GetSample(sampleOffset + 2),
				sampler->GetSample(sampleOffset + 3),
				sampler->GetSample(sampleOffset + 4),
				sampler->GetSample(sampleOffset + 5),
				depthInfo, 
				pathThroughput, bsdf, volInfo, &sampleResult);

		if (sampleResult.lastPathVertex)
			break;

		//------------------------------------------------------------------
		// Build the next vertex path ray
		//------------------------------------------------------------------

		Vector sampledDir;
		float cosSampledDir;
		Spectrum bsdfSample;
		if (bsdf.IsShadowCatcher() && (directLightResult != SHADOWED)) {
			bsdfSample = bsdf.ShadowCatcherSample(&sampledDir, &lastPdfW, &cosSampledDir, &lastBSDFEvent);

			if (sampleResult.firstPathVertex) {
				// In this case I have also to set the value of the alpha channel to 0.0
				sampleResult.alpha = 0.f;
			}
		} else {
			bsdfSample = bsdf.Sample(&sampledDir,
					sampler->GetSample(sampleOffset + 6),
					sampler->GetSample(sampleOffset + 7),
					&lastPdfW, &cosSampledDir, &lastBSDFEvent);
			sampleResult.passThroughPath = false;
		}

		assert (!bsdfSample.IsNaN() && !bsdfSample.IsInf() && !bsdfSample.IsNeg());
		if (bsdfSample.Black())
			break;
		assert (!isnan(lastPdfW) && !isinf(lastPdfW) && (lastPdfW >= 0.f));

		if (sampleResult.firstPathVertex)
			sampleResult.firstPathVertexEvent = lastBSDFEvent;

		sampleResult.specularGlossyCausticPath = IsStillSpecularGlossyCausticPath(sampleResult.specularGlossyCausticPath,
				bsdf, lastBSDFEvent, depthInfo);

		// Increment path depth informations
		depthInfo.IncDepths(lastBSDFEvent);

		Spectrum throughputFactor(1.f);
		// Russian Roulette
		float rrProb;
		if (!(lastBSDFEvent & SPECULAR) && (depthInfo.GetRRDepth() >= rrDepth)) {
			rrProb = RenderEngine::RussianRouletteProb(bsdfSample, rrImportanceCap);
			if (rrProb < sampler->GetSample(sampleOffset + 8))
				break;

			// Increase path contribution
			throughputFactor /= rrProb;
		} else
			rrProb = 1.f;

		throughputFactor *= bsdfSample;

		pathThroughput *= throughputFactor;
		assert (!pathThroughput.IsNaN() && !pathThroughput.IsInf());

		// This is valid for irradiance AOV only if it is not a SPECULAR material and
		// first path vertex. Set or update sampleResult.irradiancePathThroughput
		if (sampleResult.firstPathVertex) {
			if (!(bsdf.GetEventTypes() & SPECULAR))
				sampleResult.irradiancePathThroughput = INV_PI * AbsDot(bsdf.hitPoint.shadeN, sampledDir) / rrProb;
			else
				sampleResult.irradiancePathThroughput = Spectrum();
		} else
			sampleResult.irradiancePathThroughput *= throughputFactor;

		// Update volume information
		volInfo.Update(lastBSDFEvent, bsdf);

		eyeRay.Update(bsdf.GetRayOrigin(sampledDir), sampledDir);
		lastNormal = bsdf.hitPoint.intoObject ? bsdf.hitPoint.shadeN : -bsdf.hitPoint.shadeN;
		lastFromVolume =  bsdf.IsVolume();
		lastGlossiness = bsdf.GetGlossiness();
	}

	sampleResult.rayCount = (float)(device->GetTotalRaysCount() - deviceRayCount);
	
	if (photonGICache && (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECTPATHMIX) &&
			!photonGIShowIndirectPathMixUsed)
		sampleResult.radiance[0] = Spectrum(1.f, 0.f, 0.f);
}

//------------------------------------------------------------------------------
// RenderLightSample methods
//------------------------------------------------------------------------------

SampleResult &PathTracer::AddLightSampleResult(vector<SampleResult> &sampleResults,
		const Film *film) const {
	const u_int size = sampleResults.size();
	sampleResults.resize(size + 1);

	SampleResult &sampleResult = sampleResults[size];
	sampleResult.Init(Film::RADIANCE_PER_SCREEN_NORMALIZED, film->GetRadianceGroupCount());

	return sampleResult;
}

void PathTracer::ConnectToEye(IntersectionDevice *device, const Scene *scene,
		const Film *film, const float time, const float u0,
		const LightSource &light,
		const BSDF &bsdf, const Point &lensPoint,
		const Spectrum &flux, PathVolumeInfo volInfo,
		vector<SampleResult> &sampleResults) const {
	// I don't connect camera invisible objects with the eye
	if (bsdf.IsCameraInvisible())
		return;

	Vector eyeDir(bsdf.hitPoint.p - lensPoint);
	const float eyeDistance = eyeDir.Length();
	eyeDir /= eyeDistance;

	BSDFEvent event;
	const Spectrum bsdfEval = bsdf.Evaluate(-eyeDir, &event);

	if (!bsdfEval.Black()) {
		Ray eyeRay(lensPoint, eyeDir,
				0.f,
				eyeDistance,
				time);
		scene->camera->ClampRay(&eyeRay);
		eyeRay.UpdateMinMaxWithEpsilon();

		float filmX, filmY;
		if (scene->camera->GetSamplePosition(&eyeRay, &filmX, &filmY)) {
			const u_int *subRegion = film->GetSubRegion();

			if ((filmX >= subRegion[0]) && (filmX <= subRegion[1]) &&
					(filmY >= subRegion[2]) && (filmY <= subRegion[3])) {
				// I have to flip the direction of the traced ray because
				// the information inside PathVolumeInfo are about the path from
				// the light toward the camera (i.e. ray.o would be in the wrong
				// place).
				Ray traceRay(bsdf.GetRayOrigin(-eyeRay.d), -eyeRay.d,
						eyeDistance - eyeRay.maxt,
						eyeDistance - eyeRay.mint,
						time);
				traceRay.UpdateMinMaxWithEpsilon();
				RayHit traceRayHit;

				BSDF bsdfConn;
				Spectrum connectionThroughput;
				if (!scene->Intersect(device, true, true, &volInfo, u0, &traceRay, &traceRayHit, &bsdfConn,
						&connectionThroughput)) {
					// Nothing was hit, the light path vertex is visible

					const float cameraPdfW = scene->camera->GetPDF(eyeRay, filmX, filmY);
					const float fluxToRadianceFactor = cameraPdfW / (eyeDistance * eyeDistance);

					SampleResult &sampleResult = AddLightSampleResult(sampleResults, film);
					sampleResult.filmX = filmX;
					sampleResult.filmY = filmY;

					sampleResult.pixelX = Floor2UInt(filmX);
					sampleResult.pixelY = Floor2UInt(filmY);
					assert (sampleResult.pixelX >= subRegion[0]);
					assert (sampleResult.pixelX <= subRegion[1]);
					assert (sampleResult.pixelY >= subRegion[2]);
					assert (sampleResult.pixelY <= subRegion[3]);

					// Add radiance from the light source
					sampleResult.radiance[light.GetID()] = connectionThroughput * flux * fluxToRadianceFactor * bsdfEval;
				}
			}
		}
	}
}

//------------------------------------------------------------------------------
// RenderLightSample
//------------------------------------------------------------------------------

void PathTracer::RenderLightSample(IntersectionDevice *device, const Scene *scene, const Film *film,
		Sampler *sampler, vector<SampleResult> &sampleResults) const {
	sampleResults.clear();

	Spectrum lightPathFlux;

	const float timeSample = sampler->GetSample(12);
	const float time = scene->camera->GenerateRayTime(timeSample);

	// Select one light source
	float lightPickPdf;
	const LightSource *light = scene->lightDefs.GetEmitLightStrategy()->
			SampleLights(sampler->GetSample(2), &lightPickPdf);

	if (light) {
		// Initialize the light path
		float lightEmitPdfW;
		Ray nextEventRay;
		lightPathFlux = light->Emit(*scene,
			sampler->GetSample(3), sampler->GetSample(4), sampler->GetSample(5), sampler->GetSample(6), sampler->GetSample(7),
				&nextEventRay.o, &nextEventRay.d, &lightEmitPdfW);
		nextEventRay.UpdateMinMaxWithEpsilon();
		nextEventRay.time = time;

		if (lightPathFlux.Black())
			return;

		lightPathFlux /= lightEmitPdfW * lightPickPdf;
		assert (!lightPathFlux.IsNaN() && !lightPathFlux.IsInf());

		// Sample a point on the camera lens
		Point lensPoint;
		if (!scene->camera->SampleLens(time, sampler->GetSample(8), sampler->GetSample(9),
				&lensPoint))
			return;

		//----------------------------------------------------------------------
		// Trace the light path
		//----------------------------------------------------------------------

		PathVolumeInfo volInfo;
		PathDepthInfo depthInfo;
		while (depthInfo.depth < maxPathDepth.depth) {
			const u_int sampleOffset = lightSampleBootSize +  depthInfo.depth * lightSampleStepSize;

			RayHit nextEventRayHit;
			BSDF bsdf;
			Spectrum connectionThroughput;
			const bool hit = scene->Intersect(device, true, false, &volInfo, sampler->GetSample(sampleOffset),
					&nextEventRay, &nextEventRayHit, &bsdf,
					&connectionThroughput);

			if (hit) {
				// Something was hit

				lightPathFlux *= connectionThroughput;

				//--------------------------------------------------------------
				// Try to connect the light path vertex with the eye
				//--------------------------------------------------------------

				if (!hybridBackForwardEnable || (depthInfo.depth > 0)) {
					ConnectToEye(device, scene, film,
							nextEventRay.time, sampler->GetSample(sampleOffset + 1),
							*light, bsdf, lensPoint, lightPathFlux, volInfo, sampleResults);
				}

				if (depthInfo.depth == maxPathDepth.depth - 1)
					break;

				//--------------------------------------------------------------
				// Build the next vertex path ray
				//--------------------------------------------------------------

				float bsdfPdf;
				Vector sampledDir;
				BSDFEvent lastBSDFEvent;
				float cosSampleDir;
				const Spectrum bsdfSample = bsdf.Sample(&sampledDir,
						sampler->GetSample(sampleOffset + 2),
						sampler->GetSample(sampleOffset + 3),
						&bsdfPdf, &cosSampleDir, &lastBSDFEvent);
				if (bsdfSample.Black() ||
						(hybridBackForwardEnable && (!(lastBSDFEvent & SPECULAR) &&
							!((lastBSDFEvent & GLOSSY) && (bsdf.GetGlossiness() <= hybridBackForwardGlossinessThreshold)))))
					break;

				if (depthInfo.GetRRDepth() >= rrDepth) {
					// Russian Roulette
					const float prob = RenderEngine::RussianRouletteProb(bsdfSample, rrImportanceCap);
					if (sampler->GetSample(sampleOffset + 4) < prob)
						bsdfPdf *= prob;
					else
						break;
				}

				lightPathFlux *= bsdfSample;
				assert (!lightPathFlux.IsNaN() && !lightPathFlux.IsInf());

				// Update volume information
				volInfo.Update(lastBSDFEvent, bsdf);

				// Increment path depth informations
				depthInfo.IncDepths(lastBSDFEvent);

				nextEventRay.Update(bsdf.GetRayOrigin(sampledDir), sampledDir);
			} else {
				// Ray lost in space...
				break;
			}
		}
	}
}

//------------------------------------------------------------------------------
// ParseOptions
//------------------------------------------------------------------------------

void PathTracer::ParseOptions(const luxrays::Properties &cfg, const luxrays::Properties &defaultProps) {
	// Path depth settings
	maxPathDepth.depth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.total")).Get<int>());
	maxPathDepth.diffuseDepth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.diffuse")).Get<int>());
	maxPathDepth.glossyDepth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.glossy")).Get<int>());
	maxPathDepth.specularDepth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.specular")).Get<int>());

	// For compatibility with the past
	if (cfg.IsDefined("path.maxdepth") &&
			!cfg.IsDefined("path.pathdepth.total") &&
			!cfg.IsDefined("path.pathdepth.diffuse") &&
			!cfg.IsDefined("path.pathdepth.glossy") &&
			!cfg.IsDefined("path.pathdepth.specular")) {
		const u_int maxDepth = Max(0, cfg.Get("path.maxdepth").Get<int>());
		maxPathDepth.depth = maxDepth;
		maxPathDepth.diffuseDepth = maxDepth;
		maxPathDepth.glossyDepth = maxDepth;
		maxPathDepth.specularDepth = maxDepth;
	}

	// Russian Roulette settings
	rrDepth = (u_int)Max(1, cfg.Get(defaultProps.Get("path.russianroulette.depth")).Get<int>());
	rrImportanceCap = Clamp(cfg.Get(defaultProps.Get("path.russianroulette.cap")).Get<float>(), 0.f, 1.f);

	// Clamping settings
	// clamping.radiance.maxvalue is the old radiance clamping, now converted in variance clamping
	sqrtVarianceClampMaxValue = cfg.Get(Property("path.clamping.radiance.maxvalue")(0.f)).Get<float>();
	if (cfg.IsDefined("path.clamping.variance.maxvalue"))
		sqrtVarianceClampMaxValue = cfg.Get(defaultProps.Get("path.clamping.variance.maxvalue")).Get<float>();
	sqrtVarianceClampMaxValue = Max(0.f, sqrtVarianceClampMaxValue);

	forceBlackBackground = cfg.Get(defaultProps.Get("path.forceblackbackground.enable")).Get<bool>();
	
	hybridBackForwardEnable = cfg.Get(defaultProps.Get("path.hybridbackforward.enable")).Get<bool>();
	if (hybridBackForwardEnable) {
		hybridBackForwardPartition = Clamp(cfg.Get(defaultProps.Get("path.hybridbackforward.partition")).Get<float>(), 0.f, 1.f);
		hybridBackForwardGlossinessThreshold = Clamp(cfg.Get(defaultProps.Get("path.hybridbackforward.glossinessthreshold")).Get<float>(), 0.f, 1.f);
	}

	// Update eye sample size
	eyeSampleBootSize = 5;
	eyeSampleStepSize = 9;
	eyeSampleSize = 
		eyeSampleBootSize + // To generate eye ray
		(maxPathDepth.depth + 1) * eyeSampleStepSize; // For each path vertex
	
	// Update light sample size
	lightSampleBootSize = 13;
	lightSampleStepSize = 5;
	lightSampleSize = 
		lightSampleBootSize + // To generate eye ray
		maxPathDepth.depth * lightSampleStepSize; // For each path vertex

}

//------------------------------------------------------------------------------
// Static methods used by RenderEngineRegistry
//------------------------------------------------------------------------------

Properties PathTracer::ToProperties(const Properties &cfg) {
	Properties props;
	
	if (cfg.IsDefined("path.maxdepth") &&
			!cfg.IsDefined("path.pathdepth.total") &&
			!cfg.IsDefined("path.pathdepth.diffuse") &&
			!cfg.IsDefined("path.pathdepth.glossy") &&
			!cfg.IsDefined("path.pathdepth.specular")) {
		const u_int maxDepth = Max(0, cfg.Get("path.maxdepth").Get<int>());
		props << 
				Property("path.pathdepth.total")(maxDepth) <<
				Property("path.pathdepth.diffuse")(maxDepth) <<
				Property("path.pathdepth.glossy")(maxDepth) <<
				Property("path.pathdepth.specular")(maxDepth);
	} else {
		props <<
				cfg.Get(GetDefaultProps().Get("path.pathdepth.total")) <<
				cfg.Get(GetDefaultProps().Get("path.pathdepth.diffuse")) <<
				cfg.Get(GetDefaultProps().Get("path.pathdepth.glossy")) <<
				cfg.Get(GetDefaultProps().Get("path.pathdepth.specular"));
	}

	props <<
			cfg.Get(GetDefaultProps().Get("path.hybridbackforward.enable")) <<
			cfg.Get(GetDefaultProps().Get("path.hybridbackforward.partition")) <<
			cfg.Get(GetDefaultProps().Get("path.hybridbackforward.glossinessthreshold")) <<
			cfg.Get(GetDefaultProps().Get("path.russianroulette.depth")) <<
			cfg.Get(GetDefaultProps().Get("path.russianroulette.cap")) <<
			cfg.Get(GetDefaultProps().Get("path.clamping.variance.maxvalue")) <<
			cfg.Get(GetDefaultProps().Get("path.forceblackbackground.enable")) <<
			Sampler::ToProperties(cfg);

	return props;
}

const Properties &PathTracer::GetDefaultProps() {
	static Properties props = Properties() <<
			Property("path.hybridbackforward.enable")(false) <<
			Property("path.hybridbackforward.partition")(0.8) <<
			Property("path.hybridbackforward.glossinessthreshold")(.05f) <<
			Property("path.pathdepth.total")(6) <<
			Property("path.pathdepth.diffuse")(4) <<
			Property("path.pathdepth.glossy")(4) <<
			Property("path.pathdepth.specular")(6) <<
			Property("path.russianroulette.depth")(3) <<
			Property("path.russianroulette.cap")(.5f) <<
			Property("path.clamping.variance.maxvalue")(0.f) <<
			Property("path.forceblackbackground.enable")(false);

	return props;
}
