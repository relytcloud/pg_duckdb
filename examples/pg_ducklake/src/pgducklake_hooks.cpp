/*
 * pgducklake_hooks.cpp -- Planner and utility hooks.
 *
 * @scope backend: install planner and utility hooks
 *
 * Installs pg_ducklake planner and utility hooks.
 *
 * Planner hook:
 * - rewrites variant -> / ->> operators into variant_extract() FuncExpr nodes
 *   so pg_duckdb deparses them as function calls, not operator syntax
 * - rewrites regclass overloads of ducklake functions into text-arg versions
 * - delegates INSERT UNNEST planning optimization to pgducklake_direct_insert
 *
 * Utility hook:
 * - catches explicit COMMIT utility statements and commits DuckDB early
 * - intercepts CREATE INDEX USING ducklake_sorted to set DuckLake sort order
 * - intercepts DROP INDEX on ducklake_sorted indexes to reset sort order
 * - detaches the DuckLake catalog on DROP EXTENSION pg_ducklake so that
 *   a subsequent CREATE EXTENSION can re-attach a fresh catalog
 */

/* libpgddb planner pulls in duckdb.hpp -- must parse before any header
 * that defines PG's FATAL macro (i.e. before anything that includes
 * postgres.h). pgducklake_{copy_from,direct_insert,sorted_by}.hpp
 * do, so this stays at the top. */
#include "pgddb/pgddb_planner.hpp"
#include "pgddb/pgddb_table_am.hpp"

#include "pgducklake/pgducklake_hooks.hpp"
#include "pgducklake/pgducklake_call_handler.hpp"
#include "pgducklake/pgducklake_copy_from.hpp"
#include "pgducklake/pgducklake_defs.hpp"
#include "pgducklake/pgducklake_direct_insert.hpp"
#include "pgducklake/pgducklake_duckdb.hpp"
#include "pgducklake/pgducklake_duckdb_query.hpp"
#include "pgducklake/pgducklake_fdw.hpp"
#include "pgducklake/pgducklake_functions.hpp"
#include "pgducklake/pgducklake_guc.hpp"
#include "pgducklake/pgducklake_sorted_by.hpp"
#include "pgducklake/pgducklake_types.hpp"
#include "pgddb/pgddb_duckdb.hpp"

#include <string>

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parse_func.h"
#include "tcop/tcopprot.h"
#include "catalog/pg_proc.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "pgddb/pgddb_ruleutils.h"
}

/* Include after PG headers so Oid is defined. */
#include "pgducklake/pgducklake_variant.hpp"

