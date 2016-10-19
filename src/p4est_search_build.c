/*
  This file is part of p4est.
  p4est is a C library to manage a collection (a forest) of multiple
  connected adaptive quadtrees or octrees in parallel.

  Copyright (C) 2010 The University of Texas System
  Written by Carsten Burstedde, Lucas C. Wilcox, and Tobin Isaac

  p4est is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  p4est is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with p4est; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#ifndef P4_TO_P8
#include <p4est_algorithms.h>
#include <p4est_bits.h>
#include <p4est_communication.h>
#include <p4est_search_build.h>
#else
#include <p8est_algorithms.h>
#include <p8est_bits.h>
#include <p8est_communication.h>
#include <p8est_search_build.h>
#endif

/** Context object for building a new p4est from search callbacks.
 */
struct p4est_search_build
{
  p4est_t            *p4est;    /**< New forest being built. */
  p4est_init_t        init_fn;  /**< Passed to quadrant completion. */
  p4est_init_t        add_init_fn;      /**< Used for added quadrants. */

  /* Context for the tree walk */
  int                 cur_maxlevel;     /**< Current tree's maxlevel on input. */
  p4est_topidx_t      cur_tree;         /**< Current tree under examination. */
  p4est_tree_t       *tree;             /**< Pointer to current tree. */
  p4est_quadrant_t    prev;             /**< Previously added in this tree.
                                             If there is none yet, we set
                                             its level to -1. */
  sc_array_t         *tquadrants;       /**< Points to the tree's quadrants. */
};

static void
p4est_search_build_begin_tree (p4est_search_build_t * build,
                               p4est_topidx_t which_tree,
                               p4est_locidx_t quadrants_offset)
{
  p4est_t            *p4est;

  /* check sanity of call */
  P4EST_ASSERT (build != NULL);
  P4EST_ASSERT (build->p4est != NULL);

  /* check sanity of build object */
  p4est = build->p4est;
  P4EST_ASSERT (p4est->first_local_tree <= which_tree &&
                which_tree <= p4est->last_local_tree);
  P4EST_ASSERT (0 <= quadrants_offset);

  /* initialize context for a new tree */
  build->cur_tree = which_tree;
  build->tree = p4est_tree_array_index (p4est->trees, build->cur_tree);
  build->tree->quadrants_offset = quadrants_offset;
  build->tquadrants = &build->tree->quadrants;
  build->prev.level = -1;
  P4EST_ASSERT (build->tquadrants->elem_size == sizeof (p4est_quadrant_t));
  P4EST_ASSERT (build->tquadrants->elem_count == 0);

  /* we had remembered the original tree's maxlevel */
  build->cur_maxlevel = build->tree->maxlevel;
  build->tree->maxlevel = 0;
}

