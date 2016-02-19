/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015 Mathias Preiner.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorslvef.h"
#include "btorbitvec.h"
#include "btorclone.h"
#include "btorcore.h"
#include "btorexp.h"
#include "btormodel.h"
#include "btorslvfun.h"
#include "btorsynthfun.h"
#include "normalizer/btorskolemize.h"
#include "simplifier/btorder.h"
#include "simplifier/btorminiscope.h"
#include "utils/btorhashint.h"
#include "utils/btoriter.h"

// TODO (ma): debug
#include "dumper/btordumpbtor.h"

//#define PRINT_DBG

/*------------------------------------------------------------------------*/

static void
setup_exists_solver (BtorEFSolver *slv)
{
  Btor *e_solver, *f_solver;
  BtorNode *cur, *var;
  BtorNodeMapIterator it;
  BtorHashTableIterator hit;
  BtorNodeMap *exists_vars, *exists_ufs;

  f_solver = slv->f_solver;
  //  e_solver = btor_new_btor ();
  e_solver = btor_clone_formula (f_solver);
  //  btor_copy_opts (e_solver->mm, &f_solver->options,
  //		  &e_solver->options);
  exists_vars = btor_new_node_map (e_solver);
  exists_ufs  = btor_new_node_map (e_solver);
  btor_set_msg_prefix_btor (e_solver, "exists");
  btor_set_opt (e_solver, BTOR_OPT_AUTO_CLEANUP_INTERNAL, 1);

  btor_init_node_map_iterator (&it, slv->f_exists_vars);
  while (btor_has_next_node_map_iterator (&it))
  {
    cur = btor_next_node_map_iterator (&it);
    var = btor_match_node_by_id (e_solver, cur->id);
    //      var = btor_var_exp (e_solver,
    //			  btor_get_exp_width (f_solver, cur),
    //			  btor_get_symbol_exp (f_solver, cur));
    btor_map_node (exists_vars, var, cur);
  }

  btor_init_node_hash_table_iterator (&hit, slv->f_solver->ufs);
  while (btor_has_next_hash_table_iterator (&hit))
  {
    cur = btor_next_hash_table_iterator (&hit);
    var = btor_match_node_by_id (e_solver, cur->id);
    btor_map_node (exists_ufs, var, cur);
  }

  btor_init_node_map_iterator (&it, slv->f_forall_vars);
  while (btor_has_next_node_map_iterator (&it))
  {
    cur = btor_next_node_map_iterator (&it);
    var = btor_match_node_by_id (e_solver, cur->id);
    btor_release_exp (e_solver, var);
  }

  btor_release_exp (e_solver,
                    btor_match_node_by_id (
                        e_solver, BTOR_REAL_ADDR_NODE (slv->f_formula)->id));

  e_solver->slv      = btor_new_fun_solver (e_solver);
  slv->e_solver      = e_solver;
  slv->e_exists_vars = exists_vars;
  slv->e_exists_ufs  = exists_ufs;
}

static BtorNode *
invert_formula (Btor *btor)
{
  assert (btor->synthesized_constraints->count == 0);
  assert (btor->varsubst_constraints->count == 0);
  assert (btor->embedded_constraints->count == 0);

  BtorNode *cur, *root, *and;
  BtorPtrHashTable *t;
  BtorHashTableIterator it;

  root = 0;
  /* make conjunction of constraints and add negated formula */
  btor_init_node_hash_table_iterator (&it, btor->unsynthesized_constraints);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    t   = (BtorPtrHashTable *) it.stack[it.pos];
    cur = btor_next_node_hash_table_iterator (&it);
    assert (btor_get_ptr_hash_table (t, cur));
    btor_remove_ptr_hash_table (t, cur, 0, 0);
    BTOR_REAL_ADDR_NODE (cur)->constraint = 0;
    if (root)
    {
      and = btor_and_exp (btor, root, cur);
      btor_release_exp (btor, root);
      root = and;
    }
    else
      root = btor_copy_exp (btor, cur);
    btor_release_exp (btor, cur);
  }
  assert (btor->unsynthesized_constraints->count == 0);
  if (root) return BTOR_INVERT_NODE (root);
  return 0;
}

