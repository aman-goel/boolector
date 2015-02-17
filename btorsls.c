/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorbitvec.h"
#include "btorcore.h"
#include "btordbg.h"
#include "btordcr.h"
#include "btorhash.h"
#include "btoriter.h"
#include "btorlog.h"
#include "btormisc.h"
#include "btormodel.h"
#ifndef NBTORLOG
#include "btorprintmodel.h"
#endif

#include <math.h>

#define BTOR_SLS_MAXSTEPS_CFACT 100  // TODO best value? used by Z3 (c4)
// TODO best restart scheme? used by Z3
#define BTOR_SLS_MAXSTEPS(i) \
  (BTOR_SLS_MAXSTEPS_CFACT * ((i) &1u ? 1 : 1 << ((i) >> 1)))

#define BTOR_SLS_SCORE_CFACT 0.5      // TODO best value? used by Z3 (c1)
#define BTOR_SLS_SCORE_F_CFACT 0.025  // TODO best value? used by Z3 (c3)
#define BTOR_SLS_SCORE_F_PROB 20      // = 0.05 TODO best value? used by Z3 (sp)
#define BTOR_SLS_SELECT_CFACT 20      // TODO best value? used by Z3 (c2)

static int
hamming_distance (Btor *btor, BitVector *bv1, BitVector *bv2)
{
  assert (bv1);
  assert (bv2);
  assert (bv1->width == bv2->width);
  assert (bv1->len == bv2->len);

  int res;
  BitVector *bv, *bvdec = 0, *zero, *ones, *tmp;

  zero = btor_new_bv (btor->mm, bv1->width);
  ones = btor_not_bv (btor->mm, zero);
  bv   = btor_xor_bv (btor->mm, bv1, bv2);
  for (res = 0; !btor_compare_bv (bv, zero); res++)
  {
    bvdec = btor_add_bv (btor->mm, bv, ones);
    tmp   = bv;
    bv    = btor_and_bv (btor->mm, bv, bvdec);
    btor_free_bv (btor->mm, tmp);
  }
  if (bvdec) btor_free_bv (btor->mm, bvdec);
  btor_free_bv (btor->mm, bv);
  btor_free_bv (btor->mm, ones);
  btor_free_bv (btor->mm, zero);
  return res;
}

// TODO find a better heuristic this might be too expensive
// this is not necessary the actual minimum, but the minimum if you flip
// bits in bv1 s.t. bv1 < bv2 (if bv2 is 0, we need to flip 1 bit in bv2, too)
static int
min_flip (Btor *btor, BitVector *bv1, BitVector *bv2)
{
  assert (bv1);
  assert (bv2);
  assert (bv1->width == bv2->width);
  assert (bv1->len == bv2->len);

  int i, res, b1;
  BitVector *tmp, *zero;

  zero = btor_new_bv (btor->mm, bv2->width);
  tmp  = btor_copy_bv (btor->mm, bv1);
  for (res = 0, i = tmp->width - 1; i >= 0; i--)
  {
    if (!(b1 = btor_get_bit_bv (tmp, i))) continue;
    res += 1;
    btor_set_bit_bv (tmp, i, 0);
    btor_print_bv (tmp);
    btor_print_bv (bv2);
    if (btor_compare_bv (tmp, bv2) < 0) break;
  }
  res = !btor_compare_bv (zero, bv2) ? res + 1 : res;
  btor_free_bv (btor->mm, zero);
  btor_free_bv (btor->mm, tmp);
  return res;
}