p4est_search_build_t *
p4est_search_build_new (p4est_t * from, size_t data_size,
                        p4est_init_t init_fn, void *user_pointer)
{
  int                 ell;
  p4est_topidx_t      jt, num_trees;
  p4est_t            *p4est;
  p4est_tree_t       *ftree, *ptree;
  p4est_search_build_t *build;

  P4EST_ASSERT (p4est_is_valid (from));

  /* create a new p4est structure to be populated */
  build = P4EST_ALLOC (p4est_search_build_t, 1);
  build->p4est = p4est = P4EST_ALLOC (p4est_t, 1);
  memcpy (p4est, from, sizeof (p4est_t));
  num_trees = from->connectivity->num_trees;

  /* remove anything that we will not use from the template forest */
  p4est->mpicomm_owned = 0;
  p4est->data_size = data_size;
  p4est->user_pointer = user_pointer;
  p4est->revision = 0;
  p4est->local_num_quadrants = 0;
  p4est->global_num_quadrants = 0;
  p4est->global_first_quadrant = NULL;
  p4est->global_first_position = NULL;
  p4est->trees = NULL;
  p4est->user_data_pool = NULL;
  p4est->quadrant_pool = NULL;
  p4est->inspect = NULL;

  /* start populating missing members */
  p4est->global_first_quadrant =
    P4EST_ALLOC (p4est_gloidx_t, p4est->mpisize + 1);
  p4est->global_first_position =
    P4EST_ALLOC (p4est_quadrant_t, p4est->mpisize + 1);
  memcpy (p4est->global_first_position, from->global_first_position,
          (p4est->mpisize + 1) * sizeof (p4est_quadrant_t));
  p4est->trees = sc_array_new_size (sizeof (p4est_tree_t), num_trees);
  for (jt = 0; jt < num_trees; ++jt) {
    ftree = p4est_tree_array_index (from->trees, jt);
    ptree = p4est_tree_array_index (p4est->trees, jt);
    sc_array_init (&ptree->quadrants, sizeof (p4est_quadrant_t));
    ptree->first_desc = ftree->first_desc;
    ptree->last_desc = ftree->last_desc;
    ptree->quadrants_offset = 0;
    memset (ptree->quadrants_per_level, 0,
            (P4EST_QMAXLEVEL + 1) * sizeof (p4est_locidx_t));
    for (ell = P4EST_QMAXLEVEL + 1; ell <= P4EST_MAXLEVEL; ++ell) {
      ptree->quadrants_per_level[ell] = -1;
    }

    /* this is tempmorary just to pass the information along */
    ptree->maxlevel = ftree->maxlevel;
  }
  if (p4est->data_size > 0) {
    p4est->user_data_pool = sc_mempool_new (p4est->data_size);
  }
  p4est->quadrant_pool = sc_mempool_new (sizeof (p4est_quadrant_t));

  /* initialize context structure */
  build->init_fn = init_fn;
  build->add_init_fn = init_fn;
  if (p4est->first_local_tree >= 0) {
    p4est_search_build_begin_tree (build, p4est->first_local_tree, 0);
  }

  /*
   * what remains to be filled:
   * local_num_quadrants
   * local trees:
   *   quadrants
   *   quadrants_offset
   *   quadrants_per_level
   *   maxlevel
   * global_first_quadrant, global_num_quadrants:
   *   filled after local_num_quadrants has been set correctly
   */

  return build;
}

void
p4est_search_build_init_add (p4est_search_build_t * build,
                             p4est_init_t add_init_fn)
{
  P4EST_ASSERT (build != NULL);
  P4EST_ASSERT (build->p4est != NULL);

  build->add_init_fn = add_init_fn;
}

