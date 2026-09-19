#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kerndb {

enum class ColumnType : std::uint8_t {
    kInt,
    kText,
};

using Value = std::variant<std::int64_t, std::string>;

struct ColumnDefinition {
    std::string name;
    ColumnType type;

    [[nodiscard]] bool operator==(const ColumnDefinition&) const = default;
};

using Schema = std::vector<ColumnDefinition>;
using Tuple = std::vector<Value>;

enum class QueryResultKind : std::uint8_t {
    kCreateTable,
    kCreateIndex,
    kInsert,
    kSelect,
};

struct QueryResult {
    QueryResultKind kind;
    Schema columns;
    std::vector<Tuple> rows;
    std::uint64_t rows_affected{0U};
};

[[nodiscard]] ColumnType ValueType(const Value& value) noexcept;
[[nodiscard]] std::string_view ColumnTypeName(ColumnType type) noexcept;
[[nodiscard]] bool ValuesEqual(const Value& left, const Value& right) noexcept;
[[nodiscard]] std::string ToString(const Value& value);

}  // namespace kerndb