namespace {

planner_hook_type prev_planner_hook = NULL;
ProcessUtility_hook_type prev_process_utility_hook = NULL;

/* Cached function OIDs for variant extract -- indexed by
 * {is_arrow, is_int_arg}: [0]=-> text, [1]=-> int, [2]=->> text, [3]=->> int */
static Oid variant_funcoids[4] = {InvalidOid, InvalidOid, InvalidOid, InvalidOid};

static Oid LookupVariantFunc(const char *name, Oid arg2_type, Oid variant_type_oid) {
  List *func_name = list_make2(makeString(pstrdup(PGDUCKLAKE_PG_SCHEMA)), makeString(pstrdup(name)));
  Oid args[2] = {variant_type_oid, arg2_type};
  return LookupFuncName(func_name, 2, args, true);
}

static void EnsureVariantFuncOids(Oid variant_type_oid) {
  if (OidIsValid(variant_funcoids[0]) && OidIsValid(variant_funcoids[1]) && OidIsValid(variant_funcoids[2]) &&
      OidIsValid(variant_funcoids[3]))
    return;
  variant_funcoids[0] = LookupVariantFunc("pg_variant_extract_json", TEXTOID, variant_type_oid);
  variant_funcoids[1] = LookupVariantFunc("pg_variant_extract_json_idx", INT4OID, variant_type_oid);
  variant_funcoids[2] = LookupVariantFunc("pg_variant_extract", TEXTOID, variant_type_oid);
  variant_funcoids[3] = LookupVariantFunc("pg_variant_extract_idx", INT4OID, variant_type_oid);
}

static void InvalidateVariantCaches() {
  for (int i = 0; i < 4; i++)
    variant_funcoids[i] = InvalidOid;
}

struct VariantOpMutatorCtx {
  Oid variant_type_oid;
};

/* Pre-scan: check if the query tree contains any OpExpr with a variant
 * left-arg. Returns true (stop walking) as soon as one is found. */
static bool HasVariantOpWalker(Node *node, void *ctx_ptr) {
  if (node == NULL)
    return false;

  if (IsA(node, OpExpr)) {
    OpExpr *op = (OpExpr *)node;
    if (list_length(op->args) == 2) {
      auto *ctx = (VariantOpMutatorCtx *)ctx_ptr;
      Node *arg1 = (Node *)linitial(op->args);
      if (exprType(arg1) == ctx->variant_type_oid)
        return true;
    }
  }

  if (IsA(node, Query)) {
#if PG_VERSION_NUM >= 160000
    return query_tree_walker((Query *)node, HasVariantOpWalker, ctx_ptr, 0);
#else
    return query_tree_walker((Query *)node, (bool (*)())((void *)HasVariantOpWalker), ctx_ptr, 0);
#endif
  }

#if PG_VERSION_NUM >= 160000
  return expression_tree_walker(node, HasVariantOpWalker, ctx_ptr);
#else
  return expression_tree_walker(node, (bool (*)())((void *)HasVariantOpWalker), ctx_ptr);
#endif
}

/*
 * Rewrite variant -> and ->> OpExpr nodes to variant_extract FuncExpr.
 *
 * DuckDB has no -> operator for VARIANT, so we convert:
 *   v -> 'key'  => pg_variant_extract_json(v, 'key')      (returns variant)
 *   v -> 0      => pg_variant_extract_json_idx(v, 0)       (returns variant)
 *   v ->> 'key' => pg_variant_extract(v, 'key')            (returns text)
 *   v ->> 0     => pg_variant_extract_idx(v, 0)             (returns text)
 */
static Node *RewriteVariantOpMutator(Node *node, void *ctx_ptr) {
  if (node == NULL)
    return NULL;

  if (IsA(node, Query)) {
#if PG_VERSION_NUM >= 160000
    return (Node *)query_tree_mutator((Query *)node, RewriteVariantOpMutator, ctx_ptr, 0);
#else
    return (Node *)query_tree_mutator((Query *)node, (Node * (*)())((void *)RewriteVariantOpMutator), ctx_ptr, 0);
#endif
  }

  if (!IsA(node, OpExpr))
    goto default_mutate;

  {
    auto *ctx = (VariantOpMutatorCtx *)ctx_ptr;
    OpExpr *op = (OpExpr *)node;
    if (list_length(op->args) != 2)
      goto default_mutate;

    Node *arg1 = (Node *)linitial(op->args);
    if (exprType(arg1) != ctx->variant_type_oid)
      goto default_mutate;

    /* Distinguish -> (returns variant) from ->> (returns text) via
     * opresulttype, avoiding a get_opname() syscache lookup per node. */
    bool is_arrow = (op->opresulttype == ctx->variant_type_oid);
    bool is_text_arrow = (op->opresulttype == TEXTOID);
    if (!is_arrow && !is_text_arrow)
      goto default_mutate;

    Node *arg2 = (Node *)lsecond(op->args);
    Oid arg2_type = exprType(arg2);
    if (arg2_type != TEXTOID && arg2_type != INT4OID)
      goto default_mutate;

    EnsureVariantFuncOids(ctx->variant_type_oid);

    /* Index into variant_funcoids[]: {is_arrow, is_int_arg} */
    int idx = (is_arrow ? 0 : 2) | (arg2_type == INT4OID ? 1 : 0);
    Oid target_funcid = variant_funcoids[idx];
    Oid result_type = is_arrow ? ctx->variant_type_oid : TEXTOID;

    if (!OidIsValid(target_funcid))
      goto default_mutate;

    /* Recurse into children first */
#if PG_VERSION_NUM >= 160000
    arg1 = expression_tree_mutator(arg1, RewriteVariantOpMutator, ctx_ptr);
    arg2 = expression_tree_mutator(arg2, RewriteVariantOpMutator, ctx_ptr);
#else
    arg1 = expression_tree_mutator(arg1, (Node * (*)())((void *)RewriteVariantOpMutator), ctx_ptr);
    arg2 = expression_tree_mutator(arg2, (Node * (*)())((void *)RewriteVariantOpMutator), ctx_ptr);
#endif

    FuncExpr *func = makeNode(FuncExpr);
    func->funcid = target_funcid;
    func->funcresulttype = result_type;
    func->funcretset = false;
    func->funcvariadic = false;
    func->funcformat = COERCE_EXPLICIT_CALL;
    func->funccollid = InvalidOid;
    func->inputcollid = op->inputcollid;
    func->args = list_make2(arg1, arg2);
    func->location = op->location;
    return (Node *)func;
  }

default_mutate:
#if PG_VERSION_NUM >= 160000
  return expression_tree_mutator(node, RewriteVariantOpMutator, ctx_ptr);
#else
  return expression_tree_mutator(node, (Node * (*)())((void *)RewriteVariantOpMutator), ctx_ptr);
#endif
}

static Query *RewriteVariantOperators(Query *parse) {
  Oid variant_oid = pgducklake::GetVariantTypeOid();
  if (!OidIsValid(variant_oid))
    return parse;

  VariantOpMutatorCtx ctx = {variant_oid};
#if PG_VERSION_NUM >= 160000
  if (!query_tree_walker(parse, HasVariantOpWalker, &ctx, 0))
    return parse;
#else
  if (!query_tree_walker(parse, (bool (*)())((void *)HasVariantOpWalker), &ctx, 0))
    return parse;
#endif

#if PG_VERSION_NUM >= 160000
  return (Query *)query_tree_mutator(parse, RewriteVariantOpMutator, &ctx, 0);
#else
  return (Query *)query_tree_mutator(parse, (Node * (*)())((void *)RewriteVariantOpMutator), &ctx, 0);
#endif
}

/*
 * Rewrite a single FuncExpr from regclass to text-arg form.
 *
 * ducklake.list_files('t'::regclass)
 *   -> ducklake.list_files('public', 't')
 */
void TryRewriteRegclassFunc(FuncExpr *func) {
  if (list_length(func->args) < 1)
    return;

  Node *first_arg = (Node *)linitial(func->args);
  if (!IsA(first_arg, Const))
    return;
  Const *regclass_const = (Const *)first_arg;
  if (regclass_const->consttype != REGCLASSOID)
    return;

  /* Confirm function belongs to the ducklake schema */
  Oid ducklake_nsp = get_namespace_oid(PGDUCKLAKE_PG_SCHEMA, true);
  if (!OidIsValid(ducklake_nsp))
    return;
  if (get_func_namespace(func->funcid) != ducklake_nsp)
    return;

  /* Look up the text-arg version; skip if none exists (e.g. get_partition) */
  char *func_name = get_func_name(func->funcid);
  List *func_name_list = list_make2(makeString(pstrdup(PGDUCKLAKE_PG_SCHEMA)), makeString(func_name));

  int old_nargs = list_length(func->args);
  int new_nargs = old_nargs + 1; /* regclass -> (text, text) */
  Oid *new_argtypes = (Oid *)palloc(sizeof(Oid) * new_nargs);
  new_argtypes[0] = TEXTOID;
  new_argtypes[1] = TEXTOID;

  int i = 2;
  ListCell *lc;
  for_each_from(lc, func->args, 1) {
    new_argtypes[i++] = exprType((Node *)lfirst(lc));
  }

  Oid text_funcid = LookupFuncName(func_name_list, new_nargs, new_argtypes, true);
  if (!OidIsValid(text_funcid))
    return;

  if (regclass_const->constisnull)
    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("table argument cannot be NULL")));

  Oid relid = DatumGetObjectId(regclass_const->constvalue);

  /* Validate that the target is a ducklake table */
  Oid ducklake_am_oid = get_am_oid("ducklake", false);
  Relation rel = relation_open(relid, AccessShareLock);
  Oid rel_am = rel->rd_rel->relam;
  relation_close(rel, AccessShareLock);

  if (rel_am != ducklake_am_oid)
    ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("table \"%s\" is not a DuckLake table", get_rel_name(relid))));

  /* Resolve OID to schema + table names */
  char *schema_name = get_namespace_name(get_rel_namespace(relid));
  char *table_name = get_rel_name(relid);

  /* Build new args: (schema_text, table_text, remaining...) */
  Const *schema_const = makeConst(TEXTOID, -1, InvalidOid, -1, CStringGetTextDatum(schema_name), false, false);
  Const *table_const = makeConst(TEXTOID, -1, InvalidOid, -1, CStringGetTextDatum(table_name), false, false);

  List *new_args = list_make2(schema_const, table_const);
  for_each_from(lc, func->args, 1) {
    new_args = lappend(new_args, lfirst(lc));
  }

  func->funcid = text_funcid;
  func->args = new_args;
}

