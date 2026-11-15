# KernDB Phase 8: Advanced SQL and Query Planning

Phase 8 adds a compact advanced SELECT pipeline on top of the Phase 0–6 engine.
WAL and crash recovery remain postponed. The implementation is rule based and
does not claim statistics-driven or cost-based optimization.

## Supported syntax

```sql
SELECT column [, column ...]
FROM table
[INNER JOIN table ON left_table.column = right_table.column
 | LEFT JOIN table ON left_table.column = right_table.column]
[WHERE column comparison literal]
[GROUP BY column [, column ...]]
[HAVING aggregate(column_or_star) comparison literal]
[ORDER BY column [ASC | DESC] [, column [ASC | DESC] ...]]
[LIMIT non_negative_integer]

SELECT aggregate(column_or_star) [, ...] FROM table [...]
EXPLAIN SELECT ...
```

`INNER` is optional before `JOIN`. A column can be qualified with its table
name. KernDB accepts one join between two tables; aliases and additional joins
are not implemented. A join condition compares one column from each input using
equality. SELECT predicates and HAVING support `=`, `!=`, `<`, `<=`, `>`, and
`>=`, and each clause accepts one predicate.

Supported aggregates are `COUNT(*)`, `COUNT(column)`, `SUM(column)`,
`AVG(column)`, `MIN(column)`, and `MAX(column)`. `SUM` and `AVG` require an
`INT` column. The other aggregates accept `INT` or `TEXT`. `AVG` returns a
`REAL` result value; table columns remain `INT` or `TEXT`.

## Semantics and limits

- ORDER BY supports multiple columns and is stable for equal keys. NULL result
  values sort last in either direction.
- LIMIT is applied after filtering, grouping, HAVING, and sorting. `LIMIT 0`
  returns no rows. Negative values and values outside the supported integer
  range are rejected.
- LEFT JOIN emits a NULL result value for each right-side column when no row
  matches. `COUNT(column)` ignores these NULL values; `COUNT(*)` counts the
  joined row.
- GROUP BY supports one or more columns. NULL group keys compare equal for
  grouping. Selected nonaggregate columns must be listed in GROUP BY.
- An aggregate query without GROUP BY returns one row for empty input. COUNT
  returns zero; SUM, AVG, MIN, and MAX return NULL. GROUP BY over empty input
  returns no groups.
- SUM detects signed 64-bit overflow and returns a resource error. AVG uses a
  floating result. MIN and MAX compare integers numerically and text
  lexicographically.
- KernDB still does not store SQL NULL. NULL is an output-only value used by
  LEFT JOIN and empty aggregate results. INSERT and UPDATE cannot write it.
- WHERE and HAVING do not support AND/OR, subqueries, expressions, or multiple
  predicates. HAVING compares one aggregate result with one literal. ORDER BY
  in grouped queries is limited to GROUP BY columns.
- Hash joins, spilling, aliases, DISTINCT, range index scans, and cost-based
  planning are not implemented.

## Planning and EXPLAIN

The optimizer uses deterministic rules:

1. Apply a single-table predicate while preparing its scan input. For joins, a
   predicate is applied to its referenced input before the nested-loop join;
   a LEFT JOIN with a right-side WHERE predicate also applies the required
   residual filter after null extension.
2. Use the persistent B+ tree for an `INT` equality predicate only when a
   matching index exists. Otherwise use a sequential scan. In-memory tables
   always use sequential scans.
3. Execute joins with a nested-loop operator, then aggregate/group, sort, limit,
   and project in SQL order.

The planner has no table statistics or reliable cardinality estimates, so it
does not estimate costs. Storage scans currently return full tuples; projection
pushdown into the storage layer is not available. No performance improvement
has been measured or is claimed for Phase 8.

`EXPLAIN SELECT ...` returns a `QUERY PLAN` result with separate logical and
physical trees. For example, an indexed persistent equality query displays
`Index Scan`; an unindexed query displays `Sequential Scan`. Other displayed
operators are limited to those present in the current execution path: Filter,
Nested Loop Join, Aggregate, Sort, Limit, and Projection.

Example:

```sql
EXPLAIN SELECT department_id, COUNT(*)
FROM employees
GROUP BY department_id
HAVING COUNT(*) > 1
ORDER BY department_id DESC
LIMIT 10;
```

## Verification

The advanced SQL cases live in the existing parser, binder, planner, executor,
and database integration suites. They cover sort direction and ties, LIMIT
edges, qualified and ambiguous join columns, INNER/LEFT joins, NULL extension,
all aggregate functions, grouped and empty input, HAVING, feature combinations,
invalid types, index selection, and EXPLAIN. The full Phase 0–6 regression suite
remains part of the same executable and CTest preset.

Build and run from the repository root:

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
.\build\debug\tests\kerndb_phase2_tests.exe
```

There is no WAL, crash recovery, MVCC, or cross-process locking in this phase.
