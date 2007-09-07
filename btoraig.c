#include "btoraig.h"
#include "btorhash.h"
#include "btorsat.h"
#include "btorutil.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/*------------------------------------------------------------------------*/
/* BEGIN OF DECLARATIONS                                                  */
/*------------------------------------------------------------------------*/

struct BtorAIGUniqueTable
{
  int size;
  int num_elements;
  BtorAIG **chains;
};

typedef struct BtorAIGUniqueTable BtorAIGUniqueTable;

#define BTOR_INIT_AIG_UNIQUE_TABLE(mm, table) \
  do                                          \
  {                                           \
    assert (mm != NULL);                      \
    (table).size         = 1;                 \
    (table).num_elements = 0;                 \
    BTOR_CNEW (mm, (table).chains);           \
  } while (0)

#define BTOR_RELEASE_AIG_UNIQUE_TABLE(mm, table)     \
  do                                                 \
  {                                                  \
    assert (mm != NULL);                             \
    BTOR_DELETEN (mm, (table).chains, (table).size); \
  } while (0)

#define BTOR_AIG_UNIQUE_TABLE_LIMIT 30
#define BTOR_AIG_UNIQUE_TABLE_PRIME 2000000137u

struct BtorAIGMgr
{
  BtorMemMgr *mm;
  BtorAIGUniqueTable table;
  int id;
  int verbosity;
  BtorSATMgr *smgr;
  BtorCNFEnc cnf_enc;
};

/*------------------------------------------------------------------------*/
/* END OF DECLARATIONS                                                    */
/*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*/
/* BEGIN OF IMPLEMENTATION                                                */
/*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*/
/* Auxilliary                                                             */
/*------------------------------------------------------------------------*/

static void
print_verbose_msg (char *msg)
{
  assert (msg != NULL);
  fprintf (stderr, "[btoraig] %s", msg);
  fflush (stderr);
}

/*------------------------------------------------------------------------*/
/* BtorAIG                                                                */
/*------------------------------------------------------------------------*/
static BtorAIG *
new_and_aig (BtorAIGMgr *amgr, BtorAIG *left, BtorAIG *right)
{
  BtorAIG *aig = NULL;
  assert (amgr != NULL);
  assert (!BTOR_IS_CONST_AIG (left));
  assert (!BTOR_IS_CONST_AIG (right));
  BTOR_NEW (amgr->mm, aig);
  assert (amgr->id < INT_MAX);
  aig->id                    = amgr->id++;
  BTOR_LEFT_CHILD_AIG (aig)  = left;
  BTOR_RIGHT_CHILD_AIG (aig) = right;
  aig->refs                  = 1;
  aig->cnf_id                = 0;
  aig->next                  = NULL;
  aig->mark                  = 0;
  aig->pos_imp               = 0;
  aig->neg_imp               = 0;
  return aig;
}

static void
delete_aig_node (BtorAIGMgr *amgr, BtorAIG *aig)
{
  assert (amgr != NULL);
  if (!BTOR_IS_CONST_AIG (aig)) BTOR_DELETE (amgr->mm, aig);
}

static unsigned int
compute_aig_hash (BtorAIG *aig, int table_size)
{
  unsigned int hash = 0u;
  assert (!BTOR_IS_INVERTED_AIG (aig));
  assert (BTOR_IS_AND_AIG (aig));
  assert (table_size > 0);
  assert (btor_is_power_of_2_util (table_size));
  hash = (unsigned int) BTOR_REAL_ADDR_AIG (BTOR_LEFT_CHILD_AIG (aig))->id
         + (unsigned int) BTOR_REAL_ADDR_AIG (BTOR_RIGHT_CHILD_AIG (aig))->id;
  hash = (hash * BTOR_AIG_UNIQUE_TABLE_PRIME) & (table_size - 1);
  return hash;
}

static void
delete_aig_unique_table_entry (BtorAIGMgr *amgr, BtorAIG *aig)
{
  unsigned int hash = 0u;
  BtorAIG *cur      = NULL;
  BtorAIG *prev     = NULL;
  assert (amgr != NULL);
  assert (!BTOR_IS_INVERTED_AIG (aig));
  assert (BTOR_IS_AND_AIG (aig));
  hash = compute_aig_hash (aig, amgr->table.size);
  cur  = amgr->table.chains[hash];
  while (cur != aig && cur != NULL)
  {
    assert (!BTOR_IS_INVERTED_AIG (cur));
    prev = cur;
    cur  = cur->next;
  }
  assert (cur != NULL);
  if (prev == NULL)
    amgr->table.chains[hash] = cur->next;
  else
    prev->next = cur->next;
  amgr->table.num_elements--;
  delete_aig_node (amgr, cur);
}

static void
inc_aig_ref_counter (BtorAIG *aig)
{
  if (!BTOR_IS_CONST_AIG (aig))
  {
    assert (BTOR_REAL_ADDR_AIG (aig)->refs < INT_MAX);
    BTOR_REAL_ADDR_AIG (aig)->refs++;
  }
}

