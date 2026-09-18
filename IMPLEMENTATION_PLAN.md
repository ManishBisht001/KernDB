# KernDB implementation plan

## Repository assessment

**Assessment date:** 2026-09-14

The repository contains only an initialized .git directory. It has no commits,
tracked files, source tree, CMake project, tests, dependency manager, or existing
architecture to preserve.

KernDB can establish its subsystem boundaries from the first commit. The
recommended baseline is C++20, CMake, CTest, and the standard library. The engine
will implement its own SQL lexer/parser and will not embed, invoke, or depend on
SQLite, PostgreSQL, MySQL, DuckDB, or another database engine.

## Design principles and scope

- Start as an embedded, single-process C++ engine. A development CLI calls the
  same public API a future web adapter calls.
- Keep SQL front end, execution, storage, buffering, indexing, locking, and
  recovery separate. AST types do not reach storage; page bytes do not reach
  the executor.
- Favor persistent-format versioning, checksums, testable interfaces, and failure
  injection over feature count or premature optimization.
- Build in correctness order: in-memory semantics; paged persistence; buffer
  pool; indexes; transactions; concurrency; WAL/recovery; advanced operations.
- Add metrics and structured events as non-blocking engine interfaces early.
  A web UI consumes them later, but is not part of the engine.
- Use stable numeric identifiers for objects, pages, records, transactions, and
  queries. Never use a user-supplied name as a physical identifier.

The first vertical slice remains deliberately small:

~~~
CREATE TABLE people (id INT, name TEXT);
INSERT INTO people VALUES (1, 'Ada');
SELECT id, name FROM people WHERE id = 1;
~~~

Joins, aggregates, NULL, secondary indexes, multi-statement transactions,
concurrent writers, and crash guarantees are later milestones.

## Recommended architecture

~~~
Client / development CLI / future web adapter
                 |
             db::Database
                 |
  SQL frontend -> binder -> planner -> executor
                 |                    |
              catalog             transaction manager
                                      |
                        lock manager / log manager
                                      |
                      access methods (heap, B+ tree)
                                      |
                         buffer pool / page guards
                                      |
                    disk manager / filesystem abstraction
                                      |
                         database and WAL files

MetricsRegistry and EventSink observe every layer without controlling it.
~~~

| Module | Owns | Must not know |
| --- | --- | --- |
| common | IDs, status/result, byte codecs, configuration, assertions | SQL and page policy |
| types | SQL values, schemas, tuple codecs, comparisons | file layout, locks |
| parser | tokens, source spans, AST, grammar | catalog internals, pages |
| binder | name resolution and type checking against catalog | raw disk bytes |
| catalog | table/index metadata and object IDs | parser strategy, buffer frames |
| planner | logical and physical plan selection | page and file APIs |
| execution | operators and result production | direct filesystem access |
| storage | pages, heap, allocation, disk files, record layout | SQL strings and AST |
| buffer | cached frames, pins, latches, replacement, flush | SQL semantics |
| index | B+ tree and key codecs | SQL grammar |
| concurrency | transactions, locks, deadlock detection | tuple-byte encoding |
| recovery | WAL, checkpoints, redo, undo, startup recovery | planner details |
| observability | metric snapshots and structured events | control over query success |
| shell | interactive development interface | internal subsystem classes |

Execution operators receive interfaces through an ExecutionContext, rather than
concrete disk or buffer objects. This makes subsystem tests simple and prevents
dependencies from bypassing the storage stack.

## Data flow: SQL query to disk

### Reads

1. A caller opens a Session and passes SQL to Database::Execute.
2. The native lexer emits tokens with byte, line, and column positions; the
   parser emits an AST. Syntax failures identify source range and expectation.
3. The binder resolves tables/columns through the catalog, assigns column IDs,
   and verifies types. Missing or ambiguous identifiers fail here.
4. The planner creates a logical operator tree, then a physical plan. Initial
   choices are rule based; later they consult catalog statistics.