static void
setup_forall_solver (BtorEFSolver *slv)
{
  Btor *f_solver;
  BtorNode *cur, *param, *subst, *var, *root;
  BtorHashTableIterator it;
  BtorNodeMap *map, *exists_vars, *forall_vars, *m;
  BtorFunSolver *fslv;

  f_solver = btor_clone_formula (slv->btor);
  btor_set_msg_prefix_btor (f_solver, "forall");
  exists_vars = btor_new_node_map (f_solver);
  forall_vars = btor_new_node_map (f_solver);

  /* configure options */
  btor_set_opt (f_solver, BTOR_OPT_MODEL_GEN, 1);
  btor_set_opt (f_solver, BTOR_OPT_INCREMENTAL, 1);
  /* disable variable substitution (not sound in the context of quantifiers) */
  // TODO (ma): check if it can still be used (may be sound since we are in
  // QF_BV)
  btor_set_opt (f_solver, BTOR_OPT_VAR_SUBST, 1);

  btor_init_node_hash_table_iterator (&it, f_solver->bv_vars);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    btor_map_node (exists_vars,
                   btor_copy_exp (f_solver, cur),
                   btor_copy_exp (f_solver, cur));
    //      printf ("exists var: %s (%d)\n", node2string (cur), cur->refs);
  }

  /* instantiate exists/forall params with fresh variables */
  btor_init_substitutions (f_solver);
  btor_init_node_hash_table_iterator (&it, f_solver->quantifiers);
  map = btor_new_node_map (f_solver);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    assert (BTOR_IS_QUANTIFIER_NODE (cur));
    param = cur->e[0];
    var   = btor_var_exp (f_solver, btor_get_exp_width (f_solver, param), 0);
    btor_map_node (map, param, var);
    if (btor_param_is_exists_var (param))
    {
      m = exists_vars;
      //	  printf ("exists var: %s (%s)\n", node2string (param),
      // node2string (var));
    }
    else
    {
      assert (btor_param_is_forall_var (param));
      m = forall_vars;
      //	  printf ("forall var: %s\n", node2string (param));
    }
    btor_map_node (m, var, btor_copy_exp (f_solver, param));
  }

  /* eliminate quantified terms by substituting bound variables with fresh
   * variables */
  btor_init_node_hash_table_iterator (&it, f_solver->quantifiers);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    assert (BTOR_IS_QUANTIFIER_NODE (cur));
    subst = btor_substitute_terms (f_solver, btor_binder_get_body (cur), map);
    btor_map_node (map, cur, subst);
    assert (!btor_get_ptr_hash_table (f_solver->substitutions, cur));
    btor_insert_substitution (f_solver, cur, subst, 0);
    btor_release_exp (f_solver, subst);
  }
  btor_delete_node_map (map);

  btor_substitute_and_rebuild (f_solver, f_solver->substitutions);
  btor_delete_substitutions (f_solver);
  (void) btor_simplify (f_solver);

  assert (f_solver->exists_vars->count == 0);
  assert (f_solver->forall_vars->count == 0);
  assert (f_solver->quantifiers->count == 0);

  root = invert_formula (f_solver);
  assert (root);
  slv->f_formula = root;

  assert (!f_solver->slv);
  fslv                = (BtorFunSolver *) btor_new_fun_solver (f_solver);
  fslv->assume_lemmas = true;
  f_solver->slv       = (BtorSolver *) fslv;
  slv->f_solver       = f_solver;
  slv->f_exists_vars  = exists_vars;
  slv->f_forall_vars  = forall_vars;
}