// score
//
// bw m == 1:
//   s (e[1], A) = A (e[1])
//
// bw m > 1:
//
//   score (e0[bw] /\ e1[bw], A)    =
//       1/2 * (score (e0[bw], A) + score (e1[bw], A))
//
//   score (-(e0[bw] /\ ... /\ e1[bw]), A) =
//       max (score (-e0[bw], A), score (-e1[bw], A))
//
//   score (e0[bw] = e1[bw], A) =
//       (A (e0) == A (e1))
//	 ? 1.0
//	 : c1 * (1 - (h (A(e0), A(e1)) / bw)
//
//   score (e0[bw] != e1[bw], A) =
//       (A (e0) == A (e1) ? 0.0 : 1.0
//
//   s (e0[bw] < e1[bw], A) =
//       (A (e0) < A (e1))
//	 ? 1.0
//	 : c1 * (1 - (min number of bits to flip s.t. e0[bw] < e1[bw]) / bw)
//
static double
compute_sls_score_node (Btor *btor,
                        BtorPtrHashTable **bv_model,
                        BtorPtrHashTable **fun_model,
                        BtorPtrHashTable *score,
                        BtorNode *exp)
{
  assert (btor);
  assert (bv_model);
  assert (fun_model);
  assert (score);
  assert (check_id_table_aux_mark_unset_dbg (btor));
  assert (exp);

  int i;
  double res, s0, s1;
  BtorNode *cur, *real_cur, *e;
  BitVector *bv0, *bv1;
  BtorPtrHashBucket *b;
  BtorNodePtrStack stack, unmark_stack;
#ifndef NBTORLOG
  char *a0, *a1;
#endif

  res = 0.0;
  assert (BTOR_IS_BV_EQ_NODE (BTOR_REAL_ADDR_NODE (exp))
          || BTOR_IS_ULT_NODE (BTOR_REAL_ADDR_NODE (exp))
          || BTOR_REAL_ADDR_NODE (exp)->len == 1);

  if ((b = btor_find_in_ptr_hash_table (score, exp))) return b->data.asDbl;

  BTOR_INIT_STACK (stack);
  BTOR_INIT_STACK (unmark_stack);

  BTOR_PUSH_STACK (btor->mm, stack, exp);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    if (real_cur->aux_mark == 2 || btor_find_in_ptr_hash_table (score, cur))
      continue;

    if (real_cur->aux_mark == 0)
    {
      real_cur->aux_mark = 1;
      BTOR_PUSH_STACK (btor->mm, stack, cur);
      BTOR_PUSH_STACK (btor->mm, unmark_stack, real_cur);

      if (BTOR_IS_AND_NODE (real_cur) && real_cur->len == 1)
      {
        for (i = 0; i < real_cur->arity; i++)
        {
          e = BTOR_IS_AND_NODE (real_cur) && BTOR_IS_INVERTED_NODE (cur)
                  ? BTOR_INVERT_NODE (real_cur->e[i])
                  : real_cur->e[i];
          BTOR_PUSH_STACK (btor->mm, stack, e);
        }
      }
    }
    else
    {
      assert (real_cur->aux_mark == 1);
      real_cur->aux_mark = 2;

      if (!BTOR_IS_BV_EQ_NODE (real_cur) && !BTOR_IS_ULT_NODE (real_cur)
          && real_cur->len != 1)
        continue;

      BTORLOG ("");
      BTORLOG ("*** compute sls score for: %s(%s)",
               BTOR_IS_INVERTED_NODE (cur) ? "-" : " ",
               node2string (cur));

      if (BTOR_IS_AND_NODE (real_cur))
      {
        assert (real_cur->len == 1);
        if (BTOR_IS_INVERTED_NODE (cur))
        {
          assert (btor_find_in_ptr_hash_table (
              score, BTOR_INVERT_NODE (real_cur->e[0])));
          assert (btor_find_in_ptr_hash_table (
              score, BTOR_INVERT_NODE (real_cur->e[1])));

          s0 = btor_find_in_ptr_hash_table (score,
                                            BTOR_INVERT_NODE (real_cur->e[0]))
                   ->data.asDbl;
          s1 = btor_find_in_ptr_hash_table (score,
                                            BTOR_INVERT_NODE (real_cur->e[1]))
                   ->data.asDbl;
#ifndef NBTORLOG
          if (btor->options.loglevel.val)
          {
            a0 = (char *) btor_get_bv_model_str_aux (
                btor, bv_model, fun_model, BTOR_INVERT_NODE (real_cur->e[0]));
            a1 = (char *) btor_get_bv_model_str_aux (
                btor, bv_model, fun_model, BTOR_INVERT_NODE (real_cur->e[1]));
            BTORLOG ("      assignment e[0]: %s", a0);
            BTORLOG ("      assignment e[1]: %s", a1);
            btor_freestr (btor->mm, a0);
            btor_freestr (btor->mm, a1);
            BTORLOG ("      sls score e[0]: %f", s0);
            BTORLOG ("      sls score e[1]: %f", s1);
          }
#endif
          res = s0 > s1 ? s0 : s1;
        }
        else
        {
          assert (btor_find_in_ptr_hash_table (score, real_cur->e[0]));
          assert (btor_find_in_ptr_hash_table (score, real_cur->e[1]));

          s0 = btor_find_in_ptr_hash_table (score, real_cur->e[0])->data.asDbl;
          s1 =
              btor_find_in_ptr_hash_table (score, (real_cur->e[1]))->data.asDbl;
#ifndef NBTORLOG
          if (btor->options.loglevel.val)
          {
            a0 = (char *) btor_get_bv_model_str_aux (
                btor, bv_model, fun_model, real_cur->e[0]);
            a1 = (char *) btor_get_bv_model_str_aux (
                btor, bv_model, fun_model, real_cur->e[1]);
            BTORLOG ("      assignment e[0]: %s", a0);
            BTORLOG ("      assignment e[1]: %s", a1);
            btor_freestr (btor->mm, a0);
            btor_freestr (btor->mm, a1);
            BTORLOG ("      sls score e[0]: %f", s0);
            BTORLOG ("      sls score e[1]: %f", s1);
          }
#endif
          res = (s0 + s1) / 2;
        }
      }
      else if (BTOR_IS_BV_EQ_NODE (real_cur))
      {
        bv0 = (BitVector *) btor_get_bv_model_aux (
            btor, bv_model, fun_model, real_cur->e[0]);
        bv1 = (BitVector *) btor_get_bv_model_aux (
            btor, bv_model, fun_model, real_cur->e[1]);
#ifndef NBTORLOG
        if (btor->options.loglevel.val)
        {
          a0 = (char *) btor_get_bv_model_str_aux (
              btor, bv_model, fun_model, real_cur->e[0]);
          a1 = (char *) btor_get_bv_model_str_aux (
              btor, bv_model, fun_model, real_cur->e[1]);
          BTORLOG ("      assignment e[0]: %s", a0);
          BTORLOG ("      assignment e[1]: %s", a1);
          btor_freestr (btor->mm, a0);
          btor_freestr (btor->mm, a1);
        }
#endif
        if (BTOR_IS_INVERTED_NODE (cur))
          res = !btor_compare_bv (bv0, bv1) ? 0.0 : 1.0;
        else
          res = !btor_compare_bv (bv0, bv1)
                    ? 1.0
                    : BTOR_SLS_SCORE_CFACT
                          * (1 - hamming_distance (btor, bv0, bv1)) / 2;
      }
      else if (BTOR_IS_ULT_NODE (real_cur))
      {
        bv0 = (BitVector *) btor_get_bv_model_aux (
            btor, bv_model, fun_model, real_cur->e[0]);
        bv1 = (BitVector *) btor_get_bv_model_aux (
            btor, bv_model, fun_model, real_cur->e[1]);
#ifndef NBTORLOG
        if (btor->options.loglevel.val)
        {
          a0 = (char *) btor_get_bv_model_str_aux (
              btor, bv_model, fun_model, real_cur->e[0]);
          a1 = (char *) btor_get_bv_model_str_aux (
              btor, bv_model, fun_model, real_cur->e[1]);
          BTORLOG ("      assignment e[0]: %s", a0);
          BTORLOG ("      assignment e[1]: %s", a1);
          btor_freestr (btor->mm, a0);
          btor_freestr (btor->mm, a1);
        }
#endif
        if (BTOR_IS_INVERTED_NODE (cur))
          res =
              btor_compare_bv (bv0, bv1) >= 0
                  ? 1.0
                  : BTOR_SLS_SCORE_CFACT * (1 - min_flip (btor, bv0, bv1) / 2);
        else
          res =
              btor_compare_bv (bv0, bv1) < 0
                  ? 1.0
                  : BTOR_SLS_SCORE_CFACT * (1 - min_flip (btor, bv0, bv1) / 2);
      }
      else
      {
        assert (real_cur->len == 1);
#ifndef NBTORLOG
        if (btor->options.loglevel.val)
        {
          a0 = (char *) btor_get_bv_model_str_aux (
              btor, bv_model, fun_model, cur);
          BTORLOG ("      assignment : %s", a0);
          btor_freestr (btor->mm, a0);
        }
#endif
        res = ((BitVector *) btor_get_bv_model_aux (
                   btor, bv_model, fun_model, cur))
                  ->bits[0];
      }

      assert (!btor_find_in_ptr_hash_table (score, cur));
      b             = btor_insert_in_ptr_hash_table (score, cur);
      b->data.asDbl = res;

      BTORLOG ("      sls score : %f", res);
    }
  }

  /* cleanup */
  while (!BTOR_EMPTY_STACK (unmark_stack))
    BTOR_POP_STACK (unmark_stack)->aux_mark = 0;
  BTOR_RELEASE_STACK (btor->mm, unmark_stack);
  BTOR_RELEASE_STACK (btor->mm, stack);

  assert (btor_find_in_ptr_hash_table (score, exp));
  assert (res == btor_find_in_ptr_hash_table (score, exp)->data.asDbl);
  return res;
}

