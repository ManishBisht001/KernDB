#pragma once

#include <cstddef>
#include <cstdint>

namespace kerndb {

inline constexpr std::size_t kInitialPageSizeBytes = 4096U;
inline constexpr std::uint32_t kPersistentFormatVersion = 1U;

struct EngineOptions {
    bool telemetry_enabled{true};
};

}  // namespace kerndb