static void
refine_exists_solver (BtorEFSolver *slv, BtorNodeMap *synth_funs)
{
  uint32_t i;
  Btor *f_solver, *e_solver;
  BtorNodeMap *map;
  BtorNodeMapIterator it;
  BtorHashTableIterator h_it;
  BtorNode *var, *var_fs, *uf_fs, *uf_es, *c, *res, *app, *eq;
  BtorNodePtrStack consts, args;
  const BtorBitVector *bv;
  const BtorPtrHashTable *uf_model;
  BtorBitVectorTuple *bv_tup;
  BtorMemMgr *mm;

  mm       = slv->btor->mm;
  f_solver = slv->f_solver;
  e_solver = slv->e_solver;

  map = btor_new_node_map (f_solver);

  /* map f_exists_vars to e_exists_vars */
  btor_init_node_map_iterator (&it, slv->e_exists_vars);
  btor_queue_node_map_iterator (&it, slv->e_exists_ufs);
  while (btor_has_next_node_map_iterator (&it))
  {
    var_fs = it.it.bucket->data.as_ptr;
    var    = btor_next_node_map_iterator (&it);
    btor_map_node (map, var_fs, var);
  }

  BTOR_INIT_STACK (consts);
  /* map f_forall_vars to resp. assignment */
#ifdef PRINT_DBG
  printf ("found counter example\n");
#endif
  btor_init_node_map_iterator (&it, slv->f_forall_vars);
  while (btor_has_next_node_map_iterator (&it))
  {
    var = btor_next_node_map_iterator (&it);
    bv  = btor_get_bv_model (f_solver, btor_simplify_exp (f_solver, var));
#ifdef PRINT_DBG
    printf ("%s := ", node2string (btor_mapped_node (slv->f_forall_vars, var)));
    btor_print_bv (bv);
#endif
    c = btor_const_exp (e_solver, (BtorBitVector *) bv);
    btor_map_node (map, var, c);
    BTOR_PUSH_STACK (mm, consts, c);
  }

  // TODO (ma): f_solver->ufs are not the correct functions!
  //            need to query synth_funs that are mapped to the resp. uf
  if (synth_funs)
  {
    btor_init_node_map_iterator (&it, synth_funs);
    while (btor_has_next_node_map_iterator (&it))
    {
      uf_fs = btor_next_node_map_iterator (&it);
      uf_model =
          btor_get_fun_model (f_solver, btor_mapped_node (synth_funs, uf_fs));

      if (!uf_model) continue;

      uf_es = btor_mapped_node (map, uf_fs);

      btor_init_hash_table_iterator (&h_it, uf_model);
      while (btor_has_next_hash_table_iterator (&h_it))
      {
        bv     = h_it.bucket->data.as_ptr;
        bv_tup = btor_next_hash_table_iterator (&h_it);

        BTOR_INIT_STACK (args);
        for (i = 0; i < bv_tup->arity; i++)
        {
          c = btor_const_exp (e_solver, bv_tup->bv[i]);
          BTOR_PUSH_STACK (mm, args, c);
        }
        c = btor_const_exp (e_solver, (BtorBitVector *) bv);

        app = btor_apply_exps (
            e_solver, BTOR_COUNT_STACK (args), args.start, uf_es);
        eq = btor_ne_exp (e_solver, app, c);
        btor_assert_exp (e_solver, eq);
        btor_release_exp (e_solver, app);
        btor_release_exp (e_solver, eq);
        btor_release_exp (e_solver, c);

        while (!BTOR_EMPTY_STACK (args))
          btor_release_exp (e_solver, BTOR_POP_STACK (args));
        BTOR_RELEASE_STACK (mm, args);
      }

#ifdef PRINT_DBG
      printf ("%s\n", node2string (var));
      BtorHashTableIterator mit;
      BtorBitVectorTuple *tup;
      btor_init_hash_table_iterator (&mit, uf_model);
      while (btor_has_next_hash_table_iterator (&mit))
      {
        bv  = mit.bucket->data.as_ptr;
        tup = btor_next_hash_table_iterator (&mit);

        for (i = 0; i < tup->arity; i++)
        {
          printf ("a ");
          btor_print_bv (tup->bv[i]);
        }
        printf ("r ");
        btor_print_bv (bv);
      }
#endif
    }
  }
  assert (f_solver->unsynthesized_constraints->count == 0);
  assert (f_solver->synthesized_constraints->count == 0);
  assert (f_solver->embedded_constraints->count == 0);
  assert (f_solver->varsubst_constraints->count == 0);
  // TODO (ma): search for symbolic candidates
  /* quantifier instantiation with counter example of universal variables */
  res = btor_recursively_rebuild_exp_clone (
      f_solver,
      e_solver,
      BTOR_INVERT_NODE (slv->f_formula),
      map,
      btor_get_opt (e_solver, BTOR_OPT_REWRITE_LEVEL));

  while (!BTOR_EMPTY_STACK (consts))
    btor_release_exp (e_solver, BTOR_POP_STACK (consts));
  BTOR_RELEASE_STACK (mm, consts);

  btor_delete_node_map (map);
  btor_assert_exp (e_solver, res);
  btor_release_exp (e_solver, res);
}