bool RewriteRegclassWalker(Node *node, void *context) {
  if (node == NULL)
    return false;

  if (IsA(node, FuncExpr))
    TryRewriteRegclassFunc((FuncExpr *)node);

  if (IsA(node, Query)) {
#if PG_VERSION_NUM >= 160000
    return query_tree_walker((Query *)node, RewriteRegclassWalker, context, 0);
#else
    return query_tree_walker((Query *)node, (bool (*)())((void *)RewriteRegclassWalker), context, 0);
#endif
  }

#if PG_VERSION_NUM >= 160000
  return expression_tree_walker(node, RewriteRegclassWalker, context);
#else
  return expression_tree_walker(node, (bool (*)())((void *)RewriteRegclassWalker), context);
#endif
}

Query *RewriteRegclassFunctions(Query *parse) {
#if PG_VERSION_NUM >= 160000
  query_tree_walker(parse, RewriteRegclassWalker, NULL, 0);
#else
  query_tree_walker(parse, (bool (*)())((void *)RewriteRegclassWalker), NULL, 0);
#endif
  return parse;
}

/*
 * Returns true if any RangeTblEntry in the query references a relation
 * whose table-AM is registered in libpgddb's per-process registry (i.e. a
 * ducklake-AM table for this consumer). Walks subqueries and expressions.
 */
