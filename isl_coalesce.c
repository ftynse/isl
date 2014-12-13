/*
 * Copyright 2008-2009 Katholieke Universiteit Leuven
 * Copyright 2010      INRIA Saclay
 * Copyright 2012-2013 Ecole Normale Superieure
 * Copyright 2014      INRIA Rocquencourt
 *
 * Use of this software is governed by the MIT license
 *
 * Written by Sven Verdoolaege, K.U.Leuven, Departement
 * Computerwetenschappen, Celestijnenlaan 200A, B-3001 Leuven, Belgium
 * and INRIA Saclay - Ile-de-France, Parc Club Orsay Universite,
 * ZAC des vignes, 4 rue Jacques Monod, 91893 Orsay, France 
 * and Ecole Normale Superieure, 45 rue d’Ulm, 75230 Paris, France
 * and Inria Paris - Rocquencourt, Domaine de Voluceau - Rocquencourt,
 * B.P. 105 - 78153 Le Chesnay, France
 */

#include "isl_map_private.h"
#include <isl_seq.h>
#include <isl/options.h>
#include "isl_tab.h"
#include <isl_mat_private.h>
#include <isl_local_space_private.h>
#include <isl_vec_private.h>

#define STATUS_ERROR		-1
#define STATUS_REDUNDANT	 1
#define STATUS_VALID	 	 2
#define STATUS_SEPARATE	 	 3
#define STATUS_CUT	 	 4
#define STATUS_ADJ_EQ	 	 5
#define STATUS_ADJ_INEQ	 	 6

static int status_in(isl_int *ineq, struct isl_tab *tab)
{
	enum isl_ineq_type type = isl_tab_ineq_type(tab, ineq);
	switch (type) {
	default:
	case isl_ineq_error:		return STATUS_ERROR;
	case isl_ineq_redundant:	return STATUS_VALID;
	case isl_ineq_separate:		return STATUS_SEPARATE;
	case isl_ineq_cut:		return STATUS_CUT;
	case isl_ineq_adj_eq:		return STATUS_ADJ_EQ;
	case isl_ineq_adj_ineq:		return STATUS_ADJ_INEQ;
	}
}

/* Compute the position of the equalities of basic map "bmap_i"
 * with respect to the basic map represented by "tab_j".
 * The resulting array has twice as many entries as the number
 * of equalities corresponding to the two inequalties to which
 * each equality corresponds.
 */
static int *eq_status_in(__isl_keep isl_basic_map *bmap_i,
	struct isl_tab *tab_j)
{
	int k, l;
	int *eq = isl_calloc_array(bmap_i->ctx, int, 2 * bmap_i->n_eq);
	unsigned dim;

	if (!eq)
		return NULL;

	dim = isl_basic_map_total_dim(bmap_i);
	for (k = 0; k < bmap_i->n_eq; ++k) {
		for (l = 0; l < 2; ++l) {
			isl_seq_neg(bmap_i->eq[k], bmap_i->eq[k], 1+dim);
			eq[2 * k + l] = status_in(bmap_i->eq[k], tab_j);
			if (eq[2 * k + l] == STATUS_ERROR)
				goto error;
		}
		if (eq[2 * k] == STATUS_SEPARATE ||
		    eq[2 * k + 1] == STATUS_SEPARATE)
			break;
	}

	return eq;
error:
	free(eq);
	return NULL;
}

/* Compute the position of the inequalities of basic map "bmap_i"
 * (also represented by "tab_i", if not NULL) with respect to the basic map
 * represented by "tab_j".
 */
static int *ineq_status_in(__isl_keep isl_basic_map *bmap_i,
	struct isl_tab *tab_i, struct isl_tab *tab_j)
{
	int k;
	unsigned n_eq = bmap_i->n_eq;
	int *ineq = isl_calloc_array(bmap_i->ctx, int, bmap_i->n_ineq);

	if (!ineq)
		return NULL;

	for (k = 0; k < bmap_i->n_ineq; ++k) {
		if (tab_i && isl_tab_is_redundant(tab_i, n_eq + k)) {
			ineq[k] = STATUS_REDUNDANT;
			continue;
		}
		ineq[k] = status_in(bmap_i->ineq[k], tab_j);
		if (ineq[k] == STATUS_ERROR)
			goto error;
		if (ineq[k] == STATUS_SEPARATE)
			break;
	}

	return ineq;
error:
	free(ineq);
	return NULL;
}

static int any(int *con, unsigned len, int status)
{
	int i;

	for (i = 0; i < len ; ++i)
		if (con[i] == status)
			return 1;
	return 0;
}

static int count(int *con, unsigned len, int status)
{
	int i;
	int c = 0;

	for (i = 0; i < len ; ++i)
		if (con[i] == status)
			c++;
	return c;
}

static int all(int *con, unsigned len, int status)
{
	int i;

	for (i = 0; i < len ; ++i) {
		if (con[i] == STATUS_REDUNDANT)
			continue;
		if (con[i] != status)
			return 0;
	}
	return 1;
}

/* Internal information associated to a basic map in a map
 * that is to be coalesced by isl_map_coalesce.
 *
 * "bmap" is the basic map itself (or NULL if "removed" is set)
 * "tab" is the corresponding tableau (or NULL if "removed" is set)
 * "removed" is set if this basic map has been removed from the map
 */
struct isl_coalesce_info {
	isl_basic_map *bmap;
	struct isl_tab *tab;
	int removed;
};

/* Free all the allocated memory in an array
 * of "n" isl_coalesce_info elements.
 */
static void clear_coalesce_info(int n, struct isl_coalesce_info *info)
{
	int i;

	if (!info)
		return;

	for (i = 0; i < n; ++i) {
		isl_basic_map_free(info[i].bmap);
		isl_tab_free(info[i].tab);
	}

	free(info);
}

/* Drop the basic map represented by "info".
 * That is, clear the memory associated to the entry and
 * mark it as having been removed.
 */
static void drop(struct isl_coalesce_info *info)
{
	info->bmap = isl_basic_map_free(info->bmap);
	isl_tab_free(info->tab);
	info->tab = NULL;
	info->removed = 1;
}

/* Exchange the information in "info1" with that in "info2".
 */
static void exchange(struct isl_coalesce_info *info1,
	struct isl_coalesce_info *info2)
{
	struct isl_coalesce_info info;

	info = *info1;
	*info1 = *info2;
	*info2 = info;
}

/* This type represents the kind of change that has been performed
 * while trying to coalesce two basic maps.
 *
 * isl_change_none: nothing was changed
 * isl_change_drop_first: the first basic map was removed
 * isl_change_drop_second: the second basic map was removed
 * isl_change_fuse: the two basic maps were replaced by a new basic map.
 */
enum isl_change {
	isl_change_error = -1,
	isl_change_none = 0,
	isl_change_drop_first,
	isl_change_drop_second,
	isl_change_fuse,
};

/* Replace the pair of basic maps i and j by the basic map bounded
 * by the valid constraints in both basic maps and the constraints
 * in extra (if not NULL).
 * Place the fused basic map in the position that is the smallest of i and j.
 *
 * If "detect_equalities" is set, then look for equalities encoded
 * as pairs of inequalities.
 */
