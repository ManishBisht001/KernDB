#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <vector>

#include "kerndb/storage/bplus_tree.h"
#include "kerndb/storage/page_manager.h"
#include "test_framework.h"
#include "temp_directory.h"

namespace {

[[nodiscard]] kerndb::RecordId RecordFor(std::int64_t key) {
    return kerndb::RecordId{
        .page_id = kerndb::PageId{static_cast<std::uint64_t>(key + 1024)},
        .slot_id = kerndb::SlotId{0U},
    };
}

void ExpectAllEntries(
    const kerndb::storage::BPlusTree& tree,
    std::int64_t key_count) {
    const auto entries = tree.ScanAll();
    KERNDB_EXPECT(entries.ok());
    KERNDB_EXPECT_EQ(static_cast<std::size_t>(key_count), entries.value().size());
    for (std::int64_t key = 1; key <= key_count; ++key) {
        const auto& entry = entries.value()[static_cast<std::size_t>(key - 1)];
        KERNDB_EXPECT_EQ(key, entry.key);
        KERNDB_EXPECT_EQ(RecordFor(key), entry.record_id);
        const auto found = tree.Search(key);
        KERNDB_EXPECT(found.ok());
        KERNDB_EXPECT_EQ(RecordFor(key), found.value());
    }
}

}  // namespace

KERNDB_TEST(BPlusTreeHandlesEmptyOneKeyMissingAndDuplicateLookups) {
    kerndb::test::TemporaryDirectory directory;
    auto manager = kerndb::storage::PageManager::Open(directory.path() / "tree.dat", true, 3U);
    KERNDB_EXPECT(manager.ok());
    auto tree = kerndb::storage::BPlusTree::Create(manager.value());
    KERNDB_EXPECT(tree.ok());

    const auto missing_empty = tree.value().Search(1);
    KERNDB_EXPECT(!missing_empty.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kNotFound, missing_empty.status().code());

    KERNDB_EXPECT(tree.value().Insert(-7, RecordFor(-7)).ok());
    KERNDB_EXPECT(tree.value().Insert(7, RecordFor(7)).ok());
    const auto negative = tree.value().Search(-7);
    KERNDB_EXPECT(negative.ok());
    KERNDB_EXPECT_EQ(RecordFor(-7), negative.value());
    const auto found = tree.value().Search(7);
    KERNDB_EXPECT(found.ok());
    KERNDB_EXPECT_EQ(RecordFor(7), found.value());
    const auto missing = tree.value().Search(8);
    KERNDB_EXPECT(!missing.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kNotFound, missing.status().code());

    const auto duplicate = tree.value().Insert(7, RecordFor(8));
    KERNDB_EXPECT(!duplicate.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kAlreadyExists, duplicate.status().code());
}

KERNDB_TEST(BPlusTreeMaintainsSortedEntriesAcrossLeafAndInternalSplits) {
    kerndb::test::TemporaryDirectory directory;
    auto manager = kerndb::storage::PageManager::Open(directory.path() / "tree.dat", true, 3U);
    KERNDB_EXPECT(manager.ok());
    auto tree = kerndb::storage::BPlusTree::Create(manager.value());
    KERNDB_EXPECT(tree.ok());

    constexpr std::int64_t kKeyCount = 128;
    std::vector<std::int64_t> keys;
    keys.reserve(static_cast<std::size_t>(kKeyCount));
    for (std::int64_t key = 1; key <= kKeyCount; ++key) {
        keys.push_back(key);
    }
    std::reverse(keys.begin(), keys.end());
    for (const std::int64_t key : keys) {
        KERNDB_EXPECT(tree.value().Insert(key, RecordFor(key)).ok());
    }

    ExpectAllEntries(tree.value(), kKeyCount);
    const auto page_count = manager.value().PageCount();
    KERNDB_EXPECT(page_count.ok());
    KERNDB_EXPECT(page_count.value() > 12U);
    KERNDB_EXPECT(tree.value().root_page_id() != kerndb::PageId{0U});
}

KERNDB_TEST(BPlusTreeMaintainsSortedEntriesForRandomInsertionOrder) {
    kerndb::test::TemporaryDirectory directory;
    auto manager = kerndb::storage::PageManager::Open(directory.path() / "tree.dat", true, 3U);
    KERNDB_EXPECT(manager.ok());
    auto tree = kerndb::storage::BPlusTree::Create(manager.value());
    KERNDB_EXPECT(tree.ok());

    constexpr std::int64_t kKeyCount = 96;
    std::vector<std::int64_t> keys;
    keys.reserve(static_cast<std::size_t>(kKeyCount));
    for (std::int64_t key = 1; key <= kKeyCount; ++key) {
        keys.push_back(key);
    }
    std::mt19937 generator{42U};
    std::shuffle(keys.begin(), keys.end(), generator);
    for (const std::int64_t key : keys) {
        KERNDB_EXPECT(tree.value().Insert(key, RecordFor(key)).ok());
    }

    ExpectAllEntries(tree.value(), kKeyCount);
}

KERNDB_TEST(BPlusTreePersistsRootLeavesAndEntriesAcrossReopen) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "tree.dat";
    kerndb::PageId root_page_id;
    {
        auto manager = kerndb::storage::PageManager::Open(path, true, 3U);
        KERNDB_EXPECT(manager.ok());
        auto tree = kerndb::storage::BPlusTree::Create(manager.value());
        KERNDB_EXPECT(tree.ok());
        for (std::int64_t key = 96; key >= 1; --key) {
            KERNDB_EXPECT(tree.value().Insert(key, RecordFor(key)).ok());
        }
        root_page_id = tree.value().root_page_id();
        KERNDB_EXPECT(root_page_id != kerndb::PageId{0U});
    }

    auto reopened_manager = kerndb::storage::PageManager::Open(path, false, 3U);
    KERNDB_EXPECT(reopened_manager.ok());
    const auto reopened_tree = kerndb::storage::BPlusTree::Open(
        reopened_manager.value(), root_page_id);
    KERNDB_EXPECT(reopened_tree.ok());
    ExpectAllEntries(reopened_tree.value(), 96);
    const auto missing = reopened_tree.value().Search(97);
    KERNDB_EXPECT(!missing.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kNotFound, missing.status().code());
}
