/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2007-2009 Robert Daniel Brummayer.
 *  Copyright (C) 2007-2012 Armin Biere.
 *  Copyright (C) 2012-2015 Mathias Preiner.
 *  Copyright (C) 2014-2016 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#ifndef BTORITER_H_INCLUDED
#define BTORITER_H_INCLUDED

#include "btorcore.h"
#include "utils/btorhashptr.h"
#include "utils/btorhashptr2.h"
#include "utils/btornodemap.h"

#include <stdbool.h>

/*------------------------------------------------------------------------*/
/* node iterators                                                         */
/*------------------------------------------------------------------------*/

typedef struct BtorNodeIterator
{
  const Btor *btor; /* required for unique table iterator */
  int pos;          /* required for unique table iterator */
#ifndef NDEBUG
  int num_elements;
#endif
  BtorNode *cur;
} BtorNodeIterator;

#define BTOR_NEXT_PARENT(exp) \
  (BTOR_REAL_ADDR_NODE (exp)->next_parent[BTOR_GET_TAG_NODE (exp)])

#define BTOR_PREV_PARENT(exp) \
  (BTOR_REAL_ADDR_NODE (exp)->prev_parent[BTOR_GET_TAG_NODE (exp)])

void btor_init_apply_parent_iterator (BtorNodeIterator *it,
                                      const BtorNode *exp);
bool btor_has_next_apply_parent_iterator (const BtorNodeIterator *it);
BtorNode *btor_next_apply_parent_iterator (BtorNodeIterator *it);

void btor_init_parent_iterator (BtorNodeIterator *it, const BtorNode *exp);
bool btor_has_next_parent_iterator (const BtorNodeIterator *it);
BtorNode *btor_next_parent_iterator (BtorNodeIterator *it);

void btor_init_lambda_iterator (BtorNodeIterator *it, BtorNode *exp);
bool btor_has_next_lambda_iterator (const BtorNodeIterator *it);
BtorNode *btor_next_lambda_iterator (BtorNodeIterator *it);

void btor_init_param_iterator (BtorNodeIterator *it, BtorNode *exp);
bool btor_has_next_param_iterator (const BtorNodeIterator *it);
BtorNode *btor_next_param_iterator (BtorNodeIterator *it);

void btor_init_unique_table_iterator (BtorNodeIterator *it, const Btor *exp);
bool btor_has_next_unique_table_iterator (const BtorNodeIterator *it);
BtorNode *btor_next_unique_table_iterator (BtorNodeIterator *it);

/*------------------------------------------------------------------------*/

typedef struct BtorArgsIterator
{
  int pos;
  BtorNode *cur;
  const BtorNode *exp;
} BtorArgsIterator;

void btor_init_args_iterator (BtorArgsIterator *it, const BtorNode *exp);
bool btor_has_next_args_iterator (const BtorArgsIterator *it);
BtorNode *btor_next_args_iterator (BtorArgsIterator *it);

/*------------------------------------------------------------------------*/
/* hash table iterators		                                          */
/*------------------------------------------------------------------------*/

#define BTOR_HASH_TABLE_ITERATOR_STACK_SIZE 8

typedef struct BtorHashTableIterator
{
  BtorPtrHashBucket *bucket;
  void *cur;
  bool reversed;
  uint8_t num_queued;
  uint8_t pos;
  const BtorPtrHashTable *stack[BTOR_HASH_TABLE_ITERATOR_STACK_SIZE];

  // TODO: this replaces the above as soon as hashptrtable is replaced
  //       with the new implementation
  //  void *cur;
  size_t cur_pos;
  const BtorPtrHashTable2 *cur_table;

  /* queue fields */
  //  bool reversed;
  //  uint8_t num_queued;
  uint8_t queue_pos;
  const BtorPtrHashTable2 *stack2[BTOR_HASH_TABLE_ITERATOR_STACK_SIZE];
} BtorHashTableIterator;

void btor_init_hash_table_iterator (BtorHashTableIterator *it,
                                    const BtorPtrHashTable *t);
