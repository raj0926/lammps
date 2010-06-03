/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "pair_soft_omp.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "update.h"
#include "neigh_list.h"
#include "memory.h"
#include "error.h"

#if defined(_OPENMP)
#include <omp.h>
#endif

using namespace LAMMPS_NS;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define EPSILON 1.0e-20
/* ---------------------------------------------------------------------- */

PairSoftOMP::PairSoftOMP(LAMMPS *lmp) : PairOMP(lmp)
{
  PI = 4.0*atan(1.0);
}

/* ---------------------------------------------------------------------- */

PairSoftOMP::~PairSoftOMP()
{
  if (allocated) {
    memory->destroy_2d_int_array(setflag);
    memory->destroy_2d_double_array(cutsq);

    memory->destroy_2d_double_array(prestart);
    memory->destroy_2d_double_array(prestop);
    memory->destroy_2d_double_array(prefactor);
    memory->destroy_2d_double_array(cut);
  }
}

/* ---------------------------------------------------------------------- */

void PairSoftOMP::compute(int eflag, int vflag)
{
  int i,j;

  if (eflag || vflag) {
    ev_setup(eflag,vflag);
    ev_setup_thr(eflag,vflag);
  } else evflag = vflag_fdotr = 0;

  // set current prefactor
  // for minimization, set to prestop
  // for dynamics, ramp from prestart to prestop
  // for 0-step dynamics, set to prestart

  double delta = update->ntimestep - update->beginstep;
  if (update->whichflag == 2) delta = 1.0;
  else if (update->nsteps) delta /= update->endstep - update->beginstep;
  else delta = 0.0;
  int ntypes = atom->ntypes;
  for (i = 1; i <= ntypes; i++)
    for (j = 1; j <= ntypes; j++)
      prefactor[i][j] = prestart[i][j] +
	delta * (prestop[i][j] - prestart[i][j]);

  if (evflag) {
    if (eflag) {
      if (force->newton_pair) return eval<1,1,1>();
      else return eval<1,1,0>();
    } else {
      if (force->newton_pair) return eval<1,0,1>();
      else return eval<1,0,0>();
    }
  } else {
    if (force->newton_pair) return eval<0,0,1>();
    else return eval<0,0,0>();
  }
}

template <int EVFLAG, int EFLAG, int NEWTON_PAIR>
void PairSoftOMP::eval()
{

#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {
    int i,j,ii,jj,inum,jnum,itype,jtype,tid;
    double xtmp,ytmp,ztmp,delx,dely,delz,evdwl,fpair;
    double r,rsq,arg,factor_lj;
    int *ilist,*jlist,*numneigh,**firstneigh;

    evdwl = 0.0;

    const int nlocal = atom->nlocal;
    const int nall = nlocal + atom->nghost;
    const int nthreads = comm->nthreads;

    double **x = atom->x;
    int *type = atom->type;
    double *special_lj = force->special_lj;

    inum = list->inum;
    ilist = list->ilist;
    numneigh = list->numneigh;
    firstneigh = list->firstneigh;

    // loop over neighbors of my atoms

    int iifrom, iito;
    double **f = loop_setup_thr(atom->f,iifrom,iito,tid,inum,nall,nthreads);
    for (ii = iifrom; ii < iito; ++ii) {
      i = ilist[ii];
      xtmp = x[i][0];
      ytmp = x[i][1];
      ztmp = x[i][2];
      itype = type[i];
      jlist = firstneigh[i];
      jnum = numneigh[i];

      for (jj = 0; jj < jnum; jj++) {
	j = jlist[jj];

	if (j < nall) factor_lj = 1.0;
	else {
	  factor_lj = special_lj[j/nall];
	  j %= nall;
	}

	delx = xtmp - x[j][0];
	dely = ytmp - x[j][1];
	delz = ztmp - x[j][2];
	rsq = delx*delx + dely*dely + delz*delz;
	jtype = type[j];

	if ((rsq < cutsq[itype][jtype]) && (rsq > EPSILON)) {
	  r = sqrt(rsq);
          arg = PI*r/cut[itype][jtype];
	  fpair = factor_lj * prefactor[itype][jtype] *
	    sin(arg) * PI/cut[itype][jtype]/r;

	  f[i][0] += delx*fpair;
	  f[i][1] += dely*fpair;
	  f[i][2] += delz*fpair;
	  if (NEWTON_PAIR || j < nlocal) {
	    f[j][0] -= delx*fpair;
	    f[j][1] -= dely*fpair;
	    f[j][2] -= delz*fpair;
	  }

	  if (EFLAG) {
	    evdwl = factor_lj * prefactor[itype][jtype] * (1.0+cos(arg));
	  }

	  if (EVFLAG) ev_tally_thr(i,j,nlocal,NEWTON_PAIR,
				   evdwl,0.0,fpair,delx,dely,delz,tid);
	}
      }
    }

    // reduce per thread forces into global force array.
    force_reduce_thr(atom->f,nall,nthreads,tid);
  }
  ev_reduce_thr();
  if (vflag_fdotr) virial_compute();
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairSoftOMP::allocate()
{
  allocated = 1;
  int n = atom->ntypes;

  setflag = memory->create_2d_int_array(n+1,n+1,"pair:setflag");
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;

  cutsq = memory->create_2d_double_array(n+1,n+1,"pair:cutsq");

  prestart = memory->create_2d_double_array(n+1,n+1,"pair:prestart");
  prestop = memory->create_2d_double_array(n+1,n+1,"pair:prestop");
  prefactor = memory->create_2d_double_array(n+1,n+1,"pair:prefactor");
  cut = memory->create_2d_double_array(n+1,n+1,"pair:cut");

  // init prestart and prestop to 0.0
  // since pair_hybrid can use all types even if pair_soft sub-class
  //   never sets them

  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++) {
      prestart[i][j] = 0.0;
      prestop[i][j] = 0.0;
    }
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairSoftOMP::settings(int narg, char **arg)
{
  if (narg != 1) error->all("Illegal pair_style command");

  cut_global = force->numeric(arg[0]);

  // reset cutoffs that have been explicitly set

  if (allocated) {
    int i,j;
    for (i = 1; i <= atom->ntypes; i++)
      for (j = i+1; j <= atom->ntypes; j++)
	if (setflag[i][j]) cut[i][j] = cut_global;
  }
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairSoftOMP::coeff(int narg, char **arg)
{
  if (narg < 4 || narg > 5) error->all("Incorrect args for pair coefficients");
  if (!allocated) allocate();

  int ilo,ihi,jlo,jhi;
  force->bounds(arg[0],atom->ntypes,ilo,ihi);
  force->bounds(arg[1],atom->ntypes,jlo,jhi);

  double prestart_one = force->numeric(arg[2]);
  double prestop_one = force->numeric(arg[3]);

  double cut_one = cut_global;
  if (narg == 5) cut_one = force->numeric(arg[4]);

  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      prestart[i][j] = prestart_one;
      prestop[i][j] = prestop_one;
      cut[i][j] = cut_one;
      setflag[i][j] = 1;
      count++;
    }
  }

  if (count == 0) error->all("Incorrect args for pair coefficients");
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairSoftOMP::init_one(int i, int j)
{
  // always mix prefactors geometrically

  if (setflag[i][j] == 0) {
    prestart[i][j] = sqrt(prestart[i][i]*prestart[j][j]);
    prestop[i][j] = sqrt(prestop[i][i]*prestop[j][j]);
    cut[i][j] = mix_distance(cut[i][i],cut[j][j]);
  }

  prestart[j][i] = prestart[i][j];
  prestop[j][i] = prestop[i][j];
  cut[j][i] = cut[i][j];

  return cut[i][j];
}