static BtorAIG *
inc_aig_ref_counter_and_return (BtorAIG *aig)
{
  inc_aig_ref_counter (aig);
  return aig;
}

static BtorAIG **
find_and_aig (BtorAIGMgr *amgr, BtorAIG *left, BtorAIG *right)
{
  BtorAIG **result  = NULL;
  BtorAIG *cur      = NULL;
  BtorAIG *temp     = NULL;
  unsigned int hash = 0u;
  assert (amgr != NULL);
  assert (!BTOR_IS_CONST_AIG (left));
  assert (!BTOR_IS_CONST_AIG (right));
  hash = (((unsigned int) BTOR_REAL_ADDR_AIG (left)->id
           + (unsigned int) BTOR_REAL_ADDR_AIG (right)->id)
          * BTOR_AIG_UNIQUE_TABLE_PRIME)
         & (amgr->table.size - 1);
  result = amgr->table.chains + hash;
  cur    = *result;
  if (BTOR_REAL_ADDR_AIG (right)->id < BTOR_REAL_ADDR_AIG (left)->id)
  {
    temp  = left;
    left  = right;
    right = temp;
  }
  while (cur != NULL)
  {
    assert (!BTOR_IS_INVERTED_AIG (cur));
    assert (BTOR_IS_AND_AIG (cur));
    if (BTOR_LEFT_CHILD_AIG (cur) == left
        && BTOR_RIGHT_CHILD_AIG (cur) == right)
      break;
    else
    {
      result = &(cur->next);
      cur    = *result;
    }
  }
  return result;
}

static void
enlarge_aig_unique_table (BtorAIGMgr *amgr)
{
  BtorMemMgr *mm       = NULL;
  BtorAIG **new_chains = NULL;
  int i                = 0;
  int size             = 0;
  int new_size         = 0;
  unsigned int hash    = 0u;
  BtorAIG *temp        = NULL;
  BtorAIG *cur         = NULL;
  assert (amgr != NULL);
  size     = amgr->table.size;
  new_size = size << 1;
  assert (new_size / size == 2);
  mm = amgr->mm;
  BTOR_CNEWN (mm, new_chains, new_size);
  for (i = 0; i < size; i++)
  {
    cur = amgr->table.chains[i];
    while (cur != NULL)
    {
      assert (!BTOR_IS_INVERTED_AIG (cur));
      assert (BTOR_IS_AND_AIG (cur));
      temp             = cur->next;
      hash             = compute_aig_hash (cur, new_size);
      cur->next        = new_chains[hash];
      new_chains[hash] = cur;
      cur              = temp;
    }
  }
  BTOR_DELETEN (mm, amgr->table.chains, size);
  amgr->table.size   = new_size;
  amgr->table.chains = new_chains;
}

BtorAIG *
btor_copy_aig (BtorAIGMgr *amgr, BtorAIG *aig)
{
  (void) amgr;
  assert (amgr != NULL);
  if (BTOR_IS_CONST_AIG (aig)) return aig;
  return inc_aig_ref_counter_and_return (aig);
}

void
btor_mark_aig (BtorAIGMgr *amgr, BtorAIG *aig, int new_mark)
{
  BtorMemMgr *mm = NULL;
  BtorAIGPtrStack stack;
  BtorAIG *cur = NULL;
  assert (amgr != NULL);
  assert (!BTOR_IS_CONST_AIG (aig));
  mm = amgr->mm;
  BTOR_INIT_STACK (stack);
  BTOR_PUSH_STACK (mm, stack, aig);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur = BTOR_REAL_ADDR_AIG (BTOR_POP_STACK (stack));
    if (cur->mark != new_mark)
    {
      cur->mark = new_mark;
      if (BTOR_IS_AND_AIG (cur))
      {
        BTOR_PUSH_STACK (mm, stack, BTOR_RIGHT_CHILD_AIG (cur));
        BTOR_PUSH_STACK (mm, stack, BTOR_LEFT_CHILD_AIG (cur));
      }
    }
  }
  BTOR_RELEASE_STACK (mm, stack);
}

void
btor_release_aig (BtorAIGMgr *amgr, BtorAIG *aig)
{
  BtorMemMgr *mm = NULL;
  BtorAIGPtrStack stack;
  BtorAIG *cur = BTOR_REAL_ADDR_AIG (aig);
  assert (amgr != NULL);
  mm = amgr->mm;
  if (!BTOR_IS_CONST_AIG (aig))
  {
    assert (cur->refs > 0);
    if (cur->refs > 1)
      cur->refs--;
    else
    {
      assert (cur->refs == 1);
      BTOR_INIT_STACK (stack);
      BTOR_PUSH_STACK (mm, stack, cur);
      while (!BTOR_EMPTY_STACK (stack))
      {
        cur = BTOR_REAL_ADDR_AIG (BTOR_POP_STACK (stack));
        if (cur->refs > 1)
          cur->refs--;
        else
        {
          assert (cur->refs == 1);
          if (BTOR_IS_VAR_AIG (cur))
            delete_aig_node (amgr, cur);
          else
          {
            assert (BTOR_IS_AND_AIG (cur));
            BTOR_PUSH_STACK (mm, stack, BTOR_RIGHT_CHILD_AIG (cur));
            BTOR_PUSH_STACK (mm, stack, BTOR_LEFT_CHILD_AIG (cur));
            delete_aig_unique_table_entry (amgr, cur);
          }
        }
      }
      BTOR_RELEASE_STACK (mm, stack);
    }
  }
}