static enum isl_change fuse(int i, int j, struct isl_coalesce_info *info,
	int *eq_i, int *ineq_i, int *eq_j, int *ineq_j,
	__isl_keep isl_mat *extra, int detect_equalities)
{
	int k, l;
	struct isl_basic_map *fused = NULL;
	struct isl_tab *fused_tab = NULL;
	unsigned total = isl_basic_map_total_dim(info[i].bmap);
	unsigned extra_rows = extra ? extra->n_row : 0;

	if (j < i)
		return fuse(j, i, info, eq_j, ineq_j, eq_i, ineq_i, extra,
				detect_equalities);

	fused = isl_basic_map_alloc_space(isl_space_copy(info[i].bmap->dim),
		    info[i].bmap->n_div,
		    info[i].bmap->n_eq + info[j].bmap->n_eq,
		    info[i].bmap->n_ineq + info[j].bmap->n_ineq + extra_rows);
	if (!fused)
		goto error;

	for (k = 0; k < info[i].bmap->n_eq; ++k) {
		if (eq_i && (eq_i[2 * k] != STATUS_VALID ||
			     eq_i[2 * k + 1] != STATUS_VALID))
			continue;
		l = isl_basic_map_alloc_equality(fused);
		if (l < 0)
			goto error;
		isl_seq_cpy(fused->eq[l], info[i].bmap->eq[k], 1 + total);
	}

	for (k = 0; k < info[j].bmap->n_eq; ++k) {
		if (eq_j && (eq_j[2 * k] != STATUS_VALID ||
			     eq_j[2 * k + 1] != STATUS_VALID))
			continue;
		l = isl_basic_map_alloc_equality(fused);
		if (l < 0)
			goto error;
		isl_seq_cpy(fused->eq[l], info[j].bmap->eq[k], 1 + total);
	}

	for (k = 0; k < info[i].bmap->n_ineq; ++k) {
		if (ineq_i[k] != STATUS_VALID)
			continue;
		l = isl_basic_map_alloc_inequality(fused);
		if (l < 0)
			goto error;
		isl_seq_cpy(fused->ineq[l], info[i].bmap->ineq[k], 1 + total);
	}

	for (k = 0; k < info[j].bmap->n_ineq; ++k) {
		if (ineq_j[k] != STATUS_VALID)
			continue;
		l = isl_basic_map_alloc_inequality(fused);
		if (l < 0)
			goto error;
		isl_seq_cpy(fused->ineq[l], info[j].bmap->ineq[k], 1 + total);
	}

	for (k = 0; k < info[i].bmap->n_div; ++k) {
		int l = isl_basic_map_alloc_div(fused);
		if (l < 0)
			goto error;
		isl_seq_cpy(fused->div[l], info[i].bmap->div[k], 1 + 1 + total);
	}

	for (k = 0; k < extra_rows; ++k) {
		l = isl_basic_map_alloc_inequality(fused);
		if (l < 0)
			goto error;
		isl_seq_cpy(fused->ineq[l], extra->row[k], 1 + total);
	}

	if (detect_equalities)
		fused = isl_basic_map_detect_inequality_pairs(fused, NULL);
	fused = isl_basic_map_gauss(fused, NULL);
	ISL_F_SET(fused, ISL_BASIC_MAP_FINAL);
	if (ISL_F_ISSET(info[i].bmap, ISL_BASIC_MAP_RATIONAL) &&
	    ISL_F_ISSET(info[j].bmap, ISL_BASIC_MAP_RATIONAL))
		ISL_F_SET(fused, ISL_BASIC_MAP_RATIONAL);

	fused_tab = isl_tab_from_basic_map(fused, 0);
	if (isl_tab_detect_redundant(fused_tab) < 0)
		goto error;

	isl_basic_map_free(info[i].bmap);
	info[i].bmap = fused;
	isl_tab_free(info[i].tab);
	info[i].tab = fused_tab;
	drop(&info[j]);

	return isl_change_fuse;
error:
	isl_tab_free(fused_tab);
	isl_basic_map_free(fused);
	return isl_change_error;
}

/* Given a pair of basic maps i and j such that all constraints are either
 * "valid" or "cut", check if the facets corresponding to the "cut"
 * constraints of i lie entirely within basic map j.
 * If so, replace the pair by the basic map consisting of the valid
 * constraints in both basic maps.
 * Checking whether the facet lies entirely within basic map j
 * is performed by checking whether the constraints of basic map j
 * are valid for the facet.  These tests are performed on a rational
 * tableau to avoid the theoretical possibility that a constraint
 * that was considered to be a cut constraint for the entire basic map i
 * happens to be considered to be a valid constraint for the facet,
 * even though it cuts off the same rational points.
 *
 * To see that we are not introducing any extra points, call the
 * two basic maps A and B and the resulting map U and let x
 * be an element of U \setminus ( A \cup B ).
 * A line connecting x with an element of A \cup B meets a facet F
 * of either A or B.  Assume it is a facet of B and let c_1 be
 * the corresponding facet constraint.  We have c_1(x) < 0 and
 * so c_1 is a cut constraint.  This implies that there is some
 * (possibly rational) point x' satisfying the constraints of A
 * and the opposite of c_1 as otherwise c_1 would have been marked
 * valid for A.  The line connecting x and x' meets a facet of A
 * in a (possibly rational) point that also violates c_1, but this
 * is impossible since all cut constraints of B are valid for all
 * cut facets of A.
 * In case F is a facet of A rather than B, then we can apply the
 * above reasoning to find a facet of B separating x from A \cup B first.
 */
static enum isl_change check_facets(int i, int j,
	struct isl_coalesce_info *info, int *ineq_i, int *ineq_j)
{
	int k, l;
	struct isl_tab_undo *snap, *snap2;
	unsigned n_eq = info[i].bmap->n_eq;

	snap = isl_tab_snap(info[i].tab);
	if (isl_tab_mark_rational(info[i].tab) < 0)
		return isl_change_error;
	snap2 = isl_tab_snap(info[i].tab);

	for (k = 0; k < info[i].bmap->n_ineq; ++k) {
		if (ineq_i[k] != STATUS_CUT)
			continue;
		if (isl_tab_select_facet(info[i].tab, n_eq + k) < 0)
			return isl_change_error;
		for (l = 0; l < info[j].bmap->n_ineq; ++l) {
			int stat;
			if (ineq_j[l] != STATUS_CUT)
				continue;
			stat = status_in(info[j].bmap->ineq[l], info[i].tab);
			if (stat != STATUS_VALID)
				break;
		}
		if (isl_tab_rollback(info[i].tab, snap2) < 0)
			return isl_change_error;
		if (l < info[j].bmap->n_ineq)
			break;
	}

	if (k < info[i].bmap->n_ineq) {
		if (isl_tab_rollback(info[i].tab, snap) < 0)
			return isl_change_error;
		return isl_change_none;
	}
	return fuse(i, j, info, NULL, ineq_i, NULL, ineq_j, NULL, 0);
}

/* Check if info->bmap contains the basic map represented
 * by the tableau "tab".
 */
static int contains(struct isl_coalesce_info *info, int *ineq_i,
	struct isl_tab *tab)
{
	int k, l;
	unsigned dim;
	isl_basic_map *bmap = info->bmap;

	dim = isl_basic_map_total_dim(bmap);
	for (k = 0; k < bmap->n_eq; ++k) {
		for (l = 0; l < 2; ++l) {
			int stat;
			isl_seq_neg(bmap->eq[k], bmap->eq[k], 1+dim);
			stat = status_in(bmap->eq[k], tab);
			if (stat != STATUS_VALID)
				return 0;
		}
	}

	for (k = 0; k < bmap->n_ineq; ++k) {
		int stat;
		if (ineq_i[k] == STATUS_REDUNDANT)
			continue;
		stat = status_in(bmap->ineq[k], tab);
		if (stat != STATUS_VALID)
			return 0;
	}
	return 1;
}

/* Basic map "i" has an inequality (say "k") that is adjacent
 * to some inequality of basic map "j".  All the other inequalities
 * are valid for "j".
 * Check if basic map "j" forms an extension of basic map "i".
 *
 * Note that this function is only called if some of the equalities or
 * inequalities of basic map "j" do cut basic map "i".  The function is
 * correct even if there are no such cut constraints, but in that case
 * the additional checks performed by this function are overkill.
 *
 * In particular, we replace constraint k, say f >= 0, by constraint
 * f <= -1, add the inequalities of "j" that are valid for "i"
 * and check if the result is a subset of basic map "j".
 * If so, then we know that this result is exactly equal to basic map "j"
 * since all its constraints are valid for basic map "j".
 * By combining the valid constraints of "i" (all equalities and all
 * inequalities except "k") and the valid constraints of "j" we therefore
 * obtain a basic map that is equal to their union.
 * In this case, there is no need to perform a rollback of the tableau
 * since it is going to be destroyed in fuse().
 *
 *
 *	|\__			|\__
 *	|   \__			|   \__
 *	|      \_	=>	|      \__
 *	|_______| _		|_________\
 *
 *
 *	|\			|\
 *	| \			| \
 *	|  \			|  \
 *	|  |			|   \
 *	|  ||\		=>      |    \
 *	|  || \			|     \
 *	|  ||  |		|      |
 *	|__||_/			|_____/
 */
