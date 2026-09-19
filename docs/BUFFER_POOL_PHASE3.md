# KernDB Phase 3: Buffer Pool

Phase 3 routes normal table-page access through a bounded RAM cache. The cache
reduces repeated file reads while preserving the fixed-size, checksum-protected
page format introduced in Phase 2.

## Disk pages and RAM frames

A **disk page** is the durable 4096-byte page identified by `PageId` in a table
file. A **frame** is one equally sized slot in RAM that can temporarily hold a
disk page. The buffer-pool frame count is configurable when opening a
`PageManager` or `BufferPoolManager`; it is 16 by default.

The pool maintains a page table:

```text
PageId -> frame id -> Page bytes, pin count, dirty flag
```

A fetch consults this table first. A resident page is a cache hit; otherwise it
is a cache miss and the pool reads the page from `DiskManager` into a free or
evictable frame. A corrupt or absent miss fails before it evicts a valid frame.

## Pins and safe access

`FetchPage` and `NewPage` return a move-only `PinnedPage` guard. A guard owns
one pin and is the only supported way to access the page held by a frame. Its
reference remains valid until `Release()` or guard destruction, so callers do
not retain an evictable raw page pointer. Moving a guard transfers its pin;
copying is disabled.

Pin counts are not caller-managed: releasing a guard calls the manager's
internal unpin operation. This makes double-unpin and use-after-eviction
mistakes substantially harder. Calling `mutable_page()` also marks the guard's
frame dirty; read-only `page()` access does not.

## Dirty pages and flushing

A dirty frame differs from the durable page on disk. `FlushPage` writes one
dirty resident page, and `FlushAllPages` writes every dirty frame then asks the
disk layer to flush. Dirty frames are also written before eviction and during
orderly buffer-pool destruction. `PageManager::WritePage` retains its Phase 2
compatibility behavior by flushing after its guarded update; callers that need
deferred write-back can use the buffer-pool guard API directly.

## Eviction and deletion

Only frames with pin count zero may be evicted. Phase 3 uses deterministic LRU
among those unpinned frames: when a guard releases the last pin, its frame is
added to the newest end of the replacer; the oldest frame is the next victim.
If every frame is pinned, fetch and allocation return `resource_exhausted`
instead of evicting a live page.

`DeletePage` also rejects pinned pages. The current fixed-file format has no
durable free-page map, so physical deletion is intentionally limited to the
final page of a file; arbitrary-ID reuse is deferred to a later storage phase.

## End-to-end flow

```text
TableHeap
  -> PageManager
     -> BufferPoolManager
        -> frame/page table/LRU
           -> DiskManager
              -> table .dat file
```

`PersistentStorage` keeps a table's `PageManager` alive for the database
instance, allowing repeated INSERT and SELECT operations to share its cache.
On cache misses the page is validated by `DiskManager`/`Page` deserialization,
so existing checksum corruption handling remains in force.

## Telemetry

When a `MetricsRegistry` is supplied, the pool publishes counters named:

- `buffer_pool.page_hits`
- `buffer_pool.page_misses`
- `buffer_pool.evictions`
- `buffer_pool.dirty_flushes`
- `buffer_pool.failed_fetches`
- `buffer_pool.failed_allocations`

These counters distinguish normal cache activity from all-pinned and I/O or
validation failures without changing query behavior.