static void
compute_sls_scores_aux (Btor *btor,
                        BtorPtrHashTable **bv_model,
                        BtorPtrHashTable **fun_model,
                        BtorPtrHashTable *roots,
                        BtorPtrHashTable *score)
{
  assert (btor);
  assert (bv_model);
  assert (fun_model);
  assert (check_id_table_mark_unset_dbg (btor));
  assert (roots);

  int i;
  BtorNode *cur, *real_cur, *e;
  BtorNodePtrStack stack, unmark_stack;
  BtorHashTableIterator it;

  BTOR_INIT_STACK (stack);
  BTOR_INIT_STACK (unmark_stack);

  /* collect roots */
  init_node_hash_table_iterator (&it, roots);
  while (has_next_node_hash_table_iterator (&it))
    BTOR_PUSH_STACK (btor->mm, stack, next_node_hash_table_iterator (&it));

  /* compute score */
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    if (real_cur->mark == 2 || btor_find_in_ptr_hash_table (score, cur))
      continue;

    if (real_cur->mark == 0)
    {
      real_cur->mark = 1;
      BTOR_PUSH_STACK (btor->mm, stack, cur);
      BTOR_PUSH_STACK (btor->mm, unmark_stack, real_cur);
      for (i = 0; i < real_cur->arity; i++)
      {
        e = BTOR_IS_INVERTED_NODE (cur) ? BTOR_INVERT_NODE (real_cur->e[i])
                                        : real_cur->e[i];
        BTOR_PUSH_STACK (btor->mm, stack, e);
      }
    }
    else
    {
      assert (real_cur->mark == 1);
      real_cur->mark = 2;
      if (!BTOR_IS_BV_EQ_NODE (real_cur) && !BTOR_IS_ULT_NODE (real_cur)
          && real_cur->len != 1)
        continue;
      compute_sls_score_node (btor, bv_model, fun_model, score, cur);
    }
  }

  /* cleanup */
  while (!BTOR_EMPTY_STACK (unmark_stack))
    BTOR_POP_STACK (unmark_stack)->mark = 0;

  BTOR_RELEASE_STACK (btor->mm, stack);
  BTOR_RELEASE_STACK (btor->mm, unmark_stack);
}

