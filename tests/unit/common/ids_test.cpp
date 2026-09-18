#include <cstdint>
#include <string>

#include "kerndb/ids.h"
#include "test_framework.h"

KERNDB_TEST(StrongIdsKeepStableNumericValues) {
    const kerndb::TableId table_id{42U};
    const kerndb::PageId page_id{7U};
    const kerndb::SlotId slot_id{3U};

    KERNDB_EXPECT(table_id.valid());
    KERNDB_EXPECT(page_id.valid());
    KERNDB_EXPECT_EQ(std::uint64_t{42U}, table_id.value());
    KERNDB_EXPECT_EQ(std::string("42"), kerndb::ToString(table_id));
    KERNDB_EXPECT_EQ(std::string("7:3"), kerndb::ToString(kerndb::RecordId{
                                              .page_id = page_id,
                                              .slot_id = slot_id,
                                          }));
}

KERNDB_TEST(DefaultIdsAndRecordsAreInvalid) {
    const kerndb::TableId table_id;
    const kerndb::RecordId record_id;

    KERNDB_EXPECT(!table_id.valid());
    KERNDB_EXPECT(!record_id.valid());
    KERNDB_EXPECT_EQ(kerndb::TableId::kInvalidValue, table_id.value());
}