static enum isl_change is_adj_ineq_extension(int i, int j,
	struct isl_coalesce_info *info, int *eq_i, int *ineq_i,
	int *eq_j, int *ineq_j)
{
	int k;
	struct isl_tab_undo *snap;
	unsigned n_eq = info[i].bmap->n_eq;
	unsigned total = isl_basic_map_total_dim(info[i].bmap);
	int r;

	if (isl_tab_extend_cons(info[i].tab, 1 + info[j].bmap->n_ineq) < 0)
		return isl_change_error;

	for (k = 0; k < info[i].bmap->n_ineq; ++k)
		if (ineq_i[k] == STATUS_ADJ_INEQ)
			break;
	if (k >= info[i].bmap->n_ineq)
		isl_die(isl_basic_map_get_ctx(info[i].bmap), isl_error_internal,
			"ineq_i should have exactly one STATUS_ADJ_INEQ",
			return isl_change_error);

	snap = isl_tab_snap(info[i].tab);

	if (isl_tab_unrestrict(info[i].tab, n_eq + k) < 0)
		return isl_change_error;

	isl_seq_neg(info[i].bmap->ineq[k], info[i].bmap->ineq[k], 1 + total);
	isl_int_sub_ui(info[i].bmap->ineq[k][0], info[i].bmap->ineq[k][0], 1);
	r = isl_tab_add_ineq(info[i].tab, info[i].bmap->ineq[k]);
	isl_seq_neg(info[i].bmap->ineq[k], info[i].bmap->ineq[k], 1 + total);
	isl_int_sub_ui(info[i].bmap->ineq[k][0], info[i].bmap->ineq[k][0], 1);
	if (r < 0)
		return isl_change_error;

	for (k = 0; k < info[j].bmap->n_ineq; ++k) {
		if (ineq_j[k] != STATUS_VALID)
			continue;
		if (isl_tab_add_ineq(info[i].tab, info[j].bmap->ineq[k]) < 0)
			return isl_change_error;
	}

	if (contains(&info[j], ineq_j, info[i].tab))
		return fuse(i, j, info, eq_i, ineq_i, eq_j, ineq_j, NULL, 0);

	if (isl_tab_rollback(info[i].tab, snap) < 0)
		return isl_change_error;

	return isl_change_none;
}


/* Both basic maps have at least one inequality with and adjacent
 * (but opposite) inequality in the other basic map.
 * Check that there are no cut constraints and that there is only
 * a single pair of adjacent inequalities.
 * If so, we can replace the pair by a single basic map described
 * by all but the pair of adjacent inequalities.
 * Any additional points introduced lie strictly between the two
 * adjacent hyperplanes and can therefore be integral.
 *
 *        ____			  _____
 *       /    ||\		 /     \
 *      /     || \		/       \
 *      \     ||  \	=>	\        \
 *       \    ||  /		 \       /
 *        \___||_/		  \_____/
 *
 * The test for a single pair of adjancent inequalities is important
 * for avoiding the combination of two basic maps like the following
 *
 *       /|
 *      / |
 *     /__|
 *         _____
 *         |   |
 *         |   |
 *         |___|
 *
 * If there are some cut constraints on one side, then we may
 * still be able to fuse the two basic maps, but we need to perform
 * some additional checks in is_adj_ineq_extension.
 */
static enum isl_change check_adj_ineq(int i, int j,
	struct isl_coalesce_info *info, int *eq_i, int *ineq_i,
	int *eq_j, int *ineq_j)
{
	int count_i, count_j;
	int cut_i, cut_j;

	count_i = count(ineq_i, info[i].bmap->n_ineq, STATUS_ADJ_INEQ);
	count_j = count(ineq_j, info[j].bmap->n_ineq, STATUS_ADJ_INEQ);

	if (count_i != 1 && count_j != 1)
		return isl_change_none;

	cut_i = any(eq_i, 2 * info[i].bmap->n_eq, STATUS_CUT) ||
		any(ineq_i, info[i].bmap->n_ineq, STATUS_CUT);
	cut_j = any(eq_j, 2 * info[j].bmap->n_eq, STATUS_CUT) ||
		any(ineq_j, info[j].bmap->n_ineq, STATUS_CUT);

	if (!cut_i && !cut_j && count_i == 1 && count_j == 1)
		return fuse(i, j, info, NULL, ineq_i, NULL, ineq_j, NULL, 0);

	if (count_i == 1 && !cut_i)
		return is_adj_ineq_extension(i, j, info,
						eq_i, ineq_i, eq_j, ineq_j);

	if (count_j == 1 && !cut_j)
		return is_adj_ineq_extension(j, i, info,
						eq_j, ineq_j, eq_i, ineq_i);

	return isl_change_none;
}

/* Basic map "i" has an inequality "k" that is adjacent to some equality
 * of basic map "j".  All the other inequalities are valid for "j".
 * Check if basic map "j" forms an extension of basic map "i".
 *
 * In particular, we relax constraint "k", compute the corresponding
 * facet and check whether it is included in the other basic map.
 * If so, we know that relaxing the constraint extends the basic
 * map with exactly the other basic map (we already know that this
 * other basic map is included in the extension, because there
 * were no "cut" inequalities in "i") and we can replace the
 * two basic maps by this extension.
 * Place this extension in the position that is the smallest of i and j.
 *        ____			  _____
 *       /    || 		 /     |
 *      /     ||  		/      |
 *      \     ||   	=>	\      |
 *       \    ||		 \     |
 *        \___||		  \____|
 */
static enum isl_change is_adj_eq_extension(int i, int j, int k,
	struct isl_coalesce_info *info, int *eq_i, int *ineq_i,
	int *eq_j, int *ineq_j)
{
	int change = isl_change_none;
	int super;
	struct isl_tab_undo *snap, *snap2;
	unsigned n_eq = info[i].bmap->n_eq;

	if (isl_tab_is_equality(info[i].tab, n_eq + k))
		return isl_change_none;

	snap = isl_tab_snap(info[i].tab);
	if (isl_tab_relax(info[i].tab, n_eq + k) < 0)
		return isl_change_error;
	snap2 = isl_tab_snap(info[i].tab);
	if (isl_tab_select_facet(info[i].tab, n_eq + k) < 0)
		return isl_change_error;
	super = contains(&info[j], ineq_j, info[i].tab);
	if (super) {
		if (isl_tab_rollback(info[i].tab, snap2) < 0)
			return isl_change_error;
		info[i].bmap = isl_basic_map_cow(info[i].bmap);
		if (!info[i].bmap)
			return isl_change_error;
		isl_int_add_ui(info[i].bmap->ineq[k][0],
				info[i].bmap->ineq[k][0], 1);
		ISL_F_SET(info[i].bmap, ISL_BASIC_MAP_FINAL);
		drop(&info[j]);
		if (j < i)
			exchange(&info[i], &info[j]);
		change = isl_change_fuse;
	} else
		if (isl_tab_rollback(info[i].tab, snap) < 0)
			return isl_change_error;

	return change;
}

/* Data structure that keeps track of the wrapping constraints
 * and of information to bound the coefficients of those constraints.
 *
 * bound is set if we want to apply a bound on the coefficients
 * mat contains the wrapping constraints
 * max is the bound on the coefficients (if bound is set)
 */
struct isl_wraps {
	int bound;
	isl_mat *mat;
	isl_int max;
};

/* Update wraps->max to be greater than or equal to the coefficients
 * in the equalities and inequalities of info->bmap that can be removed
 * if we end up applying wrapping.
 */
static void wraps_update_max(struct isl_wraps *wraps,
	struct isl_coalesce_info *info, int *eq, int *ineq)
{
	int k;
	isl_int max_k;
	unsigned total = isl_basic_map_total_dim(info->bmap);

	isl_int_init(max_k);

	for (k = 0; k < info->bmap->n_eq; ++k) {
		if (eq[2 * k] == STATUS_VALID &&
		    eq[2 * k + 1] == STATUS_VALID)
			continue;
		isl_seq_abs_max(info->bmap->eq[k] + 1, total, &max_k);
		if (isl_int_abs_gt(max_k, wraps->max))
			isl_int_set(wraps->max, max_k);
	}

	for (k = 0; k < info->bmap->n_ineq; ++k) {
		if (ineq[k] == STATUS_VALID || ineq[k] == STATUS_REDUNDANT)
			continue;
		isl_seq_abs_max(info->bmap->ineq[k] + 1, total, &max_k);
		if (isl_int_abs_gt(max_k, wraps->max))
			isl_int_set(wraps->max, max_k);
	}

	isl_int_clear(max_k);
}

/* Initialize the isl_wraps data structure.
 * If we want to bound the coefficients of the wrapping constraints,
 * we set wraps->max to the largest coefficient
 * in the equalities and inequalities that can be removed if we end up
 * applying wrapping.
 */
static void wraps_init(struct isl_wraps *wraps, __isl_take isl_mat *mat,
	struct isl_coalesce_info *info, int i, int j,
	int *eq_i, int *ineq_i, int *eq_j, int *ineq_j)
{
	isl_ctx *ctx;

	wraps->bound = 0;
	wraps->mat = mat;
	if (!mat)
		return;
	ctx = isl_mat_get_ctx(mat);
	wraps->bound = isl_options_get_coalesce_bounded_wrapping(ctx);
	if (!wraps->bound)
		return;
	isl_int_init(wraps->max);
	isl_int_set_si(wraps->max, 0);
	wraps_update_max(wraps, &info[i], eq_i, ineq_i);
	wraps_update_max(wraps, &info[j], eq_j, ineq_j);
}

