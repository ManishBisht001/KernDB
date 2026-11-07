# KernDB Phase 6: Concurrency Control and Strict 2PL

Phase 6 allows concurrent calls from separate persistent `Database` sessions
opened on the same normalized database directory in the same process. It adds
transaction IDs and lifecycle state, catalog/table S/X locks, deadlock
detection, and synchronization for shared persistent storage structures. The
existing Phase 5 mutation undo remains the transaction rollback mechanism.

## Session and transaction ownership

Each `Database` object is one session and can own at most one explicit
transaction. Calls through one object are serialized by its session mutex;
concurrency comes from distinct `Database::Open` sessions. Opening the same
normalized path in the process-local runtime registry shares one catalog,
`PersistentStorage`, `TransactionManager`, and `LockManager`. A default
in-memory `Database()` owns a private runtime and is not shared with other
in-memory instances.

`TransactionManager` allocates process-unique transaction IDs and owns each
runtime's active transaction contexts. The lifecycle states are `ACTIVE`,
`COMMITTED`, and `ABORTED`. Storage undo histories and lock ownership are keyed
by transaction ID, so independent sessions do not share undo state. Autocommit
statements use implicit transactions; explicit `BEGIN`, `COMMIT`, and
`ROLLBACK` retain Phase 5 semantics.

## Lock resources and compatibility

The transaction lock manager protects two resource kinds: the database catalog
and a table identified by `TableId`. Its compatibility matrix is:

| Held lock | Requested S | Requested X |
|---|---:|---:|
| S | Compatible | Conflicts |
| X | Conflicts | Conflicts |

Ordinary statements acquire catalog S. `CREATE TABLE` and `CREATE INDEX`
acquire catalog X; `CREATE INDEX` also takes table X. A table `SELECT` takes
table S. `INSERT`, `UPDATE`, and `DELETE` take table X. Transactional DDL
remains rejected by Phase 5, so DDL locks are acquired by implicit statements.

Locks are coarse at table granularity; there are no row or index-key locks.
This protects every row and index mapping in a table through the public SQL
path. Calls on separate tables do not conflict in the logical lock manager,
although the persistent storage mutex serializes physical mutations.

## Strict two-phase locking

KernDB uses a rigorous form of strict 2PL: each transaction retains every S and
X lock through `COMMIT` or `ROLLBACK`. In particular, shared locks are not
released after a read statement. An explicit transaction acquires catalog S
at `BEGIN`, then acquires table locks as statements access tables. An
autocommit statement releases its locks when its implicit transaction finishes.
The catalog lock prevents DDL from changing metadata while another transaction
uses it.

Because every statement executed through `Database::Execute` acquires its
catalog/table lock before binding or accessing storage, the table-level strict
2PL policy provides conflict-serializable execution for SQL transactions using
this API. It does not provide row-level concurrency or MVCC snapshots. A
statement error compensates its own partial mutation; if compensation succeeds,
an explicit transaction remains active with its locks held. A deadlock victim
is fully rolled back and aborted before its locks are released when storage undo
succeeds. If storage undo itself returns an error, KernDB reports the error but
currently retires the transaction and releases its locks; full restoration is
not guaranteed in that failure case.

## Queues and deadlock handling

The lock manager uses a mutex, condition variable, and per-resource FIFO wait
queue. Compatible S requests may join the preceding shared cohort, but a new
request does not bypass an earlier incompatible waiter. When acquisition would
block, the manager rebuilds a wait-for graph from current holders and earlier
incompatible waiters. A cycle causes the current requesting transaction to be
selected as victim. The request returns `deadlock`; `Database` rolls that
transaction's persistent undo history back, marks the transaction `ABORTED`,
releases all its locks, and notifies waiters. The graph is derived from live
lock-table state for each blocked request, so there is no separately retained
graph state to become stale.

The policy is deterministic with respect to the request that closes a detected
cycle: the requesting transaction is aborted. It is not an age-based or
cost-based victim policy.

## Lock and latch ordering

The public execution path follows this order:

1. Serialize a call on its `Database` session.
2. Acquire the transaction's catalog lock.
3. Acquire the transaction's table lock, when the plan accesses a table.
4. Enter persistent storage, which takes its shared mutex exclusively for
   mutation/DDL/transaction-history changes and shared for scans/lookups.
