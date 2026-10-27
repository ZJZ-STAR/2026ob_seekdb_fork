#!/usr/bin/env python3
"""Run existing File SQL tests on the combined AI COLUMN kernel instance."""
import json
import os
from pathlib import Path
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
workspace = repo.parents[1]
output = workspace / 'results/ai-column/file-sql'
output.mkdir(parents=True, exist_ok=True)
tests = ['run_parser_tests.py', 'run_reader_tests.py', 'run_mysqltest.py',
         'test_file_sql.py', 'test_native_edges.py']
results = []
instance_name = os.environ.get('SEEKDB_AI_COLUMN_INSTANCE', 'seekdb-ai-column')
if not instance_name or Path(instance_name).name != instance_name or instance_name in ('.', '..'):
    raise ValueError('Invalid SEEKDB_AI_COLUMN_INSTANCE')
for name in tests:
    original = repo / 'unittest/sql/engine/basic/file_sql' / name
    source = original.read_text()
    source = source.replace('/home/PeterPan/2026ob/src/seekdb', str(repo))
    source = source.replace("w/'src/seekdb", "w/'src/seekdb-ai-column")
    source = source.replace("'results/file-sql", "'results/ai-column/file-sql")
    source = source.replace('/home/PeterPan/2026ob/results/file-sql',
                            '/home/PeterPan/2026ob/results/ai-column/file-sql')
    source = source.replace("'run/seekdb-file-sql", "'run/" + instance_name)
    source = source.replace("'tests/file-sql", "'tests/ai-column-file-sql")
    source = source.replace('/home/PeterPan/2026ob/tests/file-sql',
                            '/home/PeterPan/2026ob/tests/ai-column-file-sql')
    generated = output / name
    generated.write_text("__file__ = " + repr(str(original)) + "\n" + source)
    result = subprocess.run([sys.executable, str(generated)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True)
    (output / (name + '.output.txt')).write_text(result.stdout)
    results.append({'test': name, 'exit_code': result.returncode})
    print(json.dumps(results[-1]), flush=True)
    if result.returncode:
        print(result.stdout[-6000:], flush=True)
(output / 'integration-summary.json').write_text(json.dumps(results, indent=2) + '\n')
raise SystemExit(int(any(result['exit_code'] for result in results)))
