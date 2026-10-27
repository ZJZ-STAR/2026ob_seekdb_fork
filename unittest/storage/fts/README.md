# Full-text performance regressions

Run against a source-built, dedicated test instance. The SQL scripts create test
databases prefixed `fts_perf_regression_`; the large benchmark additionally
recreates `fts_large_bench` using the repository's unchanged benchmark scripts.

## C++ character and lifecycle checks

After `bash build.sh release --make -j8 seekdb`:

```bash
python3 unittest/storage/fts/run_cpp_tests.py
```

The tests compare character classification against the old repeated-decode
implementation for all 1,112,064 Unicode scalar values. They also cover malformed
and truncated input, unsupported collations, and an uninitialized dictionary hub.
The build must include the repository's GoogleTest dependency.

## Differential SQL checks

Requires Python 3 with PyMySQL and a Unix socket. First run the complete official
FTS benchmark so `fts_large_bench.docs` exists. On the unmodified binary:

```bash
python3 unittest/storage/fts/differential_regression.py \
  --socket /path/to/sql.sock --record --golden /tmp/fts-golden.json \
  --output /tmp/fts-before.json
```

Restart the same instance with the optimized binary and run:

```bash
python3 unittest/storage/fts/differential_regression.py \
  --socket /path/to/sql.sock --golden /tmp/fts-golden.json \
  --output /tmp/fts-after.json
python3 unittest/storage/fts/plan_regression.py \
  --socket /path/to/sql.sock --optimized --output /tmp/fts-plans.json
```

The 1,760 observations include exact TOKENIZE output (including frequencies and
document lengths), both IK modes, BENG/space/ngram compatibility, 1,000-character
segment boundaries, invalid input, result IDs, rounded relevance scores,
numeric thresholds, aggregation/HAVING/window functions, implicit/explicit score
ordering, LIMIT/OFFSET, transactional DML and full benchmark ID sets. Eight
concurrent connections run first, exercising cold IK snapshot publication after
restart. Expected SQL errors are compared by error code. Scores are rounded to
10 decimal places; JSON token arrays and SQL rows retain their original order.
After each DML stage the test runs `ANALYZE TABLE`. Natural-language queries run
twice before recording the third result, allowing the existing range-estimate
based document-frequency statistics to settle on both binaries. The comparison
still checks every recorded ID and score; it does not discard differing rows.

## Existing mysqltest suites and fixed score regression

```bash
python3 unittest/storage/fts/run_mysqltests.py \
  --socket /path/to/sql.sock --port ACTUAL_TCP_PORT \
  --output /tmp/fts-mysqltest tokenize_function parser_properties simple_query score_elision
```

Use the actual listener port when `mysql_port_mode=random`; `@@port` still reports
the configured port. The runner only supplies connection variables and compares
against checked-in result files. `cn_word` additionally expects the suite's
`admin` account and `OBMYSQL_PWD` environment variable. On a dedicated test
instance, `--provision-admin` creates that fixture if absent and removes it after
the run.

`score_elision.test` covers membership counts and IDs, score projection and
ordering, numeric thresholds, Boolean mode, prepared parameter changes and DML.
Its expected result was recorded from the unmodified competition binary.

The plan tests deliberately retain BM25 for implicit relevance ordering and
for any consumer of a numeric/Boolean MATCH value outside the WHERE predicate.
The planner enables membership scans only for scalar `COUNT(*)` with a sole
natural-language MATCH predicate and no other row/score consumers.

`membership_regression.py --socket /path/to/sql.sock --output /tmp/membership.json`
provides a separate oracle for signed/unsigned/composite keys, heap and partitioned
tables, index-back output, DML, correlated rescans and over 256 unique query terms.
