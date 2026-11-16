# KernDB Command Reference and Architecture Guide

This guide describes the implementation currently present in this repository. KernDB is a small, embedded C++ database engine. Its SQL support is intentionally narrow; a keyword being familiar SQL does not mean KernDB accepts it.

Status labels used below:

- **SUPPORTED** means the current parser and execution path handle the documented form.
- **PARTIAL** means the feature works only within the stated limits.
- **NOT SUPPORTED** means the parser, binder, or execution path does not implement it.

## 1. CLI Commands

The executable is `kerndb_shell`. The source entry point is `apps/kerndb_shell/main.cpp`; argument parsing and the REPL live in `apps/kerndb_shell/shell.cpp`, function `kerndb::shell::Run`.

| Option / mode | Exact syntax | Example | What it does |
|---|---|---|---|
| Help | `kerndb_shell --help` or `kerndb_shell -h` | `kerndb_shell --help` | Prints usage and exits with code 0. |
| Version | `kerndb_shell --version` | `kerndb_shell --version` | Prints `KernDB 0.0.0` and exits with code 0. |
| Persistent database | `kerndb_shell --database DIRECTORY` | `kerndb_shell --database .\stu_db` | Opens or creates the database directory. If no `--execute` option is given, starts the persistent REPL. `--database` may appear only once. |
| Execute SQL | `kerndb_shell --execute "SQL"` | `kerndb_shell --execute "SELECT id FROM stu;"` | Executes one SQL statement. This option may be repeated; all statements use the same database instance for that process. If any `--execute` is present, the program exits after executing them instead of starting the REPL. |
| In-memory REPL | `kerndb_shell` | `kerndb_shell` | Starts an in-memory database and reads one SQL statement per input line. Type `.quit` to exit. Data is lost when the process ends. |
| Persistent REPL | `kerndb_shell --database DIRECTORY` | `kerndb_shell --database .\stu_db` | Starts a one-line REPL backed by the selected directory. Type `.quit` to exit. |

The usage shape printed by the program is:

```text
kerndb_shell [--database DIRECTORY] [--execute "SQL"]...
kerndb_shell --help | --version
```

There is **NOT SUPPORTED** CLI syntax for a SQL script/file argument, `--file`, batch file execution, a separate `--interactive` switch, or multiple SQL statements inside one `--execute` string. Use repeated `--execute` options for multiple statements. The parser accepts one optional trailing semicolon on each statement. Unknown options and missing option values are rejected; invalid arguments return exit code 2, SQL or database errors return 1.

## 2. Complete SQL Command List

### SQL forms the parser accepts

These forms are reconstructed from `src/parser/parser.cpp` and its AST in `src/parser/ast.h`. Square brackets below mean an optional part of the syntax; `...` means a comma-separated repetition. Identifiers start with a letter or `_`, then contain letters, digits, or `_`. SQL keywords are case-insensitive; the binder normalizes object and column names case-insensitively. A statement may end in one semicolon.

```sql
CREATE TABLE table_name (column_name INT|TEXT [, column_name INT|TEXT ...])
CREATE INDEX index_name ON table_name (column_name)
INSERT INTO table_name VALUES (literal [, literal ...])
SELECT select_item [, select_item ...] FROM table_name
  [INNER JOIN table_name ON column_ref = column_ref | LEFT JOIN table_name ON column_ref = column_ref]
  [WHERE column_ref comparison literal]
  [GROUP BY column_ref [, column_ref ...]]
  [HAVING aggregate(column_ref_or_*) comparison literal]
  [ORDER BY column_ref [ASC|DESC] [, column_ref [ASC|DESC] ...]]
  [LIMIT non_negative_integer]
EXPLAIN SELECT ...
UPDATE table_name SET column_name = literal WHERE column_name = literal
DELETE FROM table_name WHERE column_name = literal
BEGIN;
COMMIT;
ROLLBACK;
```

`literal` is a signed 64-bit integer or a single-quoted string. A single quote inside a string is written twice, for example `'Ada''s'`. `--` starts a line comment. SELECT `WHERE` and `HAVING` support `=`, `!=`, `<`, `<=`, `>`, and `>=`; UPDATE and DELETE still use equality predicates. `CREATE TABLE` needs at least one column. `INSERT` takes one row's values in table-column order and has no column-name list. `SELECT` must name at least one projection; `*`, aliases, expressions, and multiple joins are not supported. KernDB accepts at most one join between two tables, and its ON condition must compare columns from both inputs with equality.

Stored columns use `INT` (signed 64-bit) or `TEXT`; persistent inline `TEXT` is limited to 1024 bytes. `AVG` returns a `REAL` query result. KernDB does not store SQL NULL, but LEFT JOIN and empty SUM/AVG/MIN/MAX results can contain output-only NULL values. Binder checks literal and aggregate types.

### Supported statements and implementation trail