static              p4est_locidx_t
p4est_search_build_end_tree (p4est_search_build_t * build)
{
#ifdef P4EST_ENABLE_DEBUG
  int                 ell;
#endif
  p4est_t            *p4est;
  p4est_quadrant_t   *q;

  /* check sanity of call */
  P4EST_ASSERT (build != NULL);
  P4EST_ASSERT (build->p4est != NULL);
  P4EST_ASSERT (build->tree != NULL);

  p4est = build->p4est;
  P4EST_ASSERT (build->tree->quadrants_per_level[P4EST_MAXLEVEL] == -1);
#ifdef P4EST_ENABLE_DEBUG
  for (ell = P4EST_QMAXLEVEL; ell > 0; --ell) {
    if (build->tree->quadrants_per_level[ell] > 0) {
      P4EST_ASSERT (build->tree->maxlevel == ell);
      break;
    }
  }
#endif
  P4EST_ASSERT (build->tquadrants->elem_size == sizeof (p4est_quadrant_t));

  /* do the heavy lifting: complete this tree as coarsely as possible */
  if (build->tquadrants->elem_count == 0) {
#ifdef P4EST_ENABLE_DEBUG
    int                 maxl;
    int                 onetree;
    p4est_quadrant_t    q1, q2;
    p4est_quadrant_t    cand, desc;
#endif
    p4est_quadrant_t    a;
    const p4est_quadrant_t *qfp, *qlp;

    P4EST_ASSERT (build->tree->maxlevel == 0);
    P4EST_ASSERT (build->prev.level == -1);

#ifdef P4EST_ENABLE_DEBUG
    /* this is older code now used for checking consistency */
    maxl = build->cur_maxlevel;
    q1 = build->tree->first_desc;
    q2 = build->tree->last_desc;
    if (maxl < P4EST_QMAXLEVEL) {
      p4est_quadrant_ancestor (&q1, maxl, &q1);
      p4est_quadrant_ancestor (&q2, maxl, &q2);
    }

    /* we enlarge the first quadrant as much as possible */
    while (q1.level > 0) {
      if (p4est_quadrant_child_id (&q1) != 0) {
        /* only child zero may be coarsened without spreading forward */
        break;
      }
      p4est_quadrant_parent (&q1, &cand);
      p4est_quadrant_last_descendant (&cand, &desc, P4EST_QMAXLEVEL);
      if (p4est_quadrant_compare (&desc, &build->tree->last_desc) > 0) {
        /* we would grow q1 so much that it would spread beyond the partition */
        break;
      }
      q1 = cand;
    }

    /* we enlarge the last quadrant as much as possible */
    while (q2.level > 0) {
      if (p4est_quadrant_child_id (&q2) != P4EST_CHILDREN - 1) {
        /* only last child may be coarsened without spreading backward */
        break;
      }
      p4est_quadrant_parent (&q2, &cand);
      p4est_quadrant_first_descendant (&cand, &desc, P4EST_QMAXLEVEL);
      if (p4est_quadrant_compare (&desc, &build->tree->first_desc) < 0) {
        /* we would grow q2 so much that it would spread beyond the partition */
        break;
      }
      q2 = cand;
    }
    P4EST_ASSERT (p4est_quadrant_compare (&q1, &q2) <= 0);

    if (p4est_quadrant_is_equal (&q1, &q2)) {
      /* special case: we create a subtree that is one quadrant q1 */
      P4EST_ASSERT (p4est_quadrant_is_first_last (&build->tree->first_desc,
                                                  &build->tree->last_desc,
                                                  &q1));
      onetree = 1;
    }
    else {
      /* tree boundaries are q1 and q2 */
      P4EST_ASSERT (!p4est_quadrant_overlaps (&q1, &q2));
      onetree = 0;
    }
#endif

    /* work with the recently added quadrant_enlarge functions */
    qfp = &build->tree->first_desc;
    qlp = &build->tree->last_desc;
    p4est_nearest_common_ancestor (qfp, qlp, &a);
    if (p4est_quadrant_is_first_last (qfp, qlp, &a)) {
      /* the tree consists of only a */

      /* double checking */
      P4EST_ASSERT (onetree);
      P4EST_ASSERT (p4est_quadrant_is_equal (&a, &q1));

      /* stuff the quadrant into the tree data structure */
      q = (p4est_quadrant_t *) sc_array_push (build->tquadrants);
      *q = a;
      p4est_quadrant_init_data (p4est, build->cur_tree, q, build->init_fn);
      build->tree->quadrants_per_level[q->level] = 1;
      build->tree->maxlevel = (int) q->level;
    }
    else {
      int                 idf, idl;
      p4est_quadrant_t    cf, cl;
      p4est_quadrant_t    qf, ql;

      /* find the children of a containing qf, ql */
      idf = p4est_quadrant_ancestor_id (qfp, a.level + 1);
      idl = p4est_quadrant_ancestor_id (qlp, a.level + 1);
      P4EST_ASSERT (idf < idl);
      p4est_quadrant_child (&a, &cf, idf);
      p4est_quadrant_child (&a, &cl, idl);

      /* we modify these quadrants */
      qf = *qfp;
      ql = *qlp;

      /* the tree must be filled up between its first and last end */
      P4EST_ASSERT (!p4est_quadrant_overlaps (&qf, &ql));
      p4est_quadrant_enlarge_first (&cf, &qf);
      p4est_quadrant_enlarge_last (&cl, &ql);
      P4EST_ASSERT (!p4est_quadrant_overlaps (&qf, &ql));

      /* double checking */
      P4EST_ASSERT (!onetree);
      P4EST_ASSERT (p4est_quadrant_is_equal (&qf, &q1));
      P4EST_ASSERT (p4est_quadrant_is_equal (&ql, &q2));

      /* do the filling up */
      p4est_complete_region (p4est, &qf, 1, &ql, 1,
                             build->tree, build->cur_tree, build->init_fn);
    }
  }
  else {
    p4est_complete_subtree (p4est, build->cur_tree, build->init_fn);
    P4EST_ASSERT (&build->tree->quadrants == build->tquadrants);
  }

  /* the tree is now complete, we can report its number of quadrants */
  return
    build->tree->quadrants_offset +
    (p4est_locidx_t) build->tquadrants->elem_count;
}

