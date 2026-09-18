add_executable(kerndb_phase2_tests
    test_support/test_main.cpp
    unit/binder/binder_test.cpp
    unit/catalog/catalog_test.cpp
    unit/common/byte_codec_test.cpp
    unit/common/ids_test.cpp
    unit/common/result_test.cpp
    unit/observability/telemetry_test.cpp
    unit/execution/executor_test.cpp
    unit/parser/lexer_test.cpp
    unit/parser/parser_test.cpp
    unit/planner/planner_test.cpp
    unit/storage/catalog_store_test.cpp
    unit/storage/database_metadata_test.cpp
    unit/storage/disk_manager_test.cpp
    unit/storage/heap_test.cpp
    unit/storage/page_test.cpp
    unit/storage/record_test.cpp
    unit/types/types_test.cpp
    integration/database_execution_test.cpp
    integration/persistent_database_test.cpp
    integration/shell_smoke_test.cpp
)

target_link_libraries(kerndb_phase2_tests
    PRIVATE
        KernDB::kerndb
        kerndb_shell_support
)

target_include_directories(kerndb_phase2_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_support"
        "${PROJECT_SOURCE_DIR}/apps/kerndb_shell"
        "${PROJECT_SOURCE_DIR}/src"
)

kerndb_apply_warnings(kerndb_phase2_tests)
kerndb_enable_sanitizers(kerndb_phase2_tests)

add_test(NAME kerndb.phase2.unit_and_integration COMMAND kerndb_phase2_tests)
add_test(NAME kerndb.phase2.shell_help COMMAND kerndb_shell --help)
set_tests_properties(kerndb.phase2.shell_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: kerndb_shell"
)
add_test(NAME kerndb.phase2.shell_version COMMAND kerndb_shell --version)
set_tests_properties(kerndb.phase2.shell_version PROPERTIES
    PASS_REGULAR_EXPRESSION "KernDB 0.0.0"
)
