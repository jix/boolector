/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2013-2014 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */
#ifndef BTORCLONE_H_INCLUDED
#define BTORCLONE_H_INCLUDED

#include "btormap.h"

/* Clone an existing boolector instance. */
Btor *btor_clone_btor (Btor *btor);

/* Clone the expression layer of an existing boolector instance. */
Btor *btor_clone_exp_layer (Btor *btor,
                            BtorNodeMap **exp_map,
                            BtorNodeMap **aig_map);

/* Clone 'exp' (and all expressions below) of an existing boolector instance
 * 'btor' into an existing boolector instance 'clone'. 'exp_map' must contain
 * all previously cloned expressions. Similarly, 'aig_map' must contain all
 * previously cloned aigs, if given, else exp-layer only is cloned. */
BtorNode *btor_clone_exp_tree (Btor *btor,
                               Btor *clone,
                               BtorNode *exp,
                               BtorNodeMap *exp_map,
                               BtorAIGMap *aig_map);
#endif