5. The transaction manager provides execution context. Pull-based scan, filter,
   and projection operators request typed tuples from access methods.
6. Heap or B+ tree code fetches RAII page guards from the buffer pool. On a
   miss, the pool selects an unpinned victim, flushes it if eligible, reads the
   requested page through the disk manager, and returns it pinned.
7. Heap/index code decodes visible records. The executor yields result rows and
   releases guards promptly. Metrics and correlated events are emitted.

### Writes in the mature system

1. The executor uses a transaction ID and acquires logical locks under the
   isolation policy.
2. Heap/index code fetches and write-latches pages, updates bytes, and creates a
   physiological WAL record containing redo/undo information.
3. The log manager assigns an LSN and appends the record. The changed page stores
   that page LSN. Commit forces its commit record in durable mode.
4. The buffer pool may flush later, but only after page_lsn is no greater than
   the durable WAL LSN.
5. Commit/abort completes transaction state and releases locks by policy.

### Startup recovery

1. Validate database metadata, format version, page size, catalog roots, and
   checksums.
2. Read checkpoint and WAL; analyze active transactions and dirty pages; redo
   records newer than each page LSN; undo incomplete transactions.
3. Record recovery completion before admitting sessions. Expose recovery time,
   redo/undo counts, and corruption failures through startup events.

## Storage architecture

### Files and identifiers

Use a database directory rather than a single unstructured file:

~~~
<database>/
  database.meta       # format version, page size, catalog root, UUID
  catalog.dat         # catalog and system pages
  tables/<table-id>.dat
  indexes/<index-id>.dat
  wal/<segment>.log
  tmp/                # future restart-safe operator files
~~~

TableId, IndexId, PageId, RecordId (RID), TransactionId, and LSN are fixed-width
unsigned types. A RID is page ID plus slot number, never a byte offset. Central
byte readers/writers encode on-disk values in documented little-endian order;
C++ struct layout is not a file format.

### Common page format

Use a configurable fixed page size; **4096 bytes** is recommended initially. It
is recorded in database.meta and is immutable for one database directory.

~~~
+---------------- common page header ----------------+
| magic | format version | page type | self page ID   |
| page LSN | checksum | flags | reserved             |
+----------------------------------------------------+
| type-specific content                              |
+----------------------------------------------------+
~~~

Define page types only when owned by a subsystem: metadata, catalog, heap, B+
tree internal, B+ tree leaf, free-space map, and future overflow. Page reads
validate magic, length, self identity, version, and checksum. A mismatch is a
corruption error, never silently ignored.

### Heap and record format

Heap tables use slotted pages:

~~~
+-------------- heap header --------------+
| slot count | free start | free end       |
+------------------------------------------+
| tuple bytes grow upward ->               |
|                          <- slot entries |
+------------------------------------------+

slot = tuple offset | tuple length | generation/flags
~~~

Slots remain tombstones until documented reuse rules permit recycling. The
generation detects stale RIDs. Compaction can move tuple bytes under a write
latch while preserving RID stability by updating slot offsets.

Start with schema-order tuple encoding, schema version, optional null bitmap,
fixed-width INT, and bounded inline TEXT. Define tuple/text maxima. Large
variable values later use overflow pages. Initial allocation may scan heap pages
for free space; a later free-space map is advisory while the page stays
authoritative.

## Buffer pool architecture

The buffer pool is the only normal path from access methods to persistent pages:

~~~
PageId -> page-table entry -> frame
frame  -> bytes, pin count, dirty flag, page LSN, latch, replacer state
~~~

- FetchPageRead and FetchPageWrite return move-only RAII guards. A guard owns a
  pin and shared/exclusive latch and releases both deterministically.
- NewPage allocates a persistent page and initializes its common header.
- Callers cannot adjust pin counts; only guards unpin.
- A deterministic LRU or clock replacer is sufficient initially. If all frames
  are pinned, return BufferPoolExhausted instead of waiting indefinitely.
