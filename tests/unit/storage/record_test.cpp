#include <cstdint>
#include <string>

#include "kerndb/storage/record.h"
#include "test_framework.h"

KERNDB_TEST(RecordCodecRoundTripsSchemaOrderedIntAndText) {
    const kerndb::Schema schema{
        {.name = "id", .type = kerndb::ColumnType::kInt},
        {.name = "name", .type = kerndb::ColumnType::kText},
    };
    const kerndb::Tuple tuple{std::int64_t{-3}, std::string("Ada")};
    const auto encoded = kerndb::storage::EncodeRecord(schema, tuple);
    KERNDB_EXPECT(encoded.ok());
    const auto decoded = kerndb::storage::DecodeRecord(schema, encoded.value());
    KERNDB_EXPECT(decoded.ok());
    KERNDB_EXPECT_EQ(tuple, decoded.value());
}

KERNDB_TEST(RecordCodecRejectsOversizedAndMalformedValues) {
    const kerndb::Schema schema{
        {.name = "name", .type = kerndb::ColumnType::kText},
    };
    const kerndb::Tuple oversized{std::string(kerndb::storage::kMaxInlineTextBytes + 1U, 'x')};
    KERNDB_EXPECT(!kerndb::storage::EncodeRecord(schema, oversized).ok());

    const std::vector<std::byte> malformed{std::byte{1U}, std::byte{0U}, std::byte{4U}};
    KERNDB_EXPECT(!kerndb::storage::DecodeRecord(schema, malformed).ok());

    const std::vector<std::byte> unsupported_version{
        std::byte{2U}, std::byte{0U}, std::byte{1U}, std::byte{0U},
    };
    const auto decoded = kerndb::storage::DecodeRecord(schema, unsupported_version);
    KERNDB_EXPECT(!decoded.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kUnsupported, decoded.status().code());
}
