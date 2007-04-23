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
 
#define FIT_FULL_3D	0
#define FIT_2D		1

#include "analysis.h"
#include "model.h"
#include "projections.h"
#include "container.h"

#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iomanip>

#include <gsl/gsl_blas.h>
#include <gsl/gsl_multifit_nlin.h>
#include <gsl/gsl_statistics_double.h>
#include <gsl/gsl_rng.h>

#include <astro/system/options.h>
#include <astro/exceptions.h>
#include <astro/util.h>
#include <astro/math.h>
#include <astro/system/log.h>
#include <astro/io/format.h>
#include <astro/useall.h>

using namespace std;

extern "C"
int model_fdf (const gsl_vector * v, void *params, gsl_vector * f, gsl_matrix * J)
{
	model_fitter *m = (model_fitter *)params;

	m->set_parameters(v);
	m->fdf(f, J);
}

extern "C"
int model_f (const gsl_vector * v, void *params, gsl_vector * f)
{
	return model_fdf(v, params, f, NULL);
}

extern "C"
int model_df (const gsl_vector * v, void *params, gsl_matrix * J)
{
	return model_fdf(v, params, NULL, J);
}

spinner spin;
bool print_fitter_progress = false;
int print_state (size_t iter, gsl_multifit_fdfsolver * s, int dof, model_fitter &m)
{
	spin.tick();

	if(!print_fitter_progress) { return 0; }

	fprintf (stderr, "iter: %3u x = ", iter);
	int fmt = 0;
	FOR(0, dof)
	{
		//fprintf(stderr, "%15.8f ", gsl_vector_get(s->x, i));
		while(m.fixed[fmt]) fmt++;
		std::string sfmt(m.param_format[fmt]); sfmt += " ";
	  	fprintf(stderr, sfmt.c_str(), gsl_vector_get(s->x, i));
		fmt++;
	}
	//fprintf(stderr, "|f(x)| = %.8g\n", gsl_blas_dnrm2 (s->f));
	double chi2 = sqr(gsl_blas_dnrm2 (s->f));
	chi2 = chi2 / (m.ndata() - m.ndof());
	fprintf(stderr, "chi^2/dof = %.8g\n", chi2);
}

int model_fitter::fit(int cullIter, const std::vector<double> &nsigma)
{
	//
	// Generic fit driver
	//
	int n = ndata();	// number of data points

	spin.start();

	// allocate arrays, copy non-constant parameters into the array
	const int ndof = this->ndof();
	gsl_vector *v = gsl_vector_alloc(ndof);
	get_parameters(v);

	// initialize and set up the solver
	gsl_multifit_fdfsolver *s = gsl_multifit_fdfsolver_alloc (gsl_multifit_fdfsolver_lmsder, n, ndof);

	gsl_multifit_function_fdf f;
	f.f = &model_f;
	f.df = &model_df;
	f.fdf = &model_fdf;
	f.n = n;
	f.p = ndof;
	f.params = (void *)this;
	gsl_multifit_fdfsolver_set (s, &f, v);

	//{ THROW(EAny, std::string(io::format("status = %s\n") << gsl_strerror (0))); }

	// iterate
	int status = GSL_CONTINUE;
	for(int iter = 0; status == GSL_CONTINUE && iter < 10000; iter++)
	{
		status = gsl_multifit_fdfsolver_iterate (s);

		print_state (iter, s, ndof, *this);
		if (status && status != GSL_CONTINUE) { break; }; status = 0;
		//if (status) { break; }

//		status = gsl_multifit_test_delta (s->dx, s->x, 1e-6, 1e-6);
		status = gsl_multifit_test_delta (s->dx, s->x, epsabs, epsrel);
	}
	spin.stop(); cerr << "\n";

	if(status != 0) { THROW(EAny, std::string(io::format("status = %s\n") << gsl_strerror (status))); }

	// extract fitted parameters
	set_parameters(s->x);
	// expand the covariance matrix
	gsl_matrix *cov = gsl_matrix_alloc (ndof, ndof);
	gsl_multifit_covar (s->J, 0.0, cov);
	int np = nparams;
	covar.resize(np*np); fill(covar.begin(), covar.end(), 0.);
	int y = 0;
	FORj(r, 0, np) // rows
	{
		if(fixed[r]) continue;
		int x = 0;
		FORj(c, 0, np) // columns
		{
			if(fixed[c]) continue;
			covar[r*np + c] = gsl_matrix_get(cov, y, x);
			x++;
		}
		y++;
	}

	// chi^2/DOF
	double chi = gsl_blas_dnrm2(s->f);
	chi2_per_dof = sqr(chi)/ (n - ndof);

	// free the memory	
	gsl_matrix_free(cov);
	gsl_multifit_fdfsolver_free (s);

	std::map<int, int> resmap1, resmap;
	residual_distribution(resmap1, .25);
	FOREACH(resmap1) { std::cerr << .25*(*i).first << "\t" << (*i).second << "\n"; }

	// fold the distribution
	FOREACH(resmap1) { resmap[std::abs(i->first)] += i->second; }

/*	int nsigmaidx = nsigma.size() ? (int)(nsigma[0]/.25) : 1;
	if(cullIter-- && resmap.upper_bound(nsigmaidx) != resmap.end())*/

	// find maximum deviation
	typeof(resmap.begin()) it = resmap.end(); it--; double maxsig = 0.25*(*it).first;
	std::cerr << "MAXSIGMA = " << maxsig << "\n";
	int k = -1;
	FOR(0, nsigma.size()) { if(nsigma[i] <= maxsig) { k = i; break; } }
	std::cerr << "K = " << k << "\n";
	if(cullIter-- && k != -1)
	{
		std::cerr << "\tCulling nsigma > " << nsigma[k] << ": ";
		cull(nsigma[k]);

		// reset to initial parameters
		set_parameters(v);
		if(k+1 != nsigma.size())
		{ 
			std::vector<double> nsigma2(nsigma.begin()+k+1, nsigma.end());
			fit(cullIter, nsigma2);
		} else {
			fit(cullIter, nsigma);
		}
	}
	gsl_vector_free(v);
}

