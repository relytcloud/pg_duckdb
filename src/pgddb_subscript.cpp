/*
 * pgddb_subscript.cpp -- shared SubscriptRoutines for "DuckDB row" /
 * "DuckDB struct" passthrough pseudo-types. Each consumer wraps the
 * routines in its own DECLARE_PG_FUNCTION entry point.
 *
 * Error messages compute the container type's bare schema-qualified name
 * (e.g. "duckdb.row", "ducklake.duckdb_row") via NameStr lookup, so the
 * consumer's pseudo-type name shows up without per-consumer wiring and
 * without format_type_be's reserved-keyword quoting.
 */

#include "pgddb/pgddb_subscript.h"

extern "C" {
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "pgddb/vendor/pg_list.hpp"
}

namespace pgddb::pg {

namespace {

/* "<schema>.<typname>" without format_type_be quoting. */
char *
bare_type_name(Oid type_oid) {
	HeapTuple tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tp))
		return pstrdup("?");
	Form_pg_type typ = (Form_pg_type)GETSTRUCT(tp);
	char *schema = get_namespace_name(typ->typnamespace);
	char *result = psprintf("%s.%s", schema ? schema : "?", NameStr(typ->typname));
	ReleaseSysCache(tp);
	return result;
}

/*
 * If the consumer ships an "unresolved_type" pseudo-type in the same
 * schema as its row/struct types, use that as the subscript result type
 * so chained `::int`, `+5` etc. resolve via the consumer's
 * unresolved_type cast/operator table. Otherwise fall back to the
 * container type itself (chained `[...]` and `::type` resolve via the
 * container's WITH INOUT casts).
 */
Oid
unresolved_type_for_container(Oid container_oid) {
	HeapTuple tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(container_oid));
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	Oid container_nsp = ((Form_pg_type)GETSTRUCT(tp))->typnamespace;
	ReleaseSysCache(tp);
	return GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, PointerGetDatum("unresolved_type"),
	                       ObjectIdGetDatum(container_nsp));
}

void
duckdb_row_transform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate, bool is_slice,
                     bool is_assignment) {
	char *type_name = bare_type_name(sbsref->refcontainertype);

	if (is_assignment) {
		elog(ERROR, "Assignment to %s is not supported", type_name);
	}
	if (indirection == NIL) {
		elog(ERROR, "Subscripting %s with an empty subscript is not supported", type_name);
	}

	bool first = true;
	foreach_node(A_Indices, subscript, indirection) {
		if (first) {
			if (!subscript->uidx || subscript->lidx) {
				elog(ERROR, "Creating a slice out of %s is not supported", type_name);
			}
			Node *expr = transformExpr(pstate, subscript->uidx, pstate->p_expr_kind);
			int expr_location = exprLocation(subscript->uidx);
			Oid expr_type = exprType(expr);
			Node *coerced = coerce_to_target_type(pstate, expr, expr_type, TEXTOID, -1, COERCION_IMPLICIT,
			                                       COERCE_IMPLICIT_CAST, expr_location);
			if (!coerced) {
				ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
				                errmsg("%s subscript must have text type", type_name),
				                parser_errposition(pstate, expr_location)));
			}
			if (!IsA(expr, Const)) {
				ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
				                errmsg("%s subscript must be a constant", type_name),
				                parser_errposition(pstate, expr_location)));
			}
			if (castNode(Const, expr)->constisnull) {
				ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				                errmsg("%s subscript cannot be NULL", type_name),
				                parser_errposition(pstate, expr_location)));
			}
			sbsref->refupperindexpr = lappend(sbsref->refupperindexpr, coerced);
			if (is_slice) {
				sbsref->reflowerindexpr = lappend(sbsref->reflowerindexpr, NULL);
			}
			first = false;
			continue;
		}
		Node *upper = subscript->uidx ? transformExpr(pstate, subscript->uidx, pstate->p_expr_kind) : NULL;
		sbsref->refupperindexpr = lappend(sbsref->refupperindexpr, upper);
		if (is_slice) {
			Node *lower = subscript->lidx ? transformExpr(pstate, subscript->lidx, pstate->p_expr_kind) : NULL;
			sbsref->reflowerindexpr = lappend(sbsref->reflowerindexpr, lower);
		}
	}

	Oid unresolved = unresolved_type_for_container(sbsref->refcontainertype);
	sbsref->refrestype = OidIsValid(unresolved) ? unresolved : sbsref->refcontainertype;
	sbsref->reftypmod = -1;
}

void
duckdb_loose_transform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate, bool is_slice,
                       bool is_assignment) {
	char *type_name = bare_type_name(sbsref->refcontainertype);

	if (is_assignment) {
		elog(ERROR, "Assignment to %s is not supported", type_name);
	}
	if (indirection == NIL) {
		elog(ERROR, "Subscripting %s with an empty subscript is not supported", type_name);
	}

	foreach_node(A_Indices, subscript, indirection) {
		Node *upper = subscript->uidx ? transformExpr(pstate, subscript->uidx, pstate->p_expr_kind) : NULL;
		sbsref->refupperindexpr = lappend(sbsref->refupperindexpr, upper);
		if (is_slice) {
			Node *lower = subscript->lidx ? transformExpr(pstate, subscript->lidx, pstate->p_expr_kind) : NULL;
			sbsref->reflowerindexpr = lappend(sbsref->reflowerindexpr, lower);
		}
	}

	Oid unresolved = unresolved_type_for_container(sbsref->refcontainertype);
	sbsref->refrestype = OidIsValid(unresolved) ? unresolved : sbsref->refcontainertype;
	sbsref->reftypmod = -1;
}

/* The planner hook reroutes any row-var query to DuckDB, so the exec
 * stubs below should never fire. They error loudly if they do. */
bool
duckdb_exec_check(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	char *type_name = strVal(op->d.sbsref_subscript.state->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres executor", type_name);
}

void
duckdb_exec_fetch(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	char *type_name = strVal(op->d.sbsref_subscript.state->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres executor", type_name);
}

void
duckdb_exec_assign(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	char *type_name = strVal(op->d.sbsref_subscript.state->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres executor", type_name);
}

void
duckdb_exec_fetch_old(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	char *type_name = strVal(op->d.sbsref_subscript.state->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres executor", type_name);
}

void
duckdb_exec_setup(const SubscriptingRef *sbsref, SubscriptingRefState *sbsrefstate, SubscriptExecSteps *methods) {
	sbsrefstate->workspace = makeString(bare_type_name(sbsref->refcontainertype));
	methods->sbs_check_subscripts = duckdb_exec_check;
	methods->sbs_fetch = duckdb_exec_fetch;
	methods->sbs_assign = duckdb_exec_assign;
	methods->sbs_fetch_old = duckdb_exec_fetch_old;
}

} // namespace

const SubscriptRoutines duckdb_row_subscript_routines = {
    .transform = duckdb_row_transform,
    .exec_setup = duckdb_exec_setup,
    .fetch_strict = false,
    .fetch_leakproof = true,
    .store_leakproof = true,
};

const SubscriptRoutines duckdb_loose_subscript_routines = {
    .transform = duckdb_loose_transform,
    .exec_setup = duckdb_exec_setup,
    .fetch_strict = false,
    .fetch_leakproof = true,
    .store_leakproof = true,
};

} // namespace pgddb::pg
