#include <variant>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "kerndb/types.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "test_framework.h"

KERNDB_TEST(PlannerAlwaysLowersSelectToASequentialScan) {
    kerndb::InMemoryCatalog catalog;
    const kerndb::Schema schema{
        kerndb::ColumnDefinition{.name = "id", .type = kerndb::ColumnType::kInt},
    };
    KERNDB_EXPECT(catalog.CreateTable("people", schema).ok());

    const auto ast = kerndb::parser::ParseSql("SELECT id FROM people WHERE id = 1");
    KERNDB_EXPECT(ast.ok());
    const auto bound = kerndb::binder::BindStatement(ast.value(), catalog);
    KERNDB_EXPECT(bound.ok());

    const kerndb::planner::LogicalPlan logical = kerndb::planner::BuildLogicalPlan(bound.value());
    KERNDB_EXPECT(std::holds_alternative<kerndb::planner::LogicalSelect>(logical));
    const auto physical = kerndb::planner::PlanStatement(bound.value());
    KERNDB_EXPECT(physical.ok());
    KERNDB_EXPECT(std::holds_alternative<kerndb::planner::PhysicalSequentialScan>(
        physical.value()));
}
