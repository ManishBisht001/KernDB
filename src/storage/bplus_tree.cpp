#include "kerndb/storage/bplus_tree.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "kerndb/storage/buffer_pool.h"

namespace kerndb::storage {
namespace {

constexpr std::uint32_t kNodeMagic = 0x4250544EU;
constexpr std::uint16_t kNodeFormatVersion = 1U;
constexpr std::size_t kNodeMagicOffset = 0U;
constexpr std::size_t kNodeVersionOffset = 4U;
constexpr std::size_t kNodeKindOffset = 6U;
constexpr std::size_t kNodeHeaderSizeOffset = 7U;
constexpr std::size_t kNodeKeyCountOffset = 8U;
constexpr std::size_t kNodeParentOffset = 12U;
constexpr std::size_t kNodeLinkOffset = 20U;
constexpr std::size_t kNodeHeaderSize = 32U;
constexpr std::size_t kLeafEntrySize = 20U;
constexpr std::size_t kInternalEntrySize = 16U;

enum class NodeKind : std::uint8_t {
    kLeaf = 1U,
    kInternal = 2U,
};

[[nodiscard]] Status TreeError(ErrorCode code, std::string message) {
    return Status::Error(code, std::move(message));
}

void WriteUInt16At(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU));
    }
}

void WriteUInt32At(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU));
    }
}

void WriteUInt64At(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU));
    }
}

[[nodiscard]] std::uint16_t ReadUInt16At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint16_t result = 0U;
    for (std::size_t index = 0U; index < sizeof(result); ++index) {
        const auto byte = static_cast<std::uint16_t>(
            std::to_integer<unsigned char>(bytes[offset + index]));
        result = static_cast<std::uint16_t>(
            result | static_cast<std::uint16_t>(byte << (index * 8U)));
    }
    return result;
}

[[nodiscard]] std::uint32_t ReadUInt32At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t result = 0U;
    for (std::size_t index = 0U; index < sizeof(result); ++index) {
        result |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                  << (index * 8U);
    }
    return result;
}

[[nodiscard]] std::uint64_t ReadUInt64At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t result = 0U;
    for (std::size_t index = 0U; index < sizeof(result); ++index) {
        result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                  << (index * 8U);
    }
    return result;
}

[[nodiscard]] Result<PageId> DecodePageId(std::uint64_t raw, bool allow_invalid) {
    const PageId page_id{raw};
    if (!page_id.valid() && !allow_invalid) {
        return TreeError(ErrorCode::kCorruption, "B+ tree node contains an invalid page ID");
    }
    return page_id;
}

}  // namespace

struct BPlusTree::Node {
    NodeKind kind{NodeKind::kLeaf};
    PageId parent_page_id{};
    PageId next_leaf_page_id{};
    std::vector<BPlusTreeEntry> entries;
    std::vector<std::int64_t> keys;
    std::vector<PageId> children;
};

