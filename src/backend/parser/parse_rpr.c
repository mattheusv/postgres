/*-------------------------------------------------------------------------
 *
 * parse_rpr.c
 *	  Handle Row Pattern Recognition clauses in parser.
 *
 * This file transforms RPR-related clauses from raw parse tree to planner
 * structures during query analysis:
 *   - Validates frame options (must start at CURRENT ROW, no EXCLUDE)
 *   - Validates PATTERN variable count (max RPR_VARID_MAX)
 *   - Transforms DEFINE clause into TargetEntry list
 *   - Stores PATTERN/SKIP TO/INITIAL clauses for planner
 *
 * Pattern optimization and compilation to NFA bytecode happens later
 * in the planner (see optimizer/plan/rpr.c).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_rpr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/rpr.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_rpr.h"
#include "parser/parse_target.h"

/* Forward declarations */
static void validateRPRPatternVarCount(ParseState *pstate, RPRPatternNode *node,
									   List *rpDefs, List **varNames);
static List *transformDefineClause(ParseState *pstate, WindowClause *wc,
								   WindowDef *windef, List **targetlist);
static void check_rpr_nav_expr(RPRNavExpr *nav, ParseState *pstate);
static bool check_rpr_nav_nesting_walker(Node *node, void *context);

/*
 * transformRPR
 *		Process Row Pattern Recognition related clauses.
 *
 * Validates and transforms RPR clauses from parse tree to planner structures:
 *   - Validates frame options (must start at CURRENT ROW, no EXCLUDE)
 *   - Stores AFTER MATCH SKIP TO clause
 *   - Stores SEEK/INITIAL clause
 *   - Transforms DEFINE clause into TargetEntry list
 *   - Stores PATTERN AST for deparsing (optimization happens in planner)
 *
 * Returns early if windef has no rpCommonSyntax (non-RPR window).
 */
void
transformRPR(ParseState *pstate, WindowClause *wc, WindowDef *windef,
			 List **targetlist)
{
	/* Window definition must exist when called */
	Assert(windef != NULL);

	/*
	 * Row Pattern Common Syntax clause exists?
	 */
	if (windef->rpCommonSyntax == NULL)
		return;

	/* Check Frame options */

	/* Frame type must be "ROW" */
	if (wc->frameOptions & FRAMEOPTION_GROUPS)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("cannot use FRAME option GROUPS with row pattern recognition"),
				 errhint("Use ROWS instead."),
				 parser_errposition(pstate,
									windef->frameLocation >= 0 ?
									windef->frameLocation : windef->location)));
	if (wc->frameOptions & FRAMEOPTION_RANGE)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("cannot use FRAME option RANGE with row pattern recognition"),
				 errhint("Use ROWS instead."),
				 parser_errposition(pstate,
									windef->frameLocation >= 0 ?
									windef->frameLocation : windef->location)));

	/* Frame must start at current row */
	if ((wc->frameOptions & FRAMEOPTION_START_CURRENT_ROW) == 0)
	{
		const char *frameType = "ROWS";
		const char *startBound = "unknown";

		/* Determine current start bound */
		if (wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
			startBound = "UNBOUNDED PRECEDING";
		else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING)
			startBound = "offset PRECEDING";
		else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING)
			startBound = "offset FOLLOWING";

		/* At least one valid frame start option should be set */
		Assert((wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING) ||
			   (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING) ||
			   (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING));

		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("FRAME must start at CURRENT ROW when using row pattern recognition"),
				 errdetail("Current frame starts with %s.", startBound),
				 errhint("Use: %s BETWEEN CURRENT ROW AND ...", frameType),
				 parser_errposition(pstate, windef->frameLocation >= 0 ? windef->frameLocation : windef->location)));
	}

	/* EXCLUDE options are not permitted */
	if ((wc->frameOptions & FRAMEOPTION_EXCLUSION) != 0)
	{
		const char *excludeType = "EXCLUDE";

		/* Determine which EXCLUDE option was used */
		if (wc->frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW)
			excludeType = "EXCLUDE CURRENT ROW";
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_GROUP)
			excludeType = "EXCLUDE GROUP";
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_TIES)
			excludeType = "EXCLUDE TIES";

		/* At least one valid exclude option should be set */
		Assert((wc->frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW) ||
			   (wc->frameOptions & FRAMEOPTION_EXCLUDE_GROUP) ||
			   (wc->frameOptions & FRAMEOPTION_EXCLUDE_TIES));

		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("cannot use EXCLUDE options with row pattern recognition"),
				 errdetail("Frame definition includes %s.", excludeType),
				 errhint("Remove the EXCLUDE clause from the window definition."),
				 parser_errposition(pstate, windef->excludeLocation >= 0 ? windef->excludeLocation : windef->location)));
	}

	/* Transform AFTER MATCH SKIP TO clause */
	wc->rpSkipTo = windef->rpCommonSyntax->rpSkipTo;

	/* Transform SEEK or INITIAL clause */
	wc->initial = windef->rpCommonSyntax->initial;

	/* Transform DEFINE clause into list of TargetEntry's */
	wc->defineClause = transformDefineClause(pstate, wc, windef, targetlist);

	/* Store PATTERN AST for deparsing */
	wc->rpPattern = windef->rpCommonSyntax->rpPattern;
}

