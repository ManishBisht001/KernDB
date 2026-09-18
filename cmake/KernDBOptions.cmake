include_guard(GLOBAL)

option(KERNDB_BUILD_SHELL "Build the KernDB development CLI." ON)
option(KERNDB_BUILD_TESTS "Build KernDB's repository-owned tests." ON)
option(KERNDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for KernDB targets." ON)
option(KERNDB_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer on Linux/WSL." OFF)

function(kerndb_enable_sanitizers target_name)
    if(NOT KERNDB_ENABLE_SANITIZERS)
        return()
    endif()

    if(WIN32)
        message(WARNING
            "KERNDB_ENABLE_SANITIZERS is intended for Linux/WSL. "
            "Use MSVC Debug runtime checks on native Windows instead.")
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target_name} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target_name} PRIVATE
            -fsanitize=address,undefined
        )
        return()
    endif()

    message(FATAL_ERROR
        "KERNDB_ENABLE_SANITIZERS requires a Clang or GNU compiler on Linux/WSL.")
endfunction()