namespace {

[[nodiscard]] Status ValidateNode(const BPlusTree::Node& node) {
    if (node.kind == NodeKind::kLeaf) {
        if (node.entries.size() > kBPlusTreeMaxLeafEntries) {
            return TreeError(ErrorCode::kCorruption, "B+ tree leaf exceeds its configured capacity");
        }
        for (std::size_t index = 0U; index < node.entries.size(); ++index) {
            if (!node.entries[index].record_id.valid()) {
                return TreeError(ErrorCode::kCorruption, "B+ tree leaf contains an invalid record ID");
            }
            if (index != 0U && node.entries[index - 1U].key >= node.entries[index].key) {
                return TreeError(ErrorCode::kCorruption, "B+ tree leaf keys are not strictly sorted");
            }
        }
        return Status::Ok();
    }

    if (node.keys.size() > kBPlusTreeMaxInternalKeys ||
        node.children.size() != node.keys.size() + 1U) {
        return TreeError(ErrorCode::kCorruption, "B+ tree internal node shape is invalid");
    }
    for (std::size_t index = 0U; index < node.keys.size(); ++index) {
        if (index != 0U && node.keys[index - 1U] >= node.keys[index]) {
            return TreeError(ErrorCode::kCorruption, "B+ tree internal keys are not strictly sorted");
        }
    }
    for (const PageId child_page_id : node.children) {
        if (!child_page_id.valid()) {
            return TreeError(ErrorCode::kCorruption, "B+ tree internal node has an invalid child");
        }
    }
    return Status::Ok();
}

[[nodiscard]] Result<BPlusTree::Node> DecodeNode(const Page& page) {
    if (page.type() != PageType::kIndex) {
        return TreeError(ErrorCode::kCorruption, "B+ tree page does not have index page type");
    }
    const std::span<const std::byte> bytes = page.payload();
    if (ReadUInt32At(bytes, kNodeMagicOffset) != kNodeMagic ||
        ReadUInt16At(bytes, kNodeVersionOffset) != kNodeFormatVersion ||
        std::to_integer<std::uint8_t>(bytes[kNodeHeaderSizeOffset]) != kNodeHeaderSize) {
        return TreeError(ErrorCode::kCorruption, "B+ tree node header is invalid");
    }
    const std::uint8_t raw_kind = std::to_integer<std::uint8_t>(bytes[kNodeKindOffset]);
    NodeKind kind = NodeKind::kLeaf;
    if (raw_kind == static_cast<std::uint8_t>(NodeKind::kLeaf)) {
        kind = NodeKind::kLeaf;
    } else if (raw_kind == static_cast<std::uint8_t>(NodeKind::kInternal)) {
        kind = NodeKind::kInternal;
    } else {
        return TreeError(ErrorCode::kCorruption, "B+ tree node kind is invalid");
    }

    const auto parent_page_id = DecodePageId(ReadUInt64At(bytes, kNodeParentOffset), true);
    if (!parent_page_id.ok()) {
        return parent_page_id.status();
    }
    const std::uint16_t key_count = ReadUInt16At(bytes, kNodeKeyCountOffset);
    BPlusTree::Node node{
        .kind = kind,
        .parent_page_id = parent_page_id.value(),
        .next_leaf_page_id = PageId{},
        .entries = {},
        .keys = {},
        .children = {},
    };

    if (kind == NodeKind::kLeaf) {
        if (key_count > kBPlusTreeMaxLeafEntries ||
            kNodeHeaderSize + static_cast<std::size_t>(key_count) * kLeafEntrySize > bytes.size()) {
            return TreeError(ErrorCode::kCorruption, "B+ tree leaf entry count is invalid");
        }
        const auto next_page_id = DecodePageId(ReadUInt64At(bytes, kNodeLinkOffset), true);
        if (!next_page_id.ok()) {
            return next_page_id.status();
        }
        node.next_leaf_page_id = next_page_id.value();
        node.entries.reserve(key_count);
        for (std::uint16_t index = 0U; index < key_count; ++index) {
            const std::size_t offset = kNodeHeaderSize + static_cast<std::size_t>(index) * kLeafEntrySize;
            const auto record_page_id = DecodePageId(ReadUInt64At(bytes, offset + 8U), false);
            if (!record_page_id.ok()) {
                return record_page_id.status();
            }
            const SlotId slot_id{ReadUInt32At(bytes, offset + 16U)};
            node.entries.push_back(BPlusTreeEntry{
                .key = std::bit_cast<std::int64_t>(ReadUInt64At(bytes, offset)),
                .record_id = RecordId{.page_id = record_page_id.value(), .slot_id = slot_id},
            });
        }
    } else {
        if (key_count > kBPlusTreeMaxInternalKeys ||
            kNodeHeaderSize + static_cast<std::size_t>(key_count) * kInternalEntrySize > bytes.size()) {
            return TreeError(ErrorCode::kCorruption, "B+ tree internal entry count is invalid");
        }
        const auto first_child = DecodePageId(ReadUInt64At(bytes, kNodeLinkOffset), false);
        if (!first_child.ok()) {
            return first_child.status();
        }
        node.keys.reserve(key_count);
        node.children.reserve(static_cast<std::size_t>(key_count) + 1U);
        node.children.push_back(first_child.value());
        for (std::uint16_t index = 0U; index < key_count; ++index) {
            const std::size_t offset = kNodeHeaderSize + static_cast<std::size_t>(index) * kInternalEntrySize;
            const auto child_page_id = DecodePageId(ReadUInt64At(bytes, offset + 8U), false);
            if (!child_page_id.ok()) {
                return child_page_id.status();
            }
            node.keys.push_back(std::bit_cast<std::int64_t>(ReadUInt64At(bytes, offset)));
            node.children.push_back(child_page_id.value());
        }
    }

    const Status validation = ValidateNode(node);
    if (!validation.ok()) {
        return validation;
    }
    return node;
}

[[nodiscard]] Status EncodeNode(Page& page, const BPlusTree::Node& node) {
    const Status validation = ValidateNode(node);
    if (!validation.ok()) {
        return validation;
    }
    if (page.type() != PageType::kIndex) {
        return TreeError(ErrorCode::kInternal, "attempted to encode a B+ tree node into a non-index page");
    }
    std::span<std::byte> bytes = page.mutable_payload();
    std::fill(bytes.begin(), bytes.end(), std::byte{0U});
    WriteUInt32At(bytes, kNodeMagicOffset, kNodeMagic);
    WriteUInt16At(bytes, kNodeVersionOffset, kNodeFormatVersion);
    bytes[kNodeKindOffset] = static_cast<std::byte>(static_cast<std::uint8_t>(node.kind));
    bytes[kNodeHeaderSizeOffset] = static_cast<std::byte>(kNodeHeaderSize);
    const std::size_t key_count = node.kind == NodeKind::kLeaf ? node.entries.size() : node.keys.size();
    WriteUInt16At(bytes, kNodeKeyCountOffset, static_cast<std::uint16_t>(key_count));
    WriteUInt64At(bytes, kNodeParentOffset, node.parent_page_id.value());

    if (node.kind == NodeKind::kLeaf) {
        WriteUInt64At(bytes, kNodeLinkOffset, node.next_leaf_page_id.value());
        for (std::size_t index = 0U; index < node.entries.size(); ++index) {
            const std::size_t offset = kNodeHeaderSize + index * kLeafEntrySize;
            WriteUInt64At(bytes, offset, std::bit_cast<std::uint64_t>(node.entries[index].key));
            WriteUInt64At(bytes, offset + 8U, node.entries[index].record_id.page_id.value());
            WriteUInt32At(bytes, offset + 16U, node.entries[index].record_id.slot_id.value());
        }
    } else {
        WriteUInt64At(bytes, kNodeLinkOffset, node.children.front().value());
        for (std::size_t index = 0U; index < node.keys.size(); ++index) {
            const std::size_t offset = kNodeHeaderSize + index * kInternalEntrySize;
            WriteUInt64At(bytes, offset, std::bit_cast<std::uint64_t>(node.keys[index]));
            WriteUInt64At(bytes, offset + 8U, node.children[index + 1U].value());
        }
    }
    return Status::Ok();
}

}  // namespace

