#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kerndb/result.h"
#include "kerndb/types.h"

namespace kerndb::storage {

inline constexpr std::uint16_t kRecordFormatVersion = 1U;
inline constexpr std::size_t kMaxInlineTextBytes = 1024U;

[[nodiscard]] Result<std::vector<std::byte>> EncodeRecord(
    const Schema& schema,
    const Tuple& tuple);
[[nodiscard]] Result<Tuple> DecodeRecord(
    const Schema& schema,
    std::span<const std::byte> encoded);

}  // namespace kerndb::storage