static bool QueryReferencesRegisteredTableAm(Node *node, void *context) {
  if (node == NULL)
    return false;

  if (IsA(node, Query)) {
    Query *q = (Query *)node;
    foreach_node(RangeTblEntry, rte, q->rtable) {
      if (rte->relid == InvalidOid)
        continue;
      if (pgddb::TableAmGetName(rte->relid) != nullptr)
        return true;
    }
#if PG_VERSION_NUM >= 160000
    return query_tree_walker(q, QueryReferencesRegisteredTableAm, context, 0);
#else
    return query_tree_walker(q, (bool (*)())((void *)QueryReferencesRegisteredTableAm), context, 0);
#endif
  }

#if PG_VERSION_NUM >= 160000
  return expression_tree_walker(node, QueryReferencesRegisteredTableAm, context);
#else
  return expression_tree_walker(node, (bool (*)())((void *)QueryReferencesRegisteredTableAm), context);
#endif
}

/*
 * Returns true if the query references any ducklake-only function (either
 * directly via FuncExpr or as an aggregate via Aggref). Walks subqueries
 * and expressions; used as the second routing trigger alongside
 * QueryReferencesRegisteredTableAm.
 */
static bool QueryReferencesDucklakeOnlyFunc(Node *node, void *context) {
  if (node == NULL)
    return false;

  if (IsA(node, FuncExpr)) {
    FuncExpr *func = castNode(FuncExpr, node);
    if (pgducklake::IsDucklakeOnlyFunction(func->funcid))
      return true;
  }

  if (IsA(node, Aggref)) {
    Aggref *agg = castNode(Aggref, node);
    if (pgducklake::IsDucklakeOnlyFunction(agg->aggfnoid))
      return true;
  }

  if (IsA(node, Query)) {
#if PG_VERSION_NUM >= 160000
    return query_tree_walker((Query *)node, QueryReferencesDucklakeOnlyFunc, context, 0);
#else
    return query_tree_walker((Query *)node, (bool (*)())((void *)QueryReferencesDucklakeOnlyFunc), context, 0);
#endif
  }

#if PG_VERSION_NUM >= 160000
  return expression_tree_walker(node, QueryReferencesDucklakeOnlyFunc, context);
#else
  return expression_tree_walker(node, (bool (*)())((void *)QueryReferencesDucklakeOnlyFunc), context);
#endif
}

