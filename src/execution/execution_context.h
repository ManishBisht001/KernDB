#pragma once

#include <optional>

#include "catalog/catalog.h"
#include "kerndb/ids.h"
#include "kerndb/storage/table_storage.h"

namespace kerndb::execution {

struct ExecutionContext {
    InMemoryCatalog& catalog;
    storage::TableStorage* table_storage{nullptr};
    std::optional<TransactionId> transaction_id;
};

}  // namespace kerndb::execution
