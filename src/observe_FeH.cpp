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

#include "config.h"

#include "gpc_cpp.h"

#include <boost/shared_ptr.hpp>
#include <boost/lambda/lambda.hpp>
#include <sstream>

#include "simulate.h"
#include "projections.h"
#include "model.h"
#include "paralax.h"
#include "analysis.h"
//#include "dm.h"
#include "io.h"
#include "gpu.h"
#include "simulate_base.h"

#include <vector>
#include <map>
#include <string>
#include <fstream>

#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_linalg.h>

#include <astro/io/format.h>
#include <astro/math.h>
#include <astro/util.h>
#include <astro/system/log.h>
#include <astro/useall.h>

using namespace boost::lambda;
namespace ct = column_types;
#include "observe.h"



#if 1

//void os_FeH_kernel(otable_ks ks, os_FeH_data par, gpu_rng_t rng, ct::cint::gpu_t comp, ct::cfloat::gpu_t XYZ, ct::cfloat::gpu_t FeH);
DECLARE_KERNEL(
	os_FeH_kernel(otable_ks ks, os_FeH_data par, gpu_rng_t rng, ct::cint::gpu_t comp, ct::cfloat::gpu_t XYZ, ct::cfloat::gpu_t FeH))

size_t os_FeH::process(otable &in, size_t begin, size_t end, rng_t &rng)
{
	// ASSUMPTIONS:
	//	- Bahcall-Soneira component tags exist in input
	//	- galactocentric XYZ coordinates exist in input
	//	- all stars are main sequence

	// fetch prerequisites
	using namespace column_types;
	cint   &comp  = in.col<int>("comp");
	cfloat &XYZ   = in.col<float>("XYZ");
	cfloat &FeH   = in.col<float>("FeH");

	CALL_KERNEL(os_FeH_kernel, otable_ks(begin, end), *this, rng, comp, XYZ, FeH);
	return nextlink->process(in, begin, end, rng);
}
#else
size_t os_FeH::process(otable &in, size_t begin, size_t end, rng_t &rng)
{
	// ASSUMPTIONS:
	//	- Bahcall-Soneira component tags exist in input
	//	- galactocentric XYZ coordinates exist in input
	//	- all stars are main sequence
		
	// fetch prerequisites
	using namespace column_types;
	cint   comp  = in.col<int>("comp");
	cfloat XYZ   = in.col<float>("XYZ");
	cfloat FeH   = in.col<float>("FeH");

	for(size_t row=begin; row != end; row++)
	{
		switch(comp[row])
		{
			case BahcallSoneira_model::THIN:
			case BahcallSoneira_model::THICK: {
				// choose the gaussian to draw from
				double p = rng.uniform()*(A[0]+A[1]);
				int i = p < A[0] ? 0 : 1;

				// calculate mean
				double muD = muInf + DeltaMu*exp(-fabs(XYZ(row, 2))/Hmu);		// Bond et al. A2
				double aZ = muD - 0.067;

				// draw
				FeH[row] = rng.gaussian(sigma[i]) + aZ + offs[i];
			} break;
			case BahcallSoneira_model::HALO:
				FeH[row] = offs[2] + rng.gaussian(sigma[2]);
				break;
			default:
				THROW(ENotImplemented, "We should have never gotten here");
		}
	}

	return nextlink->process(in, begin, end, rng);
}
#endif

bool os_FeH::construct(const Config &cfg, otable &t, opipeline &pipe)
{
	cfg.get(A[0],     "A0",     0.63f);
	cfg.get(sigma[0], "sigma0", 0.20f);
	cfg.get(offs[0],  "offs0",  0.00f);
	cfg.get(A[1],     "A1",     0.37f);
	cfg.get(sigma[1], "sigma1", 0.20f);
	cfg.get(offs[1],  "offs1",  0.14f);
	
	cfg.get(Hmu,      "Hmu",     500.f);
	cfg.get(muInf,    "muInf",  -0.82f);
	cfg.get(DeltaMu,  "deltaMu", 0.55f);

	// renormalize disk gaussian amplitudes to sum up to 1
	double sumA = A[0] + A[1];
	A[0] /= sumA; A[1] /= sumA;

	// Halo configuration
	cfg.get(sigma[2], "sigmaH",  0.30f);
	cfg.get(offs[2],  "offsH",  -1.46f);

	// Output model parameters
	MLOG(verb2) << "Normalized disk amplitudes  (A[0], A[1]): "<< A[0] << " " << A[1];
	MLOG(verb2) << "Disk sigma          (sigma[0], sigma[1]): "<< sigma[0] << " " << sigma[1];
	MLOG(verb2) << "Disk offsets          (offs[0], offs[1]): "<< offs[0] << " " << offs[1];
	MLOG(verb2) << "Disk median Z dep. (muInf, deltaMu, Hmu): "<< muInf << " " << DeltaMu << " " << Hmu;
	MLOG(verb2) << "Halo gaussian              (muH, sigmaH): "<< offs[2] << " " << sigma[2];

	return true;
}


DECLARE_KERNEL(os_fixedFeH_kernel(otable_ks ks, float fixedFeH, ct::cfloat::gpu_t FeH));
size_t os_fixedFeH::process(otable &in, size_t begin, size_t end, rng_t &rng)
{
	// ASSUMPTIONS:
	//	- Bahcall-Soneira component tags exist in input
	//	- galactocentric XYZ coordinates exist in input
	//	- all stars are main sequence
	using namespace column_types;
	cfloat &FeH   = in.col<float>("FeH");

	CALL_KERNEL(os_fixedFeH_kernel, otable_ks(begin, end), fixedFeH, FeH);
	return nextlink->process(in, begin, end, rng);
}

