# KernDB Phase 4: Persistent B+ Tree Indexes

Phase 4 adds durable, unique secondary indexes for `INT` columns. An index is a
separate page file at `indexes/<index-id>.dat`; its persistent catalog records
are held in `indexes/catalog.dat` alongside the Phase 2 table catalog.

## Tree layout

Each B+ tree node is a versioned `kIndex` page with a deterministic little-endian
codec. Leaf nodes contain sorted `INT -> RID` entries, where the RID is the
heap-page ID and slotted-page slot ID of the row. Internal nodes contain sorted
separator keys and child page IDs. The current fixed fanout is eight leaf
entries or eight internal separators, deliberately small so splits are simple
to exercise in tests.

Leaves have a `next_leaf_page_id` link. `ScanAll` follows the leftmost leaf and
then those links, so it also verifies the ordered leaf chain. Pages store a
parent page ID; this lets a leaf or internal split propagate its separator
toward the root.

The index is unique. Attempting to insert a duplicate key returns
`already_exists`; creating an index over pre-existing duplicate `INT` values
therefore fails rather than silently selecting an arbitrary row.

## Insert, split, and root maintenance

`Insert(key, rid)` descends internal separators to a leaf, verifies uniqueness,
and inserts in key order. A full leaf is split into two linked leaves and its
first right-hand key is inserted into its parent. Full internal nodes split in
the same way, promoting the middle separator. Splitting a root allocates a new
root and updates the two child parent pointers.

The tree is opened by its root page ID. Since a root split changes that ID,
`PersistentStorage` appends a new version of the index metadata record and
updates the in-memory catalog after every root change. On clean reopen the
newest metadata version supplies the durable root ID.

## Buffer-pool relationship and persistence

The B+ tree never accesses `DiskManager` directly. It allocates and fetches
`kIndex` pages through the existing Phase 3 `PageManager` and
`BufferPoolManager`, holding pages with `PinnedPage` guards. Mutations mark
frames dirty; Phase 3 eviction, explicit flushes, and orderly buffer-pool
destruction finalize checksums and write them to the index file.

## SQL integration

The supported DDL is:

~~~sql
CREATE INDEX people_id_idx ON people(id);
~~~

The binder accepts only `INT` columns. Index creation scans existing heap rows
with their RIDs and inserts every entry before making its metadata durable.
Subsequent `INSERT` statements first enforce every relevant unique index, then
write the heap row and its index entry. `SELECT ... WHERE int_column = value`
becomes an index-scan candidate: if an index is present, the executor performs
a B+ tree equality lookup and reads only the matching RID. If no appropriate
index exists, it safely falls back to the sequential scan used by earlier
phases.

This phase intentionally does not implement range scans in SQL, delete/update,
transactions, locking, WAL, crash recovery, joins, aggregates, or a cost-based
optimizer.