- Protect page-table metadata separately from per-frame contents. Never retain a
  global buffer-pool mutex during disk I/O.
- Dirty flushing observes the WAL ordering hook. Deallocation waits until no
  guard, transaction, or index can reference a page.

First metrics: hits, misses, evictions, dirty flushes, all-pinned failures, pin
duration, and I/O latency.

## Index architecture

Implement a B+ tree only after durable heap behavior is stable. Its access method
is key/RID oriented:

~~~
Insert(key, rid) | Delete(key, rid) | GetEqual(key) | RangeScan(lower, upper)
~~~

Leaves contain sorted key/RID entries and sibling IDs. Internal pages contain
separator keys and child IDs. A metadata page owns root ID and key codec version.
Start with integer keys; add composite and variable keys only after encoding,
comparison, splitting, merging, and range scans are proven.

Use documented latch coupling: acquire parent then child, releasing ancestors
only after a child is safe for the operation. Unique index checks happen in the
access method under an appropriate lock. The planner chooses an index only for
supported predicates; sequential scans remain a correct fallback.

## Transactions, locking, and concurrency

Transactions transition through:

~~~
ACTIVE -> COMMITTED
ACTIVE -> ABORTING -> ABORTED
~~~

The transaction manager owns IDs, state transitions, write sets, last LSN, and
the current session transaction. Autocommit wraps one statement; explicit BEGIN,
COMMIT, and ROLLBACK arrive later.

The recommended initial concurrent model is **strict two-phase locking (strict
2PL)**. The lock manager grants logical table, RID, and later index-key resources
with shared and exclusive locks. Retain all locks to commit/abort in the first
concurrent release. Resource queues, compatible-grant rules, waiters, and a
wait-for graph support deterministic deadlock detection; a repeatable policy such
as aborting the youngest transaction selects the victim.

| Mechanism | Protects | Lifetime | Owner |
| --- | --- | --- | --- |
| Frame/page latch | in-memory bytes/metadata | one operation | thread/guard |
| Transaction lock | logical conflict/visibility | transaction | transaction |
| Allocation mutex | allocation metadata | one update | subsystem operation |

Never wait for a lock while retaining an unrelated page latch or buffer-pool
mutex. Establish and assert lock ordering before background workers exist.

## Recovery architecture

Add WAL after reliable transactions and buffering. Use a compact
ARIES-inspired physiological design:

- record fields: LSN, prior transaction LSN, transaction ID, type, target page,
  payload, and checksum;
- update records contain redo and undo information; before/after images are
  acceptable for the initial implementation;
- page headers retain newest applied LSN;
- commit/abort are explicit records;
- WAL is segmented and tracks durable LSN;
- checkpoints describe active transactions/dirty pages but are not sole truth.

Implement append/force ordering, then redo-only recovery, loser undo, fuzzy
checkpoint analysis/redo/undo, then systematic crash-point tests. Do not claim
ACID durability until crashes are injected before/after log append, log force,
page flush, commit, B+ split, and checkpoint. Wrap filesystem durability
barriers inside disk management and test their failure translation.

## Threading model

Start synchronous and single-threaded. The mature model may use client/session
threads, an optional bounded query-worker pool, a WAL group-commit coordinator,
a buffer writer, a maintenance/checkpoint/statistics worker, and an optional
bounded event dispatcher.

Every background thread uses std::stop_token, bounded queues or condition
variables, and joins before close. No worker may retain a reference to a
destroyed database subsystem. Parallel query execution is deferred until serial
execution, locking, and recovery are proven.

## Error handling and telemetry

Use project-owned Result<T> and Status for expected errors. Each error has a
stable code, readable message, and context such as SQL span, page ID, file path,
transaction ID, or OS std::error_code. Categories: syntax/binding/type/
unsupported; constraint/transaction/deadlock; resource; I/O/corruption; and
debug-only invariant failure. Never use an uncontextualized boolean or propagate
errno across layers.