/*
 * validateRPRPatternVarCount
 *		Validate that PATTERN variables don't exceed RPR_VARID_MAX.
 *
 * Recursively traverses the pattern tree, collecting unique variable names.
 * Throws an error if the number of unique variables exceeds RPR_VARID_MAX.
 *
 * If rpDefs is non-NULL, DEFINE variable names are also collected into
 * varNames so that transformColumnRef can distinguish pattern variable
 * qualifiers from FROM-clause range variables.
 *
 * varNames is both input and output: existing names are preserved, new ones added.
 */
static void
validateRPRPatternVarCount(ParseState *pstate, RPRPatternNode *node,
						   List *rpDefs, List **varNames)
{
	ListCell   *lc;

	/* Pattern node must exist - parser always provides non-NULL root */
	Assert(node != NULL);

	check_stack_depth();

	switch (node->nodeType)
	{
		case RPR_PATTERN_VAR:
			/* Add variable name if not already in list */
			{
				bool		found = false;

				foreach(lc, *varNames)
				{
					if (strcmp(strVal(lfirst(lc)), node->varName) == 0)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/* Check against RPR_VARID_MAX before adding */
					if (list_length(*varNames) >= RPR_VARID_MAX)
						ereport(ERROR,
								(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								 errmsg("too many pattern variables"),
								 errdetail("Maximum is %d.", RPR_VARID_MAX),
								 parser_errposition(pstate,
													exprLocation((Node *) node))));

					*varNames = lappend(*varNames, makeString(pstrdup(node->varName)));
				}
			}
			break;

		case RPR_PATTERN_SEQ:
		case RPR_PATTERN_ALT:
		case RPR_PATTERN_GROUP:
			/* Recurse into children */
			foreach(lc, node->children)
			{
				validateRPRPatternVarCount(pstate, (RPRPatternNode *) lfirst(lc),
										   NULL, varNames);
			}
			break;
	}

	/*
	 * After the top-level call, also collect DEFINE variable names that are
	 * not already in the list.  This is only done once at the outermost
	 * recursion level, detected by rpDefs being non-NULL (recursive calls
	 * pass NULL).
	 */
	if (rpDefs)
	{
		foreach(lc, rpDefs)
		{
			ResTarget  *rt = (ResTarget *) lfirst(lc);
			ListCell   *lc2;
			bool		found = false;

			foreach(lc2, *varNames)
			{
				if (strcmp(strVal(lfirst(lc2)), rt->name) == 0)
				{
					found = true;
					break;
				}
			}
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("DEFINE variable \"%s\" is not used in PATTERN",
								rt->name),
						 parser_errposition(pstate, rt->location)));
		}
	}
}

/*
 * transformDefineClause
 *		Process DEFINE clause and transform ResTarget into list of TargetEntry.
 *
 * First:
 *   1. Validates PATTERN variable count and collects RPR variable names
 *
 * Then for each DEFINE variable:
 *   2. Checks for duplicate variable names in DEFINE clause
 *   3. Transforms expression via transformExpr() and ensures referenced
 *      Var nodes are present in the query targetlist (via pull_var_clause)
 *   4. Creates defineClause entry with proper resname (pattern variable name)
 *   5. Coerces expressions to boolean type
 *   6. Marks column origins and assigns collation information
 *
 * Note: Variables not in DEFINE are evaluated as TRUE by the executor.
 * Variables in DEFINE but not in PATTERN are rejected as an error.
 *
 * XXX Pattern variable qualified column references in DEFINE (e.g.
 * "A.price") are not yet supported.  Currently rejected by
 * transformColumnRef in parse_expr.c via the p_rpr_pattern_vars check.
 */
