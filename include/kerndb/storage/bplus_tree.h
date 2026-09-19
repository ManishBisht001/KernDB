#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/storage/page_manager.h"

namespace kerndb::storage {

// Small fixed fanout keeps split behavior easy to exercise while every node
// remains a normal 4096-byte Phase 2 page.
inline constexpr std::size_t kBPlusTreeMaxLeafEntries = 8U;
inline constexpr std::size_t kBPlusTreeMaxInternalKeys = 8U;

struct BPlusTreeEntry {
    std::int64_t key{0};
    RecordId record_id;
};

// A persistent unique INT-key B+ tree. All page access goes through the
// PageManager's BufferPoolManager; the tree never accesses DiskManager.
class BPlusTree {
public:
    // Defined privately in the implementation file; forward declaration is
    // public only so deterministic codecs can remain file-local helpers.
    struct Node;

    [[nodiscard]] static Result<BPlusTree> Create(PageManager& page_manager);
    [[nodiscard]] static Result<BPlusTree> Open(PageManager& page_manager, PageId root_page_id);

    [[nodiscard]] Result<void> Insert(std::int64_t key, RecordId record_id);
    [[nodiscard]] Result<RecordId> Search(std::int64_t key) const;
    [[nodiscard]] Result<std::vector<BPlusTreeEntry>> ScanAll() const;
    [[nodiscard]] PageId root_page_id() const noexcept;

private:
    BPlusTree(PageManager& page_manager, PageId root_page_id) noexcept;

    [[nodiscard]] Result<Node> ReadNode(PageId page_id) const;
    [[nodiscard]] Status WriteNode(PageId page_id, const Node& node);
    [[nodiscard]] Result<PageId> CreateNode(const Node& node);
    [[nodiscard]] Result<void> InsertIntoParent(
        PageId left_page_id,
        std::int64_t separator_key,
        PageId right_page_id,
        PageId parent_page_id);
    [[nodiscard]] Status SetParent(PageId page_id, PageId parent_page_id);

    PageManager* page_manager_;
    PageId root_page_id_;
};

}  // namespace kerndb::storage