void load_disk(vector<rzpixel> *data, const std::string &filename, int ri_bin = 0)
{
	data->clear();
	text_input_or_die(in, filename);

	std::cerr << "Loading from " << filename << "\n";

	rzpixel p;
	//bind(in, p.r, 0, p.z, 1, p.N, 2, p.V, 3); // if loading from cleanedrz.txt
#if FIT_2D
	bind(in, p.r, 0, p.z, 1, p.N, 5, p.V, 6); p.rphi = 0; // if loading from output of median3d.pl
#endif
#if FIT_FULL_3D
	bind(in, p.r, 0, p.rphi, 1, p.z, 2, p.N, 5, p.V, 6); // if loading from output of median3d_ng.pl
#endif
	while(in.next())
	{
		p.rho = p.N / p.V;
		p.sigma = sqrt(p.N) / p.V;
		p.ri_bin = ri_bin;
		data->push_back(p);
	}
};

void clean_disk(vector<rzpixel> *data, const std::string &how, model_fitter &m, const std::string &modelname)
{
	double rbeam;
	if(how == "ngpbeam")
	{
		// find the r value closest to the Sun
		rbeam = 0;
		FOREACH(*data)
		{
			rzpixel &pix = *i;
			if(abs(pix.r-8000.) < abs(rbeam-8000))
				rbeam = pix.r;
		}
		cerr << "Beam radius: r = " << rbeam << "pc\n";
	}

#if 0
	// calculate mean and sigma for each phi arc, and throw out
	// pixels which are more than nSigma away
	std::map<pair<double, double>, vector<rzpixel> > phis;
	FOREACH(*data)
	{
		phis[make_pair((*i).r, (*i).z)].push_back(*i);
	}
	int nstart = data->size();
	data->clear();
	FOREACH(phis)
	{
		vector<rzpixel> &row = (*i).second;
		vector<double> den, wt;
		FOREACH(row)
		{
			rzpixel &p = *i;
//			std::cerr << p.r << " " << p.z << " " << p.rphi << " " << p.N << " " << p.rho << " " << p.sigma << "\n";
			if(p.N < 3) continue;

			double w = 1./sqr(p.sigma);
			den.push_back(p.rho);
			wt.push_back(w);
		}

		if(den.size() < 3) { continue; }


		double mean = gsl_stats_wmean(&wt[0], 1, &den[0], 1, den.size());
		double sigma = gsl_stats_wsd_m (&wt[0], 1, &den[0], 1, den.size(), mean);

		double chi2 = 0;
		FOR(0, wt.size())
		{
			chi2 += wt[i]*sqr(den[i] - mean);
		}
		chi2 /= (wt.size() - 1);
//		std::cerr << "Stats = " << mean << " " << sigma << " " << chi2 << "\n";

		// discard everything nSigma away
		int acc = 0;
		FOREACH(row)
		{
//			if((*i).N < 3) continue;
			double tresh = 3*(*i).sigma;
			if(fabs((*i).rho - mean) > tresh) { continue; }
			data->push_back(*i);
			acc++;
		}
//		std::cerr << "Accepted, Rejected = " << acc << " " << row.size() - acc << "\n\n";
	}
	cerr << nstart - data->size() << " pixels rejected.\n";
	cerr << data->size() << " pixels accepted.\n";
#endif

	//
	// Remove the points near the plane of the galaxy
	// and close to the edges of the survey
	//
	vector<rzpixel> out;
	int magrej = 0;
	FOREACH(*data)
	{
		rzpixel &pix = *i;

#if 0 // this cleanup has already been done in median3d.pl
		double phi = deg(atan2(pix.z,pix.r-8000));

 		if(-30 < phi && phi < 15) continue;
 		if(153 < phi && phi < 180) continue;
 		if(-180 < phi && phi < -150) continue;
#endif
#if FIT_FULL_3D
		if(pix.N < 3) continue;
		Radians phi = (pix.r != 0 ? pix.rphi/pix.r : 0) - ctn::pi;
		double x = pix.r*cos(phi);
		double y = pix.r*sin(phi);
		double D = sqrt(sqr(x-Rg) + sqr(y) + sqr(pix.z));

		if(!between(D, m.d)) { magrej++; continue; }
#endif
		if(how == "minimal")
		{
			if(!(abs(pix.z) >= 75)) continue;

			double phi = deg(atan2(pix.z,pix.r-8000));
			if(between(phi, 0, 21.13223943)) { continue; }
			if(between(phi, -135.6400888, -121.3379493)) { continue; }
			if(between(phi, -180, -141.2091266)) continue;
		}
		else if(how == "thin")
		{
//			if(!(abs(pix.z) >= 75 && abs(pix.z) <= 300)) continue;
			if(abs(pix.z) <= 300) continue;
		
			double phi = deg(atan2(pix.z,pix.r-8000));
			if(between(phi, 0, 21.13223943)) { continue; }
			if(between(phi, -135.6400888, -121.3379493)) { continue; }
			if(between(phi, -180, -141.2091266)) continue;
			if(pix.r < 7277.44 && pix.z > 0) continue;
			if(pix.r > 8800 && pix.z > 0) continue;
		}
		else if(how == "thick")
		{
//			if(pix.z < 2500 && pix.z > 1000 && pix.r < 8000) continue;
//			if(pix.z < -2000 && pix.r > 8000) continue;

// /*			if(!(abs(pix.z) >= 1000 && abs(pix.z) <= 4000)) continue;
// 			if(!(abs(pix.z) >= 200 && abs(pix.z) <= 3000)) continue;*/

//			if(!(abs(pix.z) >= 200 && abs(pix.z) <= 1400)) continue;

//			if(abs(pix.z) <= 500) continue;

			if(abs(pix.z) >= 2500) continue;
			if(abs(pix.z) <= 75) continue;
#if 1
//			if(!(abs(pix.z) >= 200 && abs(pix.z) <= 3500)) continue;
//			if(pix.z > 0 && deg(atan2(pix.z-500,pix.r-8000)) < 30. && pix.r < 10000) continue;
//			if(pix.z > 0 && pix.r < 8000.) continue;
#endif
			if(modelname == "mean1.30")
			{
//				if(pix.N < 3) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
/////				if(pix.r < 7277.44 && pix.z > 0) continue;
				if(pix.r > 8800 && pix.z > 0) continue;
			}
			if(modelname == "mean1.20")
			{
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
/////				if(pix.r < 7277.44 && pix.z > 0) continue;
				if(pix.r > 8800 && pix.z > 0) continue;
#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

/////				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
			}
			if(modelname == "mean1.10")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
/////				if(pix.r < 7277.44 && pix.z > 0) continue;
				if(pix.r > 8800 && pix.z > 0) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

/////				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
			}
			if(modelname == "mean1.00")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
				if(pix.r > 8800 && pix.z > 0) continue;

				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6523.71 && pix.z > 0) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

/////				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
			}
			if(modelname == "mean0.90")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
				if(pix.r > 8800 && pix.z > 0) continue;
				
