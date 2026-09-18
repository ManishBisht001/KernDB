#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "kerndb/byte_codec.h"
#include "kerndb/result.h"
#include "test_framework.h"

KERNDB_TEST(ByteCodecWritesAndReadsLittleEndianValues) {
    kerndb::ByteWriter writer;
    writer.WriteUInt8(0xABU);
    writer.WriteUInt16(0x1234U);
    writer.WriteUInt32(0x89ABCDEFU);
    writer.WriteUInt64(0x0123456789ABCDEFULL);

    const std::span<const std::byte> bytes = writer.bytes();
    KERNDB_EXPECT_EQ(std::size_t{15U}, bytes.size());
    KERNDB_EXPECT_EQ(std::byte{0xABU}, bytes[0]);
    KERNDB_EXPECT_EQ(std::byte{0x34U}, bytes[1]);
    KERNDB_EXPECT_EQ(std::byte{0x12U}, bytes[2]);

    kerndb::ByteReader reader{bytes};
    const auto uint8_result = reader.ReadUInt8();
    const auto uint16_result = reader.ReadUInt16();
    const auto uint32_result = reader.ReadUInt32();
    const auto uint64_result = reader.ReadUInt64();

    KERNDB_EXPECT(uint8_result.ok());
    KERNDB_EXPECT(uint16_result.ok());
    KERNDB_EXPECT(uint32_result.ok());
    KERNDB_EXPECT(uint64_result.ok());
    KERNDB_EXPECT_EQ(std::uint8_t{0xABU}, uint8_result.value());
    KERNDB_EXPECT_EQ(std::uint16_t{0x1234U}, uint16_result.value());
    KERNDB_EXPECT_EQ(std::uint32_t{0x89ABCDEFU}, uint32_result.value());
    KERNDB_EXPECT_EQ(std::uint64_t{0x0123456789ABCDEFULL}, uint64_result.value());
    KERNDB_EXPECT_EQ(std::size_t{0U}, reader.remaining());
}

KERNDB_TEST(ByteReaderRejectsOutOfRangeReadsWithoutAdvancing) {
    const std::array<std::byte, 2U> bytes{
        std::byte{0x01U},
        std::byte{0x02U},
    };
    kerndb::ByteReader reader{bytes};

    const auto result = reader.ReadUInt32();
    KERNDB_EXPECT(!result.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kOutOfRange, result.status().code());
    KERNDB_EXPECT_EQ(std::size_t{0U}, reader.offset());
    KERNDB_EXPECT(result.status().ToString().find("requested=4") != std::string::npos);
}

KERNDB_TEST(ByteReaderSkipChecksBounds) {
    const std::array<std::byte, 3U> bytes{
        std::byte{0x01U},
        std::byte{0x02U},
        std::byte{0x03U},
    };
    kerndb::ByteReader reader{bytes};

    KERNDB_EXPECT(reader.Skip(2U).ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, reader.remaining());
    const kerndb::Status skip_status = reader.Skip(2U);
    KERNDB_EXPECT(!skip_status.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, reader.remaining());
}