BtorAIG *
btor_var_aig (BtorAIGMgr *amgr)
{
  BtorAIG *aig = NULL;
  assert (amgr != NULL);
  BTOR_NEW (amgr->mm, aig);
  assert (amgr->id < INT_MAX);
  aig->id                    = amgr->id++;
  BTOR_LEFT_CHILD_AIG (aig)  = NULL;
  BTOR_RIGHT_CHILD_AIG (aig) = NULL;
  aig->refs                  = 1;
  aig->cnf_id                = 0;
  aig->next                  = NULL;
  aig->mark                  = 0;
  aig->pos_imp               = 0;
  aig->neg_imp               = 0;
  return aig;
}

BtorAIG *
btor_not_aig (BtorAIGMgr *amgr, BtorAIG *aig)
{
  (void) amgr;
  assert (amgr != NULL);
  inc_aig_ref_counter (aig);
  return BTOR_INVERT_AIG (aig);
}

BtorAIG *
btor_and_aig (BtorAIGMgr *amgr, BtorAIG *left, BtorAIG *right)
{
  BtorAIG **lookup    = NULL;
  BtorAIG *real_left  = NULL;
  BtorAIG *real_right = NULL;
  assert (amgr != NULL);
  if (left == BTOR_AIG_FALSE || right == BTOR_AIG_FALSE) return BTOR_AIG_FALSE;
  if (left == BTOR_AIG_TRUE) return inc_aig_ref_counter_and_return (right);
TRY_AGAIN:
  if (right == BTOR_AIG_TRUE || (left == right))
    return inc_aig_ref_counter_and_return (left);
  if (left == BTOR_INVERT_AIG (right)) return BTOR_AIG_FALSE;
  real_left  = BTOR_REAL_ADDR_AIG (left);
  real_right = BTOR_REAL_ADDR_AIG (right);
  /* 2 level minimization rules for AIGs */
  /* first rule of contradiction */
  if (BTOR_IS_AND_AIG (real_left) && !BTOR_IS_INVERTED_AIG (left))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left) == BTOR_INVERT_AIG (right)
        || BTOR_RIGHT_CHILD_AIG (real_left) == BTOR_INVERT_AIG (right))
      return BTOR_AIG_FALSE;
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && !BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_right) == BTOR_INVERT_AIG (left)
        || BTOR_RIGHT_CHILD_AIG (real_right) == BTOR_INVERT_AIG (left))
      return BTOR_AIG_FALSE;
  }
  /* second rule of contradiction */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_AND_AIG (real_left)
      && !BTOR_IS_INVERTED_AIG (left) && !BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left)
            == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right))
        || BTOR_LEFT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right))
        || BTOR_RIGHT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right))
        || BTOR_RIGHT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right)))
      return BTOR_AIG_FALSE;
  }
  /* first rule of subsumption */
  if (BTOR_IS_AND_AIG (real_left) && BTOR_IS_INVERTED_AIG (left))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left) == BTOR_INVERT_AIG (right)
        || BTOR_RIGHT_CHILD_AIG (real_left) == BTOR_INVERT_AIG (right))
      return inc_aig_ref_counter_and_return (right);
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_right) == BTOR_INVERT_AIG (left)
        || BTOR_RIGHT_CHILD_AIG (real_right) == BTOR_INVERT_AIG (left))
      return inc_aig_ref_counter_and_return (left);
  }
  /* second rule of subsumption */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_AND_AIG (real_left)
      && BTOR_IS_INVERTED_AIG (left) && !BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left)
            == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right))
        || BTOR_LEFT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right))
        || BTOR_RIGHT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right))
        || BTOR_RIGHT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right)))
      return inc_aig_ref_counter_and_return (right);
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_AND_AIG (real_left)
      && !BTOR_IS_INVERTED_AIG (left) && BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left)
            == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right))
        || BTOR_LEFT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right))
        || BTOR_RIGHT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right))
        || BTOR_RIGHT_CHILD_AIG (real_left)
               == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right)))
      return inc_aig_ref_counter_and_return (left);
  }
  /* rule of resolution */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_AND_AIG (real_left)
      && BTOR_IS_INVERTED_AIG (left) && BTOR_IS_INVERTED_AIG (right))
  {
    if ((BTOR_LEFT_CHILD_AIG (real_left) == BTOR_LEFT_CHILD_AIG (real_right)
         && BTOR_RIGHT_CHILD_AIG (real_left)
                == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right)))
        || (BTOR_LEFT_CHILD_AIG (real_left) == BTOR_RIGHT_CHILD_AIG (real_right)
            && BTOR_RIGHT_CHILD_AIG (real_left)
                   == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right))))
      return inc_aig_ref_counter_and_return (
          BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_left)));
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_AND_AIG (real_left)
      && BTOR_IS_INVERTED_AIG (left) && BTOR_IS_INVERTED_AIG (right))
  {
    if ((BTOR_RIGHT_CHILD_AIG (real_right) == BTOR_RIGHT_CHILD_AIG (real_left)
         && BTOR_LEFT_CHILD_AIG (real_right)
                == BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_left)))
        || (BTOR_RIGHT_CHILD_AIG (real_right) == BTOR_LEFT_CHILD_AIG (real_left)
            && BTOR_LEFT_CHILD_AIG (real_right)
                   == BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_left))))
      return inc_aig_ref_counter_and_return (
          BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right)));
  }
  /* asymmetric rule of idempotency */
  if (BTOR_IS_AND_AIG (real_left) && !BTOR_IS_INVERTED_AIG (left))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left) == right
        || BTOR_RIGHT_CHILD_AIG (real_left) == right)
      return inc_aig_ref_counter_and_return (left);
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && !BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_right) == left
        || BTOR_RIGHT_CHILD_AIG (real_right) == left)
      return inc_aig_ref_counter_and_return (right);
  }
  /* symmetric rule of idempotency */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_AND_AIG (real_left)
      && !BTOR_IS_INVERTED_AIG (left) && !BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left) == BTOR_LEFT_CHILD_AIG (real_right)
        || BTOR_RIGHT_CHILD_AIG (real_left) == BTOR_LEFT_CHILD_AIG (real_right))
    {
      right = BTOR_RIGHT_CHILD_AIG (real_right);
      goto TRY_AGAIN;
    }
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_AND_AIG (real_left)
      && !BTOR_IS_INVERTED_AIG (left) && !BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_left) == BTOR_RIGHT_CHILD_AIG (real_right)
        || BTOR_RIGHT_CHILD_AIG (real_left)
               == BTOR_RIGHT_CHILD_AIG (real_right))
    {
      right = BTOR_LEFT_CHILD_AIG (real_right);
      goto TRY_AGAIN;
    }
  }
  /* asymmetric rule of substitution */
  if (BTOR_IS_AND_AIG (real_left) && BTOR_IS_INVERTED_AIG (left))
  {
    if (BTOR_RIGHT_CHILD_AIG (real_left) == right)
    {
      left = BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_left));
      goto TRY_AGAIN;
    }
    if (BTOR_LEFT_CHILD_AIG (real_left) == right)
    {
      left = BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_left));
      goto TRY_AGAIN;
    }
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_INVERTED_AIG (right))
  {
    if (BTOR_LEFT_CHILD_AIG (real_right) == left)
    {
      right = BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right));
      goto TRY_AGAIN;
    }
    if (BTOR_RIGHT_CHILD_AIG (real_right) == left)
    {
      right = BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right));
      goto TRY_AGAIN;
    }
  }
  /* symmetric rule of substitution */
  if (BTOR_IS_AND_AIG (real_left) && BTOR_IS_INVERTED_AIG (left)
      && BTOR_IS_AND_AIG (real_right) && !BTOR_IS_INVERTED_AIG (right))
  {
    if ((BTOR_RIGHT_CHILD_AIG (real_left) == BTOR_LEFT_CHILD_AIG (real_right))
        || (BTOR_RIGHT_CHILD_AIG (real_left)
            == BTOR_RIGHT_CHILD_AIG (real_right)))
    {
      left = BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_left));
      goto TRY_AGAIN;
    }
    if ((BTOR_LEFT_CHILD_AIG (real_left) == BTOR_LEFT_CHILD_AIG (real_right))
        || (BTOR_LEFT_CHILD_AIG (real_left)
            == BTOR_RIGHT_CHILD_AIG (real_right)))
    {
      left = BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_left));
      goto TRY_AGAIN;
    }
  }
  /* use commutativity */
  if (BTOR_IS_AND_AIG (real_right) && BTOR_IS_INVERTED_AIG (right)
      && BTOR_IS_AND_AIG (real_left) && !BTOR_IS_INVERTED_AIG (left))
  {
    if ((BTOR_LEFT_CHILD_AIG (real_right) == BTOR_RIGHT_CHILD_AIG (real_left))
        || (BTOR_LEFT_CHILD_AIG (real_right)
            == BTOR_LEFT_CHILD_AIG (real_left)))
    {
      right = BTOR_INVERT_AIG (BTOR_RIGHT_CHILD_AIG (real_right));
      goto TRY_AGAIN;
    }
    if ((BTOR_RIGHT_CHILD_AIG (real_right) == BTOR_RIGHT_CHILD_AIG (real_left))
        || (BTOR_RIGHT_CHILD_AIG (real_right)
            == BTOR_LEFT_CHILD_AIG (real_left)))
    {
      right = BTOR_INVERT_AIG (BTOR_LEFT_CHILD_AIG (real_right));
      goto TRY_AGAIN;
    }
  }
  lookup = find_and_aig (amgr, left, right);
  if (*lookup == NULL)
  {
    if (amgr->table.num_elements == amgr->table.size
        && btor_log_2_util (amgr->table.size) < BTOR_AIG_UNIQUE_TABLE_LIMIT)
    {
      enlarge_aig_unique_table (amgr);
      lookup = find_and_aig (amgr, left, right);
    }
    if (real_right->id < real_left->id)
      *lookup = new_and_aig (amgr, right, left);
    else
      *lookup = new_and_aig (amgr, left, right);
    inc_aig_ref_counter (left);
    inc_aig_ref_counter (right);
    assert (amgr->table.num_elements < INT_MAX);
    amgr->table.num_elements++;
  }
  else
    inc_aig_ref_counter (*lookup);
  return *lookup;
}

