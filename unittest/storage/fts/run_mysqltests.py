#!/usr/bin/env python3
"""Run the repository's existing FTS mysqltest cases on a supplied instance."""
import argparse
import json
import os
from pathlib import Path
import shutil
import secrets
import subprocess
import pymysql

parser = argparse.ArgumentParser(__doc__)
parser.add_argument('--socket', required=True)
parser.add_argument('--port', type=int, help='Actual TCP port (required with mysql_port_mode=random)')
parser.add_argument('--provision-admin', action='store_true', help='Create/remove cn_word admin fixture if absent')
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('tests', nargs='*')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
args.output.mkdir(parents=True, exist_ok=True)
output = args.output.resolve()
defaults = output / 'client.cnf'
with os.fdopen(os.open(str(defaults), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), 'w') as stream:
    stream.write('[client]\nuser=root\npassword=\nsocket=' + args.socket + '\n')
suite = repo / 'tools/deploy/mysql_test/test_suite/fts_index'
tests = args.tests or ['tokenize_function', 'parser_properties', 'cn_word', 'simple_query',
                       'basic_dml', 'create_fts_index_afterward', 'create_table_with_fts_index']
results = []
con = pymysql.connect(user='root', unix_socket=args.socket)
created_admin = False
admin_password = os.environ.get('OBMYSQL_PWD', '')
with con.cursor() as cur:
    cur.execute('SELECT @@port')
    port = args.port or cur.fetchone()[0]
    if args.provision_admin and 'cn_word' in tests:
        cur.execute("SELECT user_name FROM oceanbase.__all_user WHERE user_name='admin'")
        if not cur.fetchone():
            admin_password = secrets.token_hex(16)
            cur.execute('CREATE USER admin IDENTIFIED BY %s', (admin_password,))
            cur.execute('GRANT ALL PRIVILEGES ON *.* TO admin')
            created_admin = True
con.close()
env = dict(os.environ, OBMYSQL_MS0='127.0.0.1', OBMYSQL_PORT=str(port),
           MYSQL_TMP_DIR=str(output), OBMYSQL_PWD=admin_password)
try:
    for name in tests:
        if Path(name).name != name:
            raise ValueError('Invalid test name')
        expected = output / (name + '.result')
        shutil.copyfile(suite / 'r/mysql' / (name + '.result'), expected)
        command = [str(repo / 'deps/3rd/u01/obclient/bin/mysqltest'), '--defaults-file=' + str(defaults),
                   '--database=test', '--test-file=' + str(suite / 't' / (name + '.test')),
                   '--result-file=' + str(expected), '--logdir=' + str(output), '--tmpdir=' + str(output),
                   '--max-connect-retries=1']
        try:
            run = subprocess.run(command, cwd=str(repo / 'tools/deploy/mysql_test'), stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, universal_newlines=True, timeout=300, env=env)
            result = {'test': name, 'exit_code': run.returncode, 'output': run.stdout}
        except subprocess.TimeoutExpired as error:
            text = error.stdout or ''
            if isinstance(text, bytes):
                text = text.decode('utf-8', errors='replace')
            result = {'test': name, 'exit_code': 124, 'output': text + '\nTimed out after 300 seconds\n'}
        (output / (name + '.output.txt')).write_text(result['output'])
        print(json.dumps(result, ensure_ascii=False), flush=True)
        results.append(result)
finally:
    defaults.unlink()
    (output / 'summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    if created_admin:
        con = pymysql.connect(user='root', unix_socket=args.socket, autocommit=True)
        with con.cursor() as cur:
            cur.execute('DROP USER admin')
        con.close()
raise SystemExit(int(any(r['exit_code'] for r in results)))