static void
compute_sls_scores (Btor *btor,
                    BtorPtrHashTable *roots,
                    BtorPtrHashTable *score)
{
  assert (btor);
  assert (roots);
  assert (score);

  compute_sls_scores_aux (
      btor, &btor->bv_model, &btor->fun_model, roots, score);
}

static double
compute_sls_score_formula (Btor *btor,
                           BtorPtrHashTable *roots,
                           BtorPtrHashTable *score)
{
  assert (btor);
  assert (roots);
  assert (score);

  double res, sc, weight;
  BtorNode *root;
  BtorHashTableIterator it;

  init_node_hash_table_iterator (&it, roots);
  while (has_next_node_hash_table_iterator (&it))
  {
    weight = (double) it.bucket->data.asInt;
    root   = next_node_hash_table_iterator (&it);
    sc     = btor_find_in_ptr_hash_table (score, root)->data.asDbl;
    res += weight * sc;
  }
  return res;
}

static BtorNode *
select_candidate_constraint (Btor *btor, BtorPtrHashTable *roots, int moves)
{
  assert (btor);
  assert (btor->score_sls);
  assert (roots);

  int selected;
  double value, max_value, score;
  BtorNode *res, *cur;
  BtorHashTableIterator it;
  BtorPtrHashBucket *b, *bucket;

  res       = 0;
  max_value = 0.0;
  bucket    = 0;
  init_hash_table_iterator (&it, roots);
  while (has_next_node_hash_table_iterator (&it))
  {
    b   = it.bucket;
    cur = next_node_hash_table_iterator (&it);
    assert (btor_find_in_ptr_hash_table (btor->score_sls, cur));
    score = btor_find_in_ptr_hash_table (btor->score_sls, cur)->data.asDbl;
    if (score >= 1.0) continue;
    if (!res)
    {
      res = cur;
      continue;
    }
    else
    {
      selected = b->data.asInt;
      value    = score + BTOR_SLS_SELECT_CFACT * sqrt (log (selected) / moves);
      if (value > max_value)
      {
        res       = cur;
        max_value = value;
        bucket    = b;
      }
    }
  }
  if (bucket) bucket->data.asInt += 1; /* n times selected */

  BTORLOG ("");
  BTORLOG ("*** select candidate constraint: %s\n", node2string (res));

  return res;
}

