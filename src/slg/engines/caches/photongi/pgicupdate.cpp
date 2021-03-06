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

#include "slg/engines/caches/photongi/photongicache.h"

using namespace std;
using namespace luxrays;
using namespace slg;

bool PhotonGICache::Update(const u_int threadIndex, const Film &film,
		const boost::function<void()> &threadZeroCallback) {
	if (!params.caustic.enabled || params.caustic.updateSpp == 0)
		return false;

	// Check if it is time to update the caustic cache
	const u_int spp = (u_int)(film.GetTotalSampleCount() / film.GetPixelCount());
	const u_int deltaSpp = spp - lastUpdateSpp;
	if (deltaSpp > params.caustic.updateSpp) {
		// Time to update the caustic cache

		threadsSyncBarrier->wait();

		bool result = true;
		if (threadIndex == 0) {
			const double startTime = WallClockTime();
			
			SLG_LOG("Updating PhotonGI caustic cache: " << spp << " samples/pixel");

			// A safety check to avoid the update if visibility map has been deallocated
			if (visibilityParticles.size() == 0) {
				SLG_LOG("ERROR: Updating PhotonGI caustic cache is not possible without visibility information");
				lastUpdateSpp = spp;
				result = false;
			} else {
				// Drop previous cache
				delete causticPhotonsBVH;
				causticPhotonsBVH = nullptr;
				causticPhotons.clear();

				// Trace the photons for a new one
				TracePhotons(false, params.caustic.enabled);

				if (causticPhotons.size() > 0) {
					// Marge photons if required
					if (params.caustic.mergeRadiusScale > 0.f) {
						SLG_LOG("PhotonGI merging caustic photons BVH");
						MergeCausticPhotons();
					}

					// Build a new BVH
					SLG_LOG("PhotonGI building caustic photons BVH");
					causticPhotonsBVH = new PGICPhotonBvh(&causticPhotons, params.caustic.lookUpMaxCount,
							params.caustic.lookUpRadius, params.caustic.lookUpNormalAngle);
				}

				lastUpdateSpp = spp;

				if (threadZeroCallback)
					threadZeroCallback();
			}
			
			const float dt = WallClockTime() - startTime;
			SLG_LOG("Updating PhotonGI caustic cache done in: " << std::setprecision(3) << dt << " secs");
		}

		threadsSyncBarrier->wait();
		
		return result;
	} else
		return false;
}