Expose a UI-neutral contract:

~~~
MetricsRegistry::Snapshot() -> counters, gauges, latency histograms
EventSink::Publish(DatabaseEvent)
~~~

Events have time, severity, component, correlation/query ID, transaction ID when
applicable, and named fields. Start with an in-memory ring buffer and CLI
consumer. Do not record raw SQL values or user data by default.

## CMake, test, and source layout

Use CMake 3.25+, C++20, CTest, out-of-source builds, target-specific options,
and one initial kerndb library. Avoid global include paths, package managers, and
third-party source trees. Use a repository-owned minimal test harness initially;
adopting GoogleTest/Catch2 is an approval decision.

~~~
CMakeLists.txt
cmake/
  KernDBOptions.cmake
  KernDBWarnings.cmake
include/kerndb/
  database.h  result.h  options.h  telemetry.h
src/
  common/  types/  parser/  binder/  catalog/  planner/  execution/
  storage/disk/  storage/page/  storage/heap/
  buffer/  index/  concurrency/  recovery/  observability/
apps/kerndb_shell/
tests/
  test_support/  unit/  component/  integration/sql/  concurrency/  recovery/
benchmarks/
docs/
  IMPLEMENTATION_PLAN.md  architecture/  formats/  adr/
~~~

Suggested targets: kerndb, kerndb_shell, kerndb_test_support, independent CTest
executables, and opt-in kerndb_bench. Presets cover Debug, Release, and
sanitizers where supported.

| Level | Focus | Examples |
| --- | --- | --- |
| Unit | isolated contracts | lexer spans, codecs, slots, replacement |
| Component | subsystem plus temp files | heap reopen, B+ split, WAL segments |
| Integration | SQL to rows | create/insert/select, type errors, plan choice |
| Concurrency | controlled schedules | lock waits, deadlocks, latch ordering |
| Recovery | injected crashes | redo/undo and idempotent restart |
| Property/fuzz | invariant discovery | parser inputs, random heap/index model |
| Performance | trend regression | scan throughput, buffer hit rate |

Each test receives a separate temporary database. Formats receive boundary and
golden-byte tests. Random heap/index work compares to a small reference model.
Formatting, warning-clean builds, sanitizers, and all CTest suites run in CI.

## Phased roadmap

### Phase 0 — Foundation

**Goal:** Reproducible, testable C++ workspace and core contracts.

**Components:** root/subdirectory CMake, CTest, warnings/sanitizers, formatting,
Result/Status, IDs, byte codecs, configuration, temporary test support, CLI
skeleton, and basic metrics/events.

**Dependencies:** C++20 compiler, CMake, standard library.

**Expected files/modules:** root CMakeLists, cmake files, public headers,
src/common, src/observability, apps/kerndb_shell, test support, and unit/common.

**Tests:** status context; endian round trips; bounds/overflow; ID behavior;
temporary DB setup; metric/event behavior; Debug/sanitizer build smoke tests.

**Working at end:** a fresh checkout configures, builds, and runs tests; the CLI
starts/exits but does not create a database.

### Phase 1 — SQL frontend and in-memory execution

**Goal:** Verify language, types, and execution semantics before persistence.

**Components:** native lexer; recursive-descent or Pratt parser; AST; in-memory
catalog; binder; Value/schema/tuple model; logical/physical plans; in-memory
create/insert/sequential scan/filter/projection; result formatting.

**Dependencies:** Phase 0; no external parser.

**Expected files/modules:** src/types, parser, binder, catalog, planner,
execution; matching tests and SQL fixtures.

**Tests:** tokens, strings, comments, spans; valid/invalid grammar; binder
name/type/arity errors; expression semantics; SQL end-to-end; unsupported
feature errors; malformed-input fuzz/property tests.

**Working at end:** initial SQL subset is correct in one process, in memory only.

### Phase 2 — Durable paged heap