| Statement | Syntax and `stu` example | What it does and limits | Main components, source functions, and evidence |
|---|---|---|---|
| `CREATE TABLE` — **SUPPORTED** | `CREATE TABLE stu (id INT, name TEXT);` | Creates a table with one or more `INT`/`TEXT` columns. Duplicate column names are rejected. In the default `Database()` session it is in memory; with `--database` it creates a persistent table file and catalog entry. | Lexer `LexSql`; parser `ParseCreateTable`; AST `AstCreateTable`; binder `BindCreateTable`; catalog `InMemoryCatalog::CreateTable`; planner `BuildLogicalPlan` / `BuildPhysicalPlan`; executor `ExecuteCreateTable`; persistent storage `PersistentStorage::CreateTable`; heap pages through `PageManager`. Tests: `DatabaseExecutesTheApprovedPhaseOneVerticalSlice`, `PersistentDatabaseReopensTheApprovedVerticalSlice`. |
| `INSERT INTO` — **SUPPORTED** | `INSERT INTO stu VALUES (1, 'Aman');` | Inserts one row. Values must match all table columns in schema order and have matching types. Returns an affected-row count of 1. A unique index rejects duplicate keys. | Parser `ParseInsert`; AST `AstInsert`; binder `BindInsert`; planner `BuildPhysicalPlan`; executor `ExecuteInsert`; persistent `PersistentStorage::Insert`; heap `TableHeap::Insert`; index maintenance `InsertIndexEntry`. Tests: `DatabaseExecutesTheApprovedPhaseOneVerticalSlice`, `PersistentDatabaseBuildsMaintainsAndReopensAnIntIndex`. |
| `SELECT` — **PARTIAL** | `SELECT id, name FROM stu ORDER BY id DESC LIMIT 10;` | Reads named columns. Supports one equality join, WHERE comparisons, aggregates, GROUP BY/HAVING, ORDER BY, and LIMIT. `SELECT *`, aliases, expressions, and multiple joins are not supported. | Parser `ParseSelect`; AST `AstSelect`; binder `BindSelect`; planner logical/physical select plans; executor `ExecuteAdvancedSelect`, `ExecuteSequentialScan`, and `ExecuteIndexScan`. Tests: `ExecutorSortsStablyAndAppliesLimitAfterOrderBy`, `ExecutorRunsInnerAndLeftNestedLoopJoinsWithNullExtension`, `ExecutorComputesAggregatesGroupsHavingAndEmptyInputSemantics`. |
| `WHERE` — **PARTIAL** | `SELECT id FROM stu WHERE id >= 1;` | One SELECT column-to-literal predicate using `=`, `!=`, `<`, `<=`, `>`, or `>=`. Literal and column types must match. UPDATE and DELETE remain equality-only. For joins, qualified references resolve against either input; ambiguous unqualified references fail binding. | Parser `AstSelectPredicate`; binder `BindSelectQuery`; executor `ExecuteAdvancedSelect`; matching persistent INT equality indexes use `ExecuteIndexScan` or indexed advanced scan. Tests: `DatabaseReportsTheRequiredPhaseOneErrors`, `BinderResolvesQualifiedJoinColumnsAndRejectsAmbiguity`, `PlannerUsesAnAvailablePersistentIntIndex`. |
| `INNER JOIN`, `LEFT JOIN` — **PARTIAL** | `SELECT stu.name, visits.score FROM stu LEFT JOIN visits ON stu.id = visits.person_id;` | One equality join between two tables, executed as a nested-loop join. Qualified references are supported. LEFT JOIN appends output NULL values for unmatched right rows. Aliases and further joins are unsupported. | Parser `AstJoin`; binder `BoundJoin`; physical `Nested Loop Join`; executor `ExecuteAdvancedSelect`. Tests: `ExecutorRunsInnerAndLeftNestedLoopJoinsWithNullExtension`, `BinderResolvesQualifiedJoinColumnsAndRejectsAmbiguity`. |
| Aggregates — **PARTIAL** | `SELECT COUNT(*), SUM(score), AVG(score), MIN(score), MAX(score) FROM visits;` | Supports COUNT(*), COUNT(column), SUM, AVG, MIN, MAX. SUM/AVG require INT; MIN/MAX accept INT or TEXT. Empty input returns one aggregate row: COUNT is zero and other aggregates are NULL. | Parser aggregate items; binder type checks; executor `ComputeAggregate`. Tests: `ExecutorComputesAggregatesGroupsHavingAndEmptyInputSemantics`. |
| `GROUP BY`, `HAVING` — **PARTIAL** | `SELECT person_id, SUM(score) FROM visits GROUP BY person_id HAVING SUM(score) > 10;` | One or more grouping columns. Nonaggregate selected columns must be grouped. HAVING compares one aggregate result with one literal. | Parser `AstSelect::group_by` / `having`; binder grouping validation; executor grouping and aggregate stages. Tests: `BinderValidatesGroupingAggregateTypesAndLimit`, `ExecutorComputesAggregatesGroupsHavingAndEmptyInputSemantics`. |
| `ORDER BY`, `LIMIT` — **PARTIAL** | `SELECT id FROM stu ORDER BY id ASC LIMIT 5;` | Stable multi-column ordering with ASC/DESC. LIMIT accepts nonnegative integer values including zero and runs after ordering. NULL result values sort last. | Parser `AstOrderBy` / `limit`; binder validation; executor stable sort and row limit. Tests: `ExecutorSortsStablyAndAppliesLimitAfterOrderBy`. |
| `EXPLAIN SELECT` — **SUPPORTED** | `EXPLAIN SELECT id FROM stu WHERE id = 1;` | Returns readable logical and physical operator trees. The physical tree shows an index scan only when the persistent table has a matching INT equality index. | Parser `AstExplainSelect`; planner `LogicalExplainSelect` / `PhysicalExplainSelect`; executor `ExecuteExplain`. Tests: `DatabaseExplainsAdvancedOperatorsAndRejectsInvalidAdvancedSql`, `PersistentOptimizerExplainsAndUsesAnAvailableEqualityIndex`. |
| `CREATE INDEX` — **PARTIAL** | `CREATE INDEX stu_id_idx ON stu(id);` | Creates a unique index on one `INT` column. There can be only one index per table column. Existing duplicate values make index creation fail. Persistent databases get a B+ tree; in-memory databases retain index metadata and enforce uniqueness but do not create B+ tree pages. | Parser `ParseCreateIndex`; AST `AstCreateIndex`; binder `BindCreateIndex` (rejects non-`INT`); catalog `InMemoryCatalog::CreateIndex`; executor `ExecuteCreateIndex`; persistent `PersistentStorage::CreateIndex`; B+ tree `BPlusTree::Create` / `Insert`. Tests: `BinderResolvesCreateIndexAndRestrictsItToIntColumns`, `PersistentDatabaseBuildsMaintainsAndReopensAnIntIndex`, `PersistentDatabaseIndexesExistingRowsAcrossSplitsAndRestart`, `ExecutorAcceptsCreateIndexForTheInMemoryFallback`. |
| `UPDATE` — **PARTIAL** | `UPDATE stu SET name = 'Aman Verma' WHERE id = 1;` | Changes one assigned column for every row matching one equality predicate. Exactly one assignment is allowed, and `WHERE` is mandatory. Type checking applies. Persistent updates replace the heap record, refresh all index RIDs, and tombstone the old slot. | Parser `ParseUpdate`; AST `AstUpdate`; binder `BindUpdate`; planner `BuildPhysicalPlan`; executor `ExecuteUpdate`; persistent `PersistentStorage::UpdateMatching`; heap `TableHeap::Insert` / `Erase` / `Restore`; B+ tree `Remove` / `Insert`. Tests: `ParserBuildsAllSupportedStatementKinds`, `PersistentDatabaseUpdatesRowsAndMaintainsUniqueIndexes`, `PersistentDatabaseMutationChangesSurviveRestartWithMultipleIndexes`. |
| `DELETE` — **PARTIAL** | `DELETE FROM stu WHERE id = 1;` | Deletes every row matching one equality predicate. `WHERE` is mandatory. Persistent deletion tombstones heap slots and removes entries from indexes; deleted slot space is not reclaimed. | Parser `ParseDelete`; AST `AstDelete`; binder `BindDelete`; planner `BuildPhysicalPlan`; executor `ExecuteDelete`; persistent `PersistentStorage::DeleteMatching`; heap `TableHeap::Erase`; B+ tree `Remove`. Tests: `ParserBuildsAllSupportedStatementKinds`, `PersistentDatabaseDeletesRowsMaintainsIndexesAndReusesKeys`. |
| `BEGIN` — **SUPPORTED** | `BEGIN;` | Starts one explicit transaction on this `Database` instance. A nested `BEGIN` is rejected. `CREATE TABLE` and `CREATE INDEX` are rejected until the transaction ends. | Parser `AstBegin`; `Database::Execute` / `Database::BeginTransaction`; persistent `PersistentStorage::BeginTransaction` or the in-memory catalog snapshot. Tests: `PersistentDatabaseKeepsFailedStatementAtomicAndTransactionActive`, `InMemoryDatabaseSupportsExplicitTransactionRollback`. |
| `COMMIT` — **SUPPORTED** | `COMMIT;` | Ends the active transaction and discards its undo history. It is rejected when no transaction is active. Committed changes survive an orderly close and reopen. | Parser `AstCommit`; `Database::CommitTransaction`; `PersistentStorage::CommitTransaction`. Test: `PersistentDatabaseCommitsExplicitInsertAndPersistsIt`. |
| `ROLLBACK` — **SUPPORTED** | `ROLLBACK;` | Ends the active transaction and reverses its successful mutations in reverse order. It is rejected when no transaction is active. Closing the `Database` with a transaction active also rolls it back. | Parser `AstRollback`; `Database::RollbackTransaction` / `RollbackActiveTransaction`; `PersistentStorage::RollbackTransaction`. Tests: `PersistentDatabaseRollsBackExplicitInsertAcrossReopen`, `PersistentDatabaseRollsBackMultipleMutationsInReverseAndRestoresIndexedRid`, `PersistentDatabaseCloseRollsBackActiveTransaction`. |

