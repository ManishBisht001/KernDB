# KernDB Phase 8: CLI Demonstration Results

Run date: 2026-11-13 10:12:38 +05:30

## Scope and environment

All feature, negative, and persistence queries below were executed with the KernDB shell, using a fresh shell process per statement against the dedicated persistent database build/phase8_demo_db. Each process exited after its statement, so the later persistence checks reopen the database after orderly closes. No existing database was used or overwritten.

Executable: .\build\debug\apps\kerndb_shell\kerndb_shell.exe

The exact requested SELECT * examples are included. The command reference documents SELECT * as unsupported; their actual parser errors are recorded alongside working explicit-column equivalents.

## Demo database setup

The dedicated database directory was absent before setup. It contains two tables, six employees across three departments, and four departments, including Operations with no employees.

SQL statements executed:

~~~sql
CREATE TABLE employees (id INT, name TEXT, department_id INT, salary INT);
CREATE TABLE departments (id INT, department_name TEXT);
INSERT INTO employees VALUES (1, 'Ada', 10, 95000);
INSERT INTO employees VALUES (2, 'Grace', 10, 125000);
INSERT INTO employees VALUES (3, 'Alan', 20, 115000);
INSERT INTO employees VALUES (4, 'Barbara', 20, 80000);
INSERT INTO employees VALUES (5, 'Linus', 30, 70000);
INSERT INTO employees VALUES (6, 'Edsger', 30, 150000);
INSERT INTO departments VALUES (10, 'Engineering');
INSERT INTO departments VALUES (20, 'Research');
INSERT INTO departments VALUES (30, 'Sales');
INSERT INTO departments VALUES (40, 'Operations');
~~~

Actual setup output:

~~~text
CREATE TABLE
CREATE TABLE
INSERT 1
INSERT 1
INSERT 1
INSERT 1
INSERT 1
INSERT 1
INSERT 1
INSERT 1
INSERT 1
INSERT 1
~~~

## Feature demonstrations

Each block below records the actual SQL sent to the shell, its captured output, and the shell process exit code. A nonzero exit or an error message is reported as observed.

### A. Basic SELECT (requested SELECT *)

SQL: SELECT * FROM employees;

Actual CLI output:

~~~text
error: syntax: expected identifier, found '*' [offset=7] [line=1] [column=8]
~~~

Exit code: 1

Result: This exact requested syntax is rejected because SELECT * is not implemented. See the explicit-column query next.

### A. Basic SELECT (supported explicit-column form)

SQL: SELECT id, name, department_id, salary FROM employees ORDER BY id;

Actual CLI output:

~~~text
id | name | department_id | salary
1 | Ada | 10 | 95000
2 | Grace | 10 | 125000
3 | Alan | 20 | 115000
4 | Barbara | 20 | 80000
5 | Linus | 30 | 70000
6 | Edsger | 30 | 150000
~~~

Exit code: 0

Result: Returns all six employee rows with each requested column.

### B. ORDER BY ASC

SQL: SELECT name, salary FROM employees ORDER BY salary ASC;

Actual CLI output:

~~~text
name | salary
Linus | 70000
Barbara | 80000
Ada | 95000
Alan | 115000
Grace | 125000
Edsger | 150000
~~~

Exit code: 0

Result: Orders salaries from lowest to highest.

### C. ORDER BY DESC

SQL: SELECT name, salary FROM employees ORDER BY salary DESC;

Actual CLI output:

~~~text
name | salary
Edsger | 150000
Grace | 125000
Alan | 115000
Ada | 95000
Barbara | 80000
Linus | 70000
~~~

Exit code: 0

Result: Orders salaries from highest to lowest.

### D. Multiple-column ORDER BY

SQL: SELECT name, department_id, salary FROM employees ORDER BY department_id ASC, salary DESC;

Actual CLI output:

~~~text
name | department_id | salary
Grace | 10 | 125000
Ada | 10 | 95000
Alan | 20 | 115000
Barbara | 20 | 80000
Edsger | 30 | 150000
Linus | 30 | 70000
~~~

Exit code: 0

Result: Groups rows by department key in ascending order and orders salaries descending within each department.