**Goal:** Persist tables using correct fixed pages and records.

**Components:** database directory/open validation; disk manager; common page
format/checksum; allocator; slotted heap; table heap; persisted catalog; page
inspection.

**Dependencies:** Phase 0 contracts and Phase 1 execution interfaces.

**Expected files/modules:** src/storage/disk, storage/page, storage/heap; catalog
persistence; docs/formats; component fixtures.

**Tests:** create/open/reopen; header/version/checksum validation; exact reads;
full-page insertion; slot reuse/compaction; variable-length bounds; stable RID;
scan after reopen; corrupt/truncated diagnostics; persistent SQL integration.

**Working at end:** one writer can CREATE TABLE, INSERT, and sequential SELECT
across a clean restart. Writes are not crash-safe yet.

### Phase 3 — Buffer pool

**Goal:** Route every normal page request through a bounded, correct cache.

**Components:** frames/page table; RAII guards; LRU/clock replacer; dirty state;
flush; pin accounting; WAL-ordering hook; buffer metrics; heap migration.

**Dependencies:** Phase 2 page identity/disk manager.

**Expected files/modules:** src/buffer (manager, frame, guard, replacer), storage
adapters, and buffer tests.

**Tests:** hit/miss; replacement; all-pinned exhaustion; dirty eviction; guard
destruction; read/write latch exclusion; I/O failure integrity; small-pool heap.

**Working at end:** bounded cached heap access persists after orderly close and
emits cache/flush metrics.

### Phase 4 — B+ tree indexes and simple planning

**Goal:** Add durable integer equality/range index access.

**Components:** integer codec; B+ metadata/internal/leaf pages; search/insert/
split; leaf links; unique checks; index catalog; CREATE INDEX; index scan;
rule-based selection; basic statistics.

**Dependencies:** Phases 1 through 3.

**Expected files/modules:** src/index (tree, codecs, metadata), catalog/executor
additions, EXPLAIN representation, and index tests.

**Tests:** random/model insertion; root/leaf/internal splits; duplicates; ranges
over leaf links; reopen; small pool; heap/index consistency; eligibility and
sequential fallback; SQL integration.

**Working at end:** indexed integer equality/range predicates use B+ trees while
unindexed queries remain correct heap scans.

### Phase 5 — Mutation and single-threaded transactions

**Goal:** Add explicit transaction semantics before session interleaving.

**Components:** BEGIN/COMMIT/ROLLBACK; lifecycle/write set; update/delete; index
maintenance; initial constraints; autocommit; pre-WAL abort mechanism.

**Dependencies:** Phase 4 access methods and planner/executor.

**Expected files/modules:** src/concurrency/transaction, mutation executors,
transaction grammar/binding, statement-atomicity documentation.

**Tests:** autocommit; commit/rollback; rejected post-abort work; update/delete;
index consistency; atomic constraint failures; mutation failure injection;
orderly reopen.

**Working at end:** one thread supports correct explicit and autocommit
transactions. Crash atomicity/concurrent sessions are not promised.

### Phase 6 — Concurrent sessions and strict 2PL

**Goal:** Safely run multiple sessions without data or logical races.

**Components:** session manager; lock resource IDs/queues; S/X locks; strict
2PL; wait-for deadlock detection/victim rollback; latch-order assertions; shared
buffer pool safety; transaction/lock telemetry.

**Dependencies:** Phases 3 and 5; abort must be dependable.

**Expected files/modules:** lock manager, transaction manager, deadlock detector,
deterministic scheduler/test hooks, and concurrency documentation.

**Tests:** lock compatibility; X waits; retention; upgrade policy; deadlock
cycle/victim; wait cancellation on abort; sanitizer races; forced scan/write
interleavings; assertion that lock wait holds no page latch.

**Working at end:** local sessions provide documented strict-2PL behavior and
predictable deadlock resolution.

### Phase 7 — WAL and crash recovery