BPlusTree::BPlusTree(PageManager& page_manager, PageId root_page_id) noexcept
    : page_manager_(&page_manager),
      root_page_id_(root_page_id) {}

Result<BPlusTree> BPlusTree::Create(PageManager& page_manager) {
    BPlusTree tree{page_manager, PageId{}};
    Node root;
    root.kind = NodeKind::kLeaf;
    const auto root_page_id = tree.CreateNode(root);
    if (!root_page_id.ok()) {
        return root_page_id.status();
    }
    tree.root_page_id_ = root_page_id.value();
    return tree;
}

Result<BPlusTree> BPlusTree::Open(PageManager& page_manager, PageId root_page_id) {
    if (!root_page_id.valid()) {
        return TreeError(ErrorCode::kInvalidArgument, "B+ tree root page ID is invalid");
    }
    BPlusTree tree{page_manager, root_page_id};
    const auto root = tree.ReadNode(root_page_id);
    if (!root.ok()) {
        return root.status();
    }
    if (root.value().parent_page_id.valid()) {
        return TreeError(ErrorCode::kCorruption, "B+ tree root unexpectedly has a parent");
    }
    return tree;
}

Result<void> BPlusTree::Insert(std::int64_t key, RecordId record_id) {
    if (!record_id.valid()) {
        return TreeError(ErrorCode::kInvalidArgument, "cannot index an invalid record ID");
    }

    PageId current_page_id = root_page_id_;
    while (true) {
        const auto read_node = ReadNode(current_page_id);
        if (!read_node.ok()) {
            return read_node.status();
        }
        Node node = read_node.value();
        if (node.kind == NodeKind::kInternal) {
            const auto child = std::upper_bound(node.keys.begin(), node.keys.end(), key);
            const std::size_t child_index = static_cast<std::size_t>(child - node.keys.begin());
            current_page_id = node.children[child_index];
            continue;
        }

        const auto position = std::lower_bound(
            node.entries.begin(),
            node.entries.end(),
            key,
            [](const BPlusTreeEntry& entry, std::int64_t candidate) {
                return entry.key < candidate;
            });
        if (position != node.entries.end() && position->key == key) {
            return TreeError(ErrorCode::kAlreadyExists, "duplicate key violates the unique index")
                .WithContext("key", std::to_string(key));
        }
        node.entries.insert(position, BPlusTreeEntry{.key = key, .record_id = record_id});
        if (node.entries.size() <= kBPlusTreeMaxLeafEntries) {
            const Status write_status = WriteNode(current_page_id, node);
            if (!write_status.ok()) {
                return write_status;
            }
            return Result<void>{};
        }

        const std::size_t split_index = node.entries.size() / 2U;
        Node right_node;
        right_node.kind = NodeKind::kLeaf;
        right_node.parent_page_id = node.parent_page_id;
        right_node.next_leaf_page_id = node.next_leaf_page_id;
        right_node.entries.assign(node.entries.begin() + static_cast<std::ptrdiff_t>(split_index), node.entries.end());
        node.entries.erase(node.entries.begin() + static_cast<std::ptrdiff_t>(split_index), node.entries.end());
        const auto right_page_id = CreateNode(right_node);
        if (!right_page_id.ok()) {
            return right_page_id.status();
        }
        node.next_leaf_page_id = right_page_id.value();
        const Status write_status = WriteNode(current_page_id, node);
        if (!write_status.ok()) {
            return write_status;
        }
        return InsertIntoParent(
            current_page_id,
            right_node.entries.front().key,
            right_page_id.value(),
            node.parent_page_id);
    }
}

