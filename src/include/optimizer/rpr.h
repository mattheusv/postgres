/*-------------------------------------------------------------------------
 *
 * rpr.h
 *	  Row Pattern Recognition pattern compilation for planner
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/rpr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OPTIMIZER_RPR_H
#define OPTIMIZER_RPR_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

/* Limits and special values */
/*
 * Maximum number of unique pattern variables (varId 0 to RPR_VARID_MAX - 1).
 * Values from RPR_VARID_BEGIN (252) onward are reserved for control elements.
 */
#define RPR_VARID_MAX		251
#define RPR_QUANTITY_INF	INT32_MAX	/* unbounded quantifier */
#define RPR_COUNT_MAX		INT32_MAX	/* max runtime count (NFA state) */
#define RPR_ELEMIDX_MAX		INT16_MAX	/* max pattern elements */
#define RPR_ELEMIDX_INVALID	((RPRElemIdx) -1)	/* invalid index */
#define RPR_DEPTH_MAX		(UINT8_MAX - 1) /* max pattern nesting depth: 254 */
#define RPR_DEPTH_NONE		UINT8_MAX	/* no enclosing group (top-level) */

/* Special varId values for control elements (252-255) */
#define RPR_VARID_BEGIN		((RPRVarId) 252)	/* group begin */
#define RPR_VARID_END		((RPRVarId) 253)	/* group end */
#define RPR_VARID_ALT		((RPRVarId) 254)	/* alternation start */
#define RPR_VARID_FIN		((RPRVarId) 255)	/* pattern finish */

/* Element flags */
#define RPR_ELEM_RELUCTANT			0x01	/* reluctant (non-greedy)
											 * quantifier */
#define RPR_ELEM_EMPTY_LOOP			0x02	/* END: group body can produce
											 * empty match */
#define RPR_ELEM_ABSORBABLE_BRANCH	0x04	/* element in absorbable region */
#define RPR_ELEM_ABSORBABLE			0x08	/* absorption judgment point */

/* Accessor macros for RPRPatternElement */
#define RPRElemIsReluctant(e)			(((e)->flags & RPR_ELEM_RELUCTANT) != 0)
#define RPRElemCanEmptyLoop(e)			(((e)->flags & RPR_ELEM_EMPTY_LOOP) != 0)
#define RPRElemIsAbsorbableBranch(e)	(((e)->flags & RPR_ELEM_ABSORBABLE_BRANCH) != 0)
#define RPRElemIsAbsorbable(e)			(((e)->flags & RPR_ELEM_ABSORBABLE) != 0)
#define RPRElemIsVar(e)			((e)->varId <= RPR_VARID_MAX)
#define RPRElemIsBegin(e)		((e)->varId == RPR_VARID_BEGIN)
#define RPRElemIsEnd(e)			((e)->varId == RPR_VARID_END)
#define RPRElemIsAlt(e)			((e)->varId == RPR_VARID_ALT)
#define RPRElemIsFin(e)			((e)->varId == RPR_VARID_FIN)
#define RPRElemCanSkip(e)		((e)->min == 0)

extern List *collectPatternVariables(RPRPatternNode *pattern);
extern void buildDefineVariableList(List *defineClause,
									List **defineVariableList);
extern RPRPattern *buildRPRPattern(RPRPatternNode *pattern, List *defineVariableList,
								   RPSkipTo rpSkipTo, int frameOptions,
								   bool hasMatchStartDependent);

#endif							/* OPTIMIZER_RPR_H */