PlannedStmt *DucklakePlannerHook(Query *parse, const char *query_string, int cursor_options,
                                 ParamListInfo bound_params) {
  if (pgducklake::enable_direct_insert) {
    PlannedStmt *direct_insert_plan = pgducklake::TryCreateDirectInsertPlan(parse, bound_params);
    if (direct_insert_plan)
      return direct_insert_plan;
  }

  /* Pure PG catalog rewrites -- no DuckDB needed, must run before init (#149) */
  parse = RewriteVariantOperators(parse);
  parse = RewriteRegclassFunctions(parse);

  if (pgddb::DuckDBManager::IsInitialized()) {
    /* ATTACH databases for any ducklake FDW tables before planning. */
    pgducklake::RegisterForeignTablesInQuery(parse);
  }

  /*
   * If the query touches any ducklake-AM table, or references any
   * ducklake-only function (prosrc='duckdb_only_function'), route the
   * whole query to DuckDB via libpgddb's CustomScan. Otherwise fall
   * through to PG's standard planner (or whatever prev_planner_hook
   * points at).
   */
  if (QueryReferencesRegisteredTableAm((Node *)parse, NULL) ||
      QueryReferencesDucklakeOnlyFunc((Node *)parse, NULL) ||
      pgducklake::QueryReferencesDucklakeForeignTable(parse)) {
    return pgddb::PlanNode(parse, cursor_options, /*throw_error=*/true);
  }

  return prev_planner_hook(parse, query_string, cursor_options, bound_params);
}

bool IsCommitUtilityStmt(PlannedStmt *pstmt) {
  if (!pstmt || !pstmt->utilityStmt || !IsA(pstmt->utilityStmt, TransactionStmt))
    return false;

  auto *stmt = castNode(TransactionStmt, pstmt->utilityStmt);
  return stmt->kind == TRANS_STMT_COMMIT;
}

void ForceDuckDBCommitOnExplicitCommit() {
  const char *duckdb_errmsg = nullptr;
  int rc = pgducklake::ExecuteDuckDBQuery("COMMIT", &duckdb_errmsg);
  if (rc == 0)
    return;

  // Explicit PG COMMIT should always be mirrored to DuckDB COMMIT here.
  ereport(ERROR, (errmsg("pg_ducklake commit hook failed to commit DuckDB: %s",
                         duckdb_errmsg ? duckdb_errmsg : "unknown error")));
}

/*
 * Check whether the statement is DROP EXTENSION pg_ducklake.
 */
bool IsDropDucklakeExtensionStmt(PlannedStmt *pstmt) {
  if (!pstmt || !pstmt->utilityStmt || !IsA(pstmt->utilityStmt, DropStmt))
    return false;

  auto *drop = castNode(DropStmt, pstmt->utilityStmt);
  if (drop->removeType != OBJECT_EXTENSION)
    return false;

  ListCell *lc;
  foreach (lc, drop->objects) {
    char *extname = strVal(lfirst(lc));
    if (strcmp(extname, PGDUCKLAKE_PG_EXTENSION) == 0)
      return true;
  }
  return false;
}

/*
 * Check whether a procedure OID belongs to the ducklake schema and was
 * registered via the ducklake_only_procedure C stub.
 */
bool IsDucklakeOnlyProcedure(Oid funcid) {
  Oid ducklake_nsp = get_namespace_oid(PGDUCKLAKE_PG_SCHEMA, true);
  if (!OidIsValid(ducklake_nsp))
    return false;
  if (get_func_namespace(funcid) != ducklake_nsp)
    return false;
  HeapTuple tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
  if (!HeapTupleIsValid(tp))
    return false;
  bool isnull;
  Datum prosrc_datum = SysCacheGetAttr(PROCOID, tp, Anum_pg_proc_prosrc, &isnull);
  if (isnull) {
    ReleaseSysCache(tp);
    return false;
  }
  char *prosrc_str = TextDatumGetCString(prosrc_datum);
  ReleaseSysCache(tp);
  return strcmp(prosrc_str, "ducklake_only_procedure") == 0;
}