`BEGIN`, `COMMIT`, and `ROLLBACK` are handled by `Database::Execute` before normal binding and planning. One explicit transaction may be active per `Database` session. Distinct `Database::Open` sessions for the same normalized persistent directory share a runtime and can execute concurrently in one process; calls through one session are serialized. Successful `INSERT`, `UPDATE`, and `DELETE` statements inside an explicit transaction remain undoable until `COMMIT`; `SELECT` sees those writes immediately. Catalog and table S/X locks are held until commit or abort. If a statement fails, its own partial persistent changes are compensated. When that compensation succeeds, the transaction remains active and can continue. Transactional DDL is unsupported: `CREATE TABLE` and `CREATE INDEX` return an error inside an explicit transaction. Autocommit remains in effect when no transaction is active.

### Checklist of requested SQL features

| Feature | Status | Exact current boundary |
|---|---|---|
| `CREATE TABLE` | **SUPPORTED** | One or more `INT`/`TEXT` columns. |
| `INSERT INTO` | **SUPPORTED** | One tuple per statement, all values in table order; no named column list. |
| `SELECT` | **PARTIAL** | Named columns; one join maximum, no `*`, aliases, or general expressions. |
| `WHERE` | **PARTIAL** | One column-to-literal predicate with standard comparison operators on SELECT. |
| `CREATE INDEX` | **PARTIAL** | Unique B+ tree for one `INT` column in persistent mode. |
| `UPDATE` | **PARTIAL** | One `SET column = literal`, mandatory single equality `WHERE`. |
| `DELETE` | **PARTIAL** | Mandatory single equality `WHERE`. |
| `BEGIN`, `COMMIT`, `ROLLBACK` | **SUPPORTED** | One explicit transaction per `Database` session; separate persistent sessions for the same directory can run concurrently within one process. No nesting. `COMMIT` and `ROLLBACK` require an active transaction. |
| Transactional DDL | **NOT SUPPORTED** | `CREATE TABLE` and `CREATE INDEX` are rejected while a transaction is active. |
| `DROP TABLE` | **NOT SUPPORTED** | No drop statement in token, AST, or parser. |
| `DROP INDEX` | **NOT SUPPORTED** | No drop statement in token, AST, or parser. |
| `ALTER TABLE` | **NOT SUPPORTED** | No alter statement in token, AST, or parser. |
| SQL `CREATE DATABASE` / `DROP DATABASE` | **NOT SUPPORTED** | Persistent directories are opened through the CLI `--database DIRECTORY` option or C++ `Database::Open`, not SQL. |
| `JOIN` | **PARTIAL** | One INNER or LEFT equality nested-loop join between two tables; no aliases or additional joins. |
| `ORDER BY` | **PARTIAL** | Multiple column references, ASC/DESC; grouped queries can order by grouping columns. |
| `GROUP BY` | **PARTIAL** | One or more columns; selected nonaggregate columns must be grouped. |
| `LIMIT` | **SUPPORTED** | Nonnegative signed 64-bit integer, including zero; applied after sort/aggregation. |
| `DISTINCT` | **NOT SUPPORTED** | No parser production or distinct operator. |
| `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` | **PARTIAL** | COUNT accepts `*` or a column; SUM/AVG accept INT; MIN/MAX accept INT or TEXT. |
| `=` | **SUPPORTED** | Equality between one column and one type-matching literal. |
| `!=`, `<`, `>`, `<=`, `>=` | **PARTIAL** | Supported for SELECT WHERE and HAVING; UPDATE/DELETE remain equality-only. `<>` is not tokenized. |
| `AND`, `OR`, `NOT` | **NOT SUPPORTED** | No boolean-expression grammar or tokens. |
| `NULL` handling | **PARTIAL** | No stored NULL, NULL literal, or nullable-column syntax. Query results use NULL for LEFT JOIN extension and empty SUM/AVG/MIN/MAX. |
| Multiple conditions | **NOT SUPPORTED** | One predicate is stored in `AstSelect`, `AstUpdate`, and `AstDelete`. |
| Multiple columns | **PARTIAL** | Supported in table definitions, insert value lists, and `SELECT` projection lists. `UPDATE` assignments and index definitions each allow one column. |
| Multiple inserted rows in one statement | **NOT SUPPORTED** | One parenthesized value list is parsed per `INSERT`; issue another statement for another row. |