/* Free the contents of the isl_wraps data structure.
 */
static void wraps_free(struct isl_wraps *wraps)
{
	isl_mat_free(wraps->mat);
	if (wraps->bound)
		isl_int_clear(wraps->max);
}

/* Is the wrapping constraint in row "row" allowed?
 *
 * If wraps->bound is set, we check that none of the coefficients
 * is greater than wraps->max.
 */
static int allow_wrap(struct isl_wraps *wraps, int row)
{
	int i;

	if (!wraps->bound)
		return 1;

	for (i = 1; i < wraps->mat->n_col; ++i)
		if (isl_int_abs_gt(wraps->mat->row[row][i], wraps->max))
			return 0;

	return 1;
}

/* For each non-redundant constraint in info->bmap (as determined by info->tab),
 * wrap the constraint around "bound" such that it includes the whole
 * set "set" and append the resulting constraint to "wraps".
 * "wraps" is assumed to have been pre-allocated to the appropriate size.
 * wraps->n_row is the number of actual wrapped constraints that have
 * been added.
 * If any of the wrapping problems results in a constraint that is
 * identical to "bound", then this means that "set" is unbounded in such
 * way that no wrapping is possible.  If this happens then wraps->n_row
 * is reset to zero.
 * Similarly, if we want to bound the coefficients of the wrapping
 * constraints and a newly added wrapping constraint does not
 * satisfy the bound, then wraps->n_row is also reset to zero.
 */
static int add_wraps(struct isl_wraps *wraps, struct isl_coalesce_info *info,
	isl_int *bound, __isl_keep isl_set *set)
{
	int l;
	int w;
	isl_basic_map *bmap = info->bmap;
	unsigned total = isl_basic_map_total_dim(bmap);

	w = wraps->mat->n_row;

	for (l = 0; l < bmap->n_ineq; ++l) {
		if (isl_seq_is_neg(bound, bmap->ineq[l], 1 + total))
			continue;
		if (isl_seq_eq(bound, bmap->ineq[l], 1 + total))
			continue;
		if (isl_tab_is_redundant(info->tab, bmap->n_eq + l))
			continue;

		isl_seq_cpy(wraps->mat->row[w], bound, 1 + total);
		if (!isl_set_wrap_facet(set, wraps->mat->row[w], bmap->ineq[l]))
			return -1;
		if (isl_seq_eq(wraps->mat->row[w], bound, 1 + total))
			goto unbounded;
		if (!allow_wrap(wraps, w))
			goto unbounded;
		++w;
	}
	for (l = 0; l < bmap->n_eq; ++l) {
		if (isl_seq_is_neg(bound, bmap->eq[l], 1 + total))
			continue;
		if (isl_seq_eq(bound, bmap->eq[l], 1 + total))
			continue;

		isl_seq_cpy(wraps->mat->row[w], bound, 1 + total);
		isl_seq_neg(wraps->mat->row[w + 1], bmap->eq[l], 1 + total);
		if (!isl_set_wrap_facet(set, wraps->mat->row[w],
					wraps->mat->row[w + 1]))
			return -1;
		if (isl_seq_eq(wraps->mat->row[w], bound, 1 + total))
			goto unbounded;
		if (!allow_wrap(wraps, w))
			goto unbounded;
		++w;

		isl_seq_cpy(wraps->mat->row[w], bound, 1 + total);
		if (!isl_set_wrap_facet(set, wraps->mat->row[w], bmap->eq[l]))
			return -1;
		if (isl_seq_eq(wraps->mat->row[w], bound, 1 + total))
			goto unbounded;
		if (!allow_wrap(wraps, w))
			goto unbounded;
		++w;
	}

	wraps->mat->n_row = w;
	return 0;
unbounded:
	wraps->mat->n_row = 0;
	return 0;
}

/* Check if the constraints in "wraps" from "first" until the last
 * are all valid for the basic set represented by "tab".
 * If not, wraps->n_row is set to zero.
 */
static int check_wraps(__isl_keep isl_mat *wraps, int first,
	struct isl_tab *tab)
{
	int i;

	for (i = first; i < wraps->n_row; ++i) {
		enum isl_ineq_type type;
		type = isl_tab_ineq_type(tab, wraps->row[i]);
		if (type == isl_ineq_error)
			return -1;
		if (type == isl_ineq_redundant)
			continue;
		wraps->n_row = 0;
		return 0;
	}

	return 0;
}

/* Return a set that corresponds to the non-redundant constraints
 * (as recorded in tab) of bmap.
 *
 * It's important to remove the redundant constraints as some
 * of the other constraints may have been modified after the
 * constraints were marked redundant.
 * In particular, a constraint may have been relaxed.
 * Redundant constraints are ignored when a constraint is relaxed
 * and should therefore continue to be ignored ever after.
 * Otherwise, the relaxation might be thwarted by some of
 * these constraints.
 *
 * Update the underlying set to ensure that the dimension doesn't change.
 * Otherwise the integer divisions could get dropped if the tab
 * turns out to be empty.
 */
static __isl_give isl_set *set_from_updated_bmap(__isl_keep isl_basic_map *bmap,
	struct isl_tab *tab)
{
	isl_basic_set *bset;

	bmap = isl_basic_map_copy(bmap);
	bset = isl_basic_map_underlying_set(bmap);
	bset = isl_basic_set_cow(bset);
	bset = isl_basic_set_update_from_tab(bset, tab);
	return isl_set_from_basic_set(bset);
}

/* Given a basic set i with a constraint k that is adjacent to
 * basic set j, check if we can wrap
 * both the facet corresponding to k and basic map j
 * around their ridges to include the other set.
 * If so, replace the pair of basic sets by their union.
 *
 * All constraints of i (except k) are assumed to be valid for j.
 * This means that there is no real need to wrap the ridges of
 * the faces of basic map i around basic map j but since we do,
 * we have to check that the resulting wrapping constraints are valid for i.
 *        ____			  _____
 *       /    | 		 /     \
 *      /     ||  		/      |
 *      \     ||   	=>	\      |
 *       \    ||		 \     |
 *        \___||		  \____|
 *
 */
static enum isl_change can_wrap_in_facet(int i, int j, int k,
	struct isl_coalesce_info *info, int *eq_i, int *ineq_i,
	int *eq_j, int *ineq_j)
{
	enum isl_change change = isl_change_none;
	struct isl_wraps wraps;
	isl_ctx *ctx;
	isl_mat *mat;
	struct isl_set *set_i = NULL;
	struct isl_set *set_j = NULL;
	struct isl_vec *bound = NULL;
	unsigned total = isl_basic_map_total_dim(info[i].bmap);
	struct isl_tab_undo *snap;
	int n;

	set_i = set_from_updated_bmap(info[i].bmap, info[i].tab);
	set_j = set_from_updated_bmap(info[j].bmap, info[j].tab);
	ctx = isl_basic_map_get_ctx(info[i].bmap);
	mat = isl_mat_alloc(ctx, 2 * (info[i].bmap->n_eq + info[j].bmap->n_eq) +
				    info[i].bmap->n_ineq + info[j].bmap->n_ineq,
				    1 + total);
	wraps_init(&wraps, mat, info, i, j, eq_i, ineq_i, eq_j, ineq_j);
	bound = isl_vec_alloc(ctx, 1 + total);
	if (!set_i || !set_j || !wraps.mat || !bound)
		goto error;

	isl_seq_cpy(bound->el, info[i].bmap->ineq[k], 1 + total);
	isl_int_add_ui(bound->el[0], bound->el[0], 1);

	isl_seq_cpy(wraps.mat->row[0], bound->el, 1 + total);
	wraps.mat->n_row = 1;

	if (add_wraps(&wraps, &info[j], bound->el, set_i) < 0)
		goto error;
	if (!wraps.mat->n_row)
		goto unbounded;

	snap = isl_tab_snap(info[i].tab);

	if (isl_tab_select_facet(info[i].tab, info[i].bmap->n_eq + k) < 0)
		goto error;
	if (isl_tab_detect_redundant(info[i].tab) < 0)
		goto error;

	isl_seq_neg(bound->el, info[i].bmap->ineq[k], 1 + total);

	n = wraps.mat->n_row;
	if (add_wraps(&wraps, &info[i], bound->el, set_j) < 0)
		goto error;

	if (isl_tab_rollback(info[i].tab, snap) < 0)
		goto error;
	if (check_wraps(wraps.mat, n, info[i].tab) < 0)
		goto error;
	if (!wraps.mat->n_row)
		goto unbounded;

	change = fuse(i, j, info, eq_i, ineq_i, eq_j, ineq_j, wraps.mat, 0);

unbounded:
	wraps_free(&wraps);

	isl_set_free(set_i);
	isl_set_free(set_j);

	isl_vec_free(bound);

	return change;
error:
	wraps_free(&wraps);
	isl_vec_free(bound);
	isl_set_free(set_i);
	isl_set_free(set_j);
	return isl_change_error;
}

