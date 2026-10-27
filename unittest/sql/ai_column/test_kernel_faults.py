#!/usr/bin/env python3
"""Additional kernel integration coverage, including a process crash in HTTP."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import pymysql

spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run_kernel_tests.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
events = []
lock = threading.Lock()
entered = {name: threading.Event() for name in ['slow-crash', 'slow-delete']}
release = {name: threading.Event() for name in entered}
conditional_fail = True

class Mock(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        user = ''.join(m.get('content', '') for m in body.get('messages', []) if m.get('role') == 'user')
        system = ''.join(m.get('content', '') for m in body.get('messages', []) if m.get('role') == 'system')
        with lock:
            events.append({'model': body.get('model'), 'input': user, 'system': system, 'path': self.path})
            occurrence = sum(e['input'] == user for e in events)
        if user in entered and occurrence == 1:
            entered[user].set()
            release[user].wait(60)
        if user.startswith('bench-'):
            time.sleep(0.05)
        failed = self.path == '/conditional' and conditional_fail
        if failed:
            response = {'error': {'message': 'injected terminal failure'}}
        else:
            content = json.dumps({'input': user}, ensure_ascii=False) if 'valid JSON value' in system else 'mock:' + user
            if 'base-10 integer' in system:
                content = 'not-an-integer' if user.startswith('bad-int:') else '7'
            if 'exactly one number' in system:
                content = '1.25e40' if user == 'large-double' else '1000'
            if user.startswith('bad-json:'):
                content = '{'
            response = {'choices': [{'message': {'role': 'assistant', 'content': content}}]}
        raw = json.dumps(response).encode()
        try:
            self.send_response(400 if failed else 200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Mock)
threading.Thread(target=server.serve_forever, daemon=True).start()
database = 'ai_column_extra_' + uuid.uuid4().hex[:10]
models = [database + '_good', database + '_bad']
connection = None
reports = []

def connect():
    return pymysql.connect(user='root', unix_socket=str(runner.socket), autocommit=True)

def sql(statement, args=None):
    with connection.cursor() as cursor:
        cursor.execute(statement, args)
        return cursor.fetchall()

def wait(statement, expected=True, timeout=25):
    deadline = time.monotonic() + timeout
    value = None
    while time.monotonic() < deadline:
        result = sql(statement)
        value = result[0][0] if result else None
        if value == expected:
            return
        time.sleep(0.1)
    raise AssertionError((statement, value, expected))

def tasks(table):
    return "SELECT COUNT(*)=0 FROM oceanbase.__all_ai_column_task WHERE database_name='%s' AND table_name='%s'" % (database, table)

def expect_error(statement, code):
    try:
        sql(statement)
    except pymysql.MySQLError as error:
        assert error.args[0] == code, error
    else:
        raise AssertionError('Expected error %d: %s' % (code, statement))

def report(name, **values):
    item = dict(scenario=name, passed=True, **values)
    reports.append(item)
    print(json.dumps(item, ensure_ascii=False), flush=True)

try:
    connection = connect()
    sql('CREATE DATABASE ' + database)
    sql('USE ' + database)
    for index, model in enumerate(models):
        sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)', (model, json.dumps({'type': 'completion', 'model_name': 'integration-model'})))
        config = {'ai_model_name': model, 'url': 'http://127.0.0.1:%d/%s' %
                  (server.server_port, 'complete' if index == 0 else 'conditional'),
                  'access_key': 'integration-key', 'provider': 'openai', 'request_model_name': model}
        sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)', (model + '_ep', json.dumps(config)))
    sql("CREATE TABLE mixed(id BIGINT PRIMARY KEY, body VARCHAR(200), "
        "good JSON AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0), "
        "bad VARCHAR(500) AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0))" % tuple(models))
    before = len(events)
    sql('START TRANSACTION')
    sql("INSERT INTO mixed(id,body) VALUES(9,'must-rollback')")
    time.sleep(0.3)
    assert len(events) == before
    sql('ROLLBACK')
    time.sleep(0.3)
    assert len(events) == before
    report('rollback_emits_no_model_request', calls=0)

    payload = "汉字、引号'和反斜杠\\"
    sql('INSERT INTO mixed(id,body) VALUES(1,%s)', (payload,))
    wait(tasks('mixed'))
    good, bad = sql('SELECT good,bad FROM mixed WHERE id=1')[0]
    assert json.loads(good)['input'] == payload and bad is None
    sql('SET SESSION ai_column_strict_read=ON')
    assert sql('SELECT good IS NOT NULL FROM mixed')[0][0] == 1
    expect_error('SELECT bad FROM mixed WHERE id=1', 11120)
    sql('SET SESSION ai_column_strict_read=OFF')
    report('mixed_success_and_failure_publish', input_preserved=True)

    good_calls = sum(e['model'] == models[0] for e in events)
    conditional_fail = False
    sql("ALTER TABLE mixed RETRY AI COLUMN bad WHERE status='FAILED' AND id=1")
    wait('SELECT bad IS NOT NULL FROM mixed WHERE id=1')
    assert sum(e['model'] == models[0] for e in events) == good_calls
    report('conditional_retry_does_not_recompute_other_column')

    sql("CREATE TABLE numbers(id BIGINT PRIMARY KEY, body VARCHAR(200), "
        "score BIGINT AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0))" % models[0])
    sql("INSERT INTO numbers(id,body) VALUES(1,'bad-int:invalid')")
    wait(tasks('numbers'))
    assert sql('SELECT score IS NULL FROM numbers')[0][0] == 1
    sql('SET SESSION ai_column_strict_read=ON')
    expect_error('SELECT score FROM numbers', 11120)
    sql('SET SESSION ai_column_strict_read=OFF')
    report('invalid_integer_is_failed_and_null')

    sql("CREATE TABLE invalid_types(id BIGINT PRIMARY KEY, body VARCHAR(200), "
        "amount DECIMAL(3,1) AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0), "
        "short_text VARCHAR(3) AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0), "
        "broken JSON AI COLUMN(AI_COMPLETE('%s',concat('bad-json:',body))) WITH(max_retries=0))" %
        (models[0], models[0], models[0]))
    sql("INSERT INTO invalid_types(id,body) VALUES(1,'x')")
    wait(tasks('invalid_types'))
    assert sql('SELECT amount IS NULL AND short_text IS NULL AND broken IS NULL FROM invalid_types')[0][0] == 1
    report('decimal_overflow_string_overflow_and_malformed_json_fail')

    sql("CREATE TABLE floats(id BIGINT PRIMARY KEY, body VARCHAR(200), "
        "result DOUBLE AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0))" % models[0])
    sql("INSERT INTO floats(id,body) VALUES(1,'large-double')")
    wait('SELECT result>1e39 FROM floats WHERE id=1')
    report('double_preserves_floating_point_range')

    sql('CREATE TABLE existing(id BIGINT PRIMARY KEY, body VARCHAR(200))')
    sql("INSERT INTO existing VALUES(1,'old-one'),(2,'old-two')")
    sql("ALTER TABLE existing ADD COLUMN tags JSON AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0)" % models[0])
    wait('SELECT COUNT(*)=2 FROM existing WHERE tags IS NOT NULL')
    sql("ALTER TABLE existing MODIFY COLUMN tags JSON AI COLUMN(AI_COMPLETE('%s',concat('new:',body))) WITH(max_retries=0)" % models[0])
    wait("SELECT COUNT(*)=2 FROM existing WHERE JSON_UNQUOTE(JSON_EXTRACT(tags,'$.input'))=concat('new:',body)")
    report('add_and_modify_backfill_existing_rows')
    expect_error("INSERT INTO existing VALUES(3,'forbidden','{}')", 1235)
    expect_error("UPDATE existing SET tags='{}' WHERE id=1", 1235)
    expect_error("INSERT INTO existing(id,body,tags) SELECT 3,'forbidden','{}'", 1235)
    report('implicit_insert_cannot_assign_ai_result')

    sql("CREATE TABLE recovery(id BIGINT PRIMARY KEY, body VARCHAR(200), "
        "result VARCHAR(500) AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=1))" % models[0])
    sql("INSERT INTO recovery(id,body) VALUES(1,'slow-crash')")
    assert entered['slow-crash'].wait(10)
    old_request = sql("SELECT request_id FROM oceanbase.__all_ai_column_task WHERE database_name=%s AND table_name='recovery'", (database,))[0][0]
    pid = int((runner.base / 'process.pid').read_text())
    command_line = Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0')
    assert ('--base-dir=' + str(runner.base)).encode() in command_line
    os.kill(pid, signal.SIGKILL)
    release['slow-crash'].set()
    connection.close()
    connection = None
    for _ in range(100):
        if not Path('/proc/%d/exe' % pid).exists():
            break
        time.sleep(0.1)
    runner.start()
    connection = connect()
    sql('USE ' + database)
    restored = sql("SELECT request_id FROM oceanbase.__all_ai_column_task WHERE database_name=%s AND table_name='recovery'", (database,))[0][0]
    assert restored == old_request
    # Advance only this test task's lease, avoiding a one-minute test sleep.
    sql("UPDATE oceanbase.__all_ai_column_task SET lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND) "
        "WHERE database_name=%s AND table_name='recovery' AND task_state=1", (database,))
    wait("SELECT result='mock:slow-crash' FROM recovery WHERE id=1")
    wait(tasks('recovery'))
    calls = sum(e['input'] == 'slow-crash' for e in events)
    assert calls == 2
    report('crash_recovers_claimed_task_and_lost_response', actual_upstream_calls=calls)

    sql("INSERT INTO recovery(id,body) VALUES(2,'slow-delete')")
    assert entered['slow-delete'].wait(10)
    sql('DELETE FROM recovery WHERE id=2')
    release['slow-delete'].set()
    time.sleep(1)
    assert sql('SELECT COUNT(*) FROM recovery WHERE id=2')[0][0] == 0
    wait(tasks('recovery'))
    report('delete_during_model_call_cannot_restore_row')

    sql("CREATE TABLE throughput(id BIGINT PRIMARY KEY, body VARCHAR(200), "
        "result VARCHAR(500) AI COLUMN(AI_COMPLETE('%s',body)) WITH(max_retries=0))" % models[0])
    started = time.monotonic()
    sql('INSERT INTO throughput(id,body) VALUES ' + ','.join("(%d,'bench-%d')" % (i, i) for i in range(32)))
    committed = time.monotonic()
    wait('SELECT COUNT(*)=32 FROM throughput WHERE result IS NOT NULL')
    completed = time.monotonic()
    wait(tasks('throughput'))
    calls = sum(e['input'].startswith('bench-') for e in events)
    assert calls == 32
    report('batch_completion_measurement', rows=32, mock_delay_ms=50, actual_upstream_calls=calls,
           business_write_ms=round((committed-started)*1000, 2),
           commit_to_all_visible_ms=round((completed-committed)*1000, 2),
           completed_rows_per_second=round(32/(completed-committed), 2))

    sql('DROP DATABASE ' + database)
    for model in reversed(models):
        sql('CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (model + '_ep',))
        sql('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (model,))
finally:
    for event in release.values():
        event.set()
    if connection is not None and connection.open:
        connection.close()
    server.shutdown()
    server.server_close()
    (runner.output / 'fault-integration.json').write_text(json.dumps(reports, ensure_ascii=False, indent=2) + '\n')
    (runner.output / 'fault-http-events.json').write_text(json.dumps(events, ensure_ascii=False, indent=2) + '\n')
