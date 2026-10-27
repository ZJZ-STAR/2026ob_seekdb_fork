# AI COLUMN kernel tests

The implementation extends the competition CMake branch. It uses native SQL
resolution, Schema persistence, transactional DML hooks and an in-process worker.

Run on the configured school-server toolchain:

```bash
source /home/PeterPan/2026ob/env/activate.sh
cd /home/PeterPan/2026ob/src/seekdb-ai-column
export SEEKDB_AI_COLUMN_INSTANCE=seekdb-ai-column-integration
nice -n 10 bash build.sh release --make -j24 seekdb
python3 unittest/sql/ai_column/run_kernel_tests.py --restart --unit --suite --regression
python3 unittest/sql/ai_column/test_kernel_faults.py
python3 unittest/sql/ai_column/run_file_sql_integration.py
```

`run_kernel_tests.py` derives the source and workspace paths from its location.
It starts an isolated instance using `run/seekdb-ai-column`,
`data/seekdb-ai-column`, RPC port 12903, four CPUs and a 4 GB memory budget.
It verifies the executable and base directory before stopping an instance.
The SQL socket is `run/seekdb-ai-column/run/sql.sock` under the workspace.
`SEEKDB_AI_COLUMN_INSTANCE` changes both run and data directory names. The combined
kernel validation uses `seekdb-ai-column-integration`, initialized with both
features' system variables, while retaining the earlier standalone test data.

The unit runner compiles the parser, resolver and Schema tests with the existing
`compile_commands.json` toolchain and links the configured server libraries.
The five mysqltest suites use the repository's unmodified AI COLUMN tests and
their deterministic Mock services on ports 19120–19124. `--regression` also runs
the AI function enhancement and AI prompt suites against the isolated instance;
their shared Mock service must be listening on port 8000.

`test_kernel_faults.py` owns a temporary HTTP Mock and uniquely named SQL models
and database. It checks rollback without HTTP calls, mixed terminal publication,
typed failures, targeted retry, ADD/MODIFY backfill, direct assignment protection,
task recovery after SIGKILL and response loss, deletion during a model call and
32-row completion throughput. The recovery test advances only its own task's
persisted lease after restart. It records both actual upstream calls, including
the call whose response was lost. The benchmark records business write latency
and commit-to-visible completion latency separately.

Results are written to `results/ai-column` under the workspace. The scripts print
JSON records and fail with a nonzero exit code on an assertion or failed suite.

`run_file_sql_integration.py` runs the existing File SQL parser and reader tests,
official mysqltest and SQL regression scripts against this same combined kernel.
It redirects their source, socket, fixture and result paths to the isolated
AI COLUMN workspace while retaining their assertions. File SQL results are saved
under `results/ai-column/file-sql`.

## Implementation entry points

| Behavior | Source |
| --- | --- |
| Syntax | `src/sql/parser/sql_parser_mysql_mode.y` |
| Definition, dependencies and typed prompt | `src/sql/resolver/ddl/ob_ai_column_ddl.h` |
| Schema ownership and serialization | `src/share/schema/ob_column_schema.{h,cpp}` |
| System table and metadata view | `src/share/inner_table/ob_inner_table_schema_def.py` |
| Transactional invalidation and input snapshots | `src/sql/engine/dml/ob_ai_column_dml.h` |
| Leases, retries, validation and grouped publication | `src/observer/ai_service/ob_ai_column_scheduler.h` |
| Conditional retry and SHOW | `src/sql/resolver/ob_ai_column_command.h` |
| Runtime strict reads | `src/sql/ob_ai_column_strict_read.h` |

Each AI column owns hidden state, request and definition-version columns.
An affected row gets one fresh native UUID shared by its triggered AI columns.
The business transaction writes NULL/PENDING and inserts tasks with literal input
snapshots. Uncommitted tasks remain invisible to workers. Workers claim persistent
tasks with owner tokens and expiring leases, call the existing AI functions, then
publish all terminal results of a row/request in one transaction. The publication
predicate checks the request UUID, column definition and Schema version. Native
UPDATE maintains ordinary and vector indexes in that same transaction.