/* Given a pair of basic maps i and j such that j sticks out
 * of i at n cut constraints, each time by at most one,
 * try to compute wrapping constraints and replace the two
 * basic maps by a single basic map.
 * The other constraints of i are assumed to be valid for j.
 *
 * For each cut constraint t(x) >= 0 of i, we add the relaxed version
 * t(x) + 1 >= 0, along with wrapping constraints for all constraints
 * of basic map j that bound the part of basic map j that sticks out
 * of the cut constraint.
 * In particular, we first intersect basic map j with t(x) + 1 = 0.
 * If the result is empty, then t(x) >= 0 was actually a valid constraint
 * (with respect to the integer points), so we add t(x) >= 0 instead.
 * Otherwise, we wrap the constraints of basic map j that are not
 * redundant in this intersection over the union of the two basic maps.
 *
 * If any wrapping fails, i.e., if we cannot wrap to touch
 * the union, then we give up.
 * Otherwise, the pair of basic maps is replaced by their union.
 */
static enum isl_change wrap_in_facets(int i, int j, int *cuts, int n,
	struct isl_coalesce_info *info,
	int *eq_i, int *ineq_i, int *eq_j, int *ineq_j)
{
	enum isl_change change = isl_change_none;
	struct isl_wraps wraps;
	isl_ctx *ctx;
	isl_mat *mat;
	isl_set *set = NULL;
	unsigned total = isl_basic_map_total_dim(info[i].bmap);
	int max_wrap;
	int k, w;
	struct isl_tab_undo *snap;

	if (isl_tab_extend_cons(info[j].tab, 1) < 0)
		goto error;

	max_wrap = 1 + 2 * info[j].bmap->n_eq + info[j].bmap->n_ineq;
	max_wrap *= n;

	set = isl_set_union(set_from_updated_bmap(info[i].bmap, info[i].tab),
			    set_from_updated_bmap(info[j].bmap, info[j].tab));
	ctx = isl_basic_map_get_ctx(info[i].bmap);
	mat = isl_mat_alloc(ctx, max_wrap, 1 + total);
	wraps_init(&wraps, mat, info, i, j, eq_i, ineq_i, eq_j, ineq_j);
	if (!set || !wraps.mat)
		goto error;

	snap = isl_tab_snap(info[j].tab);

	wraps.mat->n_row = 0;

	for (k = 0; k < n; ++k) {
		w = wraps.mat->n_row++;
		isl_seq_cpy(wraps.mat->row[w],
			    info[i].bmap->ineq[cuts[k]], 1 + total);
		isl_int_add_ui(wraps.mat->row[w][0], wraps.mat->row[w][0], 1);
		if (isl_tab_add_eq(info[j].tab, wraps.mat->row[w]) < 0)
			goto error;
		if (isl_tab_detect_redundant(info[j].tab) < 0)
			goto error;

		if (info[j].tab->empty)
			isl_int_sub_ui(wraps.mat->row[w][0],
					wraps.mat->row[w][0], 1);
		else if (add_wraps(&wraps, &info[j],
				    wraps.mat->row[w], set) < 0)
			goto error;

		if (isl_tab_rollback(info[j].tab, snap) < 0)
			goto error;

		if (!wraps.mat->n_row)
			break;
	}

	if (k == n)
		change = fuse(i, j, info,
				eq_i, ineq_i, eq_j, ineq_j, wraps.mat, 0);

	wraps_free(&wraps);
	isl_set_free(set);

	return change;
error:
	wraps_free(&wraps);
	isl_set_free(set);
	return isl_change_error;
}

/* Given two basic sets i and j such that i has no cut equalities,
 * check if relaxing all the cut inequalities of i by one turns
 * them into valid constraint for j and check if we can wrap in
 * the bits that are sticking out.
 * If so, replace the pair by their union.
 *
 * We first check if all relaxed cut inequalities of i are valid for j
 * and then try to wrap in the intersections of the relaxed cut inequalities
 * with j.
 *
 * During this wrapping, we consider the points of j that lie at a distance
 * of exactly 1 from i.  In particular, we ignore the points that lie in
 * between this lower-dimensional space and the basic map i.
 * We can therefore only apply this to integer maps.
 *        ____			  _____
 *       / ___|_		 /     \
 *      / |    |  		/      |
 *      \ |    |   	=>	\      |
 *       \|____|		 \     |
 *        \___| 		  \____/
 *
 *	 _____			 ______
 *	| ____|_		|      \
 *	| |     |		|       |
 *	| |	|	=>	|       |
 *	|_|     |		|       |
 *	  |_____|		 \______|
 *
 *	 _______
 *	|       |
 *	|  |\   |
 *	|  | \  |
 *	|  |  \ |
 *	|  |   \|
 *	|  |    \
 *	|  |_____\
 *	|       |
 *	|_______|
 *
 * Wrapping can fail if the result of wrapping one of the facets
 * around its edges does not produce any new facet constraint.
 * In particular, this happens when we try to wrap in unbounded sets.
 *
 *	 _______________________________________________________________________
 *	|
 *	|  ___
 *	| |   |
 *	|_|   |_________________________________________________________________
 *	  |___|
 *
 * The following is not an acceptable result of coalescing the above two
 * sets as it includes extra integer points.
 *	 _______________________________________________________________________
 *	|
 *	|     
 *	|      
 *	|
 *	 \______________________________________________________________________
 */
static enum isl_change can_wrap_in_set(int i, int j,
	struct isl_coalesce_info *info, int *eq_i, int *ineq_i,
	int *eq_j, int *ineq_j)
{
	enum isl_change change = isl_change_none;
	int k, m;
	int n;
	int *cuts = NULL;
	isl_ctx *ctx;

	if (ISL_F_ISSET(info[i].bmap, ISL_BASIC_MAP_RATIONAL) ||
	    ISL_F_ISSET(info[j].bmap, ISL_BASIC_MAP_RATIONAL))
		return isl_change_none;

	n = count(ineq_i, info[i].bmap->n_ineq, STATUS_CUT);
	if (n == 0)
		return isl_change_none;

	ctx = isl_basic_map_get_ctx(info[i].bmap);
	cuts = isl_alloc_array(ctx, int, n);
	if (!cuts)
		return isl_change_error;

	for (k = 0, m = 0; m < n; ++k) {
		enum isl_ineq_type type;

		if (ineq_i[k] != STATUS_CUT)
			continue;

		isl_int_add_ui(info[i].bmap->ineq[k][0],
				info[i].bmap->ineq[k][0], 1);
		type = isl_tab_ineq_type(info[j].tab, info[i].bmap->ineq[k]);
		isl_int_sub_ui(info[i].bmap->ineq[k][0],
				info[i].bmap->ineq[k][0], 1);
		if (type == isl_ineq_error)
			goto error;
		if (type != isl_ineq_redundant)
			break;
		cuts[m] = k;
		++m;
	}

	if (m == n)
		change = wrap_in_facets(i, j, cuts, n, info,
					 eq_i, ineq_i, eq_j, ineq_j);

	free(cuts);

	return change;
error:
	free(cuts);
	return isl_change_error;
}

/* Check if either i or j has only cut inequalities that can
 * be used to wrap in (a facet of) the other basic set.
 * if so, replace the pair by their union.
 */
static enum isl_change check_wrap(int i, int j, struct isl_coalesce_info *info,
	int *eq_i, int *ineq_i, int *eq_j, int *ineq_j)
{
	enum isl_change change = isl_change_none;

	if (!any(eq_i, 2 * info[i].bmap->n_eq, STATUS_CUT))
		change = can_wrap_in_set(i, j, info,
					    eq_i, ineq_i, eq_j, ineq_j);
	if (change != isl_change_none)
		return change;

	if (!any(eq_j, 2 * info[j].bmap->n_eq, STATUS_CUT))
		change = can_wrap_in_set(j, i, info,
					    eq_j, ineq_j, eq_i, ineq_i);
	return change;
}

/* At least one of the basic maps has an equality that is adjacent
 * to inequality.  Make sure that only one of the basic maps has
 * such an equality and that the other basic map has exactly one
 * inequality adjacent to an equality.
 * We call the basic map that has the inequality "i" and the basic
 * map that has the equality "j".
 * If "i" has any "cut" (in)equality, then relaxing the inequality
 * by one would not result in a basic map that contains the other
 * basic map.
 */