bool os_fixedFeH::construct(const Config &cfg, otable &t, opipeline &pipe)
{
	if(!cfg.count("FeH")) { THROW(EAny, "Keyword 'filename' must exist in config file"); }
	cfg.get(fixedFeH, "FeH", 0.f);

	return true;
}

bool os_unresolvedMultiples::runtime_init(otable &t)
{
	// Not ready until absmag is available
	if(!osink::runtime_init(t)) { return false; }

	// by default, absmagSys1 is aliased to absmag. Drop this alias, as we're going to
	// provide absmagSys1
	t.drop_column("M1");

	// output absolute magnitudes
	otable::columndef &col = t.getColumn("absmag");
	const std::string &absmag = col.getPrimaryName();
	absmagSys = absmag + "Sys";
	std::string band = col.get_property("band");
	std::string absmagSysDef = absmagSys + "[2]{class=magnitude;alias=absmagSys;band=" + band + ";"
//		+ "fieldNames=0:" + absmag + "1,1:" + absmag + "2;}";
		+ "fieldNames=0:M1,1:M2;}";
	t.use_column(absmagSysDef);

	// number of components present
	std::string ncompDef = absmagSys + "Ncomp{type=int;fmt=%1d;}";
	t.use_column(ncompDef);

	return true;
}

DECLARE_KERNEL(os_unresolvedMultiples_kernel(otable_ks ks, gpu_rng_t rng, int nabsmag, ct::cfloat::gpu_t M, ct::cfloat::gpu_t Msys, ct::cint::gpu_t ncomp, multiplesAlgorithms::algo algo));
size_t os_unresolvedMultiples::process(otable &in, size_t begin, size_t end, rng_t &rng)
{
	// ASSUMPTIONS:
	//	- Bahcall-Soneira component tags exist in input
	//	- galactocentric XYZ coordinates exist in input
	//	- all stars are main sequence
	using namespace column_types;
	cfloat &M     = in.col<float>("absmag");
	cfloat &Msys  = in.col<float>(absmagSys);
	cint   &ncomp = in.col<int>(absmagSys+"Ncomp");

	CALL_KERNEL(os_unresolvedMultiples_kernel, otable_ks(begin, end), rng, Msys.width(), M, Msys, ncomp, algo);
	return nextlink->process(in, begin, end, rng);
}

DECLARE_TEXTURE(secProb);
DECLARE_TEXTURE(cumLF);
DECLARE_TEXTURE(invCumLF);

bool os_unresolvedMultiples::construct(const Config &cfg, otable &t, opipeline &pipe)
{
	std::string LFfile, binaryFractionFile, strAlgo;
	cfg.get(LFfile, "lumfunc", "");
	cfg.get(binaryFractionFile, "fraction_file", "");
	strAlgo = cfg.get("algorithm");

	// decide on the secondary assignment algorithm
	using namespace multiplesAlgorithms;
	if(strAlgo == "LF_M2_gt_M1") 		{ algo = LF_M2_GT_M1; }
	else if(strAlgo == "LF")		{ algo = LF; }
	else if(strAlgo == "equal_mass")	{ algo = EQUAL_MASS; }
	else { THROW(EAny, "Unknow secondary mag. assignment algorithm '" + strAlgo + "'"); }

	// Load binary fraction
	if(!binaryFractionFile.empty())
	{
		secProbManager.load(binaryFractionFile.c_str(), 64);
	}
	else
	{
		// 100% binary fraction across all plausible absolute
		// magnitudes
		std::vector<double> x, y;
		x.push_back(-100); y.push_back(1.);
		x.push_back(+100); y.push_back(1.);
		secProbManager.construct(&x[0], &y[0], x.size(), 64);
	}

	// Load luminosity function
	std::vector<double> x, y;
	if(!LFfile.empty())
	{
		text_input_or_die(datain, LFfile);
		::load(datain, x, 0, y, 1);
	}
	else
	{
		// generate uniform LF extending over a plausible range of
		// absolute magnitudes
		x.push_back(-100); y.push_back(1.);
		x.push_back(+100); y.push_back(1.);
	}

	// Construct cumulative distribution (the normalized integral of
	// piecewise linearly interpolated luminosify function)
	const int NPIX = 256;
	spline lf; lf.construct(x, y);
	double dx = (x.back() - x.front()) / (NPIX-1);
	std::vector<double> ycum(NPIX), xcum(NPIX);
	xcum[0] = x.front(); ycum[0] = 0;
	double yprev = lf(x.front());
	FOR(1, NPIX)
	{
		double xx = x.front() + i*dx;
		double yy = lf(xx);

		double dy = yy - yprev;
		double dA = (yprev + 0.5*dy)*dx;	// increase in area from y[i-1] to y[i]

		xcum[i] = xx;
		ycum[i] = ycum[i-1] + dA;

		yprev = yy;
		//std::cerr << xcum[i] << " " << ycum[i] << "\n";
	}
	double norm = ycum.back();
	FOR(0, ycum.size()) { ycum[i] /= norm; }
	//FOR(0, ycum.size()) { std::cerr << xcum[i] << " " << ycum[i] << "\n"; }

	// NOTE: WARNING: because of resampling, invCumLF(cumLF(x)) != x,
	// so DONT EVER DEPEND ON IT!
	   cumLFManager.construct(&xcum[0], &ycum[0], xcum.size(), NPIX);
	invCumLFManager.construct(&ycum[0], &xcum[0], xcum.size(), NPIX);
	//FOR(0, xcum.size()) { std::cerr << xcum[i] << " " << ycum[i] << " " << cumLFManager.sample(xcum[i]) << "\n"; }

// 	for(float u=0; u <=1; u += 0.01)
// 	{
// 		std::cerr << u << "\t" << invCumLFManager.sample(u) << "\n";
// 	}

	return true;
}