int
p4est_search_build_add (p4est_search_build_t * build,
                        p4est_topidx_t which_tree,
                        p4est_quadrant_t * quadrant)
{
  p4est_t            *p4est;
  p4est_quadrant_t   *q;
  p4est_locidx_t      quadrants_offset;

  /* check sanity of call */
  P4EST_ASSERT (build != NULL);
  P4EST_ASSERT (build->p4est != NULL);

  /* check sanity of build object */
  p4est = build->p4est;
  P4EST_ASSERT (p4est->first_local_tree <= which_tree &&
                which_tree <= p4est->last_local_tree);
  P4EST_ASSERT (p4est->first_local_tree <= build->cur_tree &&
                build->cur_tree <= p4est->last_local_tree);
  P4EST_ASSERT (which_tree >= build->cur_tree);

  /* finish up previous tree if we are entering a new one */
  while (which_tree > build->cur_tree) {
    quadrants_offset = p4est_search_build_end_tree (build);
    p4est_search_build_begin_tree (build, build->cur_tree + 1,
                                   quadrants_offset);
  }

  /* we require that the quadrant fits into the tree's range */
  /* thus it will not be legal to pass in too coarse search quadrants */
  /* it is safest to call this function only on leaves */
  P4EST_ASSERT (p4est_quadrant_in_range (&build->tree->first_desc,
                                         &build->tree->last_desc, quadrant));

  /* If there was a previous quadrant added, we may be identical
   * in which case this function does nothing and returns false.
   * If not identical, this quadrant must be non-overlapping and bigger.
   */
  P4EST_ASSERT ((build->prev.level == -1) ==
                (build->tquadrants->elem_count == 0));
  if (build->prev.level >= 0) {
    P4EST_ASSERT (p4est_quadrant_in_range (&build->tree->first_desc,
                                           &build->tree->last_desc,
                                           &build->prev));
    P4EST_ASSERT (p4est_quadrant_compare (&build->prev, quadrant) <= 0);
    if (p4est_quadrant_is_equal (&build->prev, quadrant)) {
      /* special exception */
      return 0;
    }
    P4EST_ASSERT (!p4est_quadrant_is_ancestor (&build->prev, quadrant));
  }

  /* insert this quadrant as a new leaf of the tree */
  P4EST_ASSERT (build->tquadrants->elem_size == sizeof (p4est_quadrant_t));
  q = (p4est_quadrant_t *) sc_array_push (build->tquadrants);
  *q = *quadrant;
  p4est_quadrant_init_data (p4est, which_tree, q, build->add_init_fn);
  ++build->tree->quadrants_per_level[q->level];
  if (q->level > build->tree->maxlevel) {
    build->tree->maxlevel = q->level;
  }

  /* record this quadrant for context checking */
  build->prev = *quadrant;

  /* return true for a newly added quadrant */
  return 1;
}

p4est_t            *
p4est_search_build_complete (p4est_search_build_t * build)
{
  p4est_topidx_t      jt, last_local_tree, num_trees;
  p4est_locidx_t      quadrants_offset;
  p4est_t            *p4est;
  p4est_tree_t       *ptree;

  P4EST_ASSERT (build != NULL);
  P4EST_ASSERT (build->p4est != NULL);

  /* wrap up constructing the missing bits and pieces */
  p4est = build->p4est;
  last_local_tree = p4est->last_local_tree;

  if (p4est->first_local_tree <= last_local_tree) {
    /* finish last tree of the iteration */
    P4EST_ASSERT (build->cur_tree <= last_local_tree);
    while (last_local_tree > build->cur_tree) {
      quadrants_offset = p4est_search_build_end_tree (build);
      p4est_search_build_begin_tree (build, build->cur_tree + 1,
                                     quadrants_offset);
    }
    p4est->local_num_quadrants = p4est_search_build_end_tree (build);

    /* fix quadrants_offset in empty trees > last_local_tree */
    num_trees = p4est->connectivity->num_trees;
    P4EST_ASSERT (0 <= last_local_tree && last_local_tree < num_trees);
    P4EST_ASSERT (p4est->local_num_quadrants > 0);
    for (jt = last_local_tree + 1; jt < num_trees; ++jt) {
      ptree = p4est_tree_array_index (p4est->trees, jt);
      ptree->quadrants_offset = p4est->local_num_quadrants;
    }
  }
  else {
    /* this is an empty processor */
    P4EST_ASSERT (p4est->first_local_tree == -1);
    P4EST_ASSERT (p4est->last_local_tree == -2);
    P4EST_ASSERT (p4est->local_num_quadrants == 0);
  }

  /* we do no longer require the build context */
  P4EST_FREE (build);

  /* fix global cumulative quadrant count per processor */
  p4est_comm_count_quadrants (p4est);
  P4EST_ASSERT (p4est_is_valid (p4est));

  return p4est;
}
