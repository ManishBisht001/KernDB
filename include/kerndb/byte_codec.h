#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kerndb/result.h"

namespace kerndb {

class ByteWriter {
public:
    explicit ByteWriter(std::size_t reserve_bytes = 0U);

    void WriteUInt8(std::uint8_t value);
    void WriteUInt16(std::uint16_t value);
    void WriteUInt32(std::uint32_t value);
    void WriteUInt64(std::uint64_t value);
    void WriteBytes(std::span<const std::byte> bytes);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::vector<std::byte> TakeBytes() &&;

private:
    std::vector<std::byte> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::size_t offset() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;

    [[nodiscard]] Result<std::uint8_t> ReadUInt8();
    [[nodiscard]] Result<std::uint16_t> ReadUInt16();
    [[nodiscard]] Result<std::uint32_t> ReadUInt32();
    [[nodiscard]] Result<std::uint64_t> ReadUInt64();
    [[nodiscard]] Result<std::span<const std::byte>> ReadBytes(std::size_t count);
    [[nodiscard]] Status Skip(std::size_t count);

private:
    [[nodiscard]] Result<std::span<const std::byte>> Consume(std::size_t count);

    std::span<const std::byte> bytes_;
    std::size_t offset_{0U};
};

}  // namespace kerndb
