/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef FIX_CLASS

FixStyle(neb,FixNEB)

#else

#ifndef LMP_FIX_NEB_H
#define LMP_FIX_NEB_H

#include "fix.h"

namespace LAMMPS_NS {

class FixNEB : public Fix {
 public:
  double veng,plen,nlen;
  int rclimber;
  double gradvnorm;

  FixNEB(class LAMMPS *, int, char **);
  ~FixNEB();
  int setmask();
  void init();
  void min_setup(int);
  void min_post_force(int);

 private:
  int me,nprocs,nprocs_universe;
  double kspring;
  int ireplica,nreplica;
  int procnext,procprev;
  int cmode;
  MPI_Comm uworld;

  char *id_pe;
  class Compute *pe;

  int nebatoms;                // # of active NEB atoms
  int ntotal;                  // total # of atoms, NEB or not
  int maxlocal;                // size of xprev,xnext,tangent arrays

  double **xprev,**xnext;      // coords of my owned atoms in neighbor replicas
  double **tangent;            // work vector for inter-replica forces

  double **xsend,**xrecv;      // coords to send/recv to/from other replica
  tagint *tagsend,*tagrecv;    // ditto for atom IDs

                                 // info gathered from all procs in my replica
  double **xsendall,**xrecvall;    // coords to send/recv to/from other replica
  tagint *tagsendall,*tagrecvall;  // ditto for atom IDs

  int *counts,*displacements;   // used for MPI_Gather

  void inter_replica_comm();
  void reallocate();
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running LAMMPS to see the offending line.

E: Potential energy ID for fix neb does not exist

Self-explanatory.

E: Atom count changed in fix neb

This is not allowed in a NEB calculation.

*/