static void
select_candidates (Btor *btor, BtorNode *root, BtorNodePtrStack *candidates)
{
  assert (btor);
  assert (check_id_table_mark_unset_dbg (btor));
  assert (root);
  assert (candidates);

  int i;
  BtorNode *cur, *real_cur;
  BtorNodePtrStack stack, unmark_stack;
  BitVector *bv, *bv0, *bv1;

  BTORLOG ("");
  BTORLOG ("*** select candidates");

  BTOR_INIT_STACK (stack);
  BTOR_INIT_STACK (unmark_stack);

  assert (BTOR_COUNT_STACK (*candidates) == 0);
  BTOR_PUSH_STACK (btor->mm, stack, root);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_NODE (cur);
    if (real_cur->mark) continue;
    real_cur->mark = 1;
    BTOR_PUSH_STACK (btor->mm, unmark_stack, real_cur);

    if (BTOR_IS_BV_VAR_NODE (real_cur))
    {
      BTOR_PUSH_STACK (btor->mm, *candidates, real_cur);
      BTORLOG ("  %s\n", node2string (real_cur));
      continue;
    }

    if (btor->options.sls_just.val)
    {
      /* choose candidates from controlling paths
       * (on the Boolean layer) only */
      if (BTOR_IS_AND_NODE (real_cur) && real_cur->len == 1)
      {
        bv  = (BitVector *) btor_get_bv_model (btor, real_cur);
        bv0 = (BitVector *) btor_get_bv_model (btor, real_cur->e[0]);
        bv1 = (BitVector *) btor_get_bv_model (btor, real_cur->e[1]);

        assert (bv->bits[0] == 1 || bv->bits[0] == 0);

        if (bv->bits[0] == 1)
          goto PUSH_CHILDREN;
        else
        {
          if (bv0->bits[0] == 0 && bv1->bits[0])
          {
            if (btor_compare_scores (btor, real_cur->e[0], real_cur->e[1]))
              BTOR_PUSH_STACK (btor->mm, stack, real_cur->e[0]);
            else
              BTOR_PUSH_STACK (btor->mm, stack, real_cur->e[1]);
          }
          else if (bv0->bits[0] == 0)
            BTOR_PUSH_STACK (btor->mm, stack, real_cur->e[0]);
          else
          {
            assert (bv1->bits[0] == 0);
            BTOR_PUSH_STACK (btor->mm, stack, real_cur->e[1]);
          }
        }
      }
      else
        goto PUSH_CHILDREN;
    }
    else
    {
    PUSH_CHILDREN:
      for (i = 0; i < real_cur->arity; i++)
        BTOR_PUSH_STACK (btor->mm, stack, real_cur->e[i]);
    }
  }

  /* cleanup */
  while (!BTOR_EMPTY_STACK (unmark_stack))
    BTOR_POP_STACK (unmark_stack)->mark = 0;

  BTOR_RELEASE_STACK (btor->mm, stack);
  BTOR_RELEASE_STACK (btor->mm, unmark_stack);
}

static void *
mapped_node (BtorMemMgr *mm, const void *map, const void *key)
{
  assert (map);
  assert (key);

  BtorNode *cloned_exp;

  (void) mm;
  (void) map;
  cloned_exp = (BtorNode *) key;
  assert (cloned_exp);
  return cloned_exp;
}

static void
data_as_node_ptr (BtorMemMgr *mm,
                  const void *map,
                  BtorPtrHashData *data,
                  BtorPtrHashData *cloned_data)
{
  assert (mm);
  assert (data);
  assert (cloned_data);

  BtorNode *cloned_exp;

  (void) mm;
  (void) map;
  cloned_exp = (BtorNode *) data->asPtr;
  assert (cloned_exp);
  cloned_data->asPtr = cloned_exp;
}