#if 0
static BtorNode *
construct_generalization (BtorEFSolver * slv,
			  BtorNodeMap * synth_funs)
{
  Btor *f_solver, *e_solver;
  BtorNodeMap *map;
  BtorNodeMapIterator it;
  BtorNode *var, *var_fs, *c, *res;
  BtorNodePtrStack consts;
  const BtorBitVector *bv;
  const BtorPtrHashTable *uf_model;
  BtorMemMgr *mm;

  mm = slv->btor->mm;
  f_solver = slv->f_solver;
  e_solver = slv->e_solver;

  map = btor_new_node_map (f_solver);

  BTOR_INIT_STACK (consts);
  /* map f_forall_vars to resp. assignment */
#ifdef PRINT_DBG
  printf ("found counter example\n");
#endif
  btor_init_node_map_iterator (&it, slv->f_forall_vars);
  while (btor_has_next_node_map_iterator (&it))
    {
      var = btor_next_node_map_iterator (&it);
      bv = btor_get_bv_model (f_solver,
			      btor_simplify_exp (f_solver, var));
#ifdef PRINT_DBG
      printf ("%s := ", node2string (btor_mapped_node (slv->f_forall_vars, var)));
      btor_print_bv (bv);
#endif
      c = btor_const_exp (e_solver, (BtorBitVector *) bv);
      btor_map_node (map, var, c);
      BTOR_PUSH_STACK (mm, consts, c); 
    }

  // TODO (ma): f_solver->ufs are not the correct functions!
  //            need to query synth_funs that are mapped to the resp. uf
  if (synth_funs)
    {
      btor_init_node_map_iterator (&it, synth_funs);
      while (btor_has_next_node_map_iterator (&it))
	{
	  var = btor_next_node_map_iterator (&it);
	  uf_model = btor_get_fun_model (f_solver,
			 btor_mapped_node (synth_funs, var));

	  if (!uf_model) continue;

#ifdef PRINT_DBG
	  printf ("%s\n", node2string (var));
	  BtorHashTableIterator mit;
	  BtorBitVectorTuple *tup;
	  int i;
	  btor_init_hash_table_iterator (&mit, uf_model);
	  while (btor_has_next_hash_table_iterator (&mit))
	    {
	      bv = mit.bucket->data.as_ptr;
	      tup = btor_next_hash_table_iterator (&mit);

	      for (i = 0; i < tup->arity; i++)
		{
		  printf ("a ");
		  btor_print_bv (tup->bv[i]);
		}
	      printf ("r ");
	      btor_print_bv (bv);
	    }
#endif
	}
    }

  /* map f_exists_vars to e_exists_vars */
  btor_init_node_map_iterator (&it, slv->e_exists_vars);
  btor_queue_node_map_iterator (&it, slv->e_exists_ufs);
  while (btor_has_next_node_map_iterator (&it))
    {
      var_fs = it.it.bucket->data.as_ptr;
      var = btor_next_node_map_iterator (&it);
      btor_map_node (map, var_fs, var);
    }

  assert (f_solver->unsynthesized_constraints->count == 0);
  assert (f_solver->synthesized_constraints->count == 0);
  assert (f_solver->embedded_constraints->count == 0);
  assert (f_solver->varsubst_constraints->count == 0);
  // TODO (ma): search for symbolic candidates
  /* quantifier instantiation with counter example of universal variables */
  res = btor_recursively_rebuild_exp_clone (
	    f_solver, e_solver,
	    slv->f_formula, map,
	    e_solver->options.rewrite_level.val);

  while (!BTOR_EMPTY_STACK (consts))
    btor_release_exp (e_solver, BTOR_POP_STACK (consts));
  BTOR_RELEASE_STACK (mm, consts);

  btor_delete_node_map (map);
  return res;
}
#endif