BtorAIG *
btor_or_aig (BtorAIGMgr *amgr, BtorAIG *left, BtorAIG *right)
{
  assert (amgr != NULL);
  return BTOR_INVERT_AIG (
      btor_and_aig (amgr, BTOR_INVERT_AIG (left), BTOR_INVERT_AIG (right)));
}

BtorAIG *
btor_eq_aig (BtorAIGMgr *amgr, BtorAIG *left, BtorAIG *right)
{
  BtorAIG *eq       = NULL;
  BtorAIG *eq_left  = NULL;
  BtorAIG *eq_right = NULL;
  assert (amgr != NULL);
  eq_left =
      BTOR_INVERT_AIG (btor_and_aig (amgr, left, BTOR_INVERT_AIG (right)));
  eq_right =
      BTOR_INVERT_AIG (btor_and_aig (amgr, BTOR_INVERT_AIG (left), right));
  eq = btor_and_aig (amgr, eq_left, eq_right);
  btor_release_aig (amgr, eq_left);
  btor_release_aig (amgr, eq_right);
  return eq;
}

BtorAIG *
btor_xor_aig (BtorAIGMgr *amgr, BtorAIG *left, BtorAIG *right)
{
  assert (amgr != NULL);
  return BTOR_INVERT_AIG (btor_eq_aig (amgr, left, right));
}