Unsupported features listed here are absent from the implemented token/AST/parser/planner flow, not merely omitted from the examples. `ParserRejectsUnsupportedAndMultiStatementSyntax` demonstrates parser rejection with `SELECT *` and multiple statements in one SQL string.

## 3. Complete Working Example

Build the project first using the repository's CMake preset, then run this from the project root with a **new database directory**. The example uses the built executable path created by the `debug` preset; if you installed or copied the executable, use that executable name instead.

```powershell
cmake --preset debug
cmake --build --preset debug
.\build\debug\apps\kerndb_shell\kerndb_shell.exe --database .\stu_demo_db --execute "CREATE TABLE stu (id INT, name TEXT);" --execute "INSERT INTO stu VALUES (1, 'Aman');" --execute "INSERT INTO stu VALUES (2, 'Riya');" --execute "CREATE INDEX stu_id_idx ON stu(id);" --execute "SELECT id, name FROM stu;" --execute "SELECT id FROM stu WHERE id = 1;"
```

Expected output includes both rows for the first `SELECT`, and `1` for the indexed equality lookup. To verify that the data and index survive an orderly close/restart, run a second process:

```powershell
.\build\debug\apps\kerndb_shell\kerndb_shell.exe --database .\stu_demo_db --execute "SELECT id, name FROM stu WHERE id = 2;"
```

This returns `2 | Riya`. Persistent data is stored under `stu_demo_db` and survives a clean program restart. In contrast, starting `kerndb_shell` without `--database` creates an in-memory database whose data disappears on exit. Clean-restart persistence is tested; crash-safe durability is **NOT SUPPORTED** because the repository has no WAL or crash-recovery implementation.

## 4. B+ Tree Explained Using Actual Code

### Creating an index and storing entries

Run `CREATE INDEX stu_id_idx ON stu(id);`. In persistent mode, `execution::ExecuteCreateIndex` calls `PersistentStorage::CreateIndex`. That method creates a B+ tree, scans the existing table with `TableHeap::ScanWithRecordIds`, and inserts each integer key with its record ID. Therefore an index can be built either before or after rows are inserted. It rejects duplicate keys, so the index is unique.

Each leaf entry is `INT key -> RecordId`. Here `RecordId` is a heap `PageId` plus a slotted-page `SlotId`; the index points to a row's physical slot, not a copy of the row. `BPlusTreeEntry` is declared in `include/kerndb/storage/bplus_tree.h`.

### Leaf pages, internal pages, and lookup

`src/storage/bplus_tree.cpp` encodes both node kinds into normal 4096-byte `kIndex` pages:

- A **leaf** has sorted `(key, RID)` entries and a `next_leaf_page_id` link.
- An **internal node** has sorted separator keys and one more child page ID than separator keys.
- The current test-friendly capacity is eight leaf entries or eight internal separator keys (`kBPlusTreeMaxLeafEntries` and `kBPlusTreeMaxInternalKeys`).

`BPlusTree::Search(key)` starts at the root. At each internal node it uses `std::upper_bound` over separator keys to choose a child. At the leaf it uses `std::lower_bound` and returns that entry's RID, or `not found`. `ScanAll` follows the leftmost path and leaf links, but SQL does not currently expose an index range scan.

### Splits and root updates

`BPlusTree::Insert` sorts a new key into the leaf and rejects a key already present. When a leaf exceeds capacity, the sorted entries are divided into left and right leaves; the right leaf inherits the old sibling pointer, the left leaf links to the right, and the right leaf's first key is inserted into its parent as a separator. If an internal node overflows, its middle separator is promoted and its children are divided between the nodes. If the split node had no parent, `InsertIntoParent` creates a new internal root and updates both child parent IDs.

The tree object reports the new root page ID. `PersistentStorage::InsertIndexEntry` detects that change, appends updated metadata to `indexes/catalog.dat` through `IndexCatalogStore::Append`, and updates the in-memory catalog. During initial index creation, the final root is stored after the existing rows have been loaded. On reopen, `PersistentStorage::Open` loads the newest index metadata record and opens the tree at its saved root.

### Persistent files and buffer pool

Persistent databases use `database.meta`, `catalog.dat`, `tables/<table-id>.dat`, `indexes/catalog.dat`, and `indexes/<index-id>.dat`. The tree accesses pages through `PageManager` and `BufferPoolManager`, not directly through `DiskManager`. Node reads and writes use `PinnedPage` guards; mutating a page marks it dirty. The buffer pool flushes dirty pages on eviction, explicit flush, and orderly destruction.

This is orderly persistence, not transaction durability under power loss or process crash. The `wal` directory may be created during open, but there is no WAL writer or redo/undo recovery. SQL transaction control exists, but its undo history is in memory and is not a crash-recovery log.

### Mutation and transaction undo

`Database::Execute` handles transaction control statements directly. For persistent storage, each `INSERT`, `UPDATE`, or `DELETE` uses the existing `Mutation` context in `src/storage/persistent_storage.cpp`. It adds a reverse action immediately after each heap or B+ tree change succeeds. A failed statement runs only its own actions in reverse order before returning. After a successful statement, autocommit discards those actions; inside an explicit transaction they are appended to the transaction's undo history. `ROLLBACK` applies that combined history in reverse chronological order, while `COMMIT` discards it.

For an update, the heap replacement row is inserted first, each index's old key/RID is removed and its new key/replacement-RID is inserted, and the old heap slot is tombstoned. Undo therefore restores the old heap slot, removes the new index mapping, restores the exact old key-to-RID mapping, and tombstones the replacement row. Insert undo removes newly created index entries before tombstoning the inserted row. Delete undo restores the tombstoned row and reinserts its original index mappings. The failure-injection integration test deliberately fails after a persistent update has completed the replacement insert, both index changes, and old-row tombstone.

The default in-memory `Database()` backend does not use persistent heap/index mutation actions. At `BEGIN`, it copies the `InMemoryCatalog`; successful writes are visible through that catalog to later statements in the transaction, and rollback restores the saved catalog snapshot. `COMMIT` drops the snapshot. On either backend, a statement error whose compensation succeeds leaves an explicit transaction active. A compensation failure can prevent full restoration and is reported as an internal rollback error.