static bool
is_ef_formula (BtorEFSolver *slv)
{
  assert (slv->btor->synthesized_constraints->count == 0);
  assert (slv->btor->embedded_constraints->count == 0);
  assert (slv->btor->varsubst_constraints->count == 0);

  Btor *btor;
  uint32_t i;
  bool exists_allowed, result = true;
  BtorNode *cur, *real_cur;
  BtorHashTableIterator it;
  BtorIntHashTable *cache;
  BtorMemMgr *mm;
  BtorNodePtrStack visit;
  BtorNodeIterator nit;

  btor  = slv->btor;
  mm    = btor->mm;
  cache = btor_new_int_hash_table (mm);
  BTOR_INIT_STACK (visit);
  btor_init_node_hash_table_iterator (&it, btor->unsynthesized_constraints);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur            = btor_next_node_hash_table_iterator (&it);
    real_cur       = BTOR_REAL_ADDR_NODE (cur);
    exists_allowed = true;
    if (BTOR_IS_QUANTIFIER_NODE (real_cur))
    {
      btor_init_binder_iterator (&nit, real_cur);
      while (btor_has_next_binder_iterator (&nit))
      {
        real_cur = btor_next_binder_iterator (&nit);

        if (BTOR_IS_FORALL_NODE (real_cur)) exists_allowed = false;

        if (!exists_allowed && BTOR_IS_EXISTS_NODE (real_cur))
        {
          result = false;
          goto CLEANUP_AND_EXIT;
        }
      }
      cur            = btor_binder_get_body (real_cur);
      exists_allowed = false;
    }
    BTOR_PUSH_STACK (mm, visit, cur);
    while (!BTOR_EMPTY_STACK (visit))
    {
      cur      = BTOR_POP_STACK (visit);
      real_cur = BTOR_REAL_ADDR_NODE (cur);
      assert (!BTOR_IS_QUANTIFIER_NODE (real_cur)
              || !BTOR_IS_INVERTED_NODE (cur));

      if (btor_contains_int_hash_table (cache, real_cur->id)) continue;

      btor_add_int_hash_table (cache, real_cur->id);

      if ((!exists_allowed && BTOR_IS_EXISTS_NODE (real_cur))
          || BTOR_IS_FORALL_NODE (real_cur))
      {
        result = false;
        goto CLEANUP_AND_EXIT;
      }

      for (i = 0; i < real_cur->arity; i++)
        BTOR_PUSH_STACK (mm, visit, real_cur->e[i]);
    }
  }
CLEANUP_AND_EXIT:
  BTOR_RELEASE_STACK (mm, visit);
  btor_delete_int_hash_table (cache);
  return result;
}

static void
get_failed_vars (BtorEFSolver *slv, BtorPtrHashTable *failed_vars)
{
  Btor *clone, *e_solver;
  BtorNodeMapIterator it;
  BtorHashTableIterator hit;
  BtorNode *var, *c, *a, *var_clone, *root;
  BtorPtrHashTable *assumptions;
  const BtorBitVector *bv;
#ifndef NDEBUG
  BtorSolverResult result;
#endif

  e_solver = slv->e_solver;
  clone    = btor_clone_formula (e_solver);
  btor_set_opt (clone, BTOR_OPT_AUTO_CLEANUP, 1);
  btor_set_opt (clone, BTOR_OPT_AUTO_CLEANUP_INTERNAL, 1);
  root = invert_formula (clone);
  assert (clone->varsubst_constraints->count == 0);
  assert (clone->embedded_constraints->count == 0);
  assert (clone->synthesized_constraints->count == 0);

  if (!root)
  {
    btor_delete_btor (clone);
    return;
  }

  assumptions = btor_new_ptr_hash_table (slv->btor->mm, 0, 0);
  btor_set_msg_prefix_btor (clone, "dp");
  clone->slv = btor_new_fun_solver (clone);
  btor_assert_exp (clone, root);

  btor_init_node_map_iterator (&it, slv->e_exists_vars);
  while (btor_has_next_node_map_iterator (&it))
  {
    var       = btor_next_node_map_iterator (&it);
    var_clone = btor_get_node_by_id (clone, var->id);
    assert (BTOR_IS_REGULAR_NODE (var_clone));
    assert (BTOR_IS_BV_VAR_NODE (var_clone));
    bv = btor_get_bv_model (e_solver, btor_simplify_exp (e_solver, var));
    assert (!BTOR_IS_PROXY_NODE (BTOR_REAL_ADDR_NODE (var_clone)));
    c = btor_const_exp (clone, (BtorBitVector *) bv);
    a = btor_eq_exp (clone, var_clone, c);
    btor_add_ptr_hash_table (assumptions, a)->data.as_ptr = var;
    btor_assume_exp (clone, a);
  }

#ifndef NDEBUG
  result =
#endif
      clone->slv->api.sat (clone->slv);
  assert (result == BTOR_RESULT_UNSAT);

  btor_init_hash_table_iterator (&hit, assumptions);
  while (btor_has_next_hash_table_iterator (&hit))
  {
    var = hit.bucket->data.as_ptr;
    a   = btor_next_hash_table_iterator (&hit);

    if (btor_failed_exp (clone, a)) btor_add_ptr_hash_table (failed_vars, var);
  }
  btor_delete_ptr_hash_table (assumptions);
  btor_delete_btor (clone);
}