BtorAIG *
btor_cond_aig (BtorAIGMgr *amgr,
               BtorAIG *a_cond,
               BtorAIG *a_if,
               BtorAIG *a_else)
{
  BtorAIG *cond = NULL;
  BtorAIG *and1 = NULL;
  BtorAIG *and2 = NULL;
  assert (amgr != NULL);
  and1 = btor_and_aig (amgr, a_if, a_cond);
  and2 = btor_and_aig (amgr, a_else, BTOR_INVERT_AIG (a_cond));
  cond = btor_or_aig (amgr, and1, and2);
  btor_release_aig (amgr, and1);
  btor_release_aig (amgr, and2);
  return cond;
}

void
btor_dump_aig (BtorAIGMgr *amgr, int binary, FILE *output, BtorAIG *aig)
{
  btor_dump_aigs (amgr, binary, output, &aig, 1);
}

static unsigned
btor_aiger_encode_aig (BtorPtrToIntHashTable *table, BtorAIG *aig)
{
  BtorPtrToIntHashBucket *b;
  BtorAIG *real_aig;
  unsigned res;

  if (aig == BTOR_AIG_FALSE) return 0;

  if (aig == BTOR_AIG_TRUE) return 1;

  real_aig = BTOR_REAL_ADDR_AIG (aig);

  b = btor_find_in_ptr_to_int_hash_table (table, real_aig);
  assert (b);

  res = 2 * (unsigned) b->data;

  if (BTOR_IS_INVERTED_AIG (aig)) res ^= 1;

  return res;
}