static enum isl_change check_adj_eq(int i, int j,
	struct isl_coalesce_info *info, int *eq_i, int *ineq_i,
	int *eq_j, int *ineq_j)
{
	enum isl_change change = isl_change_none;
	int k;

	if (any(eq_i, 2 * info[i].bmap->n_eq, STATUS_ADJ_INEQ) &&
	    any(eq_j, 2 * info[j].bmap->n_eq, STATUS_ADJ_INEQ))
		/* ADJ EQ TOO MANY */
		return isl_change_none;

	if (any(eq_i, 2 * info[i].bmap->n_eq, STATUS_ADJ_INEQ))
		return check_adj_eq(j, i, info, eq_j, ineq_j, eq_i, ineq_i);

	/* j has an equality adjacent to an inequality in i */

	if (any(eq_i, 2 * info[i].bmap->n_eq, STATUS_CUT))
		return isl_change_none;
	if (any(ineq_i, info[i].bmap->n_ineq, STATUS_CUT))
		/* ADJ EQ CUT */
		return isl_change_none;
	if (count(ineq_i, info[i].bmap->n_ineq, STATUS_ADJ_EQ) != 1 ||
	    any(ineq_j, info[j].bmap->n_ineq, STATUS_ADJ_EQ) ||
	    any(ineq_i, info[i].bmap->n_ineq, STATUS_ADJ_INEQ) ||
	    any(ineq_j, info[j].bmap->n_ineq, STATUS_ADJ_INEQ))
		/* ADJ EQ TOO MANY */
		return isl_change_none;

	for (k = 0; k < info[i].bmap->n_ineq; ++k)
		if (ineq_i[k] == STATUS_ADJ_EQ)
			break;

	change = is_adj_eq_extension(i, j, k, info,
					eq_i, ineq_i, eq_j, ineq_j);
	if (change != isl_change_none)
		return change;

	if (count(eq_j, 2 * info[j].bmap->n_eq, STATUS_ADJ_INEQ) != 1)
		return isl_change_none;

	change = can_wrap_in_facet(i, j, k, info, eq_i, ineq_i, eq_j, ineq_j);

	return change;
}

/* The two basic maps lie on adjacent hyperplanes.  In particular,
 * basic map "i" has an equality that lies parallel to basic map "j".
 * Check if we can wrap the facets around the parallel hyperplanes
 * to include the other set.
 *
 * We perform basically the same operations as can_wrap_in_facet,
 * except that we don't need to select a facet of one of the sets.
 *				_
 *	\\			\\
 *	 \\		=>	 \\
 *	  \			  \|
 *
 * If there is more than one equality of "i" adjacent to an equality of "j",
 * then the result will satisfy one or more equalities that are a linear
 * combination of these equalities.  These will be encoded as pairs
 * of inequalities in the wrapping constraints and need to be made
 * explicit.
 */
static enum isl_change check_eq_adj_eq(int i, int j,
	struct isl_coalesce_info *info, int *eq_i, int *ineq_i,
	int *eq_j, int *ineq_j)
{
	int k;
	enum isl_change change = isl_change_none;
	int detect_equalities = 0;
	struct isl_wraps wraps;
	isl_ctx *ctx;
	isl_mat *mat;
	struct isl_set *set_i = NULL;
	struct isl_set *set_j = NULL;
	struct isl_vec *bound = NULL;
	unsigned total = isl_basic_map_total_dim(info[i].bmap);

	if (count(eq_i, 2 * info[i].bmap->n_eq, STATUS_ADJ_EQ) != 1)
		detect_equalities = 1;

	for (k = 0; k < 2 * info[i].bmap->n_eq ; ++k)
		if (eq_i[k] == STATUS_ADJ_EQ)
			break;

	set_i = set_from_updated_bmap(info[i].bmap, info[i].tab);
	set_j = set_from_updated_bmap(info[j].bmap, info[j].tab);
	ctx = isl_basic_map_get_ctx(info[i].bmap);
	mat = isl_mat_alloc(ctx, 2 * (info[i].bmap->n_eq + info[j].bmap->n_eq) +
				    info[i].bmap->n_ineq + info[j].bmap->n_ineq,
				    1 + total);
	wraps_init(&wraps, mat, info, i, j, eq_i, ineq_i, eq_j, ineq_j);
	bound = isl_vec_alloc(ctx, 1 + total);
	if (!set_i || !set_j || !wraps.mat || !bound)
		goto error;

	if (k % 2 == 0)
		isl_seq_neg(bound->el, info[i].bmap->eq[k / 2], 1 + total);
	else
		isl_seq_cpy(bound->el, info[i].bmap->eq[k / 2], 1 + total);
	isl_int_add_ui(bound->el[0], bound->el[0], 1);

	isl_seq_cpy(wraps.mat->row[0], bound->el, 1 + total);
	wraps.mat->n_row = 1;

	if (add_wraps(&wraps, &info[j], bound->el, set_i) < 0)
		goto error;
	if (!wraps.mat->n_row)
		goto unbounded;

	isl_int_sub_ui(bound->el[0], bound->el[0], 1);
	isl_seq_neg(bound->el, bound->el, 1 + total);

	isl_seq_cpy(wraps.mat->row[wraps.mat->n_row], bound->el, 1 + total);
	wraps.mat->n_row++;

	if (add_wraps(&wraps, &info[i], bound->el, set_j) < 0)
		goto error;
	if (!wraps.mat->n_row)
		goto unbounded;

	change = fuse(i, j, info, eq_i, ineq_i, eq_j, ineq_j, wraps.mat,
			detect_equalities);

	if (0) {
error:		change = isl_change_error;
	}
unbounded:

	wraps_free(&wraps);
	isl_set_free(set_i);
	isl_set_free(set_j);
	isl_vec_free(bound);

	return change;
}

/* Check if the union of the given pair of basic maps
 * can be represented by a single basic map.
 * If so, replace the pair by the single basic map and return
 * isl_change_drop_first, isl_change_drop_second or isl_change_fuse.
 * Otherwise, return isl_change_none.
 * The two basic maps are assumed to live in the same local space.
 *
 * We first check the effect of each constraint of one basic map
 * on the other basic map.
 * The constraint may be
 *	redundant	the constraint is redundant in its own
 *			basic map and should be ignore and removed
 *			in the end
 *	valid		all (integer) points of the other basic map
 *			satisfy the constraint
 *	separate	no (integer) point of the other basic map
 *			satisfies the constraint
 *	cut		some but not all points of the other basic map
 *			satisfy the constraint
 *	adj_eq		the given constraint is adjacent (on the outside)
 *			to an equality of the other basic map
 *	adj_ineq	the given constraint is adjacent (on the outside)
 *			to an inequality of the other basic map
 *
 * We consider seven cases in which we can replace the pair by a single
 * basic map.  We ignore all "redundant" constraints.
 *
 *	1. all constraints of one basic map are valid
 *		=> the other basic map is a subset and can be removed
 *
 *	2. all constraints of both basic maps are either "valid" or "cut"
 *	   and the facets corresponding to the "cut" constraints
 *	   of one of the basic maps lies entirely inside the other basic map
 *		=> the pair can be replaced by a basic map consisting
 *		   of the valid constraints in both basic maps
 *
 *	3. there is a single pair of adjacent inequalities
 *	   (all other constraints are "valid")
 *		=> the pair can be replaced by a basic map consisting
 *		   of the valid constraints in both basic maps
 *
 *	4. one basic map has a single adjacent inequality, while the other
 *	   constraints are "valid".  The other basic map has some
 *	   "cut" constraints, but replacing the adjacent inequality by
 *	   its opposite and adding the valid constraints of the other
 *	   basic map results in a subset of the other basic map
 *		=> the pair can be replaced by a basic map consisting
 *		   of the valid constraints in both basic maps
 *
 *	5. there is a single adjacent pair of an inequality and an equality,
 *	   the other constraints of the basic map containing the inequality are
 *	   "valid".  Moreover, if the inequality the basic map is relaxed
 *	   and then turned into an equality, then resulting facet lies
 *	   entirely inside the other basic map
 *		=> the pair can be replaced by the basic map containing
 *		   the inequality, with the inequality relaxed.
 *
 *	6. there is a single adjacent pair of an inequality and an equality,
 *	   the other constraints of the basic map containing the inequality are
 *	   "valid".  Moreover, the facets corresponding to both
 *	   the inequality and the equality can be wrapped around their
 *	   ridges to include the other basic map
 *		=> the pair can be replaced by a basic map consisting
 *		   of the valid constraints in both basic maps together
 *		   with all wrapping constraints
 *
 *	7. one of the basic maps extends beyond the other by at most one.
 *	   Moreover, the facets corresponding to the cut constraints and
 *	   the pieces of the other basic map at offset one from these cut
 *	   constraints can be wrapped around their ridges to include
 *	   the union of the two basic maps
 *		=> the pair can be replaced by a basic map consisting
 *		   of the valid constraints in both basic maps together
 *		   with all wrapping constraints
 *
 *	8. the two basic maps live in adjacent hyperplanes.  In principle
 *	   such sets can always be combined through wrapping, but we impose
 *	   that there is only one such pair, to avoid overeager coalescing.
 *
 * Throughout the computation, we maintain a collection of tableaus
 * corresponding to the basic maps.  When the basic maps are dropped
 * or combined, the tableaus are modified accordingly.
 */
