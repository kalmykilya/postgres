/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/createplan.c,v 1.131 2003/01/15 23:10:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Scan *create_scan_plan(Query *root, Path *best_path);
static Join *create_join_plan(Query *root, JoinPath *best_path);
static Append *create_append_plan(Query *root, AppendPath *best_path);
static Result *create_result_plan(Query *root, ResultPath *best_path);
static Material *create_material_plan(Query *root, MaterialPath *best_path);
static SeqScan *create_seqscan_plan(Path *best_path, List *tlist,
					List *scan_clauses);
static IndexScan *create_indexscan_plan(Query *root, IndexPath *best_path,
					  List *tlist, List *scan_clauses);
static TidScan *create_tidscan_plan(TidPath *best_path, List *tlist,
					List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(Path *best_path,
						 List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(Path *best_path,
						 List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(Query *root,
					 NestPath *best_path, List *tlist,
					 List *joinclauses, List *otherclauses,
					 Plan *outer_plan, Plan *inner_plan);
static MergeJoin *create_mergejoin_plan(Query *root,
					  MergePath *best_path, List *tlist,
					  List *joinclauses, List *otherclauses,
					  Plan *outer_plan, Plan *inner_plan);
static HashJoin *create_hashjoin_plan(Query *root,
					 HashPath *best_path, List *tlist,
					 List *joinclauses, List *otherclauses,
					 Plan *outer_plan, Plan *inner_plan);
static void fix_indxqual_references(List *indexquals, IndexPath *index_path,
						List **fixed_indexquals,
						List **recheck_indexquals);
static void fix_indxqual_sublist(List *indexqual, int baserelid,
					 IndexOptInfo *index,
					 List **fixed_quals, List **recheck_quals);
static Node *fix_indxqual_operand(Node *node, int baserelid,
					 IndexOptInfo *index,
					 Oid *opclass);
static List *get_switched_clauses(List *clauses, List *outerrelids);
static List *order_qual_clauses(Query *root, List *clauses);
static void copy_path_costsize(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
			   List *indxid, List *indxqual,
			   List *indxqualorig,
			   ScanDirection indexscandir);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
			 List *tideval);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
				  Index scanrelid);
static NestLoop *make_nestloop(List *tlist,
			  List *joinclauses, List *otherclauses,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static HashJoin *make_hashjoin(List *tlist,
			  List *joinclauses, List *otherclauses,
			  List *hashclauses,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static Hash *make_hash(List *tlist, List *hashkeys, Plan *lefttree);
static MergeJoin *make_mergejoin(List *tlist,
			   List *joinclauses, List *otherclauses,
			   List *mergeclauses,
			   Plan *lefttree, Plan *righttree,
			   JoinType jointype);
static Sort *make_sort_from_pathkeys(Query *root, Plan *lefttree,
									 List *relids, List *pathkeys);


/*
 * create_plan
 *	  Creates the access plan for a query by tracing backwards through the
 *	  desired chain of pathnodes, starting at the node 'best_path'.  For
 *	  every pathnode found:
 *	  (1) Create a corresponding plan node containing appropriate id,
 *		  target list, and qualification information.
 *	  (2) Modify qual clauses of join nodes so that subplan attributes are
 *		  referenced using relative values.
 *	  (3) Target lists are not modified, but will be in setrefs.c.
 *
 *	  best_path is the best access path
 *
 *	  Returns a Plan tree.
 */
Plan *
create_plan(Query *root, Path *best_path)
{
	Plan	   *plan;

	switch (best_path->pathtype)
	{
		case T_IndexScan:
		case T_SeqScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
			plan = (Plan *) create_scan_plan(root, best_path);
			break;
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan = (Plan *) create_join_plan(root,
											 (JoinPath *) best_path);
			break;
		case T_Append:
			plan = (Plan *) create_append_plan(root,
											   (AppendPath *) best_path);
			break;
		case T_Result:
			plan = (Plan *) create_result_plan(root,
											   (ResultPath *) best_path);
			break;
		case T_Material:
			plan = (Plan *) create_material_plan(root,
												 (MaterialPath *) best_path);
			break;
		default:
			elog(ERROR, "create_plan: unknown pathtype %d",
				 best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

#ifdef NOT_USED					/* fix xfunc */
	/* sort clauses by cost/(1-selectivity) -- JMH 2/26/92 */
	if (XfuncMode != XFUNC_OFF)
	{
		set_qpqual((Plan) plan,
				   lisp_qsort(get_qpqual((Plan) plan),
							  xfunc_clause_compare));
		if (XfuncMode != XFUNC_NOR)
			/* sort the disjuncts within each clause by cost -- JMH 3/4/92 */
			xfunc_disjunct_sort(plan->qpqual);
	}
#endif

	return plan;
}

/*
 * create_scan_plan
 *	 Create a scan plan for the parent relation of 'best_path'.
 *
 *	 Returns a Plan node.
 */
static Scan *
create_scan_plan(Query *root, Path *best_path)
{
	Scan	   *plan;
	List	   *tlist = best_path->parent->targetlist;
	List	   *scan_clauses;

	/*
	 * Extract the relevant restriction clauses from the parent relation;
	 * the executor must apply all these restrictions during the scan.
	 */
	scan_clauses = get_actual_clauses(best_path->parent->baserestrictinfo);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	switch (best_path->pathtype)
	{
		case T_SeqScan:
			plan = (Scan *) create_seqscan_plan(best_path,
												tlist,
												scan_clauses);
			break;

		case T_IndexScan:
			plan = (Scan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses);
			break;

		case T_TidScan:
			plan = (Scan *) create_tidscan_plan((TidPath *) best_path,
												tlist,
												scan_clauses);
			break;

		case T_SubqueryScan:
			plan = (Scan *) create_subqueryscan_plan(best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_FunctionScan:
			plan = (Scan *) create_functionscan_plan(best_path,
													 tlist,
													 scan_clauses);
			break;

		default:
			elog(ERROR, "create_scan_plan: unknown node type: %d",
				 best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	return plan;
}

/*
 * create_join_plan
 *	  Create a join plan for 'best_path' and (recursively) plans for its
 *	  inner and outer paths.
 *
 *	  Returns a Plan node.
 */
static Join *
create_join_plan(Query *root, JoinPath *best_path)
{
	List	   *join_tlist = best_path->path.parent->targetlist;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *joinclauses;
	List	   *otherclauses;
	Join	   *plan;

	outer_plan = create_plan(root, best_path->outerjoinpath);
	inner_plan = create_plan(root, best_path->innerjoinpath);

	if (IS_OUTER_JOIN(best_path->jointype))
	{
		get_actual_join_clauses(best_path->joinrestrictinfo,
								&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = get_actual_clauses(best_path->joinrestrictinfo);
		otherclauses = NIL;
	}

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Join *) create_mergejoin_plan(root,
												  (MergePath *) best_path,
												  join_tlist,
												  joinclauses,
												  otherclauses,
												  outer_plan,
												  inner_plan);
			break;
		case T_HashJoin:
			plan = (Join *) create_hashjoin_plan(root,
												 (HashPath *) best_path,
												 join_tlist,
												 joinclauses,
												 otherclauses,
												 outer_plan,
												 inner_plan);
			break;
		case T_NestLoop:
			plan = (Join *) create_nestloop_plan(root,
												 (NestPath *) best_path,
												 join_tlist,
												 joinclauses,
												 otherclauses,
												 outer_plan,
												 inner_plan);
			break;
		default:
			elog(ERROR, "create_join_plan: unknown node type: %d",
				 best_path->path.pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

#ifdef NOT_USED

	/*
	 * * Expensive function pullups may have pulled local predicates *
	 * into this path node.  Put them in the qpqual of the plan node. *
	 * JMH, 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) plan,
				   nconc(get_qpqual((Plan) plan),
				   get_actual_clauses(get_loc_restrictinfo(best_path))));
#endif

	return plan;
}

/*
 * create_append_plan
 *	  Create an Append plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Append *
create_append_plan(Query *root, AppendPath *best_path)
{
	Append	   *plan;
	List	   *tlist = best_path->path.parent->targetlist;
	List	   *subplans = NIL;
	List	   *subpaths;

	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);

		subplans = lappend(subplans, create_plan(root, subpath));
	}

	plan = make_append(subplans, false, tlist);

	return plan;
}

/*
 * create_result_plan
 *	  Create a Result plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Result *
create_result_plan(Query *root, ResultPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	List	   *constclauses;
	Plan	   *subplan;

	if (best_path->path.parent)
		tlist = best_path->path.parent->targetlist;
	else
		tlist = NIL;			/* will be filled in later */

	if (best_path->subpath)
		subplan = create_plan(root, best_path->subpath);
	else
		subplan = NULL;

	constclauses = order_qual_clauses(root, best_path->constantqual);

	plan = make_result(tlist, (Node *) constclauses, subplan);

	return plan;
}

/*
 * create_material_plan
 *	  Create a Material plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Material *
create_material_plan(Query *root, MaterialPath *best_path)
{
	Material   *plan;
	Plan	   *subplan;

	subplan = create_plan(root, best_path->subpath);

	plan = make_material(best_path->path.parent->targetlist, subplan);

	copy_path_costsize(&plan->plan, (Path *) best_path);

	return plan;
}


/*****************************************************************************
 *
 *	BASE-RELATION SCAN METHODS
 *
 *****************************************************************************/


/*
 * create_seqscan_plan
 *	 Returns a seqscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SeqScan *
create_seqscan_plan(Path *best_path, List *tlist, List *scan_clauses)
{
	SeqScan    *scan_plan;
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->parent->relids) == 1);
	Assert(best_path->parent->rtekind == RTE_RELATION);

	scan_relid = (Index) lfirsti(best_path->parent->relids);

	scan_plan = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	copy_path_costsize(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_indexscan_plan
 *	  Returns a indexscan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 *
 * The indexqual of the path contains a sublist of implicitly-ANDed qual
 * conditions for each scan of the index(es); if there is more than one
 * scan then the retrieved tuple sets are ORed together.  The indexqual
 * and indexinfo lists must have the same length, ie, the number of scans
 * that will occur.  Note it is possible for a qual condition sublist
 * to be empty --- then no index restrictions will be applied during that
 * scan.
 */
static IndexScan *
create_indexscan_plan(Query *root,
					  IndexPath *best_path,
					  List *tlist,
					  List *scan_clauses)
{
	List	   *indxqual = best_path->indexqual;
	Index		baserelid;
	List	   *qpqual;
	Expr	   *indxqual_or_expr = NULL;
	List	   *fixed_indxqual;
	List	   *recheck_indxqual;
	List	   *indexids;
	List	   *ixinfo;
	IndexScan  *scan_plan;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->path.parent->relids) == 1);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	baserelid = lfirsti(best_path->path.parent->relids);

	/*
	 * Build list of index OIDs.
	 */
	indexids = NIL;
	foreach(ixinfo, best_path->indexinfo)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ixinfo);

		indexids = lappendi(indexids, index->indexoid);
	}

	/*
	 * The qpqual list must contain all restrictions not automatically
	 * handled by the index.  Normally the predicates in the indxqual are
	 * checked fully by the index, but if the index is "lossy" for a
	 * particular operator (as signaled by the amopreqcheck flag in
	 * pg_amop), then we need to double-check that predicate in qpqual,
	 * because the index may return more tuples than match the predicate.
	 *
	 * Since the indexquals were generated from the restriction clauses given
	 * by scan_clauses, there will normally be some duplications between
	 * the lists.  We get rid of the duplicates, then add back if lossy.
	 */
	if (length(indxqual) > 1)
	{
		/*
		 * Build an expression representation of the indexqual, expanding
		 * the implicit OR and AND semantics of the first- and
		 * second-level lists.
		 */
		List	   *orclauses = NIL;
		List	   *orclause;

		foreach(orclause, indxqual)
		{
			orclauses = lappend(orclauses,
								make_ands_explicit(lfirst(orclause)));
		}
		indxqual_or_expr = make_orclause(orclauses);

		qpqual = set_difference(scan_clauses, makeList1(indxqual_or_expr));
	}
	else if (indxqual != NIL)
	{
		/*
		 * Here, we can simply treat the first sublist as an independent
		 * set of qual expressions, since there is no top-level OR
		 * behavior.
		 */
		qpqual = set_difference(scan_clauses, lfirst(indxqual));
	}
	else
		qpqual = scan_clauses;

	/*
	 * The executor needs a copy with the indexkey on the left of each
	 * clause and with index attr numbers substituted for table ones. This
	 * pass also looks for "lossy" operators.
	 */
	fix_indxqual_references(indxqual, best_path,
							&fixed_indxqual, &recheck_indxqual);

	/*
	 * If there were any "lossy" operators, need to add back the
	 * appropriate qual clauses to the qpqual.	When there is just one
	 * indexscan being performed (ie, we have simple AND semantics), we
	 * can just add the lossy clauses themselves to qpqual.  If we have
	 * OR-of-ANDs, we'd better add the entire original indexqual to make
	 * sure that the semantics are correct.
	 */
	if (recheck_indxqual != NIL)
	{
		if (indxqual_or_expr)
		{
			/* Better do a deep copy of the original scanclauses */
			qpqual = lappend(qpqual, copyObject(indxqual_or_expr));
		}
		else
		{
			/* Subroutine already copied quals, so just append to list */
			Assert(length(recheck_indxqual) == 1);
			qpqual = nconc(qpqual, (List *) lfirst(recheck_indxqual));
		}
	}

	/* Finally ready to build the plan node */
	scan_plan = make_indexscan(tlist,
							   qpqual,
							   baserelid,
							   indexids,
							   fixed_indxqual,
							   indxqual,
							   best_path->indexscandir);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);
	/* use the indexscan-specific rows estimate, not the parent rel's */
	scan_plan->scan.plan.plan_rows = best_path->rows;

	return scan_plan;
}

/*
 * create_tidscan_plan
 *	 Returns a tidscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidScan *
create_tidscan_plan(TidPath *best_path, List *tlist, List *scan_clauses)
{
	TidScan    *scan_plan;
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->path.parent->relids) == 1);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	scan_relid = (Index) lfirsti(best_path->path.parent->relids);

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 best_path->tideval);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_subqueryscan_plan
 *	 Returns a subqueryscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SubqueryScan *
create_subqueryscan_plan(Path *best_path, List *tlist, List *scan_clauses)
{
	SubqueryScan *scan_plan;
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->parent->relids) == 1);
	/* and it must be a subquery */
	Assert(best_path->parent->rtekind == RTE_SUBQUERY);

	scan_relid = (Index) lfirsti(best_path->parent->relids);

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  best_path->parent->subplan);

	return scan_plan;
}

/*
 * create_functionscan_plan
 *	 Returns a functionscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static FunctionScan *
create_functionscan_plan(Path *best_path, List *tlist, List *scan_clauses)
{
	FunctionScan *scan_plan;
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->parent->relids) == 1);
	/* and it must be a function */
	Assert(best_path->parent->rtekind == RTE_FUNCTION);

	scan_relid = (Index) lfirsti(best_path->parent->relids);

	scan_plan = make_functionscan(tlist, scan_clauses, scan_relid);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*****************************************************************************
 *
 *	JOIN METHODS
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(Query *root,
					 NestPath *best_path,
					 List *tlist,
					 List *joinclauses,
					 List *otherclauses,
					 Plan *outer_plan,
					 Plan *inner_plan)
{
	NestLoop   *join_plan;

	if (IsA(inner_plan, IndexScan))
	{
		/*
		 * An index is being used to reduce the number of tuples scanned
		 * in the inner relation.  If there are join clauses being used
		 * with the index, we may remove those join clauses from the list of
		 * clauses that have to be checked as qpquals at the join node ---
		 * but only if there's just one indexscan in the inner path
		 * (otherwise, several different sets of clauses are being ORed
		 * together).
		 *
		 * Note we must compare against indxqualorig not the "fixed" indxqual
		 * (which has index attnos instead of relation attnos, and may have
		 * been commuted as well).
		 */
		IndexScan  *innerscan = (IndexScan *) inner_plan;
		List	   *indxqualorig = innerscan->indxqualorig;

		if (length(indxqualorig) == 1) /* single indexscan? */
		{
			/* No work needed if indxqual refers only to its own relation... */
			if (NumRelids((Node *) indxqualorig) > 1)
				joinclauses = set_difference(joinclauses,
											 lfirst(indxqualorig));
		}
	}

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  outer_plan,
							  inner_plan,
							  best_path->jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->path);

	return join_plan;
}

static MergeJoin *
create_mergejoin_plan(Query *root,
					  MergePath *best_path,
					  List *tlist,
					  List *joinclauses,
					  List *otherclauses,
					  Plan *outer_plan,
					  Plan *inner_plan)
{
	List	   *mergeclauses;
	MergeJoin  *join_plan;

	/*
	 * Remove the mergeclauses from the list of join qual clauses, leaving
	 * the list of quals that must be checked as qpquals.
	 */
	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);
	joinclauses = set_difference(joinclauses, mergeclauses);

	/*
	 * Rearrange mergeclauses, if needed, so that the outer variable
	 * is always on the left.
	 */
	mergeclauses = get_switched_clauses(best_path->path_mergeclauses,
										best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * Create explicit sort nodes for the outer and inner join paths if
	 * necessary.  The sort cost was already accounted for in the path.
	 */
	if (best_path->outersortkeys)
		outer_plan = (Plan *)
			make_sort_from_pathkeys(root,
									outer_plan,
									best_path->jpath.outerjoinpath->parent->relids,
									best_path->outersortkeys);

	if (best_path->innersortkeys)
		inner_plan = (Plan *)
			make_sort_from_pathkeys(root,
									inner_plan,
									best_path->jpath.innerjoinpath->parent->relids,
									best_path->innersortkeys);

	/*
	 * Now we can build the mergejoin node.
	 */
	join_plan = make_mergejoin(tlist,
							   joinclauses,
							   otherclauses,
							   mergeclauses,
							   outer_plan,
							   inner_plan,
							   best_path->jpath.jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}

static HashJoin *
create_hashjoin_plan(Query *root,
					 HashPath *best_path,
					 List *tlist,
					 List *joinclauses,
					 List *otherclauses,
					 Plan *outer_plan,
					 Plan *inner_plan)
{
	List	   *hashclauses;
	HashJoin   *join_plan;
	Hash	   *hash_plan;
	List	   *innerhashkeys;
	List	   *hcl;

	/*
	 * Remove the hashclauses from the list of join qual clauses, leaving
	 * the list of quals that must be checked as qpquals.
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);
	joinclauses = set_difference(joinclauses, hashclauses);

	/*
	 * Rearrange hashclauses, if needed, so that the outer variable
	 * is always on the left.
	 */
	hashclauses = get_switched_clauses(best_path->path_hashclauses,
									   best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * Extract the inner hash keys (right-hand operands of the hashclauses)
	 * to put in the Hash node.
	 */
	innerhashkeys = NIL;
	foreach(hcl, hashclauses)
	{
		innerhashkeys = lappend(innerhashkeys, get_rightop(lfirst(hcl)));
	}

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_plan->targetlist,
						  innerhashkeys,
						  inner_plan);
	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}


/*****************************************************************************
 *
 *	SUPPORTING ROUTINES
 *
 *****************************************************************************/

/*
 * fix_indxqual_references
 *	  Adjust indexqual clauses to the form the executor's indexqual
 *	  machinery needs, and check for recheckable (lossy) index conditions.
 *
 * We have four tasks here:
 *	* Index keys must be represented by Var nodes with varattno set to the
 *	  index's attribute number, not the attribute number in the original rel.
 *	* indxpath.c may have selected an index that is binary-compatible with
 *	  the actual expression operator, but not exactly the same datatype.
 *	  We must replace the expression's operator with the binary-compatible
 *	  equivalent operator that the index will recognize.
 *	* If the index key is on the right, commute the clause to put it on the
 *	  left.  (Someday the executor might not need this, but for now it does.)
 *	* If the indexable operator is marked 'amopreqcheck' in pg_amop, then
 *	  the index is "lossy" for this operator: it may return more tuples than
 *	  actually satisfy the operator condition.	For each such operator, we
 *	  must add (the original form of) the indexqual clause to the "qpquals"
 *	  of the indexscan node, where the operator will be re-evaluated to
 *	  ensure it passes.
 *
 * This code used to be entirely bogus for multi-index scans.  Now it keeps
 * track of which index applies to each subgroup of index qual clauses...
 *
 * Both the input list and the output lists have the form of lists of sublists
 * of qual clauses --- the top-level list has one entry for each indexscan
 * to be performed.  The semantics are OR-of-ANDs.
 *
 * fixed_indexquals receives a modified copy of the indexqual list --- the
 * original is not changed.  Note also that the copy shares no substructure
 * with the original; this is needed in case there is a subplan in it (we need
 * two separate copies of the subplan tree, or things will go awry).
 *
 * recheck_indexquals similarly receives a full copy of whichever clauses
 * need rechecking.
 */
static void
fix_indxqual_references(List *indexquals, IndexPath *index_path,
					  List **fixed_indexquals, List **recheck_indexquals)
{
	List	   *fixed_quals = NIL;
	List	   *recheck_quals = NIL;
	int			baserelid = lfirsti(index_path->path.parent->relids);
	List	   *ixinfo = index_path->indexinfo;
	List	   *i;

	foreach(i, indexquals)
	{
		List	   *indexqual = lfirst(i);
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ixinfo);
		List	   *fixed_qual;
		List	   *recheck_qual;

		fix_indxqual_sublist(indexqual, baserelid, index,
							 &fixed_qual, &recheck_qual);
		fixed_quals = lappend(fixed_quals, fixed_qual);
		if (recheck_qual != NIL)
			recheck_quals = lappend(recheck_quals, recheck_qual);

		ixinfo = lnext(ixinfo);
	}

	*fixed_indexquals = fixed_quals;
	*recheck_indexquals = recheck_quals;
}

/*
 * Fix the sublist of indexquals to be used in a particular scan.
 *
 * For each qual clause, commute if needed to put the indexkey operand on the
 * left, and then fix its varattno.  (We do not need to change the other side
 * of the clause.)	Also change the operator if necessary, and check for
 * lossy index behavior.
 *
 * Returns two lists: the list of fixed indexquals, and the list (usually
 * empty) of original clauses that must be rechecked as qpquals because
 * the index is lossy for this operator type.
 */
static void
fix_indxqual_sublist(List *indexqual, int baserelid, IndexOptInfo *index,
					 List **fixed_quals, List **recheck_quals)
{
	List	   *fixed_qual = NIL;
	List	   *recheck_qual = NIL;
	List	   *i;

	foreach(i, indexqual)
	{
		OpExpr	   *clause = (OpExpr *) lfirst(i);
		OpExpr	   *newclause;
		List	   *leftvarnos;
		Oid			opclass;

		if (!IsA(clause, OpExpr) || length(clause->args) != 2)
			elog(ERROR, "fix_indxqual_sublist: indexqual clause is not binary opclause");

		/*
		 * Make a copy that will become the fixed clause.
		 *
		 * We used to try to do a shallow copy here, but that fails if there
		 * is a subplan in the arguments of the opclause.  So just do a
		 * full copy.
		 */
		newclause = (OpExpr *) copyObject((Node *) clause);

		/*
		 * Check to see if the indexkey is on the right; if so, commute
		 * the clause.	The indexkey should be the side that refers to
		 * (only) the base relation.
		 */
		leftvarnos = pull_varnos((Node *) lfirst(newclause->args));
		if (length(leftvarnos) != 1 || lfirsti(leftvarnos) != baserelid)
			CommuteClause(newclause);
		freeList(leftvarnos);

		/*
		 * Now, determine which index attribute this is, change the
		 * indexkey operand as needed, and get the index opclass.
		 */
		lfirst(newclause->args) = fix_indxqual_operand(lfirst(newclause->args),
													   baserelid,
													   index,
													   &opclass);

		fixed_qual = lappend(fixed_qual, newclause);

		/*
		 * Finally, check to see if index is lossy for this operator. If
		 * so, add (a copy of) original form of clause to recheck list.
		 */
		if (op_requires_recheck(newclause->opno, opclass))
			recheck_qual = lappend(recheck_qual,
								   copyObject((Node *) clause));
	}

	*fixed_quals = fixed_qual;
	*recheck_quals = recheck_qual;
}

static Node *
fix_indxqual_operand(Node *node, int baserelid, IndexOptInfo *index,
					 Oid *opclass)
{
	/*
	 * Remove any binary-compatible relabeling of the indexkey
	 */
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	/*
	 * We represent index keys by Var nodes having the varno of the base
	 * table but varattno equal to the index's attribute number (index
	 * column position).  This is a bit hokey ... would be cleaner to use
	 * a special-purpose node type that could not be mistaken for a
	 * regular Var.  But it will do for now.
	 */
	if (IsA(node, Var))
	{
		/* If it's a var, find which index key position it occupies */
		Assert(index->indproc == InvalidOid);

		if (((Var *) node)->varno == baserelid)
		{
			int			varatt = ((Var *) node)->varattno;
			int			pos;

			for (pos = 0; pos < index->nkeys; pos++)
			{
				if (index->indexkeys[pos] == varatt)
				{
					Node	   *newnode = copyObject(node);

					((Var *) newnode)->varattno = pos + 1;
					/* return the correct opclass, too */
					*opclass = index->classlist[pos];
					return newnode;
				}
			}
		}

		/*
		 * Oops, this Var isn't an indexkey!
		 */
		elog(ERROR, "fix_indxqual_operand: var is not index attribute");
	}

	/*
	 * Else, it must be a func expression matching a functional index.
	 * Since we currently only support single-column functional indexes,
	 * the returned varattno must be 1.
	 */
	Assert(index->indproc != InvalidOid);
	Assert(is_funcclause(node));	/* not a very thorough check, but easy */

	/* classlist[0] is the only class of a functional index */
	*opclass = index->classlist[0];

	return (Node *) makeVar(baserelid, 1, exprType(node), -1, 0);
}

/*
 * get_switched_clauses
 *	  Given a list of merge or hash joinclauses (as RestrictInfo nodes),
 *	  extract the bare clauses, and rearrange the elements within the
 *	  clauses, if needed, so the outer join variable is on the left and
 *	  the inner is on the right.  The original data structure is not touched;
 *	  a modified list is returned.
 */
static List *
get_switched_clauses(List *clauses, List *outerrelids)
{
	List	   *t_list = NIL;
	List	   *i;

	foreach(i, clauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);
		OpExpr	   *clause = (OpExpr *) restrictinfo->clause;

		Assert(is_opclause(clause));
		if (is_subseti(restrictinfo->right_relids, outerrelids))
		{
			/*
			 * Duplicate just enough of the structure to allow commuting
			 * the clause without changing the original list.  Could use
			 * copyObject, but a complete deep copy is overkill.
			 */
			OpExpr	   *temp = makeNode(OpExpr);

			temp->opno = clause->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = clause->opresulttype;
			temp->opretset = clause->opretset;
			temp->args = listCopy(clause->args);
			/* Commute it --- note this modifies the temp node in-place. */
			CommuteClause(temp);
			t_list = lappend(t_list, temp);
		}
		else
			t_list = lappend(t_list, clause);
	}
	return t_list;
}

