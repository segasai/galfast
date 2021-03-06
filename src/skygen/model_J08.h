/***************************************************************************
 *   Copyright (C) 2004 by Mario Juric                                     *
 *   mjuric@astro.Princeton.EDU                                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#if 0
#ifndef J08_h__
#define J08_h__

#include "skygen.h"

// luminosity function texture reference
DEFINE_TEXTURE(J08LF, float, 1, cudaReadModeElementType, false, cudaFilterModeLinear, cudaAddressModeClamp);

// double-exponential+powerlaw Halo model
struct ALIGN(16) J08 : public modelConcept
{
	struct ALIGN(16) host_state_t
	{
		cuxTexture<float> lf;
	};
	struct state
	{
		float rho;
	};
	float rho0, l, h, z0, f, lt, ht, fh, q, n;
	float r_cut2;

	int comp_thin, comp_thick, comp_halo;

	// Management functions
	void load(host_state_t &hstate, const peyton::system::Config &cfg);
	void prerun(host_state_t &hstate, bool draw);
	void postrun(host_state_t &hstate, bool draw);

protected:
 	// Model functions
 	__device__ float sqr(float x) const { return x*x; }
	__device__ float halo_denom(float r, float z) const { return sqr(r) + sqr(q*(z + z0)); }

	__device__ float rho_thin(float r, float z)  const
	{
		float rho = expf((Rg()-r)/l  + (fabsf(z0) - fabsf(z + z0))/h);
		return rho;
	}
	__device__ float rho_thick(float r, float z) const
	{
		float rho = f * expf((Rg()-r)/lt + (fabsf(z0) - fabsf(z + z0))/ht);
		return rho;
	}
	__device__ float rho_halo(float r, float z)  const
	{
		float rho = fh * powf(Rg()/sqrtf(halo_denom(r,z)),n);
		return rho;
	}
	__device__ float rho(float r, float z)       const
	{
		if(sqr(r) + sqr(z) > r_cut2) { return 0.f; }

		float rho = rho0 * (rho_thin(r, z) + rho_thick(r, z) + rho_halo(r, z));
		return rho;
	}

public:
	__device__ float rho(float x, float y, float z, float M) const
	{
		float rh = rho(sqrtf(x*x + y*y), z);
		return rh;
	}

	__device__ void setpos(state &s, float x, float y, float z) const
	{
		s.rho = rho(x, y, z, 0.f);
	}

	__device__ float rho(state &s, float M) const
	{
		float phi = TEX1D(J08LF, M);
		return phi * s.rho;
	}

	__device__ int component(float x, float y, float z, float M, gpuRng::constant &rng) const
	{
		float r = sqrtf(x*x + y*y);

		float thin = rho_thin(r, z);
		float thick = rho_thick(r, z);
		float halo = rho_halo(r, z);
		float rho = thin+thick+halo;

		float pthin  = thin / rho;
		float pthick = (thin + thick) / rho;

		float u = rng.uniform();
		if(u < pthin) { return comp_thin; }
		else if(u < pthick) { return comp_thick; }
		else { return comp_halo; }
	}
};

MODEL_IMPLEMENTATION(J08);

#endif // #ifndef expDisk_h__
#endif