static enum isl_change coalesce_local_pair(int i, int j,
	struct isl_coalesce_info *info)
{
	enum isl_change change = isl_change_none;
	int *eq_i = NULL;
	int *eq_j = NULL;
	int *ineq_i = NULL;
	int *ineq_j = NULL;

	eq_i = eq_status_in(info[i].bmap, info[j].tab);
	if (info[i].bmap->n_eq && !eq_i)
		goto error;
	if (any(eq_i, 2 * info[i].bmap->n_eq, STATUS_ERROR))
		goto error;
	if (any(eq_i, 2 * info[i].bmap->n_eq, STATUS_SEPARATE))
		goto done;

	eq_j = eq_status_in(info[j].bmap, info[i].tab);
	if (info[j].bmap->n_eq && !eq_j)
		goto error;
	if (any(eq_j, 2 * info[j].bmap->n_eq, STATUS_ERROR))
		goto error;
	if (any(eq_j, 2 * info[j].bmap->n_eq, STATUS_SEPARATE))
		goto done;

	ineq_i = ineq_status_in(info[i].bmap, info[i].tab, info[j].tab);
	if (info[i].bmap->n_ineq && !ineq_i)
		goto error;
	if (any(ineq_i, info[i].bmap->n_ineq, STATUS_ERROR))
		goto error;
	if (any(ineq_i, info[i].bmap->n_ineq, STATUS_SEPARATE))
		goto done;

	ineq_j = ineq_status_in(info[j].bmap, info[j].tab, info[i].tab);
	if (info[j].bmap->n_ineq && !ineq_j)
		goto error;
	if (any(ineq_j, info[j].bmap->n_ineq, STATUS_ERROR))
		goto error;
	if (any(ineq_j, info[j].bmap->n_ineq, STATUS_SEPARATE))
		goto done;

	if (all(eq_i, 2 * info[i].bmap->n_eq, STATUS_VALID) &&
	    all(ineq_i, info[i].bmap->n_ineq, STATUS_VALID)) {
		drop(&info[j]);
		change = isl_change_drop_second;
	} else if (all(eq_j, 2 * info[j].bmap->n_eq, STATUS_VALID) &&
		   all(ineq_j, info[j].bmap->n_ineq, STATUS_VALID)) {
		drop(&info[i]);
		change = isl_change_drop_first;
	} else if (any(eq_i, 2 * info[i].bmap->n_eq, STATUS_ADJ_EQ)) {
		change = check_eq_adj_eq(i, j, info,
					eq_i, ineq_i, eq_j, ineq_j);
	} else if (any(eq_j, 2 * info[j].bmap->n_eq, STATUS_ADJ_EQ)) {
		change = check_eq_adj_eq(j, i, info,
					eq_j, ineq_j, eq_i, ineq_i);
	} else if (any(eq_i, 2 * info[i].bmap->n_eq, STATUS_ADJ_INEQ) ||
		   any(eq_j, 2 * info[j].bmap->n_eq, STATUS_ADJ_INEQ)) {
		change = check_adj_eq(i, j, info,
					eq_i, ineq_i, eq_j, ineq_j);
	} else if (any(ineq_i, info[i].bmap->n_ineq, STATUS_ADJ_EQ) ||
		   any(ineq_j, info[j].bmap->n_ineq, STATUS_ADJ_EQ)) {
		/* Can't happen */
		/* BAD ADJ INEQ */
	} else if (any(ineq_i, info[i].bmap->n_ineq, STATUS_ADJ_INEQ) ||
		   any(ineq_j, info[j].bmap->n_ineq, STATUS_ADJ_INEQ)) {
		change = check_adj_ineq(i, j, info,
					eq_i, ineq_i, eq_j, ineq_j);
	} else {
		if (!any(eq_i, 2 * info[i].bmap->n_eq, STATUS_CUT) &&
		    !any(eq_j, 2 * info[j].bmap->n_eq, STATUS_CUT))
			change = check_facets(i, j, info, ineq_i, ineq_j);
		if (change == isl_change_none)
			change = check_wrap(i, j, info,
						eq_i, ineq_i, eq_j, ineq_j);
	}

done:
	free(eq_i);
	free(eq_j);
	free(ineq_i);
	free(ineq_j);
	return change;
error:
	free(eq_i);
	free(eq_j);
	free(ineq_i);
	free(ineq_j);
	return isl_change_error;
}

/* Do the two basic maps live in the same local space, i.e.,
 * do they have the same (known) divs?
 * If either basic map has any unknown divs, then we can only assume
 * that they do not live in the same local space.
 */
static int same_divs(__isl_keep isl_basic_map *bmap1,
	__isl_keep isl_basic_map *bmap2)
{
	int i;
	int known;
	int total;

	if (!bmap1 || !bmap2)
		return -1;
	if (bmap1->n_div != bmap2->n_div)
		return 0;

	if (bmap1->n_div == 0)
		return 1;

	known = isl_basic_map_divs_known(bmap1);
	if (known < 0 || !known)
		return known;
	known = isl_basic_map_divs_known(bmap2);
	if (known < 0 || !known)
		return known;

	total = isl_basic_map_total_dim(bmap1);
	for (i = 0; i < bmap1->n_div; ++i)
		if (!isl_seq_eq(bmap1->div[i], bmap2->div[i], 2 + total))
			return 0;

	return 1;
}

/* Does "bmap" contain the basic map represented by the tableau "tab"
 * after expanding the divs of "bmap" to match those of "tab"?
 * The expansion is performed using the divs "div" and expansion "exp"
 * computed by the caller.
 * Then we check if all constraints of the expanded "bmap" are valid for "tab".
 */
static int contains_with_expanded_divs(__isl_keep isl_basic_map *bmap,
	struct isl_tab *tab, __isl_keep isl_mat *div, int *exp)
{
	int superset = 0;
	int *eq_i = NULL;
	int *ineq_i = NULL;

	bmap = isl_basic_map_copy(bmap);
	bmap = isl_basic_set_expand_divs(bmap, isl_mat_copy(div), exp);

	if (!bmap)
		goto error;

	eq_i = eq_status_in(bmap, tab);
	if (bmap->n_eq && !eq_i)
		goto error;
	if (any(eq_i, 2 * bmap->n_eq, STATUS_ERROR))
		goto error;
	if (any(eq_i, 2 * bmap->n_eq, STATUS_SEPARATE))
		goto done;

	ineq_i = ineq_status_in(bmap, NULL, tab);
	if (bmap->n_ineq && !ineq_i)
		goto error;
	if (any(ineq_i, bmap->n_ineq, STATUS_ERROR))
		goto error;
	if (any(ineq_i, bmap->n_ineq, STATUS_SEPARATE))
		goto done;

	if (all(eq_i, 2 * bmap->n_eq, STATUS_VALID) &&
	    all(ineq_i, bmap->n_ineq, STATUS_VALID))
		superset = 1;

done:
	isl_basic_map_free(bmap);
	free(eq_i);
	free(ineq_i);
	return superset;
error:
	isl_basic_map_free(bmap);
	free(eq_i);
	free(ineq_i);
	return -1;
}

/* Does "bmap_i" contain the basic map represented by "info_j"
 * after aligning the divs of "bmap_i" to those of "info_j".
 * Note that this can only succeed if the number of divs of "bmap_i"
 * is smaller than (or equal to) the number of divs of "info_j".
 *
 * We first check if the divs of "bmap_i" are all known and form a subset
 * of those of "bmap_j".  If so, we pass control over to
 * contains_with_expanded_divs.
 */
static int contains_after_aligning_divs(__isl_keep isl_basic_map *bmap_i,
	struct isl_coalesce_info *info_j)
{
	int known;
	isl_mat *div_i, *div_j, *div;
	int *exp1 = NULL;
	int *exp2 = NULL;
	isl_ctx *ctx;
	int subset;

	known = isl_basic_map_divs_known(bmap_i);
	if (known < 0 || !known)
		return known;

	ctx = isl_basic_map_get_ctx(bmap_i);

