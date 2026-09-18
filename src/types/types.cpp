#include "kerndb/types.h"

#include <string>
#include <variant>

namespace kerndb {

ColumnType ValueType(const Value& value) noexcept {
    return std::holds_alternative<std::int64_t>(value) ? ColumnType::kInt : ColumnType::kText;
}

std::string_view ColumnTypeName(ColumnType type) noexcept {
    switch (type) {
        case ColumnType::kInt:
            return "INT";
        case ColumnType::kText:
            return "TEXT";
    }
    return "UNKNOWN";
}

bool ValuesEqual(const Value& left, const Value& right) noexcept {
    return left == right;
}

std::string ToString(const Value& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value); integer != nullptr) {
        return std::to_string(*integer);
    }
    return std::get<std::string>(value);
}

}  // namespace kerndb
