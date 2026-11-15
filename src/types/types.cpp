#include "kerndb/types.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <variant>

namespace kerndb {

ColumnType ValueType(const Value& value) noexcept {
    if (std::holds_alternative<std::int64_t>(value)) return ColumnType::kInt;
    if (std::holds_alternative<std::string>(value)) return ColumnType::kText;
    if (std::holds_alternative<double>(value)) return ColumnType::kReal;
    return ColumnType::kNull;
}

std::string_view ColumnTypeName(ColumnType type) noexcept {
    switch (type) {
        case ColumnType::kInt:
            return "INT";
        case ColumnType::kText:
            return "TEXT";
        case ColumnType::kReal:
            return "REAL";
        case ColumnType::kNull:
            return "NULL";
    }
    return "UNKNOWN";
}

bool ValuesEqual(const Value& left, const Value& right) noexcept {
    if (std::holds_alternative<std::monostate>(left) ||
        std::holds_alternative<std::monostate>(right)) {
        return false;
    }
    if (const auto* left_integer = std::get_if<std::int64_t>(&left); left_integer != nullptr) {
        if (const auto* right_real = std::get_if<double>(&right); right_real != nullptr) {
            return static_cast<double>(*left_integer) == *right_real;
        }
    }
    if (const auto* left_real = std::get_if<double>(&left); left_real != nullptr) {
        if (const auto* right_integer = std::get_if<std::int64_t>(&right); right_integer != nullptr) {
            return *left_real == static_cast<double>(*right_integer);
        }
    }
    return left == right;
}

std::string ToString(const Value& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value); integer != nullptr) {
        return std::to_string(*integer);
    }
    if (const auto* text = std::get_if<std::string>(&value); text != nullptr) return *text;
    if (const auto* real = std::get_if<double>(&value); real != nullptr) {
        std::ostringstream output;
        output << std::setprecision(15) << *real;
        return output.str();
    }
    return "NULL";
}

}  // namespace kerndb