void
btor_dump_aigs (
    BtorAIGMgr *amgr, int binary, FILE *file, BtorAIG **aigs, int naigs)
{
  unsigned aig_id, left_id, right_id, tmp, delta;
  BtorMemMgr *mm = NULL;
  BtorAIG *aig, *left, *right;
  BtorPtrToIntHashTable *table;
  BtorPtrToIntHashBucket *p;
  int M, I, L, O, A, i;
  BtorAIGPtrStack stack;
  unsigned char ch;

  assert (naigs > 0);

  mm = amgr->mm;

  table = btor_new_ptr_to_int_hash_table (amgr->mm, 0, 0);

  /* First add inputs aka variables to hash table.
   */
  I = 0;
  BTOR_INIT_STACK (stack);
  for (i = naigs - 1; i >= 0; i--)
  {
    aig = aigs[i];
    if (!BTOR_IS_CONST_AIG (aig)) BTOR_PUSH_STACK (mm, stack, aig);
  }

  while (!BTOR_EMPTY_STACK (stack))
  {
    aig = BTOR_POP_STACK (stack);

  CONTINUE_WITHOUT_POP:

    assert (!BTOR_IS_CONST_AIG (aig));
    aig = BTOR_REAL_ADDR_AIG (aig);

    if (aig->mark) continue;

    aig->mark = 1;

    if (BTOR_IS_VAR_AIG (aig))
    {
      btor_insert_in_ptr_to_int_hash_table (table, aig, ++I);
      assert (I >= 0);
    }
    else
    {
      assert (BTOR_IS_AND_AIG (aig));

      right = BTOR_RIGHT_CHILD_AIG (aig);
      BTOR_PUSH_STACK (mm, stack, right);

      aig = BTOR_LEFT_CHILD_AIG (aig);
      goto CONTINUE_WITHOUT_POP;
    }
  }

  M = I;

  /* Then start adding ANG gates in postfix order.
   */
  assert (BTOR_EMPTY_STACK (stack));
  for (i = naigs - 1; i >= 0; i--)
  {
    aig = aigs[i];
    if (!BTOR_IS_CONST_AIG (aig)) BTOR_PUSH_STACK (mm, stack, aig);
  }

  while (!BTOR_EMPTY_STACK (stack))
  {
    aig = BTOR_POP_STACK (stack);

    if (aig)
    {
    CONTINUE_WITH_NON_ZERO_AIG:

      assert (!BTOR_IS_CONST_AIG (aig));
      aig = BTOR_REAL_ADDR_AIG (aig);

      if (!aig->mark) continue;

      aig->mark = 0;

      if (BTOR_IS_VAR_AIG (aig)) continue;

      BTOR_PUSH_STACK (mm, stack, aig);
      BTOR_PUSH_STACK (mm, stack, 0);

      right = BTOR_RIGHT_CHILD_AIG (aig);
      BTOR_PUSH_STACK (mm, stack, right);

      aig = BTOR_LEFT_CHILD_AIG (aig);
      goto CONTINUE_WITH_NON_ZERO_AIG;
    }
    else
    {
      assert (!BTOR_EMPTY_STACK (stack));

      aig = BTOR_POP_STACK (stack);
      assert (!aig->mark);

      assert (aig);
      assert (BTOR_REAL_ADDR_AIG (aig) == aig);
      assert (BTOR_IS_AND_AIG (aig));

      btor_insert_in_ptr_to_int_hash_table (table, aig, ++M);
      assert (M >= 0);
    }
  }

  A = M - I;

  BTOR_RELEASE_STACK (mm, stack);

  L = 0; /* TODO: no latches sofar */
  O = naigs;

  fprintf (file, "a%cg %d %d %d %d %d\n", binary ? 'i' : 'a', M, I, L, O, A);

  /* Only need to print inputs in non binary mode.
   */
  for (p = table->first; p; p = p->next)
  {
    aig = p->key;

    assert (aig);
    assert (!BTOR_IS_INVERTED_AIG (aig));

    if (!BTOR_IS_VAR_AIG (aig)) break;

    if (!binary) fprintf (file, "%d\n", 2 * p->data);
  }

  /* Then the outputs ...
   */
  for (i = 0; i < naigs; i++)
    fprintf (file, "%u\n", btor_aiger_encode_aig (table, aigs[i]));

  /* And finally all the AND gates.
   */
  while (p)
  {
    aig = p->key;

    assert (aig);
    assert (!BTOR_IS_INVERTED_AIG (aig));
    assert (BTOR_IS_AND_AIG (aig));

    left  = BTOR_LEFT_CHILD_AIG (aig);
    right = BTOR_RIGHT_CHILD_AIG (aig);

    aig_id   = 2 * (unsigned) p->data;
    left_id  = btor_aiger_encode_aig (table, left);
    right_id = btor_aiger_encode_aig (table, right);

    if (left_id < right_id)
    {
      tmp      = left_id;
      left_id  = right_id;
      right_id = tmp;
    }

    assert (aig_id > left_id);
    assert (left_id >= right_id); /* strict ? */

    if (binary)
    {
      for (i = 0; i < 2; i++)
      {
        delta = i ? left_id - right_id : aig_id - left_id;
        tmp   = delta;

        while (tmp & ~0x7f)
        {
          ch = tmp & 0x7f;
          ch |= 0x80;

          putc (ch, file);
          tmp >>= 7;
        }

        ch = tmp;
        putc (ch, file);
      }
    }
    else
      fprintf (file, "%u %u %u\n", aig_id, left_id, right_id);

    p = p->next;
  }

  btor_delete_ptr_to_int_hash_table (table);
}

BtorAIGMgr *
btor_new_aig_mgr (BtorMemMgr *mm, int verbosity)
{
  BtorAIGMgr *amgr = NULL;
  assert (mm != NULL);
  assert (verbosity >= -1);
  BTOR_NEW (mm, amgr);
  amgr->mm = mm;
  BTOR_INIT_AIG_UNIQUE_TABLE (mm, amgr->table);
  amgr->id        = 1;
  amgr->verbosity = verbosity;
  amgr->smgr      = btor_new_sat_mgr (mm, verbosity);
  amgr->cnf_enc   = BTOR_TSEITIN_CNF_ENC;
  return amgr;
}

