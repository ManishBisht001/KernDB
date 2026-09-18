#include "kerndb/byte_codec.h"

#include <bit>
#include <concepts>
#include <cstddef>
#include <string>

namespace kerndb {
namespace {

template <std::unsigned_integral Unsigned>
void WriteLittleEndian(std::vector<std::byte>& output, Unsigned value) {
    static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big);

    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        const auto next_byte = static_cast<unsigned char>((value >> shift) & 0xFFU);
        output.push_back(static_cast<std::byte>(next_byte));
    }
}

template <std::unsigned_integral Unsigned>
Unsigned ReadLittleEndian(std::span<const std::byte> input) {
    Unsigned result = 0U;
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        const auto byte_value = static_cast<Unsigned>(
            std::to_integer<unsigned char>(input[index]));
        result |= (byte_value << shift);
    }
    return result;
}

}  // namespace

ByteWriter::ByteWriter(std::size_t reserve_bytes) {
    bytes_.reserve(reserve_bytes);
}

void ByteWriter::WriteUInt8(std::uint8_t value) {
    WriteLittleEndian(bytes_, value);
}

void ByteWriter::WriteUInt16(std::uint16_t value) {
    WriteLittleEndian(bytes_, value);
}

void ByteWriter::WriteUInt32(std::uint32_t value) {
    WriteLittleEndian(bytes_, value);
}

void ByteWriter::WriteUInt64(std::uint64_t value) {
    WriteLittleEndian(bytes_, value);
}

void ByteWriter::WriteBytes(std::span<const std::byte> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

std::span<const std::byte> ByteWriter::bytes() const noexcept {
    return bytes_;
}

std::vector<std::byte> ByteWriter::TakeBytes() && {
    return std::move(bytes_);
}

ByteReader::ByteReader(std::span<const std::byte> bytes) noexcept
    : bytes_(bytes) {}

std::size_t ByteReader::offset() const noexcept {
    return offset_;
}

std::size_t ByteReader::remaining() const noexcept {
    return bytes_.size() - offset_;
}

Result<std::uint8_t> ByteReader::ReadUInt8() {
    const auto bytes = Consume(sizeof(std::uint8_t));
    if (!bytes.ok()) {
        return bytes.status();
    }
    return ReadLittleEndian<std::uint8_t>(bytes.value());
}

Result<std::uint16_t> ByteReader::ReadUInt16() {
    const auto bytes = Consume(sizeof(std::uint16_t));
    if (!bytes.ok()) {
        return bytes.status();
    }
    return ReadLittleEndian<std::uint16_t>(bytes.value());
}

Result<std::uint32_t> ByteReader::ReadUInt32() {
    const auto bytes = Consume(sizeof(std::uint32_t));
    if (!bytes.ok()) {
        return bytes.status();
    }
    return ReadLittleEndian<std::uint32_t>(bytes.value());
}

Result<std::uint64_t> ByteReader::ReadUInt64() {
    const auto bytes = Consume(sizeof(std::uint64_t));
    if (!bytes.ok()) {
        return bytes.status();
    }
    return ReadLittleEndian<std::uint64_t>(bytes.value());
}

Result<std::span<const std::byte>> ByteReader::ReadBytes(std::size_t count) {
    return Consume(count);
}

Status ByteReader::Skip(std::size_t count) {
    const auto bytes = Consume(count);
    if (!bytes.ok()) {
        return bytes.status();
    }
    return Status::Ok();
}

Result<std::span<const std::byte>> ByteReader::Consume(std::size_t count) {
    if (offset_ > bytes_.size() || count > bytes_.size() - offset_) {
        return Status::Error(
                   ErrorCode::kOutOfRange,
                   "byte read exceeds the available input")
            .WithContext("offset", std::to_string(offset_))
            .WithContext("requested", std::to_string(count))
            .WithContext("remaining", std::to_string(remaining()));
    }

    const auto result = bytes_.subspan(offset_, count);
    offset_ += count;
    return result;
}

}  // namespace kerndb