/*
 * CREATE VIEW pre-handler. When the view body references any duckdb-only
 * function returning a ducklake.duckdb_row (snapshots, table_info,
 * time_travel, ...), the standard PG view definition stores a single
 * duckdb_row column in pg_attribute, which loses the underlying schema
 * for introspection (\d, format_type, joins).
 *
 * Mirror pg_duckdb's EntrenchColumnsFromCall trick: plan the body via
 * pgddb::PlanNode to discover DuckDB's column names + types, deparse the
 * inner query into DuckDB SQL, and rewrite the view body to
 *   SELECT r['col1']::type1 AS col1, ... FROM ducklake.duckdb_query('<sql>') r
 * so PG sees explicit typed columns at CREATE VIEW time. The planner
 * routes SELECT-on-view back through the duckdb_query() stub, and
 * pgddb's strip_first_subscript hook (installed in pgducklake_types.cpp)
 * renders r['col'] as r.col for DuckDB.
 *
 * As a side benefit, planning at CREATE VIEW time means an invalid
 * snapshot version errors immediately instead of at first SELECT.
 */
static void
RewriteDuckdbRowViewStmt(ViewStmt *stmt, PlannedStmt *pstmt, const char *query_string) {
  if (!stmt || !stmt->query)
    return;

  Oid duckdb_row_oid = pgducklake::LookupDucklakeDuckdbRowOid();
  if (!OidIsValid(duckdb_row_oid))
    return;

  // parse_analyze the view body so we can plan it.
  RawStmt *rawstmt = makeNode(RawStmt);
  rawstmt->stmt = stmt->query;
  rawstmt->stmt_location = pstmt->stmt_location;
  rawstmt->stmt_len = pstmt->stmt_len;
  Query *viewParse = parse_analyze_fixedparams(rawstmt, query_string, NULL, 0, NULL);

  if (!IsA(viewParse, Query) || viewParse->commandType != CMD_SELECT)
    return;

  // Cheap early-exit: only trigger when the target list is a single
  // duckdb_row column. That's the case for `SELECT * FROM <duckdb-only-fn>`,
  // which is what views over ducklake.snapshots / time_travel / table_info
  // typically look like. Anything else falls through.
  if (list_length(viewParse->targetList) != 1)
    return;
  TargetEntry *tle = linitial_node(TargetEntry, viewParse->targetList);
  if (exprType((Node *)tle->expr) != duckdb_row_oid)
    return;

  // Plan via DuckDB to learn the real column names + types.
  Plan *plan = nullptr;
  PG_TRY();
  {
    plan = pgddb::PlanNode((Query *)copyObjectImpl(viewParse), CURSOR_OPT_PARALLEL_OK, /*throw_error=*/true)->planTree;
  }
  PG_CATCH();
  {
    // Re-throw: this surfaces "No snapshot found at version N" etc. as
    // CREATE VIEW errors (matching the test expectation for tt_v_bad).
    PG_RE_THROW();
  }
  PG_END_TRY();

  if (!plan)
    return;

  CustomScan *custom_scan = castNode(CustomScan, plan);
  if (list_length(custom_scan->custom_scan_tlist) == 0)
    return;

  // Deparse the original view body to DuckDB SQL so duckdb_query() can
  // execute it. pgddb_get_querydef handles the heavy lifting (function
  // call rewrites, fake-type suppression, row expansion via the hooks).
  char *duckdb_query_string = pgddb_get_querydef((Query *)copyObjectImpl(viewParse));

  // Build wrapping SELECT with one typed subscript per DuckDB column.
  StringInfo buf = makeStringInfo();
  appendStringInfoString(buf, "SELECT ");
  bool first = true;
  foreach_node(TargetEntry, plan_tle, custom_scan->custom_scan_tlist) {
    if (!first)
      appendStringInfoString(buf, ", ");
    first = false;
    Oid coltype = exprType((Node *)plan_tle->expr);
    int32 coltypmod = exprTypmod((Node *)plan_tle->expr);
    appendStringInfo(buf, "r[%s]::%s AS %s", quote_literal_cstr(plan_tle->resname),
                     format_type_with_typemod(coltype, coltypmod), quote_identifier(plan_tle->resname));
  }
  appendStringInfo(buf, " FROM ducklake.duckdb_query(%s) r", quote_literal_cstr(duckdb_query_string));

  List *parsetree_list = pg_parse_query(buf->data);
  if (list_length(parsetree_list) != 1)
    return;
  RawStmt *new_raw = linitial_node(RawStmt, parsetree_list);

  MemoryContext query_context = GetMemoryChunkContext(stmt->query);
  MemoryContext oldcontext = MemoryContextSwitchTo(query_context);
  stmt->query = (Node *)copyObjectImpl(new_raw->stmt);
  MemoryContextSwitchTo(oldcontext);
}