#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;
			}
			if(modelname == "mean0.80")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;
			}
			if(modelname == "mean0.70")
			{
//				if(abs(pix.z) >= 2000) continue;

				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0) continue;
				if(pix.r > 10500 && pix.z > 0) { continue; }

				// r=6.5 overdensity cutout
				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;
			}
			if(modelname == "mean0.65")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0) continue;

				if(pix.r < 8000 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;

				// remove halo
				if(sqr(pix.r/1000 - 8) > sqr(2.5) && pix.r > 8000) continue;
			}
			if(modelname == "mean0.55")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0) continue;

				if(pix.r < 8000 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;

				// remove halo
				if(sqr(pix.r/1000 - 8) > sqr(2.5) && pix.r > 8000) continue;
			}
 			if(modelname == "mean0.35")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0) continue;

				if(pix.r < 8000 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;

				// remove halo
				if(sqr(pix.r/1000 - 8) > sqr(2.5) && pix.r > 8000) continue;
			}
			if(modelname == "mean0.10")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0) continue;

				if(pix.r < 8000 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;
			}
/*			if(modelname == "mean0.35")
			{
				if(between(pix.r, 5000, 8000) && between(pix.z, 0, 2500)) { continue; }
			}*/
		}
		else if(how == "halo")
		{
//			if(pix.z > 0 && deg(atan2(pix.z-500,pix.r-8000)) < 30. && pix.r < 10000) continue;
//			if(pix.z > 0 && pix.z < 5000 && pix.r < 8000.) continue;

//			if(pix.r < 5000) continue;
			if(abs(pix.z) <= 75) continue;

			if(modelname == "mean1.30")
			{
//				if(pix.N < 3) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
/////				if(pix.r < 7277.44 && pix.z > 0) continue;
				if(pix.r > 8800 && pix.z > 0) continue;
			}
			if(modelname == "mean1.20")
			{
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
/////				if(pix.r < 7277.44 && pix.z > 0) continue;
				if(pix.r > 8800 && pix.z > 0) continue;
#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

/////				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
			}
			if(modelname == "mean1.10")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
/////				if(pix.r < 7277.44 && pix.z > 0) continue;
				if(pix.r > 8800 && pix.z > 0) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

/////				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
			}
			if(modelname == "mean1.00")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
				if(pix.r > 8800 && pix.z > 0) continue;

				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6523.71 && pix.z > 0) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

