# Phase 2 development guide

KernDB Phase 2 is a static C++20 library, native SQL frontend, typed executor,
and repository-owned CTest suite. `Database::Open(directory)` persists the
catalog and heap tables beneath that directory. The default `Database()` and
CLI without `--database` remain in-memory for focused SQL experiments.

This phase has a direct disk/page path only: it deliberately has no buffer pool,
indexes, transactions, locking, WAL, crash recovery, networking, or web layer.
An orderly write is visible to a later clean reopen, but Phase 2 does not claim
crash-safe durability.

## Prerequisites

- CMake 3.25 or later
- A C++20 compiler
  - Windows: MSVC in a Developer Command Prompt, or another CMake-supported C++20 toolchain
  - Linux/WSL: GCC or Clang

No package manager or third-party runtime/test dependency is required.

## Debug build and tests

~~~
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
~~~

The debug preset uses Ninja and the local MSYS2 UCRT64 GCC toolchain on Windows.
Linux/WSL users may use a CMake-supported C++20 GCC or Clang toolchain.

## Sanitizer build on Linux/WSL

~~~
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
~~~

The sanitizer preset enables AddressSanitizer and UndefinedBehaviorSanitizer for
GCC/Clang on Linux/WSL. Native Windows builds leave sanitizers disabled and use
the selected MSVC Debug checks instead.

## CLI smoke checks

After building, run the generated kerndb_shell executable:

~~~
kerndb_shell --help
kerndb_shell --version
kerndb_shell --database example.db --execute "CREATE TABLE people (id INT, name TEXT);" --execute "INSERT INTO people VALUES (1, 'Ada');"
kerndb_shell --database example.db --execute "SELECT id, name FROM people WHERE id = 1;"
~~~

CTest also executes both smoke checks as separate tests.