/*------------------------------------------------------------------------*/

static BtorEFSolver *
clone_ef_solver (Btor *clone, Btor *btor, BtorNodeMap *exp_map)
{
  (void) clone;
  (void) btor;
  (void) exp_map;
  return 0;
}

static void
delete_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  Btor *btor;
  BtorNodeMapIterator it;

  btor = slv->btor;

  if (slv->e_exists_vars)
  {
    btor_init_node_map_iterator (&it, slv->e_exists_vars);
    while (btor_has_next_node_map_iterator (&it))
    {
      btor_release_exp (slv->e_solver, btor_next_node_map_iterator (&it));
    }
    btor_delete_node_map (slv->e_exists_vars);
  }

  if (slv->e_exists_ufs)
  {
    btor_init_node_map_iterator (&it, slv->e_exists_ufs);
    while (btor_has_next_node_map_iterator (&it))
    {
      btor_release_exp (slv->e_solver, btor_next_node_map_iterator (&it));
    }
    btor_delete_node_map (slv->e_exists_ufs);
  }

  if (slv->f_exists_vars)
  {
    assert (slv->f_forall_vars);
    btor_init_node_map_iterator (&it, slv->f_exists_vars);
    btor_queue_node_map_iterator (&it, slv->f_forall_vars);
    while (btor_has_next_node_map_iterator (&it))
    {
      btor_release_exp (slv->f_solver, it.it.bucket->data.as_ptr);
      btor_release_exp (slv->f_solver, btor_next_node_map_iterator (&it));
    }
    btor_delete_node_map (slv->f_exists_vars);
    btor_delete_node_map (slv->f_forall_vars);
  }

  btor_release_exp (slv->f_solver, slv->f_formula);
  btor_delete_btor (slv->e_solver);
  btor_delete_btor (slv->f_solver);
  BTOR_DELETE (btor->mm, slv);
  btor->slv = 0;
}