static void
data_as_double (BtorMemMgr *mm,
                const void *map,
                BtorPtrHashData *data,
                BtorPtrHashData *cloned_data)
{
  assert (mm);
  assert (map);
  assert (data);
  assert (cloned_data);

  (void) mm;
  (void) map;
  cloned_data->asDbl = data->asDbl;
}

static void
reset_cone (Btor *btor,
            BtorNode *exp,
            BtorPtrHashTable *bv_model,
            BtorPtrHashTable *score)
{
  assert (btor);
  assert (check_id_table_mark_unset_dbg (btor));
  assert (exp);
  assert (bv_model);
  assert (score);

  BtorNode *cur;
  BtorNodeIterator nit;
  BtorPtrHashBucket *b;
  BtorNodePtrStack stack, unmark_stack;

  BTOR_INIT_STACK (stack);
  BTOR_INIT_STACK (unmark_stack);

  init_full_parent_iterator (&nit, exp);
  while (has_next_parent_full_parent_iterator (&nit))
    BTOR_PUSH_STACK (btor->mm, stack, next_parent_full_parent_iterator (&nit));

  while (!BTOR_EMPTY_STACK (stack))
  {
    cur = BTOR_POP_STACK (stack);
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (cur->mark) continue;
    cur->mark = 1;

    /* reset previous assignment */
    if ((b = btor_find_in_ptr_hash_table (bv_model, cur)))
    {
      btor_free_bv (btor->mm, b->data.asPtr);
      btor_remove_from_ptr_hash_table (bv_model, cur, 0, 0);
      btor_release_exp (btor, cur);
    }
    if ((b = btor_find_in_ptr_hash_table (bv_model, BTOR_INVERT_NODE (cur))))
    {
      btor_free_bv (btor->mm, b->data.asPtr);
      btor_remove_from_ptr_hash_table (bv_model, cur, 0, 0);
      btor_release_exp (btor, cur);
    }
    /* reset previous score */
    if ((b = btor_find_in_ptr_hash_table (score, cur)))
      btor_remove_from_ptr_hash_table (score, cur, 0, 0);
    if ((b = btor_find_in_ptr_hash_table (score, BTOR_INVERT_NODE (cur))))
      btor_remove_from_ptr_hash_table (score, cur, 0, 0);

    /* push parents */
    init_full_parent_iterator (&nit, cur);
    while (has_next_parent_full_parent_iterator (&nit))
      BTOR_PUSH_STACK (
          btor->mm, stack, next_parent_full_parent_iterator (&nit));
  }

  /* cleanup */
  while (!BTOR_EMPTY_STACK (unmark_stack))
    BTOR_POP_STACK (unmark_stack)->mark = 0;

  BTOR_RELEASE_STACK (btor->mm, stack);
  BTOR_RELEASE_STACK (btor->mm, unmark_stack);
}

static void
update_cone (Btor *btor,
             BtorPtrHashTable **bv_model,
             BtorPtrHashTable **fun_model,
             BtorPtrHashTable *roots,
             BtorNode *exp,
             BitVector *assignment,
             BtorPtrHashTable *score)
{
  assert (btor);
  assert (bv_model);
  assert (*bv_model);
  assert (fun_model);
  assert (*fun_model);
  assert (roots);
  assert (exp);
  assert (assignment);
  assert (score);

  reset_cone (btor, exp, *bv_model, score);
  btor_add_to_bv_model (btor, *bv_model, exp, assignment);
  btor_generate_model_aux (btor, *bv_model, *fun_model, 0);
  compute_sls_scores_aux (btor, bv_model, fun_model, roots, score);
}