### Concurrency and transaction locks

Each persistent `Database::Open` session has its own active transaction context, with at most one explicit transaction per session. Opening the same normalized database path in one process shares its catalog, persistent storage, transaction manager, and lock manager. Transaction IDs are unique across runtimes in that process. The default in-memory `Database()` creates a private runtime and does not share state with other in-memory instances.

`LockManager` protects the catalog and whole tables with S/X locks. `SELECT` requests table S; `INSERT`, `UPDATE`, `DELETE`, and `CREATE INDEX` request table X. Ordinary statements request catalog S; DDL requests catalog X. All locks are retained through commit or abort, including read locks, which is a rigorous form of strict 2PL. The compatibility rule is S/S compatible; every combination involving X conflicts. These coarse locks protect records and index mappings through the SQL API, but do not provide row-level concurrency.

When a request blocks, the lock manager rebuilds a wait-for graph from live holders and queued requests. The requesting transaction is aborted if the request creates a cycle. `Database` rolls back its persistent mutation history, marks it aborted, releases its locks, and wakes waiters. The persistent storage mutex still serializes physical writes across tables, and the buffer-pool manager mutex remains held across synchronous page reads and flushes. Coordination is process-local; separate processes opening the same directory are not protected. See [`docs/CONCURRENCY_PHASE6.md`](docs/CONCURRENCY_PHASE6.md) for lock ordering, thread-safety scope, and limitations.

### Duplicate keys and removal

An index is unique: each integer key maps to at most one RID. Duplicate inserts fail with an `already_exists` error. Creating an index on a column with existing duplicate values also fails while the tree is populated. A table can have indexes on different columns, but `InMemoryCatalog::CreateIndex` rejects a second index on the same column.

`BPlusTree::Remove(key, rid)` removes that exact key/RID pair. Current deletion does not merge or reclaim underfull B+ tree nodes; it leaves separators as safe routing bounds. Heap tombstones and old slots are not compacted or reused.

### Trace: `SELECT id, name FROM stu WHERE id = 1`

1. `Database::Execute` calls `parser::ParseSql`; the lexer emits tokens and the parser builds `AstSelect` with two projection columns and an equality predicate.
2. `binder::BindStatement` resolves `stu`, `id`, and `name` against the catalog, and checks that the `1` literal is an `INT` matching `id`.
3. The binder records whether the table has an index on the filtered integer column. For persistent storage, the rule-based planner chooses `PhysicalIndexScan` only when that index exists; otherwise it chooses a sequential scan.
4. `execution::ExecuteIndexScan` asks `TableStorage::LookupByIndex` for the indexed column and key. Persistent storage finds a matching catalog index, opens its B+ tree, calls `BPlusTree::Search(1)`, then reads the row from the heap using the returned RID. The executor checks that the returned tuple still has key `1` and projects `id, name`.
5. If there is no persistent index for that column, the storage interface reports `index_available == false` and the executor falls back to a sequential heap scan. For a default in-memory database, the index-scan executor also uses the sequential-scan path.

The relevant tests are `PlannerUsesAnAvailablePersistentIntIndex`, `PersistentDatabaseBuildsMaintainsAndReopensAnIntIndex`, and `PersistentDatabaseIndexesExistingRowsAcrossSplitsAndRestart`. SELECT comparison predicates are implemented, but range predicates currently use sequential scans; SQL range index scans are **NOT SUPPORTED**.

## 5. Phase-by-Phase Architecture

The phase names come from `IMPLEMENTATION_PLAN.md` and the phase-specific documents. The code and tests show the following implementation status:

| Phase | What is present in this repository | Status and evidence |
|---|---|---|
| 0 — Foundation | CMake/C++20 targets, `Result`/`Status`, stable ID types, byte codecs, options, temporary-directory test support, CLI shell, metrics and event interfaces. | **Implemented foundation.** See `CMakeLists.txt`, `include/kerndb/{result.h,ids.h,options.h,byte_codec.h,telemetry.h}`, `src/common`, `src/observability`, and tests under `tests/unit/common` and `tests/unit/observability`. |
| 1 — SQL frontend and in-memory execution | Native lexer and recursive-descent parser, AST, `INT`/`TEXT` values, catalog, binder, logical/physical plans, in-memory create/insert/select/filter/projection, plus later mutation statements. | **Implemented initial SQL slice**, with the precise command limits in Section 2. See `src/parser`, `src/binder`, `src/catalog`, `src/planner`, `src/execution`, and `DatabaseExecutesTheApprovedPhaseOneVerticalSlice`. |
| 2 — Durable paged heap | Database metadata, fixed-size checksum-protected pages, disk/page managers, slotted-page heap records, persisted table catalog, and reopen. | **Implemented for orderly clean reopen.** See `src/storage/{database_metadata,disk_manager,page,page_manager,heap,catalog_store,record,persistent_storage}.cpp` and `PersistentDatabaseRetainsMultipleRowsAndPhaseOneSemanticsAfterRestart`. No crash-safe durability. |
| 3 — Buffer pool | Bounded cache (16 frames by default), pin guards, dirty-page writes, LRU selection among unpinned pages, flush and telemetry paths. | **Implemented.** See `src/storage/buffer_pool.cpp` and `tests/unit/storage/buffer_pool_test.cpp`, including `BufferPoolEvictsTheLeastRecentlyUnpinnedFrame` and `BufferPoolPersistsDirtyPagesAcrossAnOrderlyRestart`. |
| 4 — B+ tree and simple planning | Persistent unique `INT` indexes, index catalog, leaf/internal pages, equality lookup, leaf/internal splits, root metadata updates, and sequential fallback. | **Implemented for equality lookup.** There is no range index scan or statistics-driven plan selection. Phase 8 adds EXPLAIN and rule-based planning. See `src/storage/bplus_tree.cpp`, `src/storage/index_catalog_store.cpp`, `src/planner/planner.cpp`, and the B+ tree/integration tests. |
| 5 — Mutation / transactions | Persistent and in-memory `INSERT`, `UPDATE`, and `DELETE`; statement compensation; explicit `BEGIN`/`COMMIT`/`ROLLBACK`; persistent reverse-action history and in-memory catalog snapshots; transaction-local read visibility; DDL rejection; close-time rollback. | **IMPLEMENTED.** Evidence includes `PersistentStorageRollsBackAnUpdateAfterHeapAndIndexChanges`, `PersistentDatabaseCommitsExplicitInsertAndPersistsIt`, `PersistentDatabaseRollsBackExplicitInsertAcrossReopen`, `PersistentDatabaseRollsBackMultipleMutationsInReverseAndRestoresIndexedRid`, `PersistentDatabaseKeepsFailedStatementAtomicAndTransactionActive`, `PersistentDatabaseCloseRollsBackActiveTransaction`, and `InMemoryDatabaseSupportsExplicitTransactionRollback`. See `docs/MUTATIONS_PHASE5.md`, `src/database.cpp`, `src/storage/persistent_storage.cpp`, and the Phase 5 integration tests. Phase 6 supplies concurrency control; WAL, crash recovery, and crash-safe durability remain unsupported. |
| 6 — Concurrency and strict 2PL | Same-process persistent sessions; independent transaction IDs and lifecycle; catalog/table S/X locks retained to completion; deadlock detection and victim rollback; synchronized persistent storage, buffer pool, and B+ tree access. | **IMPLEMENTED for the documented process-local, table-lock scope.** Evidence includes the `PersistentSessions...` integration tests, four `LockManager...` unit tests, `TransactionManagerCreatesUniqueContextsAndTracksTerminalStates`, `BufferPoolSerializesConcurrentPageGuardAccess`, and `BPlusTreeSupportsConcurrentInsertionsAndLookups`. See `docs/CONCURRENCY_PHASE6.md`, `src/concurrency/lock_manager.cpp`, `src/transaction/transaction_manager.cpp`, and `src/database.cpp`. Cross-process coordination, row locks, MVCC, WAL, and crash recovery are unsupported. |
| 7 — WAL and crash recovery | Write-ahead log, durable LSN, redo/undo, checkpoints, startup recovery, and crash-point testing. | **POSTPONED.** No WAL or crash-recovery components are implemented. |
| 8 — Advanced SQL and query planning | ORDER BY, LIMIT, one INNER/LEFT nested-loop join, aggregates, GROUP BY/HAVING, qualified columns, predicate filtering, persistent equality-index selection, and EXPLAIN. | **IMPLEMENTED for the bounded syntax in `docs/ADVANCED_SQL_PHASE8.md`.** Planning is rule based without statistics or cost estimates. Stored NULLs, projection pushdown, hash joins, spill, and performance claims are deferred. WAL/recovery remains postponed. |