/////				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
			}

			if(modelname == "mean0.90")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;
				if(pix.r > 8800 && pix.z > 0) continue;
				
#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;
			}
			if(modelname == "mean0.80")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=6.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 18) && pix.z > 0) continue;

				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;
			}
			if(modelname == "mean0.70")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0) continue;
				if(pix.r > 10500 && pix.z > 0) { continue; }

				// r=6.5 overdensity cutout
				if(pix.r < 7500 && pix.z > 0) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;
			}
			if(modelname == "mean0.65")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;

				if(!(abs(pix.z) >= 300)) continue;
			}
			if(modelname == "mean0.60")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -2000.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;

				if(!(abs(pix.z) >= 300)) continue;
			}
			if(modelname == "mean0.55")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -1125.87)) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;

				if(!(abs(pix.z) >= 300)) continue;
			}
			if(modelname == "mean0.50")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -2000.87)) continue;
				if(pix.r > 7051.67 && pix.z < -3772.78) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				if(pix.r < 6299.89 && pix.z > 0) continue;

				if(!(abs(pix.z) >= 300)) continue;
			}
			if(modelname == "mean0.45")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -2000.87)) continue;
				if(pix.r > 7051.67 && pix.z < -3772.78) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				//if(pix.r < 6299.89 && pix.z > 0) continue;

				if(!(abs(pix.z) >= 300)) continue;
			}
			if(modelname == "mean0.35")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
			//	double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
			//	if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -5000.87)) continue;
				//if(pix.r > 7051.67 && pix.z < -3772.78) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				//if(pix.r < 6299.89 && pix.z > 0) continue;

				// reject small runs beyond Virgo
				//if(pix.r < 4000 && pix.z > 0) { continue; }

				if(!(abs(pix.z) >= 1000)) continue;
				
			}
			if(modelname == "mean0.30")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
			//	double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
			//	if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -5000.87)) continue;
				//if(pix.r > 7051.67 && pix.z < -3772.78) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				//if(pix.r < 6299.89 && pix.z > 0) continue;

				// reject small runs beyond Virgo
				//if(pix.r < 4000 && pix.z > 0) { continue; }

				if(!(abs(pix.z) >= 1000)) continue;
				
			}
			if(modelname == "mean0.25")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -5000.87)) continue;
				//if(pix.r > 7051.67 && pix.z < -3772.78) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				//if(pix.r < 6299.89 && pix.z > 0) continue;

				// reject small runs beyond Virgo
				//if(pix.r < 4000 && pix.z > 0) { continue; }

				if(!(abs(pix.z) >= 1000)) continue;
				
			}
			if(
				modelname == "mean0.10"
				|| modelname == "mean0.15"
				|| modelname == "mean0.20")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 5) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
				
				// drop Monoceros
				if(between(phi2, 0, 43) && sqr(pix.r-8000)+sqr(pix.z) < sqr(16000)) continue;
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -5000.87)) continue;
				//if(pix.r > 7051.67 && pix.z < -3772.78) continue;

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				//if(pix.r < 6299.89 && pix.z > 0) continue;
				if(pix.r < 4000) continue;

				// remove all non-halo pixels
				static disk_model fiducial_halo_model;
				static bool fhm_initialized = false;
				if(!fhm_initialized)
				{
					double p[] = { 0.00207, 3979, 230, 22, 0.04, 3018, 1200, 0.00045, 1, 2 };
					std::cerr << sizeof(p) << "\n";
					FOR(0, sizeof(p)/sizeof(p[0])) { fiducial_halo_model.p[i] = p[i]; }

					fhm_initialized = true;
				}
				double denhalo = fiducial_halo_model.rho_halo(pix.r, pix.z, 0);
				double dendisk = fiducial_halo_model.rho(pix.r, pix.z, 0) - denhalo;
				double res = denhalo / dendisk;
				if(res <= 20) { continue; }

				// remove stuff close to the origin
				if(!(abs(pix.z) >= 300)) continue;
				
			}
