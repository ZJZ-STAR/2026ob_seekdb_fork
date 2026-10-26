#!/usr/bin/env python3
"""Run AI COLUMN suites against an isolated source-built seekdb instance."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import resource
import signal
import shlex
import shutil
import subprocess
import time

repo = Path(__file__).resolve().parents[3]
workspace = repo.parents[1]
output = workspace / 'results/ai-column'
base = workspace / 'run/seekdb-ai-column'
data = workspace / 'data/seekdb-ai-column'
binary = repo / 'build_release/src/observer/seekdb'
socket = base / 'run/sql.sock'
output.mkdir(parents=True, exist_ok=True)

def start(restart=False):
    import pymysql
    base.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
    pid_file = base / 'process.pid'
    running = False
    if pid_file.exists():
        pid = int(pid_file.read_text())
        try:
            actual = os.readlink('/proc/%d/exe' % pid)
            if actual.endswith(' (deleted)'):
                actual = actual[:-10]
            command_line = Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0')
            running = actual == str(binary) and ('--base-dir=' + str(base)).encode() in command_line
            if running and restart:
                os.kill(pid, signal.SIGTERM)
                for _ in range(120):
                    if not Path('/proc/%d' % pid).exists():
                        break
                    time.sleep(0.5)
                if Path('/proc/%d' % pid).exists():
                    raise RuntimeError('AI column instance did not stop after SIGTERM')
                running = False
        except OSError:
            pass
    if not running:
        # A SIGKILL may remove /proc/PID/exe before all threads release the
        # server's PID lock. Wait for that lock without deleting its file.
        native_pid = base / 'run/seekdb.pid'
        if native_pid.exists():
            with native_pid.open('r+') as lock_file:
                deadline = time.monotonic() + 30
                while True:
                    try:
                        fcntl.lockf(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        fcntl.lockf(lock_file, fcntl.LOCK_UN)
                        break
                    except BlockingIOError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError('AI column instance still holds its PID lock')
                        time.sleep(0.1)
        resource.setrlimit(resource.RLIMIT_NOFILE, (65536, 65536))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        command = [str(binary), '--nodaemon', '--base-dir=' + str(base), '--data-dir=' + str(data),
                   '--redo-dir=' + str(data / 'redo'), '--parameter=mysql_port_mode=random',
                   '--parameter=rpc_port=12903', '--parameter=cpu_count=4', '--parameter=memory_budget=4G',
                   '--parameter=datafile_maxsize=10G', '--parameter=log_disk_size=2G']
        with (output / 'startup.log').open('ab') as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
        pid_file.write_text(str(process.pid) + '\n')
    deadline = time.monotonic() + 120
    last = None
    while time.monotonic() < deadline:
        try:
            connection = pymysql.connect(user='root', password='', unix_socket=str(socket), autocommit=True)
            with connection.cursor() as cursor:
                cursor.execute('CREATE DATABASE IF NOT EXISTS test')
                cursor.execute('SELECT VERSION(), @@ai_column_strict_read')
                print(json.dumps({'started': True, 'version_and_strict': cursor.fetchone()}, ensure_ascii=False), flush=True)
                try:
                    cursor.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('__ai_column_readiness_%d')" % os.getpid())
                except pymysql.MySQLError as error:
                    if error.args[0] != 11118:
                        raise
            connection.close()
            return
        except Exception as error:
            last = error
            time.sleep(0.5)
    raise RuntimeError('Instance did not start: ' + str(last))

def mysqltests(selected, suite_name='ai_column'):
    dest = output / 'mysqltest'
    dest.mkdir(exist_ok=True)
    defaults = dest / 'client.cnf'
    with os.fdopen(os.open(str(defaults), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), 'w') as f:
        f.write('[client]\nuser=root\npassword=\nsocket=' + str(socket) + '\n')
    suite = repo / 'tools/deploy/mysql_test/test_suite' / suite_name
    results = []
    try:
        for name in selected:
            expected = dest / (name + '.result')
            shutil.copyfile(suite / 'r/mysql' / (name + '.result'), expected)
            command = [str(repo / 'deps/3rd/u01/obclient/bin/mysqltest'), '--defaults-file=' + str(defaults),
                       '--database=test', '--test-file=' + str(suite / 't' / (name + '.test')),
                       '--result-file=' + str(expected), '--logdir=' + str(dest), '--tmpdir=' + str(dest),
                       '--max-connect-retries=1']
            cwd = repo / ('tools/deploy' if suite_name == 'ai_column' else 'tools/deploy/mysql_test')
            run = subprocess.run(command, cwd=str(cwd), stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, universal_newlines=True)
            (dest / (name + '.output.txt')).write_text(run.stdout)
            result = {'test': name, 'exit_code': run.returncode, 'output': run.stdout}
            print(json.dumps(result, ensure_ascii=False), flush=True)
            results.append(result)
    finally:
        defaults.unlink()
    summary = 'summary.json' if suite_name == 'ai_column' else suite_name + '-summary.json'
    (dest / summary).write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    return any(r['exit_code'] for r in results)

def unit_tests():
    build = repo / 'build_release'
    dest = output / 'cpp'
    dest.mkdir(exist_ok=True)
    entries = json.loads((build / 'compile_commands.json').read_text())
    entry = next(e for e in entries if 'unity_ob_sql_ob_sql_resolver_ddl_0' in e['file'])
    template = shlex.split(entry['command'])
    objects = []
    for name in ['unittest/share/schema/test_ai_column_schema.cpp', 'unittest/sql/parser/test_ai_column_parser.cpp',
                 'unittest/sql/resolver/test_ai_column_resolver.cpp']:
        source = repo / name
        target = dest / (source.name + '.o')
        command = template.copy()
        command[command.index('-o') + 1] = str(target)
        command[command.index('-c') + 1] = str(source)
        command += ['-O0', '-g0']
        subprocess.run(command, cwd=entry['directory'], check=True)
        objects.append(str(target))
    directory = build / 'src/observer'
    command = shlex.split((directory / 'CMakeFiles/seekdb.dir/link.txt').read_text())
    command = [x for x in command if not x.startswith('CMakeFiles/ob_main.dir/')]
    command[command.index('-o') + 1] = str(dest / 'ai-column-tests')
    command += objects + ['-lgtest_main', '-lgtest']
    subprocess.run(command, cwd=str(directory), check=True)
    run = subprocess.run([str(dest / 'ai-column-tests'), '--gtest_output=xml:' + str(dest / 'results.xml')],
                         cwd=str(dest), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    (dest / 'output.txt').write_text(run.stdout)
    print(run.stdout, flush=True)
    return run.returncode

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--start', action='store_true')
    parser.add_argument('--restart', action='store_true')
    parser.add_argument('--unit', action='store_true')
    parser.add_argument('--suite', nargs='*')
    parser.add_argument('--regression', action='store_true')
    args = parser.parse_args()
    if args.start or args.restart:
        start(args.restart)
    failed = unit_tests() if args.unit else False
    if args.suite is not None:
        failed |= mysqltests(args.suite or ['ai_column_ddl', 'ai_column_t1_dml', 'ai_column_worker',
                                          'ai_column_retry_strict', 'ai_column_vector_index'])
    if args.regression:
        failed |= mysqltests(['ai_function_enhancement', 'ai_prompt'], 'ai_function')
    raise SystemExit(int(bool(failed)))