void DucklakeUtilityHook(PlannedStmt *pstmt, const char *query_string, bool read_only_tree,
                         ProcessUtilityContext context, ParamListInfo params, struct QueryEnvironment *query_env,
                         DestReceiver *dest, QueryCompletion *qc) {
  if (IsCommitUtilityStmt(pstmt) && pgddb::DuckDBManager::IsInitialized()) {
    elog(DEBUG1, "pg_ducklake utility hook caught COMMIT");
    ForceDuckDBCommitOnExplicitCommit();
  }

  Node *parsetree = pstmt->utilityStmt;

  if (IsA(parsetree, ViewStmt)) {
    RewriteDuckdbRowViewStmt(castNode(ViewStmt, parsetree), pstmt, query_string);
  }

  if (IsA(parsetree, CallStmt)) {
    CallStmt *call = castNode(CallStmt, parsetree);
    if (call->funcexpr && IsDucklakeOnlyProcedure(call->funcexpr->funcid)) {
      pgducklake::HandleDuckdbCall(call, query_string);
      if (qc)
        SetQueryCompletion(qc, CMDTAG_CALL, 0);
      return;
    }
  }

  /* COPY <ducklake_table> FROM STDIN -- handle before pg_duckdb rejects it */
  if (IsA(parsetree, CopyStmt)) {
    CopyStmt *copy_stmt = castNode(CopyStmt, parsetree);
    if (copy_stmt->is_from && copy_stmt->filename == NULL && copy_stmt->relation) {
      Relation rel = table_openrv(copy_stmt->relation, AccessShareLock);
      Oid am_oid = rel->rd_rel->relam;
      table_close(rel, AccessShareLock);
      static Oid ducklake_am_oid = InvalidOid;
      if (!OidIsValid(ducklake_am_oid))
        ducklake_am_oid = get_am_oid("ducklake", true);
      if (OidIsValid(ducklake_am_oid) && am_oid == ducklake_am_oid) {
        uint64 processed = pgducklake::DucklakeCopyFromStdin(copy_stmt, query_string);
        if (qc)
          SetQueryCompletion(qc, CMDTAG_COPY, processed);
        return;
      }
    }
  }

  /* CREATE INDEX ... USING ducklake_sorted */
  if (IsA(parsetree, IndexStmt)) {
    IndexStmt *idx = castNode(IndexStmt, parsetree);
    if (idx->accessMethod && strcmp(idx->accessMethod, PGDUCKLAKE_SORTED_AM) == 0) {
      pgducklake::HandleCreateSortedIndex(pstmt, query_string, read_only_tree, context, params, query_env, dest, qc,
                                          prev_process_utility_hook);
      return;
    }
  }

  /* DROP INDEX on ducklake_sorted indexes -- collect before drop */
  std::vector<pgducklake::SortedIndexDrop> sorted_drops;
  if (IsA(parsetree, DropStmt))
    sorted_drops = pgducklake::FindSortedIndexDrops(castNode(DropStmt, parsetree));

  bool dropping_extension = IsDropDucklakeExtensionStmt(pstmt);

  prev_process_utility_hook(pstmt, query_string, read_only_tree, context, params, query_env, dest, qc);

  pgducklake::HandleDropSortedIndex(sorted_drops);

  // After DROP EXTENSION completes, detach the DuckLake catalog from DuckDB
  // so that a subsequent CREATE EXTENSION can attach a fresh one.
  if (dropping_extension) {
    ducklake_detach_catalog();
    InvalidateVariantCaches();
  }
}

} // namespace

namespace pgducklake {
void InitHooks() {
  // Install planner hook after pg_duckdb (LIFO: our hook runs first).
  prev_planner_hook = planner_hook ? planner_hook : standard_planner;
  planner_hook = DucklakePlannerHook;

  // Chain ProcessUtility so we can observe COMMIT utility statements.
  prev_process_utility_hook = ProcessUtility_hook ? ProcessUtility_hook : standard_ProcessUtility;
  ProcessUtility_hook = DucklakeUtilityHook;
}

} // namespace pgducklake