There is no separate B+ tree source module directory: the implementation is currently under `src/storage`. Also, `docs/DEVELOPMENT.md` and some Phase 1/2 notes describe earlier snapshots of the engine; later Phase 3/4/5 code and the current parser/tests establish the broader implementation described here.

### High-level query path

```text
CLI or C++ caller
  -> Database::Execute
     -> lexer and parser (SQL text -> AST)
     -> binder (names and types -> bound statement)
     -> planner (logical plan -> physical plan)
     -> executor (QueryResult)
        -> in-memory catalog/tuples, or
        -> PersistentStorage
           -> TableHeap / BPlusTree
              -> PageManager -> BufferPoolManager -> DiskManager -> .dat pages
```

## 6. Syntax and Viva Cheat Sheet

### Compact command table

| Command | Exact syntax | Status | Main purpose | Main component |
|---|---|---|---|---|
| Create table | `CREATE TABLE t (c INT|TEXT [, ...])` | SUPPORTED | Define a table and its columns. | Parser, binder, catalog, executor, persistent storage |
| Insert row | `INSERT INTO t VALUES (literal [, ...])` | SUPPORTED | Add one schema-ordered row. | Binder, executor, heap |
| Select | `SELECT c [, ...] FROM t [JOIN ...] [WHERE ...] [GROUP BY ...] [ORDER BY ...] [LIMIT n]` | PARTIAL | Project, filter, join, group, sort, and limit rows. | Planner, executor, heap or B+ tree |
| Explain | `EXPLAIN SELECT ...` | SUPPORTED | Display the logical and physical query plan. | Planner, executor |
| Create index | `CREATE INDEX i ON t (c)` | PARTIAL | Create a unique `INT` equality index. | Binder, executor, B+ tree, buffer pool |
| Update | `UPDATE t SET c = literal WHERE c = literal` | PARTIAL | Change one column in matching rows. | Binder, executor, heap, index maintenance |
| Delete | `DELETE FROM t WHERE c = literal` | PARTIAL | Remove matching rows. | Binder, executor, heap, index maintenance |
| Begin transaction | `BEGIN;` | SUPPORTED | Start one explicit transaction on this `Database`. | Parser, `Database`, storage backend |
| Commit transaction | `COMMIT;` | SUPPORTED | Discard undo history and end the active transaction. | `Database`, `PersistentStorage` |
| Roll back transaction | `ROLLBACK;` | SUPPORTED | Apply retained undo actions or restore the in-memory catalog snapshot. | `Database`, storage backend |
| Drop / alter | `DROP ...` / `ALTER TABLE ...` | NOT SUPPORTED | No grammar or executor exists. | — |
| Additional SQL limitations | aliases, multiple joins, DISTINCT, compound predicates | NOT SUPPORTED | These grammar and executor features are not implemented. | — |
| Transactional DDL | `CREATE TABLE` / `CREATE INDEX` inside a transaction | NOT SUPPORTED | These statements return an error while an explicit transaction is active. | `Database::Execute` |

### Likely PBL viva questions