void
btor_delete_aig_mgr (BtorAIGMgr *amgr)
{
  BtorMemMgr *mm = NULL;
  assert (amgr != NULL);
  assert (amgr->table.num_elements == 0);
  mm = amgr->mm;
  BTOR_RELEASE_AIG_UNIQUE_TABLE (mm, amgr->table);
  btor_delete_sat_mgr (amgr->smgr);
  BTOR_DELETE (mm, amgr);
}

static void
aig_to_sat_tseitin (BtorAIGMgr *amgr, BtorAIG *aig)
{
  BtorAIGPtrStack stack;
  BtorSATMgr *smgr = NULL;
  BtorMemMgr *mm   = NULL;
  int x            = 0;
  int y            = 0;
  int z            = 0;
  BtorAIG *cur     = NULL;
  BtorAIG *left    = NULL;
  BtorAIG *right   = NULL;
  assert (amgr != NULL);
  assert (!BTOR_IS_CONST_AIG (aig));
  assert (!BTOR_IS_VAR_AIG (BTOR_REAL_ADDR_AIG (aig)));
  if (amgr->verbosity > 1)
    print_verbose_msg (
        "transforming AIG into CNF using Tseitin transformation\n");
  smgr = amgr->smgr;
  mm   = amgr->mm;
  BTOR_INIT_STACK (stack);
  BTOR_PUSH_STACK (mm, stack, aig);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur = BTOR_REAL_ADDR_AIG (BTOR_POP_STACK (stack));
    if (cur->cnf_id == 0)
    {
      if (cur->mark == 0)
      {
        if (BTOR_IS_VAR_AIG (cur))
          cur->cnf_id = btor_next_cnf_id_sat_mgr (smgr);
        else
        {
          assert (BTOR_IS_AND_AIG (cur));
          cur->mark = 1;
          BTOR_PUSH_STACK (mm, stack, cur);
          BTOR_PUSH_STACK (mm, stack, BTOR_RIGHT_CHILD_AIG (cur));
          BTOR_PUSH_STACK (mm, stack, BTOR_LEFT_CHILD_AIG (cur));
        }
      }
      else
      {
        assert (cur->mark == 1);
        assert (BTOR_IS_AND_AIG (cur));
        left        = BTOR_LEFT_CHILD_AIG (cur);
        right       = BTOR_RIGHT_CHILD_AIG (cur);
        cur->cnf_id = btor_next_cnf_id_sat_mgr (smgr);
        x           = cur->cnf_id;
        y           = BTOR_GET_CNF_ID_AIG (left);
        z           = BTOR_GET_CNF_ID_AIG (right);
        assert (x != 0);
        assert (y != 0);
        assert (z != 0);
        (void) btor_add_sat (smgr, -x);
        (void) btor_add_sat (smgr, y);
        (void) btor_add_sat (smgr, 0);
        (void) btor_add_sat (smgr, -x);
        (void) btor_add_sat (smgr, z);
        (void) btor_add_sat (smgr, 0);
        (void) btor_add_sat (smgr, -y);
        (void) btor_add_sat (smgr, -z);
        (void) btor_add_sat (smgr, x);
        (void) btor_add_sat (smgr, 0);
      }
    }
  }
  BTOR_RELEASE_STACK (mm, stack);
  btor_mark_aig (amgr, aig, 0);
}

static void
generate_cnf_ids (BtorAIGMgr *amgr, BtorAIG *aig)
{
  BtorAIGPtrStack stack;
  BtorSATMgr *smgr = NULL;
  BtorAIG *cur     = NULL;
  BtorMemMgr *mm   = NULL;
  assert (amgr != NULL);
  assert (!BTOR_IS_CONST_AIG (aig));
  smgr = amgr->smgr;
  mm   = amgr->mm;
  BTOR_INIT_STACK (stack);
  BTOR_PUSH_STACK (mm, stack, aig);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur = BTOR_REAL_ADDR_AIG (BTOR_POP_STACK (stack));
    if (cur->cnf_id == 0)
    {
      if (cur->mark == 0)
      {
        if (BTOR_IS_VAR_AIG (cur))
          cur->cnf_id = btor_next_cnf_id_sat_mgr (smgr);
        else
        {
          assert (BTOR_IS_AND_AIG (cur));
          cur->mark = 1;
          BTOR_PUSH_STACK (mm, stack, cur);
          BTOR_PUSH_STACK (mm, stack, BTOR_RIGHT_CHILD_AIG (cur));
          BTOR_PUSH_STACK (mm, stack, BTOR_LEFT_CHILD_AIG (cur));
        }
      }
      else
      {
        assert (BTOR_IS_AND_AIG (cur));
        assert (cur->mark == 1);
        cur->cnf_id = btor_next_cnf_id_sat_mgr (smgr);
      }
    }
  }
  BTOR_RELEASE_STACK (mm, stack);
  btor_mark_aig (amgr, aig, 0);
}

