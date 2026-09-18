# Phase 1 SQL reference, used by Phase 2 storage

The SQL subset remains a single-process learning slice. `Database()` owns an
in-memory catalog and tuples for its lifetime. `Database::Open(directory)` uses
the same parser, binder, planner, and executor with a durable paged heap and
persistent catalog; a clean reopen reloads tables and rows.

## Supported statements

~~~sql
CREATE TABLE people (id INT, name TEXT);
INSERT INTO people VALUES (1, 'Ada');
SELECT id, name FROM people WHERE id = 1;
~~~

The grammar is:

~~~text
statement      := create-table | insert | select [ ";" ]
create-table   := CREATE TABLE identifier "(" column-definition { "," column-definition } ")"
column-definition := identifier ( INT | TEXT )
insert         := INSERT INTO identifier VALUES "(" literal { "," literal } ")"
select         := SELECT identifier { "," identifier } FROM identifier
                  [ WHERE identifier "=" literal ]
literal        := [ "-" ] integer | string
~~~

Keywords and unquoted identifiers are ASCII case-insensitive. Identifiers are
stored as lowercase. INT is signed 64-bit. TEXT is an in-memory string, UTF-8 by
convention. Text uses single quotes; two adjacent single quotes encode one quote,
so 'Ada''s' means Ada's.

SELECT uses a sequential scan. Projection and the optional single equality
predicate are applied by the execution engine. The predicate literal must match
the selected table column type exactly.

## Intentionally unsupported

There is no SELECT *, multiple statements in one Database::Execute call, NULL,
joins, ordering, aggregates, update/delete/drop, constraints, transactions,
indexes, buffer pool, locking, WAL, or recovery. Phase 2 supports files and
fixed pages internally when a database directory is explicitly opened.

## CLI

~~~text
kerndb_shell --database people.db --execute "CREATE TABLE people (id INT, name TEXT);"
kerndb_shell --database people.db --execute "INSERT INTO people VALUES (1, 'Ada');" --execute "SELECT id, name FROM people WHERE id = 1;"
~~~

`--execute` may be repeated; all supplied statements share one database instance
for that process. `--database DIRECTORY` selects persistent Phase 2 storage.
With no options, the CLI starts a one-line in-memory REPL; use `.quit` or
end-of-file to exit.
