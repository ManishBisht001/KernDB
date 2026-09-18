#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/types.h"

namespace kerndb {

[[nodiscard]] std::string NormalizeIdentifier(std::string_view identifier);

struct InMemoryTable {
    TableId id;
    std::string name;
    Schema schema;
    std::vector<Tuple> tuples;
};

class InMemoryCatalog {
public:
    [[nodiscard]] Result<TableId> CreateTable(std::string name, Schema schema);
    [[nodiscard]] Status LoadTable(TableId id, std::string name, Schema schema);
    [[nodiscard]] Status RemoveTable(TableId id);
    [[nodiscard]] Result<InMemoryTable*> FindTableByName(std::string_view name);
    [[nodiscard]] Result<const InMemoryTable*> FindTableByName(std::string_view name) const;
    [[nodiscard]] Result<InMemoryTable*> FindTableById(TableId id);
    [[nodiscard]] Result<const InMemoryTable*> FindTableById(TableId id) const;

private:
    [[nodiscard]] TableId NextTableId();

    std::uint64_t next_table_id_{1U};
    std::map<std::string, std::uint64_t, std::less<>> table_ids_by_name_;
    std::map<std::uint64_t, InMemoryTable> tables_by_id_;
};

}  // namespace kerndb