Result<RecordId> BPlusTree::Search(std::int64_t key) const {
    PageId current_page_id = root_page_id_;
    while (true) {
        const auto read_node = ReadNode(current_page_id);
        if (!read_node.ok()) {
            return read_node.status();
        }
        const Node& node = read_node.value();
        if (node.kind == NodeKind::kInternal) {
            const auto child = std::upper_bound(node.keys.begin(), node.keys.end(), key);
            const std::size_t child_index = static_cast<std::size_t>(child - node.keys.begin());
            current_page_id = node.children[child_index];
            continue;
        }
        const auto entry = std::lower_bound(
            node.entries.begin(),
            node.entries.end(),
            key,
            [](const BPlusTreeEntry& candidate, std::int64_t target) {
                return candidate.key < target;
            });
        if (entry == node.entries.end() || entry->key != key) {
            return TreeError(ErrorCode::kNotFound, "index key does not exist")
                .WithContext("key", std::to_string(key));
        }
        return entry->record_id;
    }
}

Result<std::vector<BPlusTreeEntry>> BPlusTree::ScanAll() const {
    PageId current_page_id = root_page_id_;
    while (true) {
        const auto read_node = ReadNode(current_page_id);
        if (!read_node.ok()) {
            return read_node.status();
        }
        if (read_node.value().kind == NodeKind::kLeaf) {
            break;
        }
        current_page_id = read_node.value().children.front();
    }

    const auto page_count = page_manager_->PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    std::vector<BPlusTreeEntry> entries;
    for (std::uint64_t visited = 0U; current_page_id.valid(); ++visited) {
        if (visited >= page_count.value()) {
            return TreeError(ErrorCode::kCorruption, "B+ tree leaf sibling links contain a cycle");
        }
        const auto read_node = ReadNode(current_page_id);
        if (!read_node.ok()) {
            return read_node.status();
        }
        const Node& node = read_node.value();
        if (node.kind != NodeKind::kLeaf) {
            return TreeError(ErrorCode::kCorruption, "B+ tree leaf link points to an internal node");
        }
        entries.insert(entries.end(), node.entries.begin(), node.entries.end());
        current_page_id = node.next_leaf_page_id;
    }
    return entries;
}

PageId BPlusTree::root_page_id() const noexcept {
    return root_page_id_;
}

Result<BPlusTree::Node> BPlusTree::ReadNode(PageId page_id) const {
    auto pinned_page = page_manager_->buffer_pool().FetchPage(page_id);
    if (!pinned_page.ok()) {
        return pinned_page.status();
    }
    PinnedPage guard = std::move(pinned_page).value();
    const auto node = DecodeNode(guard.page());
    const Status release_status = guard.Release();
    if (!release_status.ok()) {
        return release_status;
    }
    if (!node.ok()) {
        return node.status();
    }
    return node.value();
}

Status BPlusTree::WriteNode(PageId page_id, const Node& node) {
    auto pinned_page = page_manager_->buffer_pool().FetchPage(page_id);
    if (!pinned_page.ok()) {
        return pinned_page.status();
    }
    PinnedPage guard = std::move(pinned_page).value();
    const Status encode_status = EncodeNode(guard.mutable_page(), node);
    const Status release_status = guard.Release();
    if (!encode_status.ok()) {
        return encode_status;
    }
    return release_status;
}

