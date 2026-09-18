#include "kerndb/storage/record.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "kerndb/byte_codec.h"

namespace kerndb::storage {
namespace {

[[nodiscard]] Status RecordError(std::string message) {
    return Status::Error(ErrorCode::kCorruption, std::move(message));
}

}  // namespace

Result<std::vector<std::byte>> EncodeRecord(const Schema& schema, const Tuple& tuple) {
    if (schema.size() != tuple.size()) {
        return Status::Error(ErrorCode::kInvalidArgument, "tuple does not match schema column count");
    }
    if (schema.size() > std::numeric_limits<std::uint16_t>::max()) {
        return Status::Error(ErrorCode::kResourceExhausted, "schema has too many columns");
    }

    ByteWriter writer;
    writer.WriteUInt16(kRecordFormatVersion);
    writer.WriteUInt16(static_cast<std::uint16_t>(schema.size()));
    for (std::size_t index = 0U; index < schema.size(); ++index) {
        if (ValueType(tuple[index]) != schema[index].type) {
            return Status::Error(ErrorCode::kType, "tuple value does not match schema type")
                .WithContext("column", schema[index].name);
        }
        if (schema[index].type == ColumnType::kInt) {
            writer.WriteUInt64(std::bit_cast<std::uint64_t>(std::get<std::int64_t>(tuple[index])));
            continue;
        }

        const std::string& text = std::get<std::string>(tuple[index]);
        if (text.size() > kMaxInlineTextBytes) {
            return Status::Error(ErrorCode::kResourceExhausted, "TEXT exceeds Phase 2 inline limit")
                .WithContext("limit", std::to_string(kMaxInlineTextBytes))
                .WithContext("actual", std::to_string(text.size()));
        }
        writer.WriteUInt32(static_cast<std::uint32_t>(text.size()));
        const std::span<const char> characters{text.data(), text.size()};
        writer.WriteBytes(std::as_bytes(characters));
    }
    return std::move(writer).TakeBytes();
}

Result<Tuple> DecodeRecord(const Schema& schema, std::span<const std::byte> encoded) {
    ByteReader reader(encoded);
    const auto version = reader.ReadUInt16();
    const auto column_count = reader.ReadUInt16();
    if (!version.ok() || !column_count.ok()) {
        return column_count.status().WithContext("record", "column count");
    }
    if (version.value() != kRecordFormatVersion) {
        return Status::Error(ErrorCode::kUnsupported, "record format version is unsupported")
            .WithContext("record_version", std::to_string(version.value()));
    }
    if (column_count.value() != schema.size()) {
        return RecordError("record column count does not match catalog schema")
            .WithContext("record_columns", std::to_string(column_count.value()))
            .WithContext("schema_columns", std::to_string(schema.size()));
    }

    Tuple tuple;
    tuple.reserve(schema.size());
    for (const ColumnDefinition& column : schema) {
        if (column.type == ColumnType::kInt) {
            const auto integer = reader.ReadUInt64();
            if (!integer.ok()) {
                return integer.status().WithContext("column", column.name);
            }
            tuple.emplace_back(std::bit_cast<std::int64_t>(integer.value()));
            continue;
        }

        const auto length = reader.ReadUInt32();
        if (!length.ok()) {
            return length.status().WithContext("column", column.name);
        }
        if (length.value() > kMaxInlineTextBytes) {
            return RecordError("stored TEXT length exceeds Phase 2 inline limit")
                .WithContext("column", column.name);
        }
        const auto bytes = reader.ReadBytes(length.value());
        if (!bytes.ok()) {
            return bytes.status().WithContext("column", column.name);
        }
        tuple.emplace_back(std::string(
            reinterpret_cast<const char*>(bytes.value().data()),
            bytes.value().size()));
    }
    if (reader.remaining() != 0U) {
        return RecordError("record contains trailing bytes");
    }
    return tuple;
}

}  // namespace kerndb::storage
