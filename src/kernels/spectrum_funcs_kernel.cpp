#include <string>
namespace luxrays { namespace ocl {
std::string KernelSource_spectrum_funcs = 
"#line 2 \"specturm_funcs.cl\"\n"
"\n"
"/***************************************************************************\n"
" *   Copyright (C) 1998-2010 by authors (see AUTHORS.txt )                 *\n"
" *                                                                         *\n"
" *   This file is part of LuxRays.                                         *\n"
" *                                                                         *\n"
" *   LuxRays is free software; you can redistribute it and/or modify       *\n"
" *   it under the terms of the GNU General Public License as published by  *\n"
" *   the Free Software Foundation; either version 3 of the License, or     *\n"
" *   (at your option) any later version.                                   *\n"
" *                                                                         *\n"
" *   LuxRays is distributed in the hope that it will be useful,            *\n"
" *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *\n"
" *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *\n"
" *   GNU General Public License for more details.                          *\n"
" *                                                                         *\n"
" *   You should have received a copy of the GNU General Public License     *\n"
" *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *\n"
" *                                                                         *\n"
" *   LuxRays website: http://www.luxrender.net                             *\n"
" ***************************************************************************/\n"
"\n"
"bool Spectrum_IsEqual(const float3 a, const float3 b) {\n"
"	return all(isequal(a, b));\n"
"}\n"
"\n"
"bool Spectrum_IsBlack(const float3 a) {\n"
"	return Spectrum_IsEqual(a, BLACK);\n"
"}\n"
"\n"
"float Spectrum_Filter(const float3 s)  {\n"
"	return fmax(s.s0, fmax(s.s1, s.s2));\n"
"}\n"
"\n"
"float Spectrum_Y(const float3 s) {\n"
"	return 0.212671f * s.s0 + 0.715160f * s.s1 + 0.072169f * s.s2;\n"
"}\n"
; } }