Result<PageId> BPlusTree::CreateNode(const Node& node) {
    auto pinned_page = page_manager_->buffer_pool().NewPage(PageType::kIndex);
    if (!pinned_page.ok()) {
        return pinned_page.status();
    }
    PinnedPage guard = std::move(pinned_page).value();
    const PageId page_id = guard.page_id();
    const Status encode_status = EncodeNode(guard.mutable_page(), node);
    const Status release_status = guard.Release();
    if (!encode_status.ok()) {
        return encode_status;
    }
    if (!release_status.ok()) {
        return release_status;
    }
    return page_id;
}

Result<void> BPlusTree::InsertIntoParent(
    PageId left_page_id,
    std::int64_t separator_key,
    PageId right_page_id,
    PageId parent_page_id) {
    if (!parent_page_id.valid()) {
        Node new_root;
        new_root.kind = NodeKind::kInternal;
        new_root.keys.push_back(separator_key);
        new_root.children = {left_page_id, right_page_id};
        const auto new_root_page_id = CreateNode(new_root);
        if (!new_root_page_id.ok()) {
            return new_root_page_id.status();
        }
        const Status left_parent_status = SetParent(left_page_id, new_root_page_id.value());
        if (!left_parent_status.ok()) {
            return left_parent_status;
        }
        const Status right_parent_status = SetParent(right_page_id, new_root_page_id.value());
        if (!right_parent_status.ok()) {
            return right_parent_status;
        }
        root_page_id_ = new_root_page_id.value();
        return Result<void>{};
    }

    const auto read_parent = ReadNode(parent_page_id);
    if (!read_parent.ok()) {
        return read_parent.status();
    }
    Node parent = read_parent.value();
    if (parent.kind != NodeKind::kInternal) {
        return TreeError(ErrorCode::kCorruption, "B+ tree child references a non-internal parent");
    }
    const auto child = std::find(parent.children.begin(), parent.children.end(), left_page_id);
    if (child == parent.children.end()) {
        return TreeError(ErrorCode::kCorruption, "B+ tree parent does not reference its child");
    }
    const std::size_t child_index = static_cast<std::size_t>(child - parent.children.begin());
    parent.keys.insert(
        parent.keys.begin() + static_cast<std::ptrdiff_t>(child_index),
        separator_key);
    parent.children.insert(
        parent.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1U),
        right_page_id);
    if (parent.keys.size() <= kBPlusTreeMaxInternalKeys) {
        const Status write_status = WriteNode(parent_page_id, parent);
        if (!write_status.ok()) {
            return write_status;
        }
        return Result<void>{};
    }

    const std::size_t split_index = parent.keys.size() / 2U;
    const std::int64_t promoted_key = parent.keys[split_index];
    Node right_parent;
    right_parent.kind = NodeKind::kInternal;
    right_parent.parent_page_id = parent.parent_page_id;
    right_parent.keys.assign(
        parent.keys.begin() + static_cast<std::ptrdiff_t>(split_index + 1U),
        parent.keys.end());
    right_parent.children.assign(
        parent.children.begin() + static_cast<std::ptrdiff_t>(split_index + 1U),
        parent.children.end());
    parent.keys.erase(parent.keys.begin() + static_cast<std::ptrdiff_t>(split_index), parent.keys.end());
    parent.children.erase(
        parent.children.begin() + static_cast<std::ptrdiff_t>(split_index + 1U),
        parent.children.end());

    const auto right_parent_page_id = CreateNode(right_parent);
    if (!right_parent_page_id.ok()) {
        return right_parent_page_id.status();
    }
    const Status write_status = WriteNode(parent_page_id, parent);
    if (!write_status.ok()) {
        return write_status;
    }
    for (const PageId child_page_id : right_parent.children) {
        const Status parent_status = SetParent(child_page_id, right_parent_page_id.value());
        if (!parent_status.ok()) {
            return parent_status;
        }
    }
    return InsertIntoParent(
        parent_page_id,
        promoted_key,
        right_parent_page_id.value(),
        parent.parent_page_id);
}

Status BPlusTree::SetParent(PageId page_id, PageId parent_page_id) {
    const auto read_node = ReadNode(page_id);
    if (!read_node.ok()) {
        return read_node.status();
    }
    Node node = read_node.value();
    node.parent_page_id = parent_page_id;
    return WriteNode(page_id, node);
}

}  // namespace kerndb::storage
