#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>

namespace kerndb {

template <typename Tag, std::unsigned_integral Underlying>
class StrongId {
public:
    using ValueType = Underlying;

    static constexpr Underlying kInvalidValue = std::numeric_limits<Underlying>::max();

    constexpr StrongId() noexcept = default;

    explicit constexpr StrongId(Underlying value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr Underlying value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value_ != kInvalidValue;
    }

    [[nodiscard]] friend constexpr auto operator<=>(const StrongId&, const StrongId&) = default;

private:
    Underlying value_{kInvalidValue};
};

struct TableIdTag {};
struct IndexIdTag {};
struct PageIdTag {};
struct SlotIdTag {};
struct TransactionIdTag {};
struct QueryIdTag {};
struct LogSequenceNumberTag {};

using TableId = StrongId<TableIdTag, std::uint64_t>;
using IndexId = StrongId<IndexIdTag, std::uint64_t>;
using PageId = StrongId<PageIdTag, std::uint64_t>;
using SlotId = StrongId<SlotIdTag, std::uint32_t>;
using TransactionId = StrongId<TransactionIdTag, std::uint64_t>;
using QueryId = StrongId<QueryIdTag, std::uint64_t>;
using LogSequenceNumber = StrongId<LogSequenceNumberTag, std::uint64_t>;

struct RecordId {
    PageId page_id;
    SlotId slot_id;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return page_id.valid() && slot_id.valid();
    }

    [[nodiscard]] bool operator==(const RecordId&) const = default;
};

template <typename Tag, std::unsigned_integral Underlying>
[[nodiscard]] inline std::string ToString(StrongId<Tag, Underlying> id) {
    return std::to_string(id.value());
}

[[nodiscard]] inline std::string ToString(RecordId record_id) {
    return ToString(record_id.page_id) + ":" + ToString(record_id.slot_id);
}

}  // namespace kerndb