static List *
transformDefineClause(ParseState *pstate, WindowClause *wc, WindowDef *windef,
					  List **targetlist)
{
	ListCell   *lc,
			   *l;
	ResTarget  *restarget,
			   *r;
	List	   *restargets;
	List	   *defineClause = NIL;
	char	   *name;
	List	   *patternVarNames = NIL;

	/*
	 * If Row Definition Common Syntax exists, DEFINE clause must exist. (the
	 * raw parser should have already checked it.)
	 */
	Assert(windef->rpCommonSyntax->rpDefs != NULL);

	/*
	 * Validate PATTERN variable count and collect all RPR variable names
	 * (PATTERN + DEFINE) for use in transformColumnRef.
	 */
	validateRPRPatternVarCount(pstate, windef->rpCommonSyntax->rpPattern,
							   windef->rpCommonSyntax->rpDefs,
							   &patternVarNames);
	pstate->p_rpr_pattern_vars = patternVarNames;

	/*
	 * Check for duplicate row pattern definition variables.  The standard
	 * requires that no two row pattern definition variable names shall be
	 * equivalent.
	 */
	restargets = NIL;
	foreach(lc, windef->rpCommonSyntax->rpDefs)
	{
		TargetEntry *teDefine;

		restarget = (ResTarget *) lfirst(lc);
		name = restarget->name;

		foreach(l, restargets)
		{
			char	   *n;

			r = (ResTarget *) lfirst(l);
			n = r->name;

			if (!strcmp(n, name))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("DEFINE variable \"%s\" appears more than once",
								name),
						 parser_errposition(pstate, exprLocation((Node *) r))));
		}

		restargets = lappend(restargets, restarget);

		/*
		 * Transform the DEFINE expression.  We must NOT add the whole
		 * expression to the query targetlist, because it may contain
		 * RPRNavExpr nodes (PREV/NEXT/FIRST/LAST) that can only be evaluated
		 * inside the owning WindowAgg.
		 *
		 * Instead, we transform the expression directly and only ensure that
		 * the individual Var nodes it references are present in the
		 * targetlist, so the planner can propagate the referenced columns.
		 */
		{
			Node	   *expr;
			List	   *vars;
			ListCell   *lc2;

			expr = transformExpr(pstate, restarget->val,
								 EXPR_KIND_RPR_DEFINE);

			/*
			 * Pull out Var nodes from the transformed expression and ensure
			 * each one is present in the targetlist.  This is needed so the
			 * planner propagates the referenced columns through the plan
			 * tree, making them available to the WindowAgg's DEFINE
			 * evaluation.
			 */
			vars = pull_var_clause(expr, 0);
			foreach(lc2, vars)
			{
				Var		   *var = (Var *) lfirst(lc2);
				bool		found = false;
				ListCell   *tl;

				foreach(tl, *targetlist)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(tl);

					if (IsA(tle->expr, Var) &&
						((Var *) tle->expr)->varno == var->varno &&
						((Var *) tle->expr)->varattno == var->varattno)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					TargetEntry *newtle;

					newtle = makeTargetEntry((Expr *) copyObject(var),
											 list_length(*targetlist) + 1,
											 NULL,
											 true);
					*targetlist = lappend(*targetlist, newtle);
				}
			}
			list_free(vars);

			/* Build the defineClause entry directly from the transformed expr */
			teDefine = makeTargetEntry((Expr *) expr,
									   list_length(defineClause) + 1,
									   pstrdup(name),
									   true);
		}

		/* build transformed DEFINE clause (list of TargetEntry) */
		defineClause = lappend(defineClause, teDefine);
	}
	list_free(restargets);
	pstate->p_rpr_pattern_vars = NIL;

	/*
	 * Make sure that the row pattern definition search condition is a boolean
	 * expression.
	 */
	foreach_ptr(TargetEntry, te, defineClause)
		te->expr = (Expr *) coerce_to_boolean(pstate, (Node *) te->expr, "DEFINE");

	/* check for nested PREV/NEXT and missing column references */
	foreach_ptr(TargetEntry, te, defineClause)
		(void) check_rpr_nav_nesting_walker((Node *) te->expr, pstate);

	/* mark column origins */
	markTargetListOrigins(pstate, defineClause);

	/* mark all nodes in the DEFINE clause tree with collation information */
	assign_expr_collations(pstate, (Node *) defineClause);

	return defineClause;
}

/*
 * check_rpr_nav_expr
 *		Validate a single RPRNavExpr node by walking its arg and offset_arg
 *		subtrees in a single pass each.  Check for illegal nesting, missing
 *		column references, and non-constant offset expressions.
 *
 * Nesting rules (SQL standard 5.6.4):
 *   - PREV/NEXT wrapping FIRST/LAST: allowed (compound navigation)
 *   - FIRST/LAST wrapping PREV/NEXT: prohibited
 *   - Same-category nesting (PREV inside PREV, FIRST inside FIRST, etc.):
 *     prohibited
 */
typedef struct
{
	int			nav_count;		/* number of RPRNavExpr nodes found */
	bool		has_column_ref; /* Var found */
	RPRNavKind	inner_kind;		/* kind of first (outermost) nested RPRNavExpr */
} NavCheckResult;