**Goal:** Recover a consistent state after every supported crash point.

**Components:** log codecs; segmented log manager; durable LSN; WAL-before-page
flush; redo/undo; startup recovery; crash points; checkpoints; torn-log/page
validation; recovery telemetry.

**Dependencies:** Phases 2, 3, 5, and 6; stable page LSN/write-chain contracts.

**Expected files/modules:** src/recovery (log record, manager, recovery,
checkpoint); recovery harness; WAL format documentation.

**Tests:** crashes before/after append, force, page flush, commit, abort, B+
split, and checkpoint; redo; loser undo; repeated recovery; torn WAL; checksum
failure; group commit.

**Working at end:** restart preserves committed work and removes uncommitted work
at every supported injected crash point.

### Phase 8 — Query capability and operations

**Goal:** Improve SQL breadth, planning, and operability.

**Components:** joins (nested loop then hash), sort/limit/aggregates, predicate
normalization, statistics, cost-informed planner, EXPLAIN, memory-limited
operators/spill, buffer writer/checkpointer, telemetry, benchmarks, maintenance.

**Dependencies:** Phases 6–7. Spilling/background work has transaction and
shutdown semantics.

**Expected files/modules:** extended planner/executors/stats/observability,
benchmarks, workload fixtures, and metrics documentation.

**Tests:** join/aggregate suites; plan golden cases; statistics bounds; memory
limit/spill cleanup; benchmark smoke; event compatibility; background shutdown/
checkpoint regression.

**Working at end:** a small but coherent database has durable concurrent indexed
storage, recovery, and telemetry ready for a separate web layer.

### Phase 9 — Approved research tracks

**Goal:** Explore advanced choices without destabilizing the baseline.

**Components:** choose deliberately from MVCC, composite/secondary indexes,
parallel execution, compression, encryption, replication/log shipping, adaptive
replacement, or vectorized/external operators.

**Dependencies:** stable Phase 8, ADR, workload evidence, and explicit approval
per track.

**Expected files/modules:** feature-specific modules and ADRs only.

**Tests:** feature semantic model, recovery/isolation interactions, performance
comparison with Phase 8, and regressions.

**Working at end:** each feature has documented trade-offs and preserves core
invariants.

## Decisions requiring approval before implementation

1. **Toolchain/platforms:** Recommend C++20, CMake 3.25+, Windows/Linux, and
   MSVC/Clang/GCC. Confirm target compilers and whether POSIX-only APIs are
   acceptable.
2. **Deployment boundary:** Recommend an embedded C++ library plus development
   CLI; no TCP server until the separate web layer. Confirm.
3. **Persistent format:** Recommend 4 KiB little-endian checksummed/versioned
   pages and no backwards-compatibility promise before v1.0. Confirm page size
   and migration expectations.
4. **SQL v0:** Recommend CREATE TABLE, INSERT, single-table SELECT ... WHERE,
   INT, and TEXT. Confirm NULL behavior, constraints, identifier casing, and
   early required types.
5. **Concurrency model:** Recommend strict 2PL/serializable behavior before
   optional MVCC. Confirm whether MVCC or snapshot isolation is a priority.
6. **Durability timing:** Recommend clean-shutdown persistence through Phase 6,
   full crash safety in Phase 7. Confirm whether minimal WAL belongs earlier.
7. **Test policy:** Recommend CTest plus an in-repo harness initially. Confirm
   whether GoogleTest or Catch2 is preferred.
8. **Telemetry consumers:** Recommend in-process snapshots/events without HTTP
   in the engine. Confirm whether web UI, Prometheus-style scraping, logs, or
   tracing should shape the future contract.
9. **Learning priority:** Confirm the most important advanced topics among
   recovery, concurrency, query processing, indexing, and OS I/O.

After approval, implementation begins only with Phase 0. Phase 1 proves SQL
semantics in memory; Phase 2 adds durable storage; the rest is deliberately
staged for correctness and learning.