static void
aig_to_sat_plaisted_greenbaum (BtorAIGMgr *amgr, BtorAIG *aig)
{
  BtorAIGPtrStack stack;
  BtorSATMgr *smgr = NULL;
  BtorMemMgr *mm   = NULL;
  int x            = 0;
  int y            = 0;
  int z            = 0;
  int is_inverted  = 0;
  BtorAIG *cur     = NULL;
  BtorAIG *left    = NULL;
  BtorAIG *right   = NULL;
  assert (amgr != NULL);
  assert (!BTOR_IS_CONST_AIG (aig));
  assert (!BTOR_IS_VAR_AIG (BTOR_REAL_ADDR_AIG (aig)));
  if (amgr->verbosity > 1)
    print_verbose_msg (
        "transforming AIG into CNF using Plaisted-Greenbaum transformation\n");
  smgr = amgr->smgr;
  generate_cnf_ids (amgr, aig);
  mm = amgr->mm;
  BTOR_INIT_STACK (stack);
  BTOR_PUSH_STACK (mm, stack, aig);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur         = BTOR_POP_STACK (stack);
    is_inverted = BTOR_IS_INVERTED_AIG (cur);
    cur         = BTOR_REAL_ADDR_AIG (cur);

    if (is_inverted && cur->neg_imp) continue;

    if (!is_inverted && cur->pos_imp) continue;

    left  = BTOR_LEFT_CHILD_AIG (cur);
    right = BTOR_RIGHT_CHILD_AIG (cur);
    x     = cur->cnf_id;
    y     = BTOR_GET_CNF_ID_AIG (left);
    z     = BTOR_GET_CNF_ID_AIG (right);
    assert (x != 0);
    assert (y != 0);
    assert (z != 0);

    if (is_inverted)
    {
      btor_add_sat (smgr, x);
      btor_add_sat (smgr, -y);
      btor_add_sat (smgr, -z);
      btor_add_sat (smgr, 0);
      cur->neg_imp = 1;
    }
    else
    {
      btor_add_sat (smgr, -x);
      btor_add_sat (smgr, y);
      btor_add_sat (smgr, 0);
      btor_add_sat (smgr, -x);
      btor_add_sat (smgr, z);
      btor_add_sat (smgr, 0);
      cur->pos_imp = 1;
    }
    if (BTOR_IS_AND_AIG (BTOR_REAL_ADDR_AIG (right)))
    {
      if (is_inverted)
        BTOR_PUSH_STACK (mm, stack, BTOR_INVERT_AIG (right));
      else
        BTOR_PUSH_STACK (mm, stack, right);
    }
    if (BTOR_IS_AND_AIG (BTOR_REAL_ADDR_AIG (left)))
    {
      if (is_inverted)
        BTOR_PUSH_STACK (mm, stack, BTOR_INVERT_AIG (left));
      else
        BTOR_PUSH_STACK (mm, stack, left);
    }
  }
  BTOR_RELEASE_STACK (mm, stack);
}

void
btor_aig_to_sat (BtorAIGMgr *amgr, BtorAIG *aig)
{
  BtorSATMgr *smgr = NULL;
  assert (amgr != NULL);
  assert (!BTOR_IS_CONST_AIG (aig));
  smgr = amgr->smgr;
  if (BTOR_IS_VAR_AIG (BTOR_REAL_ADDR_AIG (aig)))
  {
    if (BTOR_REAL_ADDR_AIG (aig)->cnf_id == 0)
      BTOR_REAL_ADDR_AIG (aig)->cnf_id = btor_next_cnf_id_sat_mgr (smgr);
    btor_add_sat (smgr, BTOR_GET_CNF_ID_AIG (aig));
    btor_add_sat (smgr, 0);
  }
  else
  {
    if (amgr->cnf_enc == BTOR_TSEITIN_CNF_ENC)
      aig_to_sat_tseitin (amgr, aig);
    else
    {
      assert (amgr->cnf_enc == BTOR_PLAISTED_GREENBAUM_CNF_ENC);
      aig_to_sat_plaisted_greenbaum (amgr, aig);
    }
  }
}

void
btor_set_cnf_enc_aig_mgr (BtorAIGMgr *amgr, BtorCNFEnc cnf_enc)
{
  assert (amgr != NULL);
  amgr->cnf_enc = cnf_enc;
}

BtorSATMgr *
btor_get_sat_mgr_aig_mgr (BtorAIGMgr *amgr)
{
  assert (amgr != NULL);
  return (amgr->smgr);
}

int
btor_get_assignment_aig (BtorAIGMgr *amgr, BtorAIG *aig)
{
  assert (amgr != NULL);
  if (aig == BTOR_AIG_TRUE) return 1;
  if (aig == BTOR_AIG_FALSE) return -1;
  if (BTOR_REAL_ADDR_AIG (aig)->cnf_id == 0) return 0;
  if (BTOR_IS_INVERTED_AIG (aig))
    return -btor_deref_sat (amgr->smgr, BTOR_REAL_ADDR_AIG (aig)->cnf_id);
  return btor_deref_sat (amgr->smgr, aig->cnf_id);
}

/*------------------------------------------------------------------------*/
/* END OF IMPLEMENTATION                                                  */
/*------------------------------------------------------------------------*/