### E. LIMIT (requested SELECT *)

SQL: SELECT * FROM employees LIMIT 3;

Actual CLI output:

~~~text
error: syntax: expected identifier, found '*' [offset=7] [line=1] [column=8]
~~~

Exit code: 1

Result: This exact requested syntax is rejected because SELECT * is unsupported; the explicit-column equivalent follows.

### E. LIMIT (supported explicit-column form)

SQL: SELECT id, name, department_id, salary FROM employees ORDER BY id LIMIT 3;

Actual CLI output:

~~~text
id | name | department_id | salary
1 | Ada | 10 | 95000
2 | Grace | 10 | 125000
3 | Alan | 20 | 115000
~~~

Exit code: 0

Result: Returns the first three rows after ordering by id.

### E. LIMIT 0

SQL: SELECT id, name FROM employees LIMIT 0;

Actual CLI output:

~~~text
id | name
~~~

Exit code: 0

Result: Returns the selected columns with zero data rows.

### F. INNER JOIN

SQL: SELECT employees.name, departments.department_name FROM employees INNER JOIN departments ON employees.department_id = departments.id;

Actual CLI output:

~~~text
name | department_name
Ada | Engineering
Grace | Engineering
Alan | Research
Barbara | Research
Linus | Sales
Edsger | Sales
~~~

Exit code: 0

Result: Returns each employee with the matching department name.

### G. LEFT JOIN

SQL: SELECT departments.department_name, employees.name FROM departments LEFT JOIN employees ON departments.id = employees.department_id;

Actual CLI output:

~~~text
department_name | name
Engineering | Ada
Engineering | Grace
Research | Alan
Research | Barbara
Sales | Linus
Sales | Edsger
Operations | NULL
~~~

Exit code: 0

Result: Preserves every department; Operations should appear with NULL for employee name.

### H. Qualified column references

SQL: SELECT employees.name, employees.salary FROM employees;

Actual CLI output:

~~~text
name | salary
Ada | 95000
Grace | 125000
Alan | 115000
Barbara | 80000
Linus | 70000
Edsger | 150000
~~~

Exit code: 0

Result: Returns columns using table-qualified references.

### I. Ambiguous column validation

SQL: SELECT id FROM employees INNER JOIN departments ON employees.department_id = departments.id;

Actual CLI output:

~~~text
error: binding: ambiguous column: id [offset=7] [line=1] [column=8]
~~~

Exit code: 1

Result: Both inputs have id; the binder should report an ambiguous-column error.

### J1. COUNT(*)

SQL: SELECT COUNT(*) FROM employees;

Actual CLI output:

~~~text
count(*)
6
~~~

Exit code: 0

Result: Counts all employee rows.

### J2. COUNT(column)

SQL: SELECT COUNT(salary) FROM employees;

Actual CLI output:

~~~text
count(salary)
6
~~~

Exit code: 0

Result: Counts non-NULL salary values.

### J3. SUM

SQL: SELECT SUM(salary) FROM employees;

Actual CLI output:

~~~text
sum(salary)
635000
~~~

Exit code: 0

Result: Returns total salary.

### J4. AVG

SQL: SELECT AVG(salary) FROM employees;

Actual CLI output:

~~~text
avg(salary)
105833.333333333
~~~

Exit code: 0

Result: Returns mean salary as a floating result.

### J5. MIN

SQL: SELECT MIN(salary) FROM employees;

Actual CLI output:

~~~text
min(salary)
70000
~~~

Exit code: 0

Result: Returns the lowest salary.

### J6. MAX

SQL: SELECT MAX(salary) FROM employees;

Actual CLI output:

~~~text
max(salary)
150000
~~~

Exit code: 0

Result: Returns the highest salary.

### K. GROUP BY

SQL: SELECT department_id, COUNT(*), AVG(salary) FROM employees GROUP BY department_id;

Actual CLI output:

~~~text
department_id | count(*) | avg(salary)
10 | 2 | 110000
20 | 2 | 97500
30 | 2 | 110000
~~~

Exit code: 0

Result: Returns one count and average salary for each department represented by employees.

### L. HAVING

