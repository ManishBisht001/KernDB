#pragma once

#include <string_view>

#include "ast.h"
#include "kerndb/result.h"

namespace kerndb::parser {

[[nodiscard]] Result<AstStatement> ParseSql(std::string_view sql);

}  // namespace kerndb::parser
