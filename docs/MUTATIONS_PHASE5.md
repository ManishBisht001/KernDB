# KernDB Phase 5: Mutations and Single-Threaded Transactions

Phase 5 supports persistent `INSERT`, `UPDATE`, and `DELETE`, unique `INT`
B+ tree index maintenance, statement-level compensation, and one explicit
transaction at a time per `Database` instance. It is synchronous and
single-threaded.

## Supported SQL

~~~sql
BEGIN;
INSERT INTO people VALUES (10, 'Ada');
UPDATE people SET name = 'Grace' WHERE id = 10;
DELETE FROM people WHERE id = 10;
COMMIT;
~~~

Use `ROLLBACK;` instead of `COMMIT;` to undo the transaction. `UPDATE` accepts
one `SET column = literal` assignment and both mutations require the existing
equality `WHERE` predicate. Literal types are checked by the binder. Matching
rows are all changed or deleted, and each result reports rows affected.

`BEGIN` rejects a nested transaction. `COMMIT` and `ROLLBACK` reject calls when
there is no active transaction. `CREATE TABLE` and `CREATE INDEX` are rejected
inside a transaction because transactional DDL is not implemented.

## Heap and index mutations

Heap deletes tombstone slots without reusing them; reads and scans skip those
slots. Updates insert replacement records because `TEXT` values can change
size, retarget each table index from the old RID to the replacement RID, then
tombstone the old slot.

The persistent unique B+ tree supports removal of an exact `INT -> RID` entry.
Before an update begins, KernDB validates the complete final key set for each
unique index. A duplicate-key update fails before touching storage. Successful
mutations update every index on the table, including indexes on columns that
were not changed, so each index points at the current row RID.

Delete does not rebalance or reclaim empty B+ tree nodes. Separators remain
safe lower routing bounds for equality search.

## Statement atomicity

Each persistent `INSERT`, `UPDATE`, or `DELETE` builds a statement-local list of
reverse actions as heap and index steps complete. If a later step fails, that
statement runs its own actions in reverse order before returning the error.
The failure-injection integration test exercises an update failure after the
replacement row and both index changes are written and the old row is
tombstoned.

When a statement succeeds in autocommit mode, its undo actions are discarded.
When it succeeds inside an explicit transaction, its actions are appended to
the transaction undo history. A failed statement rolls back only its own
partial work; if that compensation succeeds, the explicit transaction remains
active and later statements may continue. `SELECT` sees successful writes from
the same transaction. If compensation itself fails, the statement returns an
`internal` rollback error and the transaction remains active for an explicit
rollback attempt; storage I/O failure can prevent a complete restoration.

## Explicit transaction behavior

`PersistentStorage` retains the same statement undo actions until transaction
completion. `COMMIT` discards the history. `ROLLBACK` applies all retained
actions in reverse chronological order, restoring heap visibility and exact
index key-to-RID mappings. The in-memory `Database()` backend uses a catalog
snapshot for explicit rollback because it has no persistent storage mutation
context.

Closing a `Database` with an active transaction performs a rollback before its
storage closes. The persistent storage destructor also rolls back an active
transaction when used directly. Normal commit changes already flow through the
existing `PageManager` and `BufferPoolManager`, so a committed transaction
survives a clean close and reopen. Rollback is also written through those
layers before close.

## Atomicity and durability boundaries

- **Statement atomicity:** the current statement's completed heap/index steps
  are compensated on a reported operation failure.
- **Explicit transaction atomicity:** during normal single-threaded execution,
  successful mutations are undone as a group by `ROLLBACK` or close-time
  rollback; `COMMIT` retains their page changes.
- **Clean-shutdown persistence:** committed state and completed rollback state
  survive an orderly close/reopen.
- **Crash recovery:** unsupported. There is no WAL or recovery protocol, and
  page writes can reach disk before `COMMIT`. A process or machine crash during
  an active transaction can therefore leave uncommitted changes or partial
  state. No crash-safe transactional durability is claimed.

## Current limitations

- One active transaction per `Database`; no nested transactions or savepoints.
- `CREATE TABLE` and `CREATE INDEX` are not allowed inside a transaction.
- One `SET column = literal` assignment per `UPDATE`; no expressions or joins.
- Equality `WHERE` predicates only; no range mutation or range index scans.
- Unique `INT` indexes only; `TEXT` indexes are unsupported.
- No heap-page compaction or RID reuse after deletion.
- No concurrent transactions, locking, MVCC, WAL, or crash recovery.
