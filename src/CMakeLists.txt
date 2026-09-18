add_library(kerndb STATIC
    database.cpp
    binder/binder.cpp
    catalog/catalog.cpp
    common/byte_codec.cpp
    common/result.cpp
    execution/executor.cpp
    observability/telemetry.cpp
    parser/lexer.cpp
    parser/parser.cpp
    planner/planner.cpp
    storage/catalog_store.cpp
    storage/database_metadata.cpp
    storage/disk_manager.cpp
    storage/heap.cpp
    storage/page.cpp
    storage/page_manager.cpp
    storage/persistent_storage.cpp
    storage/record.cpp
    types/types.cpp
)

add_library(KernDB::kerndb ALIAS kerndb)

target_compile_features(kerndb PUBLIC cxx_std_20)

target_include_directories(kerndb
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

kerndb_apply_warnings(kerndb)
kerndb_enable_sanitizers(kerndb)