SQL: SELECT department_id, COUNT(*) FROM employees GROUP BY department_id HAVING COUNT(*) > 1;

Actual CLI output:

~~~text
department_id | count(*)
10 | 2
20 | 2
30 | 2
~~~

Exit code: 0

Result: Keeps only department groups with more than one employee.

### M. JOIN + WHERE + ORDER BY

SQL: SELECT employees.name, departments.department_name, employees.salary FROM employees INNER JOIN departments ON employees.department_id = departments.id WHERE employees.salary > 30000 ORDER BY employees.salary DESC;

Actual CLI output:

~~~text
name | department_name | salary
Edsger | Sales | 150000
Grace | Engineering | 125000
Alan | Research | 115000
Ada | Engineering | 95000
Barbara | Research | 80000
Linus | Sales | 70000
~~~

Exit code: 0

Result: Joins employee and department data, filters by salary, then sorts descending.

### N. GROUP BY + HAVING + ORDER BY

SQL: SELECT department_id, COUNT(*) FROM employees GROUP BY department_id HAVING COUNT(*) > 1 ORDER BY department_id DESC;

Actual CLI output:

~~~text
department_id | count(*)
30 | 2
20 | 2
10 | 2
~~~

Exit code: 0

Result: Filters grouped counts, then orders the remaining department keys descending.

### O. ORDER BY + LIMIT

SQL: SELECT name, salary FROM employees ORDER BY salary DESC LIMIT 3;

Actual CLI output:

~~~text
name | salary
Edsger | 150000
Grace | 125000
Alan | 115000
~~~

Exit code: 0

Result: Returns the three highest-paid employees.

### P1. EXPLAIN sequential scan, projection

SQL: EXPLAIN SELECT id, name FROM employees;

Actual CLI output:

~~~text
QUERY PLAN
Logical Plan:
Projection [id, name]
  Sequential Scan [employees]
Physical Plan:
Projection [id, name]
  Sequential Scan [employees]
~~~

Exit code: 0

Result: The captured physical plan shows Projection over Sequential Scan.

### P2. EXPLAIN filter, sort, limit, projection

SQL: EXPLAIN SELECT name, salary FROM employees WHERE salary > 100000 ORDER BY salary DESC LIMIT 2;

Actual CLI output:

~~~text
QUERY PLAN
Logical Plan:
Projection [name, salary]
  Limit [2]
    Sort [employees.salary DESC]
      Filter [employees.salary > 100000]
        Sequential Scan [employees]
Physical Plan:
Projection [name, salary]
  Limit [2]
    Sort [employees.salary DESC]
      Filter [employees.salary > 100000]
        Sequential Scan [employees]
~~~

Exit code: 0

Result: The captured physical plan shows Projection, Limit, Sort, Filter, and Sequential Scan.

### P3. EXPLAIN nested-loop join

SQL: EXPLAIN SELECT employees.name, departments.department_name FROM employees INNER JOIN departments ON employees.department_id = departments.id;

Actual CLI output:

~~~text
QUERY PLAN
Logical Plan:
Projection [name, department_name]
  Nested Loop Join (INNER) [employees.department_id = departments.id]
    Sequential Scan [employees]
    Sequential Scan [departments]
Physical Plan:
Projection [name, department_name]
  Nested Loop Join (INNER) [employees.department_id = departments.id]
    Sequential Scan [employees]
    Sequential Scan [departments]
~~~

Exit code: 0

Result: The captured physical plan shows Nested Loop Join over two Sequential Scans.

### P4. EXPLAIN aggregate

SQL: EXPLAIN SELECT department_id, COUNT(*) FROM employees GROUP BY department_id HAVING COUNT(*) > 1 ORDER BY department_id DESC;

Actual CLI output:

~~~text
QUERY PLAN
Logical Plan:
Projection [department_id, count(*)]
  Sort [employees.department_id DESC]
    Filter [HAVING COUNT > 1]
      Aggregate (GROUP BY employees.department_id)
        Sequential Scan [employees]
Physical Plan:
Projection [department_id, count(*)]
  Sort [employees.department_id DESC]
    Filter [HAVING COUNT > 1]
      Aggregate (GROUP BY employees.department_id)
        Sequential Scan [employees]