void btor_init_reversed_hash_table_iterator (BtorHashTableIterator *it,
                                             const BtorPtrHashTable *t);
void btor_queue_hash_table_iterator (BtorHashTableIterator *it,
                                     const BtorPtrHashTable *t);
bool btor_has_next_hash_table_iterator (const BtorHashTableIterator *it);
void *btor_next_hash_table_iterator (BtorHashTableIterator *it);
BtorHashTableData *btor_next_data_hash_table_iterator (
    BtorHashTableIterator *it);

void btor_init_node_hash_table_iterator (BtorHashTableIterator *it,
                                         const BtorPtrHashTable *t);
void btor_init_reversed_node_hash_table_iterator (BtorHashTableIterator *it,
                                                  const BtorPtrHashTable *t);
void btor_queue_node_hash_table_iterator (BtorHashTableIterator *it,
                                          const BtorPtrHashTable *t);
bool btor_has_next_node_hash_table_iterator (const BtorHashTableIterator *it);
BtorNode *btor_next_node_hash_table_iterator (BtorHashTableIterator *it);
BtorHashTableData *btor_next_data_node_hash_table_iterator (
    BtorHashTableIterator *it);

struct BtorHashTableIterator2
{
  void *cur;
  size_t cur_pos;
  const BtorPtrHashTable2 *cur_table;

  /* queue fields */
  bool reversed;
  uint8_t num_queued;
  uint8_t queue_pos;
  const BtorPtrHashTable2 *stack[BTOR_HASH_TABLE_ITERATOR_STACK_SIZE];
};

typedef struct BtorHashTableIterator2 BtorHashTableIterator2;

void btor_init_hash_table_iterator2 (BtorHashTableIterator *it,
                                     const BtorPtrHashTable2 *t);
void btor_init_reversed_hash_table_iterator2 (BtorHashTableIterator *it,
                                              const BtorPtrHashTable2 *t);
void btor_queue_hash_table_iterator2 (BtorHashTableIterator *it,
                                      const BtorPtrHashTable2 *t);
bool btor_has_next_hash_table_iterator2 (const BtorHashTableIterator *it);
void *btor_next_hash_table_iterator2 (BtorHashTableIterator *it);
BtorHashTableData *btor_next_data_hash_table_iterator2 (
    BtorHashTableIterator *it);

void btor_init_node_hash_table_iterator2 (BtorHashTableIterator *it,
                                          const BtorPtrHashTable2 *t);
void btor_init_reversed_node_hash_table_iterator2 (BtorHashTableIterator *it,
                                                   const BtorPtrHashTable2 *t);
void btor_queue_node_hash_table_iterator2 (BtorHashTableIterator *it,
                                           const BtorPtrHashTable2 *t);
bool btor_has_next_node_hash_table_iterator2 (const BtorHashTableIterator *it);
BtorNode *btor_next_node_hash_table_iterator2 (BtorHashTableIterator *it);
BtorHashTableData *btor_next_data_node_hash_table_iterator2 (
    BtorHashTableIterator *it);

/*------------------------------------------------------------------------*/
/* map iterators						          */
/*------------------------------------------------------------------------*/

typedef struct BtorNodeMapIterator
{
  BtorHashTableIterator it;
} BtorNodeMapIterator;

void btor_init_node_map_iterator (BtorNodeMapIterator *it,
                                  const BtorNodeMap *map);
void btor_init_reversed_node_map_iterator (BtorNodeMapIterator *it,
                                           const BtorNodeMap *map);
void btor_queue_node_map_iterator (BtorNodeMapIterator *it,
                                   const BtorNodeMap *map);
bool btor_has_next_node_map_iterator (const BtorNodeMapIterator *it);
BtorNode *btor_next_node_map_iterator (BtorNodeMapIterator *it);
BtorHashTableData *btor_next_data_node_map_iterator (BtorNodeMapIterator *it);

/*------------------------------------------------------------------------*/
#endif