/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairSoftOMP::write_restart(FILE *fp)
{
  write_restart_settings(fp);

  int i,j;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      fwrite(&setflag[i][j],sizeof(int),1,fp);
      if (setflag[i][j]) {
	fwrite(&prestart[i][j],sizeof(double),1,fp);
	fwrite(&prestop[i][j],sizeof(double),1,fp);
	fwrite(&cut[i][j],sizeof(double),1,fp);
      }
    }
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairSoftOMP::read_restart(FILE *fp)
{
  read_restart_settings(fp);

  allocate();

  int i,j;
  int me = comm->me;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      if (me == 0) fread(&setflag[i][j],sizeof(int),1,fp);
      MPI_Bcast(&setflag[i][j],1,MPI_INT,0,world);
      if (setflag[i][j]) {
	if (me == 0) {
	  fread(&prestart[i][j],sizeof(double),1,fp);
	  fread(&prestop[i][j],sizeof(double),1,fp);
	  fread(&cut[i][j],sizeof(double),1,fp);
	}
	MPI_Bcast(&prestart[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&prestop[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&cut[i][j],1,MPI_DOUBLE,0,world);
      }
    }
}

/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairSoftOMP::write_restart_settings(FILE *fp)
{
  fwrite(&cut_global,sizeof(double),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairSoftOMP::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_global,sizeof(double),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world);
}

/* ---------------------------------------------------------------------- */

double PairSoftOMP::single(int i, int j, int itype, int jtype, double rsq,
			double factor_coul, double factor_lj,
			double &fforce)
{
  double r,arg,philj;

  if (rsq < EPSILON) {
    fforce = 0.0;
    return 0.0;
  }
  r = sqrt(rsq);
  arg = PI*r/cut[itype][jtype];
  fforce = factor_lj * prefactor[itype][jtype] *
    sin(arg) * PI/cut[itype][jtype]/r;

  philj = prefactor[itype][jtype] * (1.0+cos(arg));
  return factor_lj*philj;
}

/* ---------------------------------------------------------------------- */

double PairSoftOMP::memory_usage()
{
  const int n=atom->ntypes;

  double bytes = PairOMP::memory_usage();

  bytes += 9*((n+1)*(n+1) * sizeof(double) + (n+1)*sizeof(double *));
  bytes += 1*((n+1)*(n+1) * sizeof(int) + (n+1)*sizeof(int *));

  return bytes;
}

