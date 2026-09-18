#pragma once

#include "bound_statement.h"
#include "catalog/catalog.h"
#include "kerndb/result.h"
#include "parser/ast.h"

namespace kerndb::binder {

[[nodiscard]] Result<BoundStatement> BindStatement(
    const parser::AstStatement& statement,
    const InMemoryCatalog& catalog);

}  // namespace kerndb::binder