1. **What is KernDB?** — A small C++ embedded database engine with its own SQL front end, execution pipeline, paged storage, buffer pool, and persistent B+ tree indexes.
2. **Does KernDB use SQLite or another database internally?** — No. The lexer, parser, catalog, executor, heap, and B+ tree are implemented in this repository.
3. **What happens after SQL reaches `Database::Execute`?** — It is lexed and parsed, bound to catalog objects, planned, then executed into a `QueryResult`.
4. **What does the lexer do?** — It recognizes keywords, identifiers, integer/string literals, punctuation, whitespace, and `--` comments, and records source locations.
5. **What does the parser produce?** — One AST statement variant: create table/index, insert, select, update, delete, begin, commit, or rollback.
6. **What does the binder check?** — It resolves table and column names and verifies literal/column types, insert arity, duplicate column names, and the `INT`-only index restriction.
7. **Which data types exist?** — Stored columns use `INT` (signed 64-bit) and `TEXT`; `AVG` returns `REAL`, and joins/empty aggregates can return output-only `NULL` values.
8. **Can SQL commands be combined in one SQL string?** — No. `ParseSql` accepts one statement and at most one trailing semicolon. The CLI can take multiple repeated `--execute` options.
9. **What is a RID?** — A record identifier made of a heap page ID and a slot ID; the B+ tree maps keys to these RIDs.
10. **What is a heap table?** — Rows are stored in slotted pages; a slot records where the encoded row lives on a page.
11. **What is the buffer pool?** — A bounded in-memory cache for pages. A pin guard keeps a page resident while code accesses it; dirty pages are flushed on eviction or orderly close.
12. **What page size and default pool size are used?** — Pages are 4096 bytes; the buffer pool defaults to 16 frames.
13. **How is an index organized?** — Leaf pages store sorted key/RID entries; internal pages store separator keys and child page IDs; leaves link to the next leaf.
14. **Are index keys unique?** — Yes. An indexed `INT` key can identify at most one row; duplicate keys are rejected.
15. **What happens when a B+ tree node fills up?** — It splits, a separator moves into the parent, and a root split creates a new root page.
16. **How does an equality index lookup reach the row?** — It descends the internal nodes, searches a leaf, gets a RID, then reads that heap slot.
17. **Does every integer `WHERE` use an index?** — Persistent planning chooses a matching index for `INT` equality only when that index exists; otherwise it uses a sequential scan. In-memory mode scans.
18. **Does KernDB support range queries using the B+ tree?** — SELECT accepts range comparisons but executes them with a sequential scan. SQL range index scans are **NOT SUPPORTED**.
19. **Where is persistent data stored?** — In the selected database directory, mainly `catalog.dat`, `tables/<id>.dat`, `indexes/catalog.dat`, and `indexes/<id>.dat`.
20. **Does data survive restarting the program?** — Persistent committed changes and completed rollbacks survive an orderly close and reopen with `--database DIRECTORY`. Closing with a transaction active rolls it back. The default REPL is in memory and loses data at exit.
21. **Is persistence crash-safe?** — No. There is no WAL or recovery code, and writes may reach disk before `COMMIT`; no crash-safe transactional durability is promised.
22. **How are `UPDATE` and `DELETE` implemented?** — They support one equality condition. Persistent updates write a replacement row, retarget indexes to its new RID, and tombstone the old slot; deletes remove index entries and tombstone matched slots.
23. **What does transaction support cover?** — One explicit transaction per `Database` session. Separate persistent sessions for the same directory can execute concurrently in one process; `INSERT`, `UPDATE`, and `DELETE` are undoable until commit, and `SELECT` sees the session's successful writes.
24. **How does persistent rollback restore an update?** — It reverses storage actions: restore the old heap slot, remove the new key/RID, restore the old key/RID, and tombstone the replacement row. This preserves the original RID mapping.
25. **How does the in-memory backend roll back?** — It copies the `InMemoryCatalog` at `BEGIN` and restores that snapshot on `ROLLBACK`.
26. **What happens after a statement error inside a transaction?** — Its partial persistent changes are compensated. If that succeeds, the transaction stays active and later statements may continue; a compensation failure is reported as an internal rollback error.
27. **Are nested transactions or transactional DDL supported?** — No. Nested `BEGIN` is rejected; `COMMIT`/`ROLLBACK` without an active transaction are rejected; `CREATE TABLE` and `CREATE INDEX` are rejected inside a transaction.
28. **What does closing with an active transaction do?** — `Database` attempts rollback before storage closes. This is a clean-shutdown behavior, not crash recovery.
29. **How does Phase 6 coordinate concurrent sessions?** — Catalog and table S/X locks are retained until commit or abort. Reads use S, writes use X, and deadlock detection aborts the requesting transaction that closes a cycle.
30. **What does Phase 6 not provide?** — It does not coordinate separate processes, provide row-level locks or MVCC, or implement WAL, crash recovery, or crash-safe transactional durability.
31. **Does Phase 5/6 make persistence crash-safe?** — No. Commit and rollback persist across clean close/reopen, but there is no WAL or recovery code and writes may reach disk before commit.
32. **Which common SQL features are still missing?** — Aliases, additional joins, `DISTINCT`, stored NULLs, expressions, subqueries, compound boolean conditions, range index scans, and cost-based planning are **NOT SUPPORTED**.

## 7. Limitations

- Each `Database` session has at most one active explicit transaction; its calls are serialized. Separate persistent sessions opened on the same normalized directory can run concurrently within one process.
- Locks are catalog/table-level S/X locks held until transaction completion. There are no row/key locks. Persistent storage serializes physical mutations across tables, and separate processes do not coordinate.
- Nested transactions and savepoints are unsupported. `COMMIT` and `ROLLBACK` require an active transaction.
- `CREATE TABLE` and `CREATE INDEX` are rejected inside explicit transactions; transactional DDL is not implemented.
- **Statement atomicity:** a failed persistent mutation compensates its completed steps before returning. If compensation itself encounters a storage error, complete restoration is not guaranteed; the error is reported and an explicit transaction remains available for a rollback attempt.
- **Explicit transaction atomicity:** during normal execution through `Database`, `ROLLBACK` reverses successful mutations and closing a session with an active transaction rolls it back. Concurrent sessions are isolated by rigorous table-level 2PL. This does not make the transaction crash-atomic.
- **Rollback storage errors:** if transaction-level or deadlock-victim undo encounters a storage error, KernDB reports it but currently retires the transaction and releases its locks; full restoration is not guaranteed. This error path has no failure-injection test.
- **Clean-shutdown persistence:** committed state and completed rollback state survive an orderly close and reopen.
- **Crash recovery and durability:** there is no WAL, recovery, or crash-safe transactional durability. A crash during an active transaction can leave uncommitted or partial state because page writes can reach disk before commit.
- Existing SQL limits still apply: one-row `INSERT`, one-column assignment per `UPDATE`, equality-only UPDATE/DELETE predicates, unique `INT` indexes, one two-table join, one WHERE and one HAVING predicate, no aliases/expressions/subqueries/compound predicates, and no stored NULL.
- The Phase 8 optimizer is rule based. It pushes a single filter to its scan input and selects an existing persistent INT equality index; it has no statistics or cost estimates. Projection pushdown is not available through the current full-tuple storage scan API. No Phase 8 performance improvement has been measured.