static bool
nav_check_walker(Node *node, void *context)
{
	NavCheckResult *result = (NavCheckResult *) context;

	if (node == NULL)
		return false;
	if (IsA(node, RPRNavExpr))
	{
		if (result->nav_count == 0)
			result->inner_kind = ((RPRNavExpr *) node)->kind;
		result->nav_count++;
	}
	if (IsA(node, Var))
		result->has_column_ref = true;

	return expression_tree_walker(node, nav_check_walker, context);
}

static void
check_rpr_nav_expr(RPRNavExpr *nav, ParseState *pstate)
{
	NavCheckResult result;
	bool		outer_is_physical = (nav->kind == RPR_NAV_PREV ||
									 nav->kind == RPR_NAV_NEXT);

	/* Check arg subtree: nesting + column reference in one walk */
	memset(&result, 0, sizeof(result));
	(void) nav_check_walker((Node *) nav->arg, &result);

	if (result.nav_count > 0)
	{
		bool		inner_is_physical = (result.inner_kind == RPR_NAV_PREV ||
										 result.inner_kind == RPR_NAV_NEXT);

		if (outer_is_physical && !inner_is_physical)
		{
			/*
			 * PREV/NEXT wrapping FIRST/LAST: compound navigation per SQL
			 * standard 5.6.4.  Flatten the nested RPRNavExpr into a single
			 * compound node.  The inner RPRNavExpr must be the direct arg of
			 * the outer; expressions like PREV(val + FIRST(v)) are not valid
			 * compound navigation.
			 */
			RPRNavExpr *inner;

			/* Reject triple-or-deeper nesting (e.g. PREV(FIRST(PREV(x)))) */
			if (result.nav_count > 1)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot nest row pattern navigation more than two levels deep"),
						 errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
						 parser_errposition(pstate, nav->location)));

			if (!IsA(nav->arg, RPRNavExpr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("row pattern navigation operation must be a direct argument of the outer navigation"),
						 errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
						 parser_errposition(pstate, nav->location)));
			inner = (RPRNavExpr *) nav->arg;

			/* Determine compound kind */
			if (nav->kind == RPR_NAV_PREV && inner->kind == RPR_NAV_FIRST)
				nav->kind = RPR_NAV_PREV_FIRST;
			else if (nav->kind == RPR_NAV_PREV && inner->kind == RPR_NAV_LAST)
				nav->kind = RPR_NAV_PREV_LAST;
			else if (nav->kind == RPR_NAV_NEXT && inner->kind == RPR_NAV_FIRST)
				nav->kind = RPR_NAV_NEXT_FIRST;
			else if (nav->kind == RPR_NAV_NEXT && inner->kind == RPR_NAV_LAST)
				nav->kind = RPR_NAV_NEXT_LAST;

			/* Move outer offset to compound_offset_arg */
			nav->compound_offset_arg = nav->offset_arg;

			/* Move inner offset and arg up */
			nav->offset_arg = inner->offset_arg;
			nav->arg = inner->arg;

			/* No further nesting check needed - already validated */
			return;
		}
		else if (!outer_is_physical && inner_is_physical)
		{
			/* FIRST/LAST wrapping PREV/NEXT: prohibited by standard */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("FIRST and LAST cannot contain PREV or NEXT"),
					 errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
					 parser_errposition(pstate, nav->location)));
		}
		else if (outer_is_physical && inner_is_physical)
		{
			/* PREV/NEXT wrapping PREV/NEXT: prohibited */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("PREV and NEXT cannot contain PREV or NEXT"),
					 errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
					 parser_errposition(pstate, nav->location)));
		}
		else
		{
			/* FIRST/LAST wrapping FIRST/LAST: prohibited */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("FIRST and LAST cannot contain FIRST or LAST"),
					 errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
					 parser_errposition(pstate, nav->location)));
		}
	}
	if (!result.has_column_ref)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("argument of row pattern navigation operation must include at least one column reference"),
				 parser_errposition(pstate, nav->location)));

	/* Check offset_arg: column ref + volatile in one walk */
	if (nav->offset_arg != NULL)
	{
		memset(&result, 0, sizeof(result));
		(void) nav_check_walker((Node *) nav->offset_arg, &result);

		if (result.has_column_ref ||
			contain_volatile_functions((Node *) nav->offset_arg))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("row pattern navigation offset must be a run-time constant"),
					 parser_errposition(pstate, nav->location)));
	}
}

/*
 * check_rpr_nav_nesting_walker
 *		Walk the DEFINE clause expression tree and validate each RPRNavExpr.
 */
static bool
check_rpr_nav_nesting_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, RPRNavExpr))
	{
		check_rpr_nav_expr((RPRNavExpr *) node, (ParseState *) context);
		/* don't recurse into arg; nesting already checked above */
		return false;
	}
	return expression_tree_walker(node, check_rpr_nav_nesting_walker, context);
}