/*			if(modelname == "mean0.10")
			{
				// r=6.5 overdensity cutout
				if(pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
//				if(pix.z < 0) continue;
			}*/
			if(modelname == "mean0.10")
			{
				// remove the overdensity
//-//				if(pix.z > 0 && pix.r > 8500) { continue; }
				if(pix.N < 15) { continue; }
//				if(pix.z < 0) { continue; }
				double phi = deg(atan2(pix.z,pix.r-8000));
//				std::cerr << phi << "\n";
				if(between(phi, 0, 21.13223943)) { continue; }
				if(between(phi, -135.6400888, -121.3379493)) { continue; }
				if(between(phi, -180, -141.2091266)) continue;
				if(between(phi, -42.71778654, 0)) continue;

#if 0
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 27.81) && pix.z > 0) continue;
#else				
				// r=9.5 overdensity cutout
				double phi2 = deg(atan2(pix.z-750.139,pix.r-8639.45));
				if(between(phi2, -90, 14.6) && pix.z > 0 && pix.r < 10500) continue;

				// r=6.5 overdensity cutout
				if(pix.r > 4000 && pix.r < 8000 && pix.z > 0 && pix.z < 3000) { continue; }
#endif
				// bottom overdensity
				if(between(pix.r, 8680.06, 50000) && between(pix.z, -50000, -5000.87)) continue;
				//if(pix.r > 7051.67 && pix.z < -3772.78) continue;
//				if(pix.z < 0) { continue; }

				// left error?
				if(between(phi, 146.9277825, 180)) continue;
				//if(pix.r < 6299.89 && pix.z > 0) continue;
				//if(pix.r < 4000) continue;

				if(!(abs(pix.z) >= 300)) continue;
				
			}
// 			if(modelname == "mean0.35")
// 			{
// 				if(between(pix.r, 5000, 8000) && between(pix.z, 0, 2500)) { continue; }
// 				if(between(pix.r, 0, 7000) && between(pix.z, 4000, 20000)) { continue; }
// 			}

			// special dispensation for the bluest bin
#if 0
			if(!between(pix.r, 8000, 15000)) continue;
#if 0
			if(!between(pix.r, 6000, 30000)) continue;
//			if(between(pix.r, 12500, 30000) && between(pix.z, 0, 7000)) continue;
#endif
//			if(pix.z < 0) continue;
//			if(pix.z < -10000) continue;
#endif
//			if(!(abs(pix.z) >= 500)) continue;
		}
		else if(how == "ngpbeam")
		{
			if(pix.r != rbeam) continue;
			if(pix.z < 100) continue;
		}

		out.push_back(pix);
	}

	cerr << data->size() - out.size() << " pixels rejected.\n";
	cerr << magrej << " pixels outside limits.\n";
	cerr << out.size() << " pixels accepted.\n";
	*data = out;
}

int fit_ng(int argc, char **argv);