~~~

Exit code: 0

Result: The captured physical plan shows Aggregate, a HAVING Filter, and Sort.

### P5. Create persistent INT index

SQL: CREATE INDEX employees_id_idx ON employees(id);

Actual CLI output:

~~~text
CREATE INDEX
~~~

Exit code: 0

Result: Creates an index on the unique integer employee id for persistent storage.

### P6. EXPLAIN persistent index scan

SQL: EXPLAIN SELECT name FROM employees WHERE id = 3;

Actual CLI output:

~~~text
QUERY PLAN
Logical Plan:
Projection [name]
  Filter [employees.id = 3]
    Sequential Scan [employees]
Physical Plan:
Projection [name]
  Filter [employees.id = 3]
    Index Scan [employees]
~~~

Exit code: 0

Result: The captured physical plan explicitly shows Index Scan. Its logical tree shows Sequential Scan.

### P7. Indexed equality lookup

SQL: SELECT name FROM employees WHERE id = 3;

Actual CLI output:

~~~text
name
Alan
~~~

Exit code: 0

Result: Returns Alan through the indexed equality lookup.

### Negative. Unknown column

SQL: SELECT missing_column FROM employees;

Actual CLI output:

~~~text
error: binding: column does not exist: missing_column [offset=7] [line=1] [column=8]
~~~

Exit code: 1

Result: The binder returned a column-does-not-exist error.

### Negative. Invalid ORDER BY column

SQL: SELECT name FROM employees ORDER BY missing_column;

Actual CLI output:

~~~text
error: binding: column does not exist: missing_column [offset=36] [line=1] [column=37]
~~~

Exit code: 1

Result: The binder returned a column-does-not-exist error for ORDER BY.

### Negative. Invalid LIMIT value

SQL: SELECT id FROM employees LIMIT -1;

Actual CLI output:

~~~text
error: invalid_argument: LIMIT must be a non-negative integer
~~~

Exit code: 1

Result: The CLI returned invalid_argument: LIMIT must be a non-negative integer.

### Negative. Invalid aggregate/type combination

SQL: SELECT SUM(name) FROM employees;

Actual CLI output:

~~~text
error: type: sum requires an INT column [column=name]
~~~

Exit code: 1

Result: The binder returned a type error because SUM requires an INT column.

### Negative. Invalid GROUP BY selection

SQL: SELECT department_id, name, COUNT(*) FROM employees GROUP BY department_id;

Actual CLI output:

~~~text
error: binding: selected columns must appear in GROUP BY or be aggregated [column=name]
~~~

Exit code: 1

Result: The binder rejected name because it is neither aggregated nor listed in GROUP BY.

### Negative checks leave database usable

SQL: SELECT id, name FROM employees ORDER BY id LIMIT 2;

Actual CLI output:

~~~text
id | name
1 | Ada
2 | Grace
~~~

Exit code: 0

Result: A successful query after the validation errors confirms the database remains usable.

### Persistence. Reopened employee table

SQL: SELECT id, name, department_id, salary FROM employees ORDER BY id;

Actual CLI output:

~~~text
id | name | department_id | salary
1 | Ada | 10 | 95000
2 | Grace | 10 | 125000
3 | Alan | 20 | 115000
4 | Barbara | 20 | 80000
5 | Linus | 30 | 70000
6 | Edsger | 30 | 150000
~~~

Exit code: 0

Result: A fresh CLI process reopens the same persistent database and reads the inserted rows.

### Persistence. Reopened LEFT JOIN

SQL: SELECT departments.department_name, employees.name FROM departments LEFT JOIN employees ON departments.id = employees.department_id;

Actual CLI output:

~~~text
department_name | name
Engineering | Ada
Engineering | Grace
Research | Alan
Research | Barbara
Sales | Linus
Sales | Edsger
Operations | NULL
~~~

Exit code: 0

Result: A fresh CLI process reads both tables and confirms Operations remains present with NULL.


## Regression verification

Commands were run from the repository root after the CLI demonstrations. Full captured output and process exit codes follow.

### CMake configure

Command: cmake --preset debug

Actual output:

