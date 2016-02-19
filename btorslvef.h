/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015 Mathias Preiner.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#ifndef BTORSLVEF_H_INCLUDED
#define BTORSLVEF_H_INCLUDED

#include "btorslv.h"
#include "btortypes.h"
#include "utils/btornodemap.h"

#include <stdint.h>

struct BtorEFSolver
{
  BTOR_SOLVER_STRUCT;

  Btor *e_solver;
  BtorNodeMap *e_exists_vars;
  BtorNodeMap *e_exists_ufs;
  Btor *f_solver;
  BtorNodeMap *f_exists_vars;
  BtorNodeMap *f_forall_vars;
  BtorNodeMap *f_forall_ufs;
  BtorNode *f_formula;

  struct
  {
    uint32_t refinements;
    uint32_t synth_aborts;
  } stats;

  struct
  {
    double e_solver;
    double f_solver;
    double synth;
    double qinst;
  } time;
};

typedef struct BtorEFSolver BtorEFSolver;

BtorSolver *btor_new_ef_solver (Btor *btor);

#endif