int main(int argc, char **argv)
{
	return fit_ng(argc, argv);
/*
try
{
	VERSION_DATETIME(version, "$Id: fitter.cpp,v 1.14 2007/04/23 21:13:31 mjuric Exp $");

	Options opts(
		argv[0],
		"Program for fitting r-z dataplanes of the Galaxy",
		version,
		Authorship::majuric
	);

	//# add any arguments your program needs. eg:
	opts.argument("binsfile", "File from which to read a list of rz-dataplanes to fit.");
	opts.argument("how", "Which component should be fitted [thin, thick, halo]");

	// add any options your program might need. eg:
	// opts.option("meshFactor", "meshFactor", 0, "--", Option::required, "4", "Resolution decrease between radial steps");

	parse_options(opts, argc, argv);

	/////// Start your application code here
	std::string binsfile = opts["binsfile"];
	std::string how = opts["how"];
	text_input_or_die(in, binsfile);

	model_fitter m;

	m.r = make_pair(float(15), float(21.5));

	std::string rzfile, modelname;
	if(how == "thin")
	{
		m.set_param("rho0", 0, false);
		m.set_param("l", 0, false);
		m.set_param("h", 0, false);
		m.set_param("z0", 0, true);

		m.set_param("f", 0.0, true);
		m.set_param("lt", 3500, true);
		m.set_param("ht", 400, true);
		m.set_param("fh", 0.0, true);
		m.set_param("q", 1.5, true);
		m.set_param("n", 3, true);

		bind(in, modelname, 0, m.ri.first, 1, m.ri.second, 2, rzfile, 3,
			m.param("rho0"), 4,
			m.param("l"), 5,
			m.param("h"), 6,
			m.param("z0"), 7
			);
	}
	else if(how == "thick")
	{
		m.set_param("rho0", 0, true);
		m.set_param("l", 0, false);
		m.set_param("h", 0, false);
		m.set_param("z0", 0, true);

//		m.set_param("f", 0.012, true);
		m.set_param("f", 0.02, false);
		m.set_param("lt", 3500, false);
		m.set_param("ht", 1500, false);

		m.set_param("fh", 0.0, true);
		m.set_param("q", 1.5, true);
		m.set_param("n", 3, true);

		bind(in, modelname, 0, m.ri.first, 1, m.ri.second, 2, rzfile, 3,
			m.param("rho0"), 4,
			m.param("l"), 5,
			m.param("h"), 6,
			m.param("z0"), 7
			);
	}
	else if(how == "halo")
	{
		m.set_param("rho0", 0, true);
		m.set_param("l", 0, true);
		m.set_param("h", 0, true);
		m.set_param("z0", 0, true);
		
		m.set_param("f", 0.04, true);
		m.set_param("lt", 3500, true);
		m.set_param("ht", 1100, false);

		m.set_param("fh", 0.001, false);
		m.set_param("q", 2.3, false);
		m.set_param("n", 3, false);

		bind(in, modelname, 0, m.ri.first, 1, m.ri.second, 2, rzfile, 3,
			m.param("rho0"), 4,
			m.param("l"), 5,
			m.param("h"), 6,
			m.param("z0"), 7
			);
	}
	else if(how == "ngpbeam")
	{
		m.set_param("rho0", 0.06, false);
		m.set_param("l", 2500, true);
		m.set_param("h", 270, false);
		m.set_param("z0", 24, true);

		m.set_param("f", 0.04, false);
		m.set_param("lt", 2500, true);
		m.set_param("ht", 1200, false);

		m.set_param("fh", 0.0, true);
		m.set_param("q", 1.5, true);
		m.set_param("n", 3, true);

		bind(in, modelname, 0, m.ri.first, 1, m.ri.second, 2, rzfile, 3);
	} else {
		ASSERT(0) { std::cerr << "Unknown method " << how << "\n"; }
	}

	m.print(cout, model_fitter::HEADING);
	cout << "\n";
//	cout << "# name ri0 ri1 chi2/dof rho0 l h z0 err(rho0 l h z0)\n";

	vector<rzpixel> data;
	gsl_vector *v = gsl_vector_alloc(m.ndof());
	while(in.next())
	{
		m.get_parameters(v);

		paralax.distance_limits(m.d.first, m.d.second, m.ri.first, m.ri.second, m.r.first, m.r.second);

		load_disk(&data, rzfile);
		clean_disk(&data, how, m, modelname);
		m.setdata(data);

//		m.param("l") = 1.;

		cerr << "Limits (mag) (dist) = (" << m.r.first << ", " << m.r.second << ") (" << m.d.first << ", " << m.d.second << ")\n";
		m.print(cerr);
		cerr << "Fitting " << rzfile << " ";
		m.fit(1, std::vector<double>(1, 10.));
		m.print(cerr);
		cerr << "\n";
//		cerr << "norm_thick = " << m.norm_at_Rg() << "\n";
// 		cerr << "rho(R=8kpc,Z=5kpc) = " << m.rho(8000,5000) << "\n";
// 		cerr << "rho1(R=8kpc,Z=5kpc) = " << m.rho_thin(8000,5000) << "\n";
// 		cerr << "rho2(R=8kpc,Z=5kpc) = " << m.rho_thick(8000,5000) << "\n";
// 		cerr << "rhoh(R=8kpc,Z=5kpc) = " << m.rho_halo(8000,5000) << "\n";

		std::string rzncfile(rzfile);
		int pos = rzncfile.find(".cleaned");
		rzncfile = rzncfile.replace(pos, strlen(".cleaned"), "");
		cout << setw(10) << modelname << setw(7) << m.ri.first << setw(7) << m.ri.second << setw(40) << rzncfile << setw(40) << rzfile;
		cout << " " << setw(10) << m.chi2_per_dof << " ";
		m.print(cout, model_fitter::LINE);
		cout << "\n";

		m.set_parameters(v);
	}
}
catch(EAny &e)
{
	e.print();
}
*/
}

