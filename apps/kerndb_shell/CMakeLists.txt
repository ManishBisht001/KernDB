add_library(kerndb_shell_support STATIC
    shell.cpp
)

target_link_libraries(kerndb_shell_support PRIVATE KernDB::kerndb)
target_include_directories(kerndb_shell_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
kerndb_apply_warnings(kerndb_shell_support)
kerndb_enable_sanitizers(kerndb_shell_support)

add_executable(kerndb_shell
    main.cpp
)

target_link_libraries(kerndb_shell PRIVATE kerndb_shell_support)
kerndb_apply_warnings(kerndb_shell)
kerndb_enable_sanitizers(kerndb_shell)