static void
move (Btor *btor,
      BtorPtrHashTable *roots,
      BtorNode *assertion,
      BtorNodePtrStack *candidates)
{
  assert (btor);
  assert (roots);
  assert (candidates);

  int i, j, randomized;
  double max_score, sc;
  BtorNode *can, *max_can, *cur;
  BitVector *ass, *neighbor, *max_neighbor;
  BtorPtrHashTable *bv_model, *score_sls;
  BtorPtrHashBucket *b;
  BtorHashTableIterator it;

  /* select move */

  b = btor_find_in_ptr_hash_table (btor->score_sls, assertion);
  assert (b);
  max_score    = compute_sls_score_formula (btor, roots, btor->score);
  max_neighbor = 0;
  max_can      = 0;
  randomized   = 0;

  for (i = 0; i < BTOR_COUNT_STACK (*candidates); i++)
  {
    can = BTOR_PEEK_STACK (*candidates, i);
    assert (BTOR_IS_REGULAR_NODE (can));
    ass = (BitVector *) btor_get_bv_model (btor, can);
    assert (ass);

    /* flip bits */
    for (j = 0; j < ass->width; j++)
    {
      bv_model = btor_clone_ptr_hash_table (
          btor->mm, btor->bv_model, mapped_node, data_as_node_ptr, 0, 0);
      score_sls = btor_clone_ptr_hash_table (
          btor->mm, btor->score_sls, mapped_node, data_as_double, 0, 0);

      neighbor = btor_copy_bv (btor->mm, ass);
      btor_flip_bit_bv (neighbor, j);
      /* we currently support QF_BV only, hence no funs */
      update_cone (
          btor, &bv_model, &btor->fun_model, roots, can, neighbor, score_sls);

      sc = compute_sls_score_formula (btor, roots, score_sls);
      if (sc > max_score)
      {
        max_score = sc;
        if (max_neighbor) btor_free_bv (btor->mm, max_neighbor);
        max_neighbor = neighbor;
        max_can      = can;
      }
      else
        btor_free_bv (btor->mm, neighbor);

      btor_delete_ptr_hash_table (bv_model);
      btor_delete_ptr_hash_table (score_sls);
    }

    /* increment */
    bv_model = btor_clone_ptr_hash_table (
        btor->mm, btor->bv_model, mapped_node, data_as_node_ptr, 0, 0);
    score_sls = btor_clone_ptr_hash_table (
        btor->mm, btor->score_sls, mapped_node, data_as_double, 0, 0);
    neighbor = btor_inc_bv (btor->mm, ass);
    update_cone (
        btor, &bv_model, &btor->fun_model, roots, can, neighbor, score_sls);
    b = btor_find_in_ptr_hash_table (score_sls, can);
    assert (b);
    if (b->data.asDbl > max_score)
    {
      if (max_neighbor) btor_free_bv (btor->mm, max_neighbor);
      max_neighbor = neighbor;
      max_can      = can;
    }
    else
      btor_free_bv (btor->mm, neighbor);
    btor_delete_ptr_hash_table (bv_model);
    btor_delete_ptr_hash_table (score_sls);

    /* decrement */
    bv_model = btor_clone_ptr_hash_table (
        btor->mm, btor->bv_model, mapped_node, data_as_node_ptr, 0, 0);
    score_sls = btor_clone_ptr_hash_table (
        btor->mm, btor->score_sls, mapped_node, data_as_double, 0, 0);
    neighbor = btor_dec_bv (btor->mm, ass);
    update_cone (
        btor, &bv_model, &btor->fun_model, roots, can, neighbor, score_sls);
    b = btor_find_in_ptr_hash_table (score_sls, can);
    assert (b);
    if (b->data.asDbl > max_score)
    {
      if (max_neighbor) btor_free_bv (btor->mm, max_neighbor);
      max_neighbor = neighbor;
      max_can      = can;
    }
    else
      btor_free_bv (btor->mm, neighbor);
    btor_delete_ptr_hash_table (bv_model);
    btor_delete_ptr_hash_table (score_sls);

    /* not */
    bv_model = btor_clone_ptr_hash_table (
        btor->mm, btor->bv_model, mapped_node, data_as_node_ptr, 0, 0);
    score_sls = btor_clone_ptr_hash_table (
        btor->mm, btor->score_sls, mapped_node, data_as_double, 0, 0);
    neighbor = btor_not_bv (btor->mm, ass);
    update_cone (
        btor, &bv_model, &btor->fun_model, roots, can, neighbor, score_sls);
    b = btor_find_in_ptr_hash_table (score_sls, can);
    assert (b);
    if (b->data.asDbl > max_score)
    {
      if (max_neighbor) btor_free_bv (btor->mm, max_neighbor);
      max_neighbor = neighbor;
      max_can      = can;
    }
    else
      btor_free_bv (btor->mm, neighbor);
    btor_delete_ptr_hash_table (bv_model);
    btor_delete_ptr_hash_table (score_sls);
  }

  /* move */
  if (!max_neighbor)
  {
    max_neighbor = btor_new_random_bv (btor->mm, &btor->rng, ass->width);
    randomized   = 1;
  }

  update_cone (btor,
               &btor->bv_model,
               &btor->fun_model,
               roots,
               max_can,
               max_neighbor,
               btor->score);

  /* update assertion weights */
  if (randomized)
  {
    if (!btor_pick_rand_rng (&btor->rng, 0, BTOR_SLS_SCORE_F_PROB))
    {
      /* decrease the weight of all satisfied assertions */
      init_node_hash_table_iterator (&it, roots);
      while (has_next_node_hash_table_iterator (&it))
      {
        b   = it.bucket;
        cur = next_node_hash_table_iterator (&it);
        if (btor_find_in_ptr_hash_table (btor->score, cur)->data.asDbl == 0.0)
          continue;
        if (b->data.asInt > 1) b->data.asInt -= 1;
      }
    }
    else
    {
      /* increase the weight of all unsatisfied assertions */
      init_node_hash_table_iterator (&it, roots);
      while (has_next_node_hash_table_iterator (&it))
      {
        b   = it.bucket;
        cur = next_node_hash_table_iterator (&it);
        if (btor_find_in_ptr_hash_table (btor->score, cur)->data.asDbl == 1.0)
          continue;
        b->data.asInt += 1;
      }
    }
  }

  /* cleanup */
  btor_free_bv (btor->mm, max_neighbor);
}