	div_i = isl_basic_map_get_divs(bmap_i);
	div_j = isl_basic_map_get_divs(info_j->bmap);

	if (!div_i || !div_j)
		goto error;

	exp1 = isl_alloc_array(ctx, int, div_i->n_row);
	exp2 = isl_alloc_array(ctx, int, div_j->n_row);
	if ((div_i->n_row && !exp1) || (div_j->n_row && !exp2))
		goto error;

	div = isl_merge_divs(div_i, div_j, exp1, exp2);
	if (!div)
		goto error;

	if (div->n_row == div_j->n_row)
		subset = contains_with_expanded_divs(bmap_i,
							info_j->tab, div, exp1);
	else
		subset = 0;

	isl_mat_free(div);

	isl_mat_free(div_i);
	isl_mat_free(div_j);

	free(exp2);
	free(exp1);

	return subset;
error:
	isl_mat_free(div_i);
	isl_mat_free(div_j);
	free(exp1);
	free(exp2);
	return -1;
}

/* Check if the basic map "j" is a subset of basic map "i",
 * if "i" has fewer divs that "j".
 * If so, remove basic map "j".
 *
 * If the two basic maps have the same number of divs, then
 * they must necessarily be different.  Otherwise, we would have
 * called coalesce_local_pair.  We therefore don't try anything
 * in this case.
 */
static int coalesced_subset(int i, int j, struct isl_coalesce_info *info)
{
	int superset;

	if (info[i].bmap->n_div >= info[j].bmap->n_div)
		return 0;

	superset = contains_after_aligning_divs(info[i].bmap, &info[j]);
	if (superset < 0)
		return -1;
	if (superset)
		drop(&info[j]);

	return superset;
}

/* Check if one of the basic maps is a subset of the other and, if so,
 * drop the subset.
 * Note that we only perform any test if the number of divs is different
 * in the two basic maps.  In case the number of divs is the same,
 * we have already established that the divs are different
 * in the two basic maps.
 * In particular, if the number of divs of basic map i is smaller than
 * the number of divs of basic map j, then we check if j is a subset of i
 * and vice versa.
 */
static enum isl_change check_coalesce_subset(int i, int j,
	struct isl_coalesce_info *info)
{
	int changed;

	changed = coalesced_subset(i, j, info);
	if (changed < 0 || changed)
		return changed < 0 ? isl_change_error : isl_change_drop_second;

	changed = coalesced_subset(j, i, info);
	if (changed < 0 || changed)
		return changed < 0 ? isl_change_error : isl_change_drop_first;

	return isl_change_none;
}

/* Check if the union of the given pair of basic maps
 * can be represented by a single basic map.
 * If so, replace the pair by the single basic map and return
 * isl_change_drop_first, isl_change_drop_second or isl_change_fuse.
 * Otherwise, return isl_change_none.
 *
 * We first check if the two basic maps live in the same local space.
 * If so, we do the complete check.  Otherwise, we check if one is
 * an obvious subset of the other.
 */
static enum isl_change coalesce_pair(int i, int j,
	struct isl_coalesce_info *info)
{
	int same;

	same = same_divs(info[i].bmap, info[j].bmap);
	if (same < 0)
		return isl_change_error;
	if (same)
		return coalesce_local_pair(i, j, info);

	return check_coalesce_subset(i, j, info);
}

/* Pairwise coalesce the basic maps described by the "n" elements of "info",
 * skipping basic maps that have been removed (either before or within
 * this function).
 *
 * For each basic map i, we check if it can be coalesced with respect
 * to any previously considered basic map j.
 * If i gets dropped (because it was a subset of some j), then
 * we can move on to the next basic map.
 * If j gets dropped, we need to continue checking against the other
 * previously considered basic maps.
 * If the two basic maps got fused, then we recheck the fused basic map
 * against the previously considered basic maps.
 */
static int coalesce(isl_ctx *ctx, int n, struct isl_coalesce_info *info)
{
	int i, j;

	for (i = n - 2; i >= 0; --i) {
		if (info[i].removed)
			continue;
		for (j = i + 1; j < n; ++j) {
			enum isl_change changed;

			if (info[j].removed)
				continue;
			if (info[i].removed)
				isl_die(ctx, isl_error_internal,
					"basic map unexpectedly removed",
					return -1);
			changed = coalesce_pair(i, j, info);
			switch (changed) {
			case isl_change_error:
				return -1;
			case isl_change_none:
			case isl_change_drop_second:
				continue;
			case isl_change_drop_first:
				j = n;
				break;
			case isl_change_fuse:
				j = i;
				break;
			}
		}
	}

	return 0;
}

/* Update the basic maps in "map" based on the information in "info".
 * In particular, remove the basic maps that have been marked removed and
 * update the others based on the information in the corresponding tableau.
 * Since we detected implicit equalities without calling
 * isl_basic_map_gauss, we need to do it now.
 */
static __isl_give isl_map *update_basic_maps(__isl_take isl_map *map,
	int n, struct isl_coalesce_info *info)
{
	int i;

	if (!map)
		return NULL;

	for (i = n - 1; i >= 0; --i) {
		if (info[i].removed) {
			isl_basic_map_free(map->p[i]);
			if (i != map->n - 1)
				map->p[i] = map->p[map->n - 1];
			map->n--;
			continue;
		}

		info[i].bmap = isl_basic_map_update_from_tab(info[i].bmap,
							info[i].tab);
		info[i].bmap = isl_basic_map_gauss(info[i].bmap, NULL);
		info[i].bmap = isl_basic_map_finalize(info[i].bmap);
		if (!info[i].bmap)
			return isl_map_free(map);
		ISL_F_SET(info[i].bmap, ISL_BASIC_MAP_NO_IMPLICIT);
		ISL_F_SET(info[i].bmap, ISL_BASIC_MAP_NO_REDUNDANT);
		isl_basic_map_free(map->p[i]);
		map->p[i] = info[i].bmap;
		info[i].bmap = NULL;
	}

	return map;
}

/* For each pair of basic maps in the map, check if the union of the two
 * can be represented by a single basic map.
 * If so, replace the pair by the single basic map and start over.
 *
 * Since we are constructing the tableaus of the basic maps anyway,
 * we exploit them to detect implicit equalities and redundant constraints.
 * This also helps the coalescing as it can ignore the redundant constraints.
 * In order to avoid confusion, we make all implicit equalities explicit
 * in the basic maps.  We don't call isl_basic_map_gauss, though,
 * as that may affect the number of constraints.
 * This means that we have to call isl_basic_map_gauss at the end
 * of the computation (in update_basic_maps) to ensure that
 * the basic maps are not left in an unexpected state.
 */
struct isl_map *isl_map_coalesce(struct isl_map *map)
{
	int i;
	unsigned n;
	isl_ctx *ctx;
	struct isl_coalesce_info *info = NULL;

	map = isl_map_remove_empty_parts(map);
	if (!map)
		return NULL;

	if (map->n <= 1)
		return map;

	ctx = isl_map_get_ctx(map);
	map = isl_map_sort_divs(map);
	map = isl_map_cow(map);

	if (!map)
		return NULL;

	n = map->n;

	info = isl_calloc_array(map->ctx, struct isl_coalesce_info, n);
	if (!info)
		goto error;

	for (i = 0; i < map->n; ++i) {
		info[i].bmap = isl_basic_map_copy(map->p[i]);
		info[i].tab = isl_tab_from_basic_map(info[i].bmap, 0);
		if (!info[i].tab)
			goto error;
		if (!ISL_F_ISSET(info[i].bmap, ISL_BASIC_MAP_NO_IMPLICIT))
			if (isl_tab_detect_implicit_equalities(info[i].tab) < 0)
				goto error;
		info[i].bmap = isl_tab_make_equalities_explicit(info[i].tab,
								info[i].bmap);
		if (!info[i].bmap)
			goto error;
		if (!ISL_F_ISSET(info[i].bmap, ISL_BASIC_MAP_NO_REDUNDANT))
			if (isl_tab_detect_redundant(info[i].tab) < 0)
				goto error;
	}
	for (i = map->n - 1; i >= 0; --i)
		if (info[i].tab->empty)
			drop(&info[i]);

	if (coalesce(ctx, n, info) < 0)
		goto error;

	map = update_basic_maps(map, n, info);

	clear_coalesce_info(n, info);

	return map;
error:
	clear_coalesce_info(n, info);
	isl_map_free(map);
	return NULL;
}

/* For each pair of basic sets in the set, check if the union of the two
 * can be represented by a single basic set.
 * If so, replace the pair by the single basic set and start over.
 */
struct isl_set *isl_set_coalesce(struct isl_set *set)
{
	return (struct isl_set *)isl_map_coalesce((struct isl_map *)set);
}
