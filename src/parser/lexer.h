#pragma once

#include <string_view>
#include <vector>

#include "kerndb/result.h"
#include "token.h"

namespace kerndb::parser {

[[nodiscard]] Result<std::vector<Token>> LexSql(std::string_view sql);

}  // namespace kerndb::parser