/*
 * order_qual_clauses
 *		Given a list of qual clauses that will all be evaluated at the same
 *		plan node, sort the list into the order we want to check the quals
 *		in at runtime.
 *
 * Ideally the order should be driven by a combination of execution cost and
 * selectivity, but unfortunately we have so little information about
 * execution cost of operators that it's really hard to do anything smart.
 * For now, we just move any quals that contain SubPlan references (but not
 * InitPlan references) to the end of the list.
 */
static List *
order_qual_clauses(Query *root, List *clauses)
{
	List	   *nosubplans;
	List	   *withsubplans;
	List	   *l;

	/* No need to work hard if the query is subselect-free */
	if (!root->hasSubLinks)
		return clauses;

	nosubplans = withsubplans = NIL;
	foreach(l, clauses)
	{
		Node   *clause = lfirst(l);

		if (contain_subplans(clause))
			withsubplans = lappend(withsubplans, clause);
		else
			nosubplans = lappend(nosubplans, clause);
	}

	return nconc(nosubplans, withsubplans);
}

/*
 * Copy cost and size info from a Path node to the Plan node created from it.
 * The executor won't use this info, but it's needed by EXPLAIN.
 */
static void
copy_path_costsize(Plan *dest, Path *src)
{
	if (src)
	{
		dest->startup_cost = src->startup_cost;
		dest->total_cost = src->total_cost;
		dest->plan_rows = src->parent->rows;
		dest->plan_width = src->parent->width;
	}
	else
	{
		dest->startup_cost = 0;
		dest->total_cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * This is not critical, since the decisions have already been made,
 * but it helps produce more reasonable-looking EXPLAIN output.
 * (Some callers alter the info after copying it.)
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	if (src)
	{
		dest->startup_cost = src->startup_cost;
		dest->total_cost = src->total_cost;
		dest->plan_rows = src->plan_rows;
		dest->plan_width = src->plan_width;
	}
	else
	{
		dest->startup_cost = 0;
		dest->total_cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}


/*****************************************************************************
 *
 *	PLAN NODE BUILDING ROUTINES
 *
 * Some of these are exported because they are called to build plan nodes
 * in contexts where we're not deriving the plan node from a path node.
 *
 *****************************************************************************/

static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   List *indxid,
			   List *indxqual,
			   List *indxqualorig,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indxid = indxid;
	node->indxqual = indxqual;
	node->indxqualorig = indxqualorig;
	node->indxorderdir = indexscandir;

	return node;
}

static TidScan *
make_tidscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 List *tideval)
{
	TidScan    *node = makeNode(TidScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tideval = tideval;

	return node;
}

SubqueryScan *
make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan)
{
	SubqueryScan *node = makeNode(SubqueryScan);
	Plan	   *plan = &node->scan.plan;

	/* cost is figured here for the convenience of prepunion.c */
	copy_plan_costsize(plan, subplan);
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->subplan = subplan;

	return node;
}

