/***************************************************************************
 * Copyright 1998-2015 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxRender.                                       *
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

#ifndef _SLG_PATHCPU_H
#define	_SLG_PATHCPU_H

#include "slg/slg.h"
#include "slg/engines/cpurenderengine.h"
#include "slg/samplers/sampler.h"
#include "slg/film/film.h"
#include "slg/film/filmsamplesplatter.h"
#include "slg/bsdf/bsdf.h"
#include "slg/utils/pathdepthinfo.h"

namespace slg {

//------------------------------------------------------------------------------
// Path tracing CPU render engine
//------------------------------------------------------------------------------

class PathCPURenderEngine;

class PathCPURenderThread : public CPUNoTileRenderThread {
public:
	PathCPURenderThread(PathCPURenderEngine *engine, const u_int index,
			luxrays::IntersectionDevice *device);

	friend class PathCPURenderEngine;

protected:
	void RenderFunc();
	virtual boost::thread *AllocRenderThread() { return new boost::thread(&PathCPURenderThread::RenderFunc, this); }

	void InitSampleResults(vector<SampleResult> &sampleResults) const;
	void RenderSample(const Film *film, Sampler *sampler, vector<SampleResult> &sampleResults) const;

	void GenerateEyeRay(const Film *film, luxrays::Ray &eyeRay, Sampler *sampler, SampleResult &sampleResult) const;

	bool DirectLightSampling(
		const float time, const float u0,
		const float u1, const float u2,
		const float u3, const float u4,
		const luxrays::Spectrum &pathThrouput, const BSDF &bsdf,
		PathVolumeInfo volInfo, const u_int depth,
		SampleResult *sampleResult) const;

	void DirectHitFiniteLight(const BSDFEvent lastBSDFEvent, const luxrays::Spectrum &pathThrouput,
			const float distance, const BSDF &bsdf, const float lastPdfW,
			SampleResult *sampleResult) const;
	void DirectHitInfiniteLight(const BSDFEvent lastBSDFEvent, const luxrays::Spectrum &pathThrouput,
			const luxrays::Vector &eyeDir, const float lastPdfW,
			SampleResult *sampleResult) const;
};

class PathCPURenderEngine : public CPUNoTileRenderEngine {
public:
	PathCPURenderEngine(const RenderConfig *cfg, Film *flm, boost::mutex *flmMutex);
	virtual ~PathCPURenderEngine();

	virtual RenderEngineType GetType() const { return GetObjectType(); }
	virtual std::string GetTag() const { return GetObjectTag(); }

	//--------------------------------------------------------------------------
	// Static methods used by RenderEngineRegistry
	//--------------------------------------------------------------------------

	static RenderEngineType GetObjectType() { return PATHCPU; }
	static std::string GetObjectTag() { return "PATHCPU"; }
	static luxrays::Properties ToProperties(const luxrays::Properties &cfg);
	static RenderEngine *FromProperties(const RenderConfig *rcfg, Film *flm, boost::mutex *flmMutex);

	// Path depth settings
	PathDepthInfo maxPathDepth;

	u_int rrDepth;
	float rrImportanceCap;

	// Clamping settings
	float sqrtVarianceClampMaxValue;
	float pdfClampValue;

	bool forceBlackBackground;

	friend class PathCPURenderThread;

protected:
	static const luxrays::Properties &GetDefaultProps();

	void InitPixelFilterDistribution();

	CPURenderThread *NewRenderThread(const u_int index,
			luxrays::IntersectionDevice *device) {
		return new PathCPURenderThread(this, index, device);
	}

	virtual void InitFilm();
	virtual void StartLockLess();
	virtual void StopLockLess();

	// Use for Sampler indices
	u_int sampleBootSize, sampleStepSize, sampleSize;

	FilterDistribution *pixelFilterDistribution;
};

}

#endif	/* _SLG_PATHCPU_H */
