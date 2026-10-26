#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import subprocess

workspace = Path('/home/PeterPan/2026ob')
repo = workspace / 'src/seekdb'
output = workspace / 'results/ai-functions/mysqltest'
output.mkdir(parents=True, exist_ok=True)
credentials = {'password': '', 'unix_socket': str(workspace / 'run/seekdb-ai-functions/run/sql.sock')}
defaults = output / 'client.cnf'
with os.fdopen(os.open(str(defaults), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), 'w') as f:
    f.write('[client]\nuser=root\npassword=' + credentials['password'] + '\nsocket=' + credentials['unix_socket'] + '\n')
tests = ['ai_function_enhancement', 'ai_prompt']
results = []
for test in tests:
    expected = output / (test + '.result')
    suite = repo / 'tools/deploy/mysql_test/test_suite/ai_function'
    shutil.copyfile(suite / 'r/mysql' / (test + '.result'), expected)
    command = [str(repo / 'deps/3rd/u01/obclient/bin/mysqltest'), '--defaults-file=' + str(defaults),
               '--database=test', '--test-file=' + str(suite / 't' / (test + '.test')),
               '--result-file=' + str(expected), '--logdir=' + str(output), '--tmpdir=' + str(output),
               '--max-connect-retries=1']
    run = subprocess.run(command, cwd=str(repo / 'tools/deploy/mysql_test'),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    (output / (test + '.output.txt')).write_text(run.stdout)
    result = {'test': test, 'exit_code': run.returncode, 'output': run.stdout}
    results.append(result)
    print(json.dumps(result, ensure_ascii=False), flush=True)
(output / 'summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
defaults.unlink()
raise SystemExit(int(any(x['exit_code'] for x in results)))
