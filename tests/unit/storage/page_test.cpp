#include <cstddef>
#include <cstdint>

#include "kerndb/storage/page.h"
#include "test_framework.h"

KERNDB_TEST(PageRoundTripUsesFixedSizeChecksumProtectedFormat) {
    const auto created = kerndb::storage::Page::Create(
        kerndb::storage::PageType::kHeap,
        kerndb::PageId{7U});
    KERNDB_EXPECT(created.ok());
    KERNDB_EXPECT_EQ(kerndb::kInitialPageSizeBytes, created.value().bytes().size());

    const auto loaded = kerndb::storage::Page::Deserialize(created.value().bytes(), kerndb::PageId{7U});
    KERNDB_EXPECT(loaded.ok());
    KERNDB_EXPECT_EQ(kerndb::PageId{7U}, loaded.value().page_id());
    KERNDB_EXPECT_EQ(kerndb::storage::PageType::kHeap, loaded.value().type());
}

KERNDB_TEST(PageRejectsChecksumTypeAndLocationCorruption) {
    const auto created = kerndb::storage::Page::Create(
        kerndb::storage::PageType::kHeap,
        kerndb::PageId{1U});
    KERNDB_EXPECT(created.ok());

    auto checksum_corrupt = created.value();
    checksum_corrupt.mutable_bytes()[100U] = std::byte{0xAAU};
    KERNDB_EXPECT(!kerndb::storage::Page::Deserialize(checksum_corrupt.bytes()).ok());

    auto type_corrupt = created.value();
    type_corrupt.mutable_bytes()[8U] = std::byte{0xFFU};
    KERNDB_EXPECT(type_corrupt.Finalize().ok());
    KERNDB_EXPECT(!kerndb::storage::Page::Deserialize(type_corrupt.bytes()).ok());

    KERNDB_EXPECT(!kerndb::storage::Page::Deserialize(
        created.value().bytes(),
        kerndb::PageId{2U}).ok());
}
