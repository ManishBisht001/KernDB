#include <cstdint>
#include <string>

#include "kerndb/types.h"
#include "test_framework.h"

KERNDB_TEST(ValuesHaveStableTypesAndDisplayForms) {
    const kerndb::Value integer{std::int64_t{-42}};
    const kerndb::Value text{std::string("Ada")};

    KERNDB_EXPECT_EQ(kerndb::ColumnType::kInt, kerndb::ValueType(integer));
    KERNDB_EXPECT_EQ(kerndb::ColumnType::kText, kerndb::ValueType(text));
    KERNDB_EXPECT_EQ(std::string_view("INT"), kerndb::ColumnTypeName(kerndb::ColumnType::kInt));
    KERNDB_EXPECT_EQ(std::string("-42"), kerndb::ToString(integer));
    KERNDB_EXPECT_EQ(std::string("Ada"), kerndb::ToString(text));
    KERNDB_EXPECT(kerndb::ValuesEqual(text, kerndb::Value{std::string("Ada")}));
    KERNDB_EXPECT(!kerndb::ValuesEqual(integer, text));
}