static BtorSolverResult
sat_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  double start;
  Btor *e_solver, *f_solver;
  BtorSolverResult res;
  BtorNodeMapIterator it;
  BtorNode *var, *c, *var_fs, *g, *synth_fun, *prev_synth_fun;
  BtorNodeMap *map, *e_exists_vars, *synth_funs, *prev_synth_funs;
  BtorPtrHashTable *failed_vars, *e_model_vars;
  BtorHashTableIterator hit;
  const BtorBitVector *bv;
  const BtorPtrHashTable *uf_model;

  (void) btor_simplify (slv->btor);
  //  btor_miniscope (slv->btor);
  //  btor_dump_smt2 (slv->btor, stdout);
  // btor_dump_btor (slv->btor, stdout, 1);
  btor_skolemize (slv->btor);
  // TODO (ma): optional
  btor_der (slv->btor);

  if (!is_ef_formula (slv))
  {
    // TODO (ma): proper abort
    printf ("not an exists/forall formula\n");
    abort ();
  }

  // TODO (ma): incremental support
  setup_forall_solver (slv);
  setup_exists_solver (slv);

  e_solver      = slv->e_solver;
  f_solver      = slv->f_solver;
  e_exists_vars = slv->e_exists_vars;
  synth_funs    = 0;

  g = btor_copy_exp (f_solver, slv->f_formula);
  goto CHECK_FORALL;

  while (true)
  {
    start = btor_time_stamp ();
    res   = e_solver->slv->api.sat (e_solver->slv);
    slv->time.e_solver += btor_time_stamp () - start;

    if (res == BTOR_RESULT_UNSAT) /* formula is UNSAT */
      break;

    assert (res == BTOR_RESULT_SAT);
    e_solver->slv->api.generate_model (e_solver->slv, false, false);

    failed_vars = 0;
    if (btor_get_opt (slv->btor, BTOR_OPT_EF_DUAL_PROP))
    {
      failed_vars = btor_new_ptr_hash_table (slv->btor->mm, 0, 0);
      get_failed_vars (slv, failed_vars);
      e_model_vars = failed_vars;
    }
    else
      e_model_vars = e_exists_vars->table;

#ifdef PRINT_DBG
    printf ("\nfound candidate model\n");
#endif
    /* substitute exists variables with model and assume new formula to
     * forall solver */
    map = btor_new_node_map (f_solver);
    btor_init_hash_table_iterator (&hit, e_model_vars);
    while (btor_has_next_hash_table_iterator (&hit))
    {
      var    = btor_next_hash_table_iterator (&hit);
      var_fs = btor_mapped_node (e_exists_vars, var);
      bv     = btor_get_bv_model (e_solver, btor_simplify_exp (e_solver, var));
#ifdef PRINT_DBG
      printf ("exists %s := ", node2string (var));
      btor_print_bv (bv);
#endif
      assert (!BTOR_IS_PROXY_NODE (BTOR_REAL_ADDR_NODE (var_fs)));
      c = btor_const_exp (f_solver, (BtorBitVector *) bv);
      btor_map_node (map, var_fs, c);
    }
    if (failed_vars) btor_delete_ptr_hash_table (failed_vars);

    prev_synth_funs = synth_funs;
    synth_funs      = btor_new_node_map (f_solver);
    btor_init_node_map_iterator (&it, slv->e_exists_ufs);
    while (btor_has_next_node_map_iterator (&it))
    {
      var      = btor_next_node_map_iterator (&it);
      var_fs   = btor_mapped_node (slv->e_exists_ufs, var);
      uf_model = btor_get_fun_model (e_solver, var);

      if (!uf_model) continue;
#ifdef PRINT_DBG
      printf ("exists %s\n", node2string (var));
      BtorHashTableIterator mit;
      BtorBitVectorTuple *tup;
      int i;
      btor_init_hash_table_iterator (&mit, uf_model);
      while (btor_has_next_hash_table_iterator (&mit))
      {
        bv  = mit.bucket->data.as_ptr;
        tup = btor_next_hash_table_iterator (&mit);

        for (i = 0; i < tup->arity; i++)
        {
          printf ("a ");
          btor_print_bv (tup->bv[i]);
        }
        printf ("r ");
        btor_print_bv (bv);
      }
#endif
//	  printf ("var: %s (%d, %d)\n", node2string (var_fs),
//		  uf_model->count,
//		  btor_get_fun_arity (f_solver, var_fs));
#if 0
	  synth_fun = btor_generate_lambda_model_from_fun_model (
		       f_solver, var_fs, uf_model);
#else
      start = btor_time_stamp ();
      if (prev_synth_funs)
        prev_synth_fun = btor_mapped_node (prev_synth_funs, var_fs);
      else
        prev_synth_fun = 0;
      synth_fun =
          btor_synthesize_fun (f_solver, var_fs, uf_model, prev_synth_fun);
      if (prev_synth_fun) btor_release_exp (f_solver, prev_synth_fun);
      if (synth_fun)
        btor_map_node (synth_funs, var_fs, btor_copy_exp (f_solver, synth_fun));
      else
      {
        slv->stats.synth_aborts++;
        synth_fun = btor_generate_lambda_model_from_fun_model (
            f_solver, var_fs, uf_model);
      }
      slv->time.synth += btor_time_stamp () - start;
#endif
      btor_map_node (map, var_fs, synth_fun);
    }
    if (prev_synth_funs) btor_delete_node_map (prev_synth_funs);

    g = btor_substitute_terms (f_solver, slv->f_formula, map);
    btor_init_node_map_iterator (&it, map);
    while (btor_has_next_node_map_iterator (&it))
    {
      btor_release_exp (f_solver, it.it.bucket->data.as_ptr);
      (void) btor_next_node_map_iterator (&it);
    }
    btor_delete_node_map (map);

  CHECK_FORALL:
    btor_assume_exp (f_solver, g);
    btor_release_exp (f_solver, g);

    //      printf ("check candidate model\n");
    start = btor_time_stamp ();
    res   = f_solver->slv->api.sat (f_solver->slv);
    slv->time.f_solver += btor_time_stamp () - start;
    if (res == BTOR_RESULT_UNSAT) /* formula is SAT */
    {
      res = BTOR_RESULT_SAT;
      break;
    }

    f_solver->slv->api.generate_model (f_solver->slv, false, false);
    start = btor_time_stamp ();
#if 1
    refine_exists_solver (slv, synth_funs);
#else
    g = construct_generalization (slv, synth_funs);
    assert (BTOR_INVERT_NODE (g) != e_solver->true_exp);
    //      assert (!BTOR_IS_BV_CONST_NODE (BTOR_REAL_ADDR_NODE (g)));
    //      btor_dump_btor_node (e_solver, stdout, g);
    btor_assert_exp (e_solver, BTOR_INVERT_NODE (g));
    btor_release_exp (e_solver, g);
#endif
    slv->time.qinst += btor_time_stamp () - start;
    slv->stats.refinements++;
  }

  if (synth_funs)
  {
    btor_init_node_map_iterator (&it, synth_funs);
    while (btor_has_next_node_map_iterator (&it))
    {
      synth_fun = btor_next_data_node_map_iterator (&it)->as_ptr;
      btor_release_exp (f_solver, synth_fun);
    }
    btor_delete_node_map (synth_funs);
  }
  slv->btor->last_sat_result = res;
  return res;
}