5. Access a B+ tree under its per-instance shared/exclusive latch, then fetch
   pages through the buffer pool.
6. The buffer pool protects its page table and replacement metadata with a
   manager mutex and protects frame contents with a page guard latch.

The buffer pool releases its manager mutex before acquiring a page guard latch;
page guards release the page latch before unpinning under the manager mutex.
Database acquires transaction locks before calling the executor, so a
transaction-lock wait does not retain a page guard or storage operation lock.
Transaction rollback completes storage undo before transaction locks are
released.

## Thread-safety boundary

- Separate persistent `Database::Open` sessions for one normalized directory
  in one process may call `Execute` concurrently.
- The per-session mutex serializes calls made through the same `Database`
  instance.
- Persistent storage protects its catalog/page caches and mutation undo state;
  reads use shared access and mutations use exclusive access.
- Buffer-pool metadata is synchronized independently from per-frame page
  guards. B+ tree operations use a shared/exclusive tree latch.
- The in-memory catalog is shared only inside its owning runtime; separate
  default in-memory `Database` instances are independent.
- Direct use of internal catalog, page, or storage classes outside the
  `Database` API is not covered by the session concurrency contract.

## Verification evidence

Concurrency tests cover S/S compatibility, S/X and X/X waits, deadlock
detection, waiter release after abort, unique transaction IDs, concurrent
buffer-pool page access, B+ tree insert/search, concurrent persistent sessions,
read/write conflicts, independent commits, and deadlock victim rollback. The
test names are `LockManagerAllowsSharedReadersAndBlocksExclusiveRequests`,
`LockManagerBlocksSharedRequestsBehindExclusiveHolders`,
`LockManagerDetectsUpgradeDeadlockAndWakesTheOtherWaiter`,
`LockManagerReleasesWaitersWhenTheirTransactionIsAborted`,
`TransactionManagerCreatesUniqueContextsAndTracksTerminalStates`,
`BufferPoolSerializesConcurrentPageGuardAccess`,
`BPlusTreeSupportsConcurrentInsertionsAndLookups`, and the three tests in
`tests/integration/concurrency_database_test.cpp`.

The debug verification completed with `cmake --preset debug` successful,
`cmake --build --preset debug` reporting `ninja: no work to do.`, and
`ctest --preset debug` passing 3/3 registered tests. The direct unified test
executable passed 89/89 tests on its first run and on two consecutive repeats.
CTest includes the shell help and version regressions, and the unified
executable includes the shell smoke tests.

ThreadSanitizer was probed with the configured MSYS2 UCRT64 GCC and could not
link: `ld.exe: cannot find -ltsan`. ThreadSanitizer therefore was not run. The
repository's `sanitize` preset enables AddressSanitizer and
UndefinedBehaviorSanitizer on Linux/WSL; that preset was not run on this native
Windows build. A WSL availability check also returned
`Wsl/Service/E_ACCESSDENIED`, so a Linux ThreadSanitizer build could not be
attempted in this environment.

## Limitations

- Concurrency is limited to sessions in one process. Independent processes
  opening the same directory do not coordinate locks and must not concurrently
  access that database.
- Locks cover whole tables, and the shared persistent-storage mutex serializes
  physical mutations across tables. The buffer-pool manager mutex also remains
  held across synchronous page reads and flushes. These are correctness-first
  choices, not a parallel-write design.
- There is no MVCC, row/key locking, lock telemetry, WAL, crash recovery, or
  crash-safe transactional durability. A crash can leave uncommitted changes
  because dirty pages may reach disk before commit.
- A storage error during transaction rollback can prevent full compensation.
  The current cleanup path reports that error but retires the transaction and
  releases its locks; no failure-injection test covers rollback I/O failure.
- ThreadSanitizer is not supported by the configured native Windows MSYS2 GCC
  toolchain; sanitizer support in the repository's `sanitize` preset is limited
  to AddressSanitizer and UndefinedBehaviorSanitizer on Linux/WSL. WSL access
  was denied in this environment, so ThreadSanitizer was unavailable for this
  verification run.
