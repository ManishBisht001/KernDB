#pragma once

#include "catalog/catalog.h"
#include "kerndb/storage/table_storage.h"

namespace kerndb::execution {

struct ExecutionContext {
    InMemoryCatalog& catalog;
    storage::TableStorage* table_storage{nullptr};
};

}  // namespace kerndb::execution
