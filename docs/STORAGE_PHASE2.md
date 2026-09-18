# Phase 2 storage format

Phase 2 stores an embedded database in a caller-selected directory:

~~~text
<database>/
  database.meta
  catalog.dat
  tables/<table-id>.dat
  indexes/
  wal/
  tmp/
~~~

`indexes/`, `wal/`, and `tmp/` are reserved directories only. Phase 2 has no
index, log, recovery, or temporary-file implementation.

All persistent integers use explicitly encoded little-endian bytes. No C++
object or struct representation is written to a database file. The configured
format version is `1`; the page size is 4096 bytes.

## Database metadata

`database.meta` is exactly 32 bytes: magic, format version, page size, stable
database ID, reserved catalog-root field, and reserved flags. Opening validates
its exact size, magic, version, page size, and database ID before accessing data
files.

## Common page header

Every `catalog.dat` and table page is exactly 4096 bytes. Its first 40 bytes
contain page magic, format version, page type, header size, self page ID, page
LSN (reserved for a future WAL), checksum, flags, and reserved bytes. The
checksum is a 32-bit FNV-1a checksum of the full page with the checksum field
treated as zero. Reads reject bad size, magic, version, type, self identity, or
checksum as corruption rather than accepting partial data.

Catalog pages have type `1`; heap pages have type `2`.

## Slotted pages and records

Catalog and heap pages use the same slotted-page layout after the common header:

~~~text
slot_count:u16 | free_start:u16 | free_end:u16 | reserved:u16
tuple bytes grow upward
slot entries grow downward: tuple_offset:u16 | tuple_length:u16 |
                            generation:u32 | flags:u32
~~~

A Phase 2 RID is `PageId + SlotId`. Insert-only slots use generation `1` and an
allocated flag. Deletion, slot reuse, and compaction are deferred; therefore a
stored RID is stable for the records Phase 2 can create.

Heap records are encoded in catalog-schema order: record-format version, column
count, 64-bit signed `INT` values, and `TEXT` lengths plus bytes. `TEXT` is
capped at 1024 inline bytes. The encoding rejects arity/type mismatches,
unsupported versions, malformed lengths, and trailing bytes.

## Phase-2 persistence boundary

The executor receives a table-storage interface through `ExecutionContext`.
For a persistent database this interface opens a direct page manager for the
table, scans or appends slotted heap records, and returns typed tuples. There is
no buffer pool and no concurrency control in this phase. File writes flush their
stream, but no WAL ordering, fsync contract, or crash recovery guarantee exists.