## 8. Verification

This reference reflects the current source, Phase 5, Phase 6, and Phase 8 documentation, and the verification run recorded below.

Commands run from the project root:

```text
cmake --preset debug
```

Result: configure and generate succeeded; build files were written to `build/debug`.

```text
cmake --build --preset debug
```

Result: succeeded; rebuilt the SQL front end, binder, planner, executor, `libkerndb.a`, `kerndb_shell.exe`, and `kerndb_phase2_tests.exe`.

```text
ctest --preset debug
```

Result: **3/3 CTest tests passed**: `kerndb.phase2.unit_and_integration`, `kerndb.phase2.shell_help`, and `kerndb.phase2.shell_version`.

```text
.\build\debug\tests\kerndb_phase2_tests.exe
```

Result: **97/97 unified unit and integration tests passed.** This includes the Phase 0–6 regression cases, advanced SQL unit/integration coverage, and CLI shell smoke tests.

The CLI `--help` and `--version` commands passed. An in-memory CLI run of `EXPLAIN SELECT ... GROUP BY ... HAVING ... ORDER BY ... LIMIT ...` displayed both logical and physical trees with Sequential Scan, Aggregate, Filter, Sort, Limit, and Projection operators. Persistent index-plan selection is covered by `PersistentOptimizerExplainsAndUsesAnAvailableEqualityIndex`.

No performance benchmark was run, so no performance improvement is claimed.

ThreadSanitizer availability probe:

```text
$probePath = Join-Path $env:TEMP 'kerndb_phase6_tsan_probe.exe'
$source = 'int main() { return 0; }'
$source | & 'C:\msys64\ucrt64\bin\g++.exe' -x c++ -std=c++20 -fsanitize=thread -o $probePath -
```

Result: unavailable with the configured MSYS2 UCRT64 GCC. The linker reported `cannot find -ltsan`. A WSL availability check returned `Wsl/Service/E_ACCESSDENIED`; the repository's `sanitize` preset is for AddressSanitizer and UndefinedBehaviorSanitizer on Linux/WSL.

## Source and test index

| Area | Main files | Useful tests |
|---|---|---|
| CLI | `apps/kerndb_shell/main.cpp`, `apps/kerndb_shell/shell.cpp` | `ShellHelpAndVersionAreAvailableWithoutDatabaseAccess`, `ShellExecutesMultipleStatementsAgainstOneInMemoryDatabase`, `ShellUsesPersistentDatabaseDirectoryAcrossRuns` |
| Lexer/parser/AST | `src/parser/lexer.cpp`, `src/parser/parser.cpp`, `src/parser/ast.h` | `LexerHandlesKeywordsCommentsEscapedStringsAndSpans`, `ParserBuildsAllSupportedStatementKinds`, `ParserBuildsAdvancedSelectClausesAndExplain` |
| Binder/catalog/planner | `src/binder/binder.cpp`, `src/catalog/catalog.cpp`, `src/planner/planner.cpp` | `BinderResolvesQualifiedJoinColumnsAndRejectsAmbiguity`, `BinderValidatesGroupingAggregateTypesAndLimit`, `PlannerUsesAnAvailablePersistentIntIndex` |
| Executor | `src/execution/executor.cpp`, `src/database.cpp` | `ExecutorSortsStablyAndAppliesLimitAfterOrderBy`, `ExecutorRunsInnerAndLeftNestedLoopJoinsWithNullExtension`, `ExecutorComputesAggregatesGroupsHavingAndEmptyInputSemantics`, `DatabaseExplainsAdvancedOperatorsAndRejectsInvalidAdvancedSql` |
| Heap and records | `src/storage/heap.cpp`, `src/storage/record.cpp` | `TableHeapPersistsAndScansTuplesThroughTheBufferPool`, `TableHeapTombstonesDeletedRecordsAndCanRestoreThemForRollback` |
| Buffer pool and pages | `src/storage/buffer_pool.cpp`, `src/storage/page_manager.cpp`, `src/storage/page.cpp`, `src/storage/disk_manager.cpp` | `BufferPoolEvictsTheLeastRecentlyUnpinnedFrame`, `BufferPoolPersistsDirtyPagesAcrossAnOrderlyRestart` |
| B+ tree/index catalog | `src/storage/bplus_tree.cpp`, `src/storage/index_catalog_store.cpp`, `src/storage/persistent_storage.cpp` | `BPlusTreeMaintainsSortedEntriesAcrossLeafAndInternalSplits`, `BPlusTreePersistsRootLeavesAndEntriesAcrossReopen`, `PersistentDatabaseIndexesExistingRowsAcrossSplitsAndRestart` |
| Persistent SQL and Phase 5 transactions | `src/database.cpp`, `src/storage/persistent_storage.cpp`, `docs/MUTATIONS_PHASE5.md` | `PersistentDatabaseUpdatesRowsAndMaintainsUniqueIndexes`, `PersistentDatabaseDeletesRowsMaintainsIndexesAndReusesKeys`, `PersistentStorageRollsBackAnUpdateAfterHeapAndIndexChanges`, `PersistentDatabaseCommitsExplicitInsertAndPersistsIt`, `PersistentDatabaseRollsBackMultipleMutationsInReverseAndRestoresIndexedRid`, `PersistentDatabaseKeepsFailedStatementAtomicAndTransactionActive`, `PersistentDatabaseCloseRollsBackActiveTransaction`, `InMemoryDatabaseSupportsExplicitTransactionRollback` |
| Concurrency and Phase 6 transactions | `include/kerndb/concurrency/lock_manager.h`, `src/concurrency/lock_manager.cpp`, `include/kerndb/transaction/transaction_manager.h`, `src/transaction/transaction_manager.cpp`, `src/database.cpp`, `docs/CONCURRENCY_PHASE6.md` | Four `LockManager...` tests, `TransactionManagerCreatesUniqueContextsAndTracksTerminalStates`, `BufferPoolSerializesConcurrentPageGuardAccess`, `BPlusTreeSupportsConcurrentInsertionsAndLookups`, `PersistentSessionsBlockConflictingWritesAndCommitIndependentTransactions`, `PersistentSessionsAllowConcurrentReadsAndBlockReadWriteConflicts`, `PersistentSessionsDetectDeadlockRollbackVictimAndWakeSurvivor` |