static void
generate_model_ef_solver (BtorEFSolver *slv,
                          bool model_for_all_nodes,
                          bool reset)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  // TODO (ma): for now not supported
  (void) model_for_all_nodes;
  (void) reset;

  BtorNode *cur, *param, *var_fs;
  BtorNodeMapIterator it;
  const BtorBitVector *bv;

  btor_init_bv_model (slv->btor, &slv->btor->bv_model);
  btor_init_fun_model (slv->btor, &slv->btor->fun_model);
  btor_init_node_map_iterator (&it, slv->e_exists_vars);
  while (btor_has_next_node_map_iterator (&it))
  {
    cur    = btor_next_node_map_iterator (&it);
    var_fs = btor_mapped_node (slv->e_exists_vars, cur);
    param  = btor_mapped_node (slv->f_exists_vars, var_fs);

    bv = btor_get_bv_model (slv->e_solver,
                            btor_simplify_exp (slv->e_solver, cur));
    assert (btor_get_node_by_id (slv->btor, param->id));
    btor_add_to_bv_model (slv->btor,
                          slv->btor->bv_model,
                          btor_get_node_by_id (slv->btor, param->id),
                          (BtorBitVector *) bv);
  }

  // TODO (ma): UF models
}

static void
print_stats_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  BTOR_MSG (slv->btor->msg, 1, "");
  BTOR_MSG (slv->btor->msg,
            1,
            "exists solver refinements: %u",
            slv->stats.refinements);
  BTOR_MSG (slv->btor->msg,
            1,
            "synthesize function aborts: %u",
            slv->stats.synth_aborts);
  //  printf ("****************\n");
  //  btor_print_stats_btor (slv->e_solver);
  //  btor_print_stats_btor (slv->f_solver);
}

static void
print_time_stats_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  BTOR_MSG (
      slv->btor->msg, 1, "%.2f seconds exists solver", slv->time.e_solver);
  BTOR_MSG (
      slv->btor->msg, 1, "%.2f seconds forall solver", slv->time.f_solver);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds synthesizing functions",
            slv->time.synth);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds quantifier instantiation",
            slv->time.qinst);
}

BtorSolver *
btor_new_ef_solver (Btor *btor)
{
  assert (btor);
  // TODO (ma): incremental calls not supported yet
  assert (!btor_get_opt (btor, BTOR_OPT_INCREMENTAL));

  BtorEFSolver *slv;

  BTOR_CNEW (btor->mm, slv);

  slv->kind               = BTOR_EF_SOLVER_KIND;
  slv->btor               = btor;
  slv->api.clone          = (BtorSolverClone) clone_ef_solver;
  slv->api.delet          = (BtorSolverDelete) delete_ef_solver;
  slv->api.sat            = (BtorSolverSat) sat_ef_solver;
  slv->api.generate_model = (BtorSolverGenerateModel) generate_model_ef_solver;
  slv->api.print_stats    = (BtorSolverPrintStats) print_stats_ef_solver;
  slv->api.print_time_stats =
      (BtorSolverPrintTimeStats) print_time_stats_ef_solver;

  BTOR_MSG (btor->msg, 1, "enabled ef engine");

  return (BtorSolver *) slv;
}