~~~text
-- Configuring done (0.2s)
-- Generating done (0.2s)
-- Build files have been written to: C:/Users/manis/OneDrive/Documents/ChatGPT/KernDB/build/debug
~~~

Exit code: 0

### CMake build

Command: cmake --build --preset debug

Actual output:

~~~text
ninja: no work to do.
~~~

Exit code: 0

### CTest regression suite

Command: ctest --preset debug --output-on-failure

Actual output:

~~~text
Test project C:/Users/manis/OneDrive/Documents/ChatGPT/KernDB/build/debug
    Start 1: kerndb.phase2.unit_and_integration
1/3 Test #1: kerndb.phase2.unit_and_integration ...   Passed   10.47 sec
    Start 2: kerndb.phase2.shell_help
2/3 Test #2: kerndb.phase2.shell_help .............   Passed    0.03 sec
    Start 3: kerndb.phase2.shell_version
3/3 Test #3: kerndb.phase2.shell_version ..........   Passed    0.02 sec

100% tests passed out of 3

Total Test time (real) =  10.66 sec
~~~

Exit code: 0

### Direct test executable

Command: .\build\debug\tests\kerndb_phase2_tests.exe

Actual output:

~~~text
[PASS] ShellHelpAndVersionAreAvailableWithoutDatabaseAccess
[PASS] ShellExecutesMultipleStatementsAgainstOneInMemoryDatabase
[PASS] ShellUsesPersistentDatabaseDirectoryAcrossRuns
[PASS] PersistentDatabaseReopensTheApprovedVerticalSlice
[PASS] PersistentOptimizerExplainsAndUsesAnAvailableEqualityIndex
[PASS] PersistentDatabaseReportsACatalogReferencedMissingTableFile
[PASS] PersistentDatabaseRetainsMultipleRowsAndPhaseOneSemanticsAfterRestart
[PASS] PersistentDatabaseBuildsMaintainsAndReopensAnIntIndex
[PASS] PersistentDatabaseCreatesAnIndexOverAnEmptyTable
[PASS] PersistentDatabaseIndexesExistingRowsAcrossSplitsAndRestart
[PASS] PersistentDatabaseRejectsAChecksumCorruptedTablePageOnScan
[PASS] PersistentDatabaseUpdatesRowsAndMaintainsUniqueIndexes
[PASS] PersistentDatabaseDeletesRowsMaintainsIndexesAndReusesKeys
[PASS] PersistentDatabaseMutationChangesSurviveRestartWithMultipleIndexes
[PASS] PersistentStorageRollsBackAnUpdateAfterHeapAndIndexChanges
[PASS] PersistentDatabaseCommitsExplicitInsertAndPersistsIt
[PASS] PersistentDatabaseRollsBackExplicitInsertAcrossReopen
[PASS] PersistentDatabaseRollsBackMultipleMutationsInReverseAndRestoresIndexedRid
[PASS] PersistentDatabaseKeepsFailedStatementAtomicAndTransactionActive
[PASS] PersistentDatabaseCloseRollsBackActiveTransaction
[PASS] InMemoryDatabaseSupportsExplicitTransactionRollback
[PASS] PersistentSessionsBlockConflictingWritesAndCommitIndependentTransactions
[PASS] PersistentSessionsAllowConcurrentReadsAndBlockReadWriteConflicts
[PASS] PersistentSessionsDetectDeadlockRollbackVictimAndWakeSurvivor
[PASS] DatabaseExecutesTheApprovedPhaseOneVerticalSlice
[PASS] DatabaseReportsTheRequiredPhaseOneErrors
[PASS] DatabaseExplainsAdvancedOperatorsAndRejectsInvalidAdvancedSql
[PASS] TransactionManagerCreatesUniqueContextsAndTracksTerminalStates
[PASS] ValuesHaveStableTypesAndDisplayForms
[PASS] RecordCodecRoundTripsSchemaOrderedIntAndText
[PASS] RecordCodecRejectsOversizedAndMalformedValues
[PASS] PageRoundTripUsesFixedSizeChecksumProtectedFormat
[PASS] PageRejectsChecksumTypeAndLocationCorruption
[PASS] IndexCatalogStorePersistsTheNewestVersionOfIndexMetadata
[PASS] SlottedPageKeepsSlotsStableAndValidatesReferences
[PASS] SlottedPageUsesTheLastByteOfAnEmptyPageWithoutOverlappingSlots
[PASS] TableHeapPersistsAndScansTuplesThroughTheBufferPool
[PASS] TableHeapTombstonesDeletedRecordsAndCanRestoreThemForRollback
[PASS] PageManagerAllocatesContiguousPersistentPageIds
[PASS] DiskManagerWritesReadsAndCountsFixedSizePages
[PASS] DiskManagerRejectsTruncatedFiles
[PASS] DiskManagerRejectsChecksumCorruptionPersistedToDisk
[PASS] DatabaseMetadataCreatesStableValidatedDatabaseIdentity
[PASS] DatabaseMetadataRejectsCorruptMagic
[PASS] BPlusTreeHandlesEmptyOneKeyMissingAndDuplicateLookups
[PASS] BPlusTreeMaintainsSortedEntriesAcrossLeafAndInternalSplits
[PASS] BPlusTreeMaintainsSortedEntriesForRandomInsertionOrder
[PASS] BPlusTreePersistsRootLeavesAndEntriesAcrossReopen
[PASS] BPlusTreeRemovesEntriesWithoutInvalidatingRemainingSearches
[PASS] BufferPoolCreatesFetchesAndMaintainsGuardPins
[PASS] BufferPoolFlushesDirtyPages
[PASS] BufferPoolEvictsTheLeastRecentlyUnpinnedFrame
[PASS] BufferPoolProtectsPinnedPagesAndReportsFullPoolFailure
[PASS] BufferPoolDeletesOnlyUnpinnedPages
[PASS] BufferPoolPersistsDirtyPagesAcrossAnOrderlyRestart
[PASS] BufferPoolRejectsCorruptionOnACacheMissWithoutEvictingAValidPage
[PASS] BufferPoolEmitsCacheFlushEvictionAndFailureTelemetry
[PASS] CatalogStorePersistsStableTableMetadata
[PASS] CatalogStoreRejectsMalformedCatalogRecords
[PASS] PlannerUsesAnAvailablePersistentIntIndex
[PASS] PlannerKeepsNonIntPredicatesAsSequentialScans
[PASS] ParserBuildsAllSupportedStatementKinds
[PASS] ParserRejectsUnsupportedAndMultiStatementSyntax
[PASS] ParserBuildsAdvancedSelectClausesAndExplain
[PASS] LexerHandlesKeywordsCommentsEscapedStringsAndSpans
[PASS] LexerReportsUnexpectedAndUnterminatedInput
[PASS] ExecutorCreatesInsertsFiltersAndProjectsInMemoryRows
[PASS] ExecutorAcceptsCreateIndexForTheInMemoryFallback
[PASS] ExecutorUpdatesAndDeletesInMemoryRowsAtomically
[PASS] ExecutorSortsStablyAndAppliesLimitAfterOrderBy
[PASS] ExecutorRunsInnerAndLeftNestedLoopJoinsWithNullExtension
[PASS] ExecutorComputesAggregatesGroupsHavingAndEmptyInputSemantics
[PASS] MetricsRegistryCapturesCountersAndIsThreadSafe
[PASS] MetricsRegistryRejectsInvalidNamesAndOverflow
[PASS] InMemoryEventSinkKeepsTheNewestEventsInOrder
[PASS] BufferPoolSerializesConcurrentPageGuardAccess
[PASS] BPlusTreeSupportsConcurrentInsertionsAndLookups
[PASS] LockManagerAllowsSharedReadersAndBlocksExclusiveRequests
[PASS] LockManagerBlocksSharedRequestsBehindExclusiveHolders
[PASS] LockManagerDetectsUpgradeDeadlockAndWakesTheOtherWaiter
[PASS] LockManagerReleasesWaitersWhenTheirTransactionIsAborted
[PASS] StatusCapturesErrorCodeMessageAndContext
[PASS] ResultStoresValuesAndErrors
[PASS] StatusNamesDeadlockAndAbortedTransactionErrors
[PASS] StrongIdsKeepStableNumericValues
[PASS] DefaultIdsAndRecordsAreInvalid
[PASS] ByteCodecWritesAndReadsLittleEndianValues
[PASS] ByteReaderRejectsOutOfRangeReadsWithoutAdvancing
[PASS] ByteReaderSkipChecksBounds
[PASS] CatalogUsesCaseInsensitiveNamesAndStableTableIds
[PASS] CatalogRejectsDuplicateTables
[PASS] BinderResolvesCaseInsensitiveTablesAndColumns
[PASS] BinderResolvesCreateIndexAndRestrictsItToIntColumns
[PASS] BinderRejectsDuplicateColumnsAndInvalidDataReferences
[PASS] BinderResolvesUpdateAndDeleteMutations
[PASS] BinderResolvesQualifiedJoinColumnsAndRejectsAmbiguity
[PASS] BinderValidatesGroupingAggregateTypesAndLimit
97/97 tests passed
~~~

