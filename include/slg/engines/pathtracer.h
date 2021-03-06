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

#ifndef _SLG_PATHTRACER_H
#define	_SLG_PATHTRACER_H

#include "slg/slg.h"
#include "slg/engines/cpurenderengine.h"
#include "slg/samplers/sampler.h"
#include "slg/film/film.h"
#include "slg/film/filmsamplesplatter.h"
#include "slg/bsdf/bsdf.h"
#include "slg/utils/pathdepthinfo.h"

namespace slg {

//------------------------------------------------------------------------------
// Path Tracing render code
//
// All the methods of this class must be thread-safe because they will be used
// by render threads	
//------------------------------------------------------------------------------

class PhotonGICache;

class PathTracer {
public:
	PathTracer();
	virtual ~PathTracer();

	void InitPixelFilterDistribution(const Filter *pixelFilter);
	void DeletePixelFilterDistribution();

	void SetPhotonGICache(const PhotonGICache *cache) { photonGICache = cache; }
	
	void ParseOptions(const luxrays::Properties &cfg, const luxrays::Properties &defaultProps);

	void InitEyeSampleResults(const Film *film, std::vector<SampleResult> &sampleResults) const;
	void RenderEyeSample(luxrays::IntersectionDevice *device, const Scene *scene,
			const Film *film, Sampler *sampler, std::vector<SampleResult> &sampleResults) const;

	void RenderLightSample(luxrays::IntersectionDevice *device, const Scene *scene,
			const Film *film, Sampler *sampler, std::vector<SampleResult> &sampleResults) const;

	static luxrays::Properties ToProperties(const luxrays::Properties &cfg);
	static const luxrays::Properties &GetDefaultProps();
	
	// Used for Sampler indices
	u_int eyeSampleBootSize, eyeSampleStepSize, eyeSampleSize;
	u_int lightSampleBootSize, lightSampleStepSize, lightSampleSize;

	// Path depth settings
	PathDepthInfo maxPathDepth;

	u_int rrDepth;
	float rrImportanceCap;

	// Clamping settings
	float sqrtVarianceClampMaxValue;

	float hybridBackForwardPartition, hybridBackForwardGlossinessThreshold;
	
	bool forceBlackBackground, hybridBackForwardEnable;

private:
	void GenerateEyeRay(const Camera *camera, const Film *film,
			luxrays::Ray &eyeRay, PathVolumeInfo &volInfo,
			Sampler *sampler, SampleResult &sampleResult) const;

	typedef enum {
		ILLUMINATED, SHADOWED, NOT_VISIBLE
	} DirectLightResult;

	// RenderEyeSample methods

	DirectLightResult DirectLightSampling(
		luxrays::IntersectionDevice *device, const Scene *scene,
		const float time, const float u0,
		const float u1, const float u2,
		const float u3, const float u4,
		const PathDepthInfo &depthInfo,
		const luxrays::Spectrum &pathThrouput, const BSDF &bsdf,
		PathVolumeInfo volInfo, SampleResult *sampleResult) const;

	void DirectHitFiniteLight(const Scene *scene, const PathDepthInfo &depthInfo,
			const BSDFEvent lastBSDFEvent, const luxrays::Spectrum &pathThrouput,
			const luxrays::Ray &ray, const luxrays::Normal &rayNormal, const bool rayFromVolume,
			const float distance, const BSDF &bsdf, const float lastPdfW,
			SampleResult *sampleResult) const;
	void DirectHitInfiniteLight(const Scene *scene, const PathDepthInfo &depthInfo,
			const BSDFEvent lastBSDFEvent, const luxrays::Spectrum &pathThrouput, const BSDF *bsdf,
			const luxrays::Ray &ray, const luxrays::Normal &rayNormal, const bool rayFromVolume,
			const float lastPdfW, SampleResult *sampleResult) const;
	bool CheckDirectHitVisibilityFlags(const LightSource *lightSource,
			const PathDepthInfo &depthInfo,	const BSDFEvent lastBSDFEvent) const;
	bool IsStillSpecularGlossyCausticPath(const bool isSpecularGlossyCausticPath,
			const BSDF &bsdf, const BSDFEvent lastBSDFEvent,
			const PathDepthInfo &depthInfo) const;

	// RenderLightSample methods

	SampleResult &AddLightSampleResult(std::vector<SampleResult> &sampleResults,
			const Film *film) const;
	void ConnectToEye(luxrays::IntersectionDevice *device, const Scene *scene,
			const Film *film, const float time, const float u0,
			const LightSource &light,
			const BSDF &bsdf, const luxrays::Point &lensPoint,
			const luxrays::Spectrum &flux, PathVolumeInfo volInfo,
			std::vector<SampleResult> &sampleResults) const;

	FilterDistribution *pixelFilterDistribution;
	const PhotonGICache *photonGICache;
};

}

#endif	/* _SLG_PATHTRACER_H */