typedef std::pair<double, double> dpair;
struct param_t { dpair range; bool fixed; };

param_t parse_param(const std::string &s)
{
	// reads a triplet such as:
	// <value> [value] [fixed]

	istringstream is(s);
	param_t p;

	std::string fix;
	ASSERT(is >> p.range.first);
	p.range.second = p.range.first;
	p.fixed = false;
	if(is >> fix)
	{
		if(isdigit(fix[0]))
		{
			p.range.second = atoi(fix.c_str());
			if(!(is >> fix)) { fix = "0"; }
		}
		
		if(!isdigit(fix[0]))
		{
			ASSERT(fix == "fixed");
			p.fixed = true;
		}
	}
	return p;
}

typedef std::pair<float, float> fpair;
ISTREAM(fpair &v)
{
	return in >> v.first >> v.second;
}

int fit_ng(int argc, char **argv)
{
try
{
	VERSION_DATETIME(version, "$id$");

	Options opts(argv[0], "SDSS Galactic model fitter", version, Authorship::majuric);

	std::string binsfile, how;
	//# add any arguments your program needs. eg:
	opts.argument("fitparams", "Parameter file (a ``.fit file'') with initial fit parameters (input).").bind(binsfile);
	opts.argument("method", "Which component should be fitted [thin, thick, halo] (input)").bind(how);

	opts.option("p").addname("progress").value("true").param_none().bind(print_fitter_progress).desc("Show fitting progress indicator");
	
	// add any options your program might need. eg:
	// opts.option("meshFactor", "meshFactor", 0, "--", Option::required, "4", "Resolution decrease between radial steps");

	parse_options(opts, argc, argv);

	/////// Start your application code here
	model_fitter m;

	Config cfg(binsfile);
	FOREACH(cfg) { std::cerr << (*i).first << " = " << (*i).second << "\n"; }

	// deduce fit name from filename
// 	std::string subdir = binsfile;
// 	// delete extension
// // 	int pos = subdir.rfind(".txt");
// 	if(pos == std::string::npos) pos = subdir.size();
// 	subdir.replace(pos, 4, "");
// 	// delete path
// 	pos = subdir.rfind("/"); 
// 	if(pos == std::string::npos) pos = 0;
// 	subdir.replace(0, pos, "");

	gsl_rng *r;
	int seed = 42;
	r = gsl_rng_alloc (gsl_rng_default);
	gsl_rng_set(r, seed);
	int nfits = cfg["nfits"];
	std::vector<std::string> rzfile = cfg["data"];
	std::vector<std::string> modelname = cfg["name"];
	std::string fitname = cfg["fit"];
	int ncull = cfg["ncull"];
	std::vector<double> nsigma = cfg["cullsigma"];
	cfg.get(m.epsabs, "epsabs", m.epsabs);
	cfg.get(m.epsrel, "epsrel", m.epsrel);
	m.ri = cfg["ri"];

	// hack
	m.r.clear();
	m.r.insert(m.r.begin(), m.ri.size(), make_pair(float(15), float(21.5)));

	ASSERT(rzfile.size() == modelname.size())
	{
		std::cerr << "Number of model names has to equal the number of input datafiles\n";
	}

	// load parameters from configuration file
	std::map<std::string, param_t> params;
	FOR(0, m.nparams - m.nrho + (rzfile.size()-1))
	{
		std::string param = m.param_name[i];
		if(cfg.count(param))
		{
			params[param] = parse_param(cfg[param]);
		} else {
			ASSERT(0) { std::cerr << "Initial value for " << param << " not specified\n"; }
		}
	}
	// fix unused rho parameters
	FOR(m.nparams - m.nrho + (rzfile.size()-1), m.nparams)
	{
		std::string param = m.param_name[i];
		params[param] = parse_param("0 fixed");
	}

//	cout << "# name ri0 ri1 chi2/dof rho0 l h z0 err(rho0 l h z0)\n";

	gsl_vector *v = gsl_vector_alloc(m.ndof());
	FOR(0, nfits)
	{
		vector<vector<rzpixel> > data(rzfile.size());
		vector<rzpixel> alldata;
		// assing initial parameters
		FOREACHj(j, params)
		{
			param_t &p = (*j).second;
			double val = p.range.first + gsl_rng_uniform(r)*(p.range.second - p.range.first);
			m.set_param((*j).first, val, p.fixed);
			std::cerr << (*j).first << " = " << val << " (fixed = " << p.fixed << ")\n";
		}
// 		m.print(cout, model_fitter::HEADING);
// 		cout << "\n";

		// start fitting	
		m.get_parameters(v);

		// calculate distance limits
		m.d.resize(m.r.size());
		FORj(k, 0, m.r.size())
		{
			paralax.distance_limits(m.d[k].first, m.d[k].second, m.ri[k].first, m.ri[k].second, m.r[k].first, m.r[k].second);
			cerr << "Limits (mag) (dist) = (" << m.r[k].first << ", " << m.r[k].second << ") (" << m.d[k].first << ", " << m.d[k].second << ")\n";
		}

		// load data
		FORj(j, 0, rzfile.size())
		{
			load_disk(&data[j], rzfile[j], j);
			clean_disk(&data[j], how, m, modelname[j]);
			//store_cleaned(&data[j], rzfile[j]);
			alldata.insert(alldata.end(), data[j].begin(), data[j].end());
		}
		m.setdata(alldata);
		cerr << "Total number of pixels = " << alldata.size() << "\n";

//		m.param("l") = 1.;

		m.print(cerr);
		cerr << "Fitting " << join(", ", rzfile) << " ";
		try {
			m.culled.clear();
			m.fit(ncull, nsigma);
		} catch(EAny &e) {
			e.print();
			std::cerr << "Fit failed.";
			if(nfits == 1) { exit(-1); }
			--i;
			continue;
		}
		m.print(cerr);
		cerr << "\n";
		cerr << "norm_thick = " << m.norm_at_Rg(0) << "\n";

		FOREACHj(j, m.culled)
		{
			rzpixel &pix = *j;
			cerr << pix.r << " " << pix.z << " " << pix.N << " " << pix.V << "\n";
		}

		// store the fitted pixels for chi2 grid plotting and residual maps
		std::vector<std::string> rzncfiles(rzfile.size()), rzfitfiles(rzfile.size());
		size_t maxnc = 0, maxf = 0;
		FORj(j, 0, rzfile.size())
		{
			// remove "cleaned" from filename
			std::string rzncfile(rzfile[j]);
			int pos = rzncfile.find(".cleaned");
			rzncfile = rzncfile.replace(pos, strlen(".cleaned"), "");

			// compute the fitted pixels file filename, and subdirectory
			std::string rzfitfile(rzfile[j]);
			rzfitfile = rzfitfile.replace(pos, strlen(".cleaned"), ".fitted");
			if(fitname.size())
			{
				// create the subdirectory
				pos = rzfitfile.find("/");
				pos = pos != std::string::npos ? pos + 1 : 0;

				std::string path = rzfitfile.substr(0, pos) + fitname;
				mkdir(path.c_str(), 0775);
				rzfitfile = rzfitfile.insert(pos, fitname + "/");
			}

			rzncfiles[j] = rzncfile;
			rzfitfiles[j] = rzfitfile;

			maxnc = std::max(maxnc, rzncfile.size());
			maxf  = std::max(maxf,  rzfitfile.size());
		}

		//std::cerr << "maxnc, maxf = " << maxnc << " " << maxf << "\n";
		FORj(j, 0, rzfile.size())
		{
			const std::string &rzncfile = rzncfiles[j];
			const std::string &rzfitfile = rzfitfiles[j];
			
			// dump the pixels used for fit into a new file
			std::ofstream out(rzfitfile.c_str());
			out << "# input file: " << rzfile[j] << "\n";
			FORj(k, 0, m.map.size())
			{
				rzpixel &pix = m.map[k];
				if(pix.ri_bin != j) { continue; }
				out << std::setw(10) << pix.r << " " << pix.z << " 0 0 0 " << pix.N << " " << pix.V << " " << pix.N / pix.V << "\n";
			}
			out.close();

			// fit/model names
			std::string &fit = modelname[j];
			std::string modfit = io::format("%s.%d") << fit << i;

			// write out the fits
			cout << setw(14) << modfit << setprecision(4) << setw(7) << m.ri[j].first << setw(7) << m.ri[j].second << setw(maxnc+3) << rzncfile << setw(maxf+3) << rzfitfile;
			cout << " " << setprecision(10) << m.chi2_per_dof << " ";
			m.print(cout, model_fitter::LINE, j);
			cout << " " << alldata.size() << " " << alldata.size() - m.map.size();
			cout << "\n";
			cout.flush();

			// density at the position of the Sun
			std::cerr << "\rho(R_Sun, Z_0) = " << m.rho(8000, m.z0, j) << "\n";
		}

		m.set_parameters(v);
	}
	gsl_rng_free(r);
}
catch(EAny &e)
{
	e.print();
	return -1;
}
	return 0;
}
