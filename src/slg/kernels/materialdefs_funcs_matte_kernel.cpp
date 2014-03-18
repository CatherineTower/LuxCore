#include <string>
namespace slg { namespace ocl {
std::string KernelSource_materialdefs_funcs_matte = 
"#line 2 \"materialdefs_funcs_matte.cl\"\n"
"\n"
"/***************************************************************************\n"
" * Copyright 1998-2013 by authors (see AUTHORS.txt)                        *\n"
" *                                                                         *\n"
" *   This file is part of LuxRender.                                       *\n"
" *                                                                         *\n"
" * Licensed under the Apache License, Version 2.0 (the \"License\");         *\n"
" * you may not use this file except in compliance with the License.        *\n"
" * You may obtain a copy of the License at                                 *\n"
" *                                                                         *\n"
" *     http://www.apache.org/licenses/LICENSE-2.0                          *\n"
" *                                                                         *\n"
" * Unless required by applicable law or agreed to in writing, software     *\n"
" * distributed under the License is distributed on an \"AS IS\" BASIS,       *\n"
" * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*\n"
" * See the License for the specific language governing permissions and     *\n"
" * limitations under the License.                                          *\n"
" ***************************************************************************/\n"
"\n"
"//------------------------------------------------------------------------------\n"
"// Matte material\n"
"//------------------------------------------------------------------------------\n"
"\n"
"#if defined (PARAM_ENABLE_MAT_MATTE)\n"
"\n"
"float3 MatteMaterial_Evaluate(__global Material *material,\n"
"		__global HitPoint *hitPoint, const float3 lightDir, const float3 eyeDir,\n"
"		BSDFEvent *event, float *directPdfW\n"
"		TEXTURES_PARAM_DECL) {\n"
"	if (directPdfW)\n"
"		*directPdfW = fabs(lightDir.z * M_1_PI_F);\n"
"\n"
"	*event = DIFFUSE | REFLECT;\n"
"\n"
"	const float3 kd = Spectrum_Clamp(Texture_GetSpectrumValue(&texs[material->matte.kdTexIndex], hitPoint\n"
"			TEXTURES_PARAM));\n"
"	return kd * fabs(lightDir.z * M_1_PI_F);\n"
"}\n"
"\n"
"float3 MatteMaterial_Sample(__global Material *material,\n"
"		__global HitPoint *hitPoint, const float3 fixedDir, float3 *sampledDir,\n"
"		const float u0, const float u1, \n"
"		float *pdfW, float *cosSampledDir, BSDFEvent *event,\n"
"		const BSDFEvent requestedEvent\n"
"		TEXTURES_PARAM_DECL) {\n"
"	if (!(requestedEvent & (DIFFUSE | REFLECT)) ||\n"
"			(fabs(fixedDir.z) < DEFAULT_COS_EPSILON_STATIC))\n"
"		return BLACK;\n"
"\n"
"	*sampledDir = (signbit(fixedDir.z) ? -1.f : 1.f) * CosineSampleHemisphereWithPdf(u0, u1, pdfW);\n"
"\n"
"	*cosSampledDir = fabs((*sampledDir).z);\n"
"	if (*cosSampledDir < DEFAULT_COS_EPSILON_STATIC)\n"
"		return BLACK;\n"
"\n"
"	*event = DIFFUSE | REFLECT;\n"
"\n"
"	const float3 kd = Spectrum_Clamp(Texture_GetSpectrumValue(&texs[material->matte.kdTexIndex], hitPoint\n"
"			TEXTURES_PARAM));\n"
"	return kd;\n"
"}\n"
"\n"
"#endif\n"
; } }