Exit code: 0

## Feature-wise summary

| Feature | SQL executed | Result | Status |
|---|---|---|---|
| Basic SELECT | Requested SELECT *; explicit-column equivalent | SELECT * rejected by parser; explicit projection returned all six rows | Unsupported syntax documented; alternative demonstrated |
| ORDER BY ASC/DESC and multiple keys | Queries B-D | Correct ascending, descending, and department/salary ordering | Demonstrated |
| LIMIT / LIMIT 0 | Explicit-column LIMIT 3; LIMIT 0 | Three rows and zero rows respectively | Demonstrated |
| INNER JOIN / LEFT JOIN | Queries F-G | Matching rows joined; Operations emitted with NULL employee | Demonstrated |
| Qualified / ambiguous columns | Queries H-I | Qualified references work; shared id rejected as ambiguous | Demonstrated |
| Aggregates | COUNT(*), COUNT(column), SUM, AVG, MIN, MAX | All six aggregates returned results from the six-row dataset | Demonstrated |
| GROUP BY / HAVING | Queries K-L | Department groups and groups with count > 1 returned | Demonstrated |
| Combined query clauses | Queries M-O | Join/filter/order, grouped/HAVING/order, and top-three queries returned expected rows | Demonstrated |
| EXPLAIN operators | P1-P4 | Output showed Sequential Scan, Filter, Sort, Limit, Projection, Nested Loop Join, and Aggregate | Demonstrated |
| Persistent index scan | P5-P7 | Created index; physical plan explicitly showed Index Scan; equality lookup returned Alan | Demonstrated |
| Validation errors | Negative cases | Unknown and ambiguous columns, invalid LIMIT, SUM(TEXT), and invalid grouping were rejected; follow-up SELECT succeeded | Demonstrated |
| Clean-reopen persistence | Persistence queries | Employee rows and department LEFT JOIN survived fresh shell processes | Demonstrated |
| Regression suite | CMake, CTest, direct test executable | Build succeeded, CTest passed 3/3, direct executable passed 97/97 | Passed |

## Successfully demonstrated

All implemented Phase 8 features in the requested scope were exercised through the actual KernDB CLI. In particular, the persistent equality plan showed Index Scan in the physical tree, and the reopened database retained both tables and all inserted rows.

## Failed demonstration

No documented supported Phase 8 feature failed during this run. The two exact SELECT * examples did fail at parse time; the parser error is captured above and categorized as unsupported syntax below.

## Unsupported features

- SELECT * is unsupported by the current parser. Both requested forms, SELECT * FROM employees and SELECT * FROM employees LIMIT 3, returned syntax errors. Explicit-column SELECT queries demonstrated projection and LIMIT successfully.

## Known limitations

- Grouped ORDER BY is demonstrated on a GROUP BY key, matching the documented limitation that grouped queries can order only by grouping columns.
- EXPLAIN uses deterministic rules; these plans demonstrate operator selection, not cost-based optimization or a measured speedup.
- The demo verifies orderly close and reopen only. It does not establish WAL or crash-recovery support.

No source files were changed during this demonstration, no feature implementation was added, and no commit or push was performed.