static FunctionScan *
make_functionscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid)
{
	FunctionScan *node = makeNode(FunctionScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;

	return node;
}

Append *
make_append(List *appendplans, bool isTarget, List *tlist)
{
	Append	   *node = makeNode(Append);
	Plan	   *plan = &node->plan;
	List	   *subnode;

	/* compute costs from subplan costs */
	plan->startup_cost = 0;
	plan->total_cost = 0;
	plan->plan_rows = 0;
	plan->plan_width = 0;
	foreach(subnode, appendplans)
	{
		Plan	   *subplan = (Plan *) lfirst(subnode);

		if (subnode == appendplans)		/* first node? */
			plan->startup_cost = subplan->startup_cost;
		plan->total_cost += subplan->total_cost;
		plan->plan_rows += subplan->plan_rows;
		if (plan->plan_width < subplan->plan_width)
			plan->plan_width = subplan->plan_width;
	}
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->appendplans = appendplans;
	node->isTarget = isTarget;

	return node;
}

static NestLoop *
make_nestloop(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

static HashJoin *
make_hashjoin(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *hashclauses,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

static Hash *
make_hash(List *tlist, List *hashkeys, Plan *lefttree)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * For plausibility, make startup & total costs equal total cost of
	 * input plan; this only affects EXPLAIN display not decisions.
	 */
	plan->startup_cost = plan->total_cost;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->hashkeys = hashkeys;

	return node;
}

static MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->mergeclauses = mergeclauses;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

/*
 * To use make_sort directly, you must already have marked the tlist
 * with reskey and reskeyop information.  The keys had better be
 * non-redundant, too (ie, there had better be tlist items marked with
 * each key number from 1 to keycount), or the executor will get confused!
 */
Sort *
make_sort(Query *root, List *tlist, Plan *lefttree, int keycount)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;
	Path		sort_path;		/* dummy for result of cost_sort */

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_sort(&sort_path, root, NIL,
			  lefttree->total_cost,
			  lefttree->plan_rows,
			  lefttree->plan_width);
	plan->startup_cost = sort_path.startup_cost;
	plan->total_cost = sort_path.total_cost;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->keycount = keycount;

	return node;
}

/*
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'relids' is the set of relids represented by the input node
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *
 * We must convert the pathkey information into reskey and reskeyop fields
 * of resdom nodes in the sort plan's target list.
 *
 * If the pathkeys include expressions that aren't simple Vars, we will
 * usually need to add resjunk items to the input plan's targetlist to
 * compute these expressions (since the Sort node itself won't do it).
 * If the input plan type isn't one that can do projections, this means
 * adding a Result node just to do the projection.
 */
static Sort *
make_sort_from_pathkeys(Query *root, Plan *lefttree,
						List *relids, List *pathkeys)
{
	List	   *tlist = lefttree->targetlist;
	List	   *sort_tlist;
	List	   *i;
	int			numsortkeys = 0;

	/* Create a new target list for the sort, with sort keys set. */
	sort_tlist = new_unsorted_tlist(tlist);

	foreach(i, pathkeys)
	{
		List	   *keysublist = (List *) lfirst(i);
		PathKeyItem *pathkey = NULL;
		Resdom	   *resdom = NULL;
		List	   *j;

		/*
		 * We can sort by any one of the sort key items listed in this
		 * sublist.  For now, we take the first one that corresponds to an
		 * available Var in the sort_tlist.  If there isn't any, use the
		 * first one that is an expression in the input's vars.
		 *
		 * XXX if we have a choice, is there any way of figuring out which
		 * might be cheapest to execute?  (For example, int4lt is likely
		 * much cheaper to execute than numericlt, but both might appear
		 * in the same pathkey sublist...)	Not clear that we ever will
		 * have a choice in practice, so it may not matter.
		 */
		foreach(j, keysublist)
		{
			pathkey = lfirst(j);
			Assert(IsA(pathkey, PathKeyItem));
			resdom = tlist_member(pathkey->key, sort_tlist);
			if (resdom)
				break;
		}
		if (!resdom)
		{
			/* No matching Var; look for an expression */
			foreach(j, keysublist)
			{
				pathkey = lfirst(j);
				if (is_subseti(pull_varnos(pathkey->key), relids))
					break;
			}
			if (!j)
				elog(ERROR, "make_sort_from_pathkeys: cannot find pathkey item to sort");
			/*
			 * Do we need to insert a Result node?
			 *
			 * Currently, the only non-projection-capable plan type
			 * we can see here is Append.
			 */
			if (IsA(lefttree, Append))
			{
				tlist = new_unsorted_tlist(tlist);
				lefttree = (Plan *) make_result(tlist, NULL, lefttree);
			}
			/*
			 * Add resjunk entry to input's tlist
			 */
			resdom = makeResdom(length(tlist) + 1,
								exprType(pathkey->key),
								exprTypmod(pathkey->key),
								NULL,
								true);
			tlist = lappend(tlist,
							makeTargetEntry(resdom,
											(Expr *) pathkey->key));
			lefttree->targetlist = tlist; /* just in case NIL before */
			/*
			 * Add one to sort node's tlist too.  This will be identical
			 * except we are going to set the sort key info in it.
			 */
			resdom = makeResdom(length(sort_tlist) + 1,
								exprType(pathkey->key),
								exprTypmod(pathkey->key),
								NULL,
								true);
			sort_tlist = lappend(sort_tlist,
								 makeTargetEntry(resdom,
												 (Expr *) pathkey->key));
		}
		/*
		 * The resdom might be already marked as a sort key, if the
		 * pathkeys contain duplicate entries.	(This can happen in
		 * scenarios where multiple mergejoinable clauses mention the same
		 * var, for example.) In that case the current pathkey is
		 * essentially a no-op, because only one value can be seen within
		 * any subgroup where it would be consulted.  We can ignore it.
		 */
		if (resdom->reskey == 0)
		{
			/* OK, mark it as a sort key and set the sort operator */
			resdom->reskey = ++numsortkeys;
			resdom->reskeyop = pathkey->sortop;
		}
	}

	Assert(numsortkeys > 0);

	return make_sort(root, sort_tlist, lefttree, numsortkeys);
}

Material *
make_material(List *tlist, Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

Agg *
make_agg(Query *root, List *tlist, List *qual,
		 AggStrategy aggstrategy,
		 int numGroupCols, AttrNumber *grpColIdx,
		 long numGroups, int numAggs,
		 Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	Path		agg_path;		/* dummy for result of cost_agg */
	QualCost	qual_cost;

	node->aggstrategy = aggstrategy;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->numGroups = numGroups;

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_agg(&agg_path, root,
			 aggstrategy, numAggs,
			 numGroupCols, numGroups,
			 lefttree->startup_cost,
			 lefttree->total_cost,
			 lefttree->plan_rows);
	plan->startup_cost = agg_path.startup_cost;
	plan->total_cost = agg_path.total_cost;

	/*
	 * We will produce a single output tuple if not grouping,
	 * and a tuple per group otherwise.
	 */
	if (aggstrategy == AGG_PLAIN)
		plan->plan_rows = 1;
	else
		plan->plan_rows = numGroups;

	/*
	 * We also need to account for the cost of evaluation of the qual
	 * (ie, the HAVING clause) and the tlist.  Note that cost_qual_eval
	 * doesn't charge anything for Aggref nodes; this is okay since
	 * they are really comparable to Vars.
	 *
	 * See notes in grouping_planner about why this routine and make_group
	 * are the only ones in this file that worry about tlist eval cost.
	 */
	if (qual)
	{
		cost_qual_eval(&qual_cost, qual);
		plan->startup_cost += qual_cost.startup;
		plan->total_cost += qual_cost.startup;
		plan->total_cost += qual_cost.per_tuple * plan->plan_rows;
	}
	cost_qual_eval(&qual_cost, tlist);
	plan->startup_cost += qual_cost.startup;
	plan->total_cost += qual_cost.startup;
	plan->total_cost += qual_cost.per_tuple * plan->plan_rows;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = (Plan *) NULL;

	return node;
}

Group *
make_group(Query *root,
		   List *tlist,
		   int numGroupCols,
		   AttrNumber *grpColIdx,
		   double numGroups,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;
	Path		group_path;		/* dummy for result of cost_group */
	QualCost	qual_cost;

	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_group(&group_path, root,
			   numGroupCols, numGroups,
			   lefttree->startup_cost,
			   lefttree->total_cost,
			   lefttree->plan_rows);
	plan->startup_cost = group_path.startup_cost;
	plan->total_cost = group_path.total_cost;

	/* One output tuple per estimated result group */
	plan->plan_rows = numGroups;

	/*
	 * We also need to account for the cost of evaluation of the tlist.
	 *
	 * XXX this double-counts the cost of evaluation of any expressions
	 * used for grouping, since in reality those will have been evaluated
	 * at a lower plan level and will only be copied by the Group node.
	 * Worth fixing?
	 *
	 * See notes in grouping_planner about why this routine and make_agg
	 * are the only ones in this file that worry about tlist eval cost.
	 */
	cost_qual_eval(&qual_cost, tlist);
	plan->startup_cost += qual_cost.startup;
	plan->total_cost += qual_cost.startup;
	plan->total_cost += qual_cost.per_tuple * plan->plan_rows;

	plan->qual = NIL;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = (Plan *) NULL;

	return node;
}

/*
 * distinctList is a list of SortClauses, identifying the targetlist items
 * that should be considered by the Unique filter.
 */
Unique *
make_unique(List *tlist, Plan *lefttree, List *distinctList)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			numCols = length(distinctList);
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	List	   *slitem;

	copy_plan_costsize(plan, lefttree);

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We
	 * assume all columns get compared at most of the tuples.  (XXX probably
	 * this is an overestimate.)
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows * numCols;

	/*
	 * plan->plan_rows is left as a copy of the input subplan's plan_rows;
	 * ie, we assume the filter removes nothing.  The caller must alter this
	 * if he has a better idea.
	 */

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortClause list into array of attr indexes, as wanted by
	 * exec
	 */
	Assert(numCols > 0);
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

	foreach(slitem, distinctList)
	{
		SortClause *sortcl = (SortClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, tlist);

		uniqColIdx[keyno++] = tle->resdom->resno;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;

	return node;
}

/*
 * distinctList is a list of SortClauses, identifying the targetlist items
 * that should be considered by the SetOp filter.
 */

SetOp *
make_setop(SetOpCmd cmd, List *tlist, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx)
{
	SetOp	   *node = makeNode(SetOp);
	Plan	   *plan = &node->plan;
	int			numCols = length(distinctList);
	int			keyno = 0;
	AttrNumber *dupColIdx;
	List	   *slitem;

	copy_plan_costsize(plan, lefttree);

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We
	 * assume all columns get compared at most of the tuples.
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows * numCols;

	/*
	 * We make the unsupported assumption that there will be 10% as many
	 * tuples out as in.  Any way to do better?
	 */
	plan->plan_rows *= 0.1;
	if (plan->plan_rows < 1)
		plan->plan_rows = 1;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortClause list into array of attr indexes, as wanted by
	 * exec
	 */
	Assert(numCols > 0);
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

	foreach(slitem, distinctList)
	{
		SortClause *sortcl = (SortClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, tlist);

		dupColIdx[keyno++] = tle->resdom->resno;
	}

	node->cmd = cmd;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->flagColIdx = flagColIdx;

	return node;
}

Limit *
make_limit(List *tlist, Plan *lefttree,
		   Node *limitOffset, Node *limitCount)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * If offset/count are constants, adjust the output rows count and
	 * costs accordingly.  This is only a cosmetic issue if we are at top
	 * level, but if we are building a subquery then it's important to
	 * report correct info to the outer planner.
	 */
	if (limitOffset && IsA(limitOffset, Const))
	{
		Const	   *limito = (Const *) limitOffset;
		int32		offset = DatumGetInt32(limito->constvalue);

		if (!limito->constisnull && offset > 0)
		{
			if (offset > plan->plan_rows)
				offset = (int32) plan->plan_rows;
			if (plan->plan_rows > 0)
				plan->startup_cost +=
					(plan->total_cost - plan->startup_cost)
					* ((double) offset) / plan->plan_rows;
			plan->plan_rows -= offset;
			if (plan->plan_rows < 1)
				plan->plan_rows = 1;
		}
	}
	if (limitCount && IsA(limitCount, Const))
	{
		Const	   *limitc = (Const *) limitCount;
		int32		count = DatumGetInt32(limitc->constvalue);

		if (!limitc->constisnull && count >= 0)
		{
			if (count > plan->plan_rows)
				count = (int32) plan->plan_rows;
			if (plan->plan_rows > 0)
				plan->total_cost = plan->startup_cost +
					(plan->total_cost - plan->startup_cost)
					* ((double) count) / plan->plan_rows;
			plan->plan_rows = count;
			if (plan->plan_rows < 1)
				plan->plan_rows = 1;
		}
	}

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;

	return node;
}

Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	if (subplan)
		copy_plan_costsize(plan, subplan);
	else
	{
		plan->startup_cost = 0;
		plan->total_cost = cpu_tuple_cost;
		plan->plan_rows = 1;	/* wrong if we have a set-valued function? */
		plan->plan_width = 0;	/* XXX try to be smarter? */
	}

	if (resconstantqual)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, (List *) resconstantqual);
		/* resconstantqual is evaluated once at startup */
		plan->startup_cost += qual_cost.startup + qual_cost.per_tuple;
		plan->total_cost += qual_cost.startup + qual_cost.per_tuple;
	}

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

	return node;
}