/* Note: failed assumptions -> no handling necessary, sls only works for SAT */
int
btor_sat_aux_btor_sls (Btor *btor)
{
  assert (btor);
  // TODO we currently support QF_BV only
  assert (btor->lambdas->count == 0 && btor->ufs->count == 0);

  int i, j;
  int sat_result, simp_sat_result;
  int moves;
  BtorPtrHashTable *roots;
  BtorPtrHashBucket *b;
  BtorHashTableIterator it;
  BtorNodePtrStack candidates;

  roots = 0;
  moves = 0;

  BTOR_INIT_STACK (candidates);

  if (btor->inconsistent) goto UNSAT;

  BTOR_MSG (btor->msg, 1, "calling SAT");

  simp_sat_result = btor_simplify (btor);
  btor_update_assumptions (btor);

  if (btor->inconsistent) goto UNSAT;

  // do something

  if (!btor->score_sls)
    btor->score_sls =
        btor_new_ptr_hash_table (btor->mm,
                                 (BtorHashPtr) btor_hash_exp_by_id,
                                 (BtorCmpPtr) btor_compare_exp_by_id);

  /* Generate intial model, all bv vars are initialized with zero.
   * We do not have to consider model_for_all_nodes, but let this be handled
   * by the model generation (if enabled) after SAT has been determined. */
  btor_generate_model (btor, 0);

  /* collect roots */
  roots = btor_new_ptr_hash_table (btor->mm,
                                   (BtorHashPtr) btor_hash_exp_by_id,
                                   (BtorCmpPtr) btor_compare_exp_by_id);
  assert (btor->synthesized_constraints->count == 0);
  init_node_hash_table_iterator (&it, btor->unsynthesized_constraints);
  queue_node_hash_table_iterator (&it, btor->assumptions);
  while (has_next_node_hash_table_iterator (&it))
  {
    b             = btor_insert_in_ptr_hash_table (roots,
                                       next_node_hash_table_iterator (&it));
    b->data.asInt = 1; /* initial assertion weight */
  }

  // TODO insert infinite loop here
  i = 1;

  /* compute sls score */
  compute_sls_scores (btor, roots, btor->score_sls);

  /* compute justification score for candidate selection */
  if (btor->options.sls_just.val)
  {
    btor->options.just_heuristic.val = BTOR_JUST_HEUR_BRANCH_MIN_DEP;
    btor_compute_scores (btor);
  }

  select_candidates (
      btor, select_candidate_constraint (btor, roots, moves), &candidates);

  // TODO move selection

  // for (j = 0; j < BTOR_SLS_MAXSTEPS (i); j++)
  //  {
  //    // select candidate
  //    // find best move
  //    // if move update
  //    // else randomize
  //  }

UNSAT:
  sat_result = BTOR_UNSAT;
  goto DONE;

DONE:
  btor->last_sat_result = sat_result;
  /* cleanup */
  BTOR_RELEASE_STACK (btor->mm, candidates);
  if (roots) btor_delete_ptr_hash_table (roots);
  return sat_result;
}
