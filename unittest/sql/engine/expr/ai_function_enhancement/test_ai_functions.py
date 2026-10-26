#!/usr/bin/env python3
"""SQL integration regression tests against the school-server kernel build."""
import json
import traceback
import uuid
from pathlib import Path
import pymysql
from mock_ai_service import vector

WORK = Path('/home/PeterPan/2026ob')
OUT = WORK / 'results/ai-functions'
LOG = OUT / 'requests.jsonl'
connection = pymysql.connect(user='root', unix_socket=str(WORK / 'run/seekdb-ai-functions/run/sql.sock'),
    charset='utf8mb4', autocommit=True, read_timeout=180)
cursor = connection.cursor()
cursor.execute('set ob_query_timeout=120000000')
cursor.execute('create database if not exists ai_kernel_regression')
cursor.execute('use ai_kernel_regression')
results = []


def sql(query, args=None):
    cursor.execute(query, args)
    return cursor.fetchall()


def one(query, args=None):
    return sql(query, args)[0][0]


def check(condition, detail='assertion failed'):
    assert condition, detail


def case(name, function):
    try:
        function()
        result = {'name': name, 'passed': True}
    except Exception as error:
        result = {'name': name, 'passed': False, 'error': repr(error), 'traceback': traceback.format_exc()}
    results.append(result)
    print(json.dumps(result, ensure_ascii=False), flush=True)


def fail(query, args=None):
    try:
        sql(query, args)
    except pymysql.MySQLError:
        return
    raise AssertionError('SQL unexpectedly succeeded: ' + query)


def endpoint(name, model_type, provider, route, key='mock-key'):
    for kind, target in [('AI_MODEL', name), ('AI_MODEL_ENDPOINT', name + '_ep')]:
        try: sql('call DBMS_AI_SERVICE.DROP_%s(%%s)' % kind, (target,))
        except pymysql.MySQLError: pass
    sql('call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
        (name, json.dumps({'type': model_type, 'model_name': 'regression-model'})))
    config = {'ai_model_name': name, 'scope': 'all', 'provider': provider,
              'url': 'http://127.0.0.1:8000' + route}
    if key is not None: config['access_key'] = key
    sql('call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)', (name + '_ep', json.dumps(config)))


def route(name, path):
    sql('call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
        (name + '_ep', json.dumps({'url': 'http://127.0.0.1:8000' + path})))


def events():
    return [json.loads(line) for line in LOG.read_text(encoding='utf-8').splitlines()]


def expect_echo(expression, expected, args=None):
    check(one("select ai_complete('reg_complete'," + expression + ')', args) == expected)


endpoint('reg_complete', 'completion', 'openai', '/v1/chat/completions')
endpoint('reg_embed', 'dense_embedding', 'openai', '/v1/embeddings')
endpoint('reg_rerank', 'rerank', 'siliconflow', '/v1/rerank')

case('prompt JSON structure', lambda: check(json.loads(one("select ai_prompt('{0}/{1}',json_array(1,true),json_object('k',2))"))
    == {'args': [[1, True], {'k': 2}], 'template': '{0}/{1}'}))
case('prompt repeated placeholders grow output', lambda: expect_echo("ai_prompt('{0}/{0}/{0}',repeat('z',4000))", '/'.join(['z' * 4000] * 3)))
case('prompt nested JSON and unicode', lambda: expect_echo("ai_prompt('外层{0}|{1}',ai_prompt('内层{0}','你好'),json_array(1,true))", '外层内层你好|[1, true]'))
case('prompt quotes slashes newline NUL', lambda: expect_echo("ai_prompt('{0}/{0}',%s)", 'a"\\\n\0/a"\\\n\0', ('a"\\\n\0',)))
for placeholder in ['{1}', '{999999999999999999999999999999999999}', '{18446744073709551616}']:
    case('prompt rejects index ' + placeholder, lambda placeholder=placeholder: fail(
        "select ai_complete('reg_complete',ai_prompt(%s,'x'))", (placeholder,)))
case('prompt literal nonnumeric braces', lambda: expect_echo("ai_prompt('literal {x} {} {-1}')", 'literal {x} {} {-1}'))
sql('drop table if exists lob_test')
sql('create table lob_test(t longtext, a longtext)')
sql('insert into lob_test values(%s,%s)', ('{0}/{0}', '中文' * 100000))
case('prompt out-row LOB template and parameters', lambda: check(one("select length(ai_complete('reg_complete',ai_prompt(t,a))) from lob_test") == 1200001))
case('complete plain prompt', lambda: expect_echo("'hello'", 'hello'))
case('complete NULL config', lambda: check(one("select ai_complete('reg_complete','hello',null)") == 'hello'))
case('complete JSON config', lambda: check(one("select ai_complete('reg_complete','hello',json_object('temperature',0.2,'max_tokens',16))") == 'hello'))
case('complete config transmitted', lambda: check(events()[-1]['body']['temperature'] == 0.2 and events()[-1]['body']['max_tokens'] == 16))
for config in ['[]', '1', 'null', '"x"', '{bad']:
    case('complete rejects config ' + config, lambda config=config: fail("select ai_complete('reg_complete','x',%s)", (config,)))
for mode in ['empty', 'null', 'missing', 'wrong-type', 'root-array', 'invalid-json', 'empty-body']:
    def completion_fault(mode=mode):
        route('reg_complete', '/fault/' + mode + '/chat/completions')
        fail("select ai_complete('reg_complete','x')")
        check(one('select 42') == 42)
    case('complete rejects response ' + mode, completion_fault)
route('reg_complete', '/v1/chat/completions')
case('embed default dimension', lambda: check(len(json.loads(one("select ai_embed('reg_embed','x')"))) == 768))
case('embed explicit dimension and content', lambda: check(json.loads(one("select ai_embed('reg_embed','你好',7)")) == vector('你好', 7)))
for dimension in ['0', '-1', '1.5', "'2'", 'null', '18446744073709551615']:
    case('embed rejects dimension ' + dimension, lambda dimension=dimension: fail("select ai_embed('reg_embed','x'," + dimension + ')'))
case('embed scalar NULL propagation', lambda: check(sql("select ai_embed('reg_embed',null),ai_embed(null,'x')") == ((None, None),)))
for mode in ['index', 'negative', 'float-index', 'no-index', 'dimension', 'zero-vector', 'bad-element',
             'null-element', 'bool-element', 'not-vector', 'not-finite', 'wrong-type', 'missing', 'count', 'root-array', 'invalid-json']:
    def embedding_fault(mode=mode):
        route('reg_embed', '/fault/' + mode + '/embeddings')
        fail("select ai_embed('reg_embed','x',7)")
        check(one('select 42') == 42)
    case('embed rejects response ' + mode, embedding_fault)
route('reg_embed', '/v1/embeddings')
sql('drop table if exists batch_test')
sql('create table batch_test(id int primary key,content longtext, dim int)')
rows = [(i, None if i % 7 == 0 else 'row-%d' % i, 3 if i % 2 else 5) for i in range(1, 151)]
cursor.executemany('insert into batch_test values(%s,%s,%s)', rows)


def batch_test(where='', limit='', dimensions='7', nested=False):
    before = len(events())
    projection = "ai_embed('reg_embed',content," + dimensions + ')'
    if nested: projection = 'json_length(' + projection + ')'
    actual = sql('select id,' + projection + ' from batch_test ' + where + ' order by id ' + limit)
    expected = rows
    if where: expected = [row for row in rows if row[0] % 3 == 0]
    if limit: expected = expected[4:13]
    check([row[0] for row in actual] == [row[0] for row in expected])
    for (id, value), (_, content, dimension) in zip(actual, expected):
        dimension = dimension if dimensions == 'dim' else int(dimensions)
        target = None if content is None else (dimension if nested else vector(content, dimension))
        check((json.loads(value) if value is not None and not nested else value) == target, 'row mapping: %d' % id)
    requests = events()[before:]
    check(all(None not in request['body']['input'] and len(request['body']['input']) <= 64 for request in requests))
    if not limit:
        check(any(len(request['body']['input']) > 1 for request in requests), 'cross-row requests were not batched')
    (OUT / ('batch-%s%s%s.json' % (dimensions, '-filtered' if where else '', '-nested' if nested else ''))).write_text(json.dumps(
        {'returned_rows': len(actual), 'requests': len(requests), 'request_sizes': [len(x['body']['input']) for x in requests]}, indent=2))


case('embed batch 150 rows unordered response and NULL', batch_test)
case('embed batch per-row dimensions', lambda: batch_test(dimensions='dim'))
case('embed batch filter', lambda: batch_test(where='where mod(id,3)=0'))
case('embed batch filter offset LIMIT', lambda: batch_test(where='where mod(id,3)=0', limit='limit 4,9'))
case('embed nested function table scan', lambda: batch_test(nested=True))
def batch_failure():
    route('reg_embed', '/fault/duplicate/embeddings')
    fail("select id,ai_embed('reg_embed',content,7) from batch_test")
    route('reg_embed', '/v1/embeddings')
    batch_test()
case('embed failed batch and next query recovery', batch_failure)

def rerank(objects=False):
    texts = [('apple' if i % 3 == 0 else 'other') + str(i) for i in range(47)]
    documents = [{'id': 'doc-%d' % i, 'body': text, 'metadata': {'n': i}} for i, text in enumerate(texts)] if objects else texts
    expected = sorted(range(47), key=lambda i: (-int('apple' in texts[i]), i))
    actual = json.loads(one("select ai_rerank('reg_rerank','apple',%s,%s)", (json.dumps(documents), 'body' if objects else None)))
    check(len(actual) == 47)
    if objects:
        check([x['id'] for x in actual] == ['doc-%d' % i for i in expected])
        check([x['metadata']['n'] for x in actual] == expected)
    else:
        check([x['index'] for x in actual] == expected)
case('rerank 47 strings global indices stable ties', rerank)
case('rerank 47 objects original IDs and fields', lambda: rerank(True))
for mode in ['count', 'duplicate', 'index', 'negative', 'float-index', 'missing', 'wrong-type', 'not-finite', 'root-array', 'invalid-json']:
    def rerank_fault(mode=mode):
        route('reg_rerank', '/fault/' + mode + '/rerank')
        fail("select ai_rerank('reg_rerank','x','[\"a\",\"b\"]',null)")
    case('rerank rejects response ' + mode, rerank_fault)
route('reg_rerank', '/v1/rerank')
case('rerank missing text field', lambda: fail("select ai_rerank('reg_rerank','x','[{\"id\":\"a\"}]','body')"))
case('rerank wrong document type', lambda: fail("select ai_rerank('reg_rerank','x','[1,2]',null)"))

def ollama_setup():
    endpoint('reg_ollama_complete', 'completion', 'oLlAmA', '/api/generate', key=None)
    endpoint('reg_ollama_embed', 'dense_embedding', 'OLLAMA', '/api/embed', key='')
case('ollama endpoint no key and mixed case provider', ollama_setup)
case('ollama native complete', lambda: check(one("select ai_complete('reg_ollama_complete','你好')") == '你好'))
case('ollama native embed batch', lambda: check(json.loads(one("select ai_embed('reg_ollama_embed','你好',7)")) == vector('你好', 7)))
def ollama_options():
    one("select ai_complete('reg_ollama_complete','x','{\"temperature\":0.3,\"max_tokens\":12}')")
    request = events()[-1]
    check(request['body']['options'] == {'temperature': 0.3, 'num_predict': 12})
    check(request['body']['stream'] is False and not request['has_authorization'])
case('ollama nonstreaming native generation options', ollama_options)
def alter_provider():
    sql('call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', ('reg_ollama_complete_ep', '{"provider":"Ollama"}'))
    check(one("select provider from oceanbase.DBA_OB_AI_MODEL_ENDPOINTS where endpoint_name='reg_ollama_complete_ep'") == 'Ollama')
    fail('call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', ('reg_ollama_complete_ep', '{"provider":"openai"}'))
case('ollama alter provider and keyed provider validation', alter_provider)
case('ordinary SQL remains correct', lambda: check(sql('select sum(id),count(*),count(content) from batch_test') == ((11325,150,129),)))

def dimensions_fail_locally():
    before = len(events())
    fail("select ai_embed('reg_embed','x',18446744073709551615)")
    fail("select ai_embed('reg_embed','x',2147483648)")
    check(len(events()) == before, 'out-of-range dimension reached the provider')
case('embed overflowing dimensions rejected before HTTP', dimensions_fail_locally)
def all_null_and_empty():
    before = len(events())
    check(all(value is None for _, value in sql("select id,ai_embed('reg_embed',content) from batch_test where content is null")))
    check(sql("select id,ai_embed('reg_embed',content) from batch_test where id < 0") == ())
    check(len(events()) == before)
case('embed all NULL and empty scan make no requests', all_null_and_empty)
def different_models():
    endpoint('reg_embed_other', 'dense_embedding', 'openai', '/v1/embeddings')
    actual = sql("select id,ai_embed(if(mod(id,2)=0,'reg_embed','reg_embed_other'),content,3) from batch_test order by id")
    for (id, value), (_, content, _) in zip(actual, rows):
        check((json.loads(value) if value else None) == (vector(content,3) if content else None), 'mixed model row %d' % id)
case('embed batch row-dependent models', different_models)
def ollama_batch():
    before = len(events())
    actual = sql("select id,ai_embed('reg_ollama_embed',content,3) from batch_test order by id")
    for (id, value), (_, content, _) in zip(actual, rows):
        check((json.loads(value) if value else None) == (vector(content,3) if content else None), 'ollama row %d' % id)
    check(any(len(event['body']['input']) > 1 for event in events()[before:]))
case('ollama multi-row native embed', ollama_batch)
for mode in ['wrong-type', 'missing', 'count', 'bad-element', 'dimension']:
    def ollama_fault(mode=mode):
        route('reg_ollama_embed', '/fault/' + mode + '/api/embed')
        fail("select ai_embed('reg_ollama_embed','x',7)")
    case('ollama rejects embedding response ' + mode, ollama_fault)
route('reg_ollama_embed', '/api/embed')
def successful_retry():
    route('reg_complete', '/fault/retry-once/' + uuid.uuid4().hex + '/chat/completions')
    before = len(events())
    check(one("select ai_complete('reg_complete','recovered')") == 'recovered')
    check(len(events()) - before == 2)
    route('reg_complete', '/v1/chat/completions')
case('HTTP retry discards the previous error body', successful_retry)
def permanent_http_error():
    route('reg_embed', '/fault/http-error/embeddings')
    fail("select ai_embed('reg_embed','x',7)")
    route('reg_embed', '/v1/embeddings')
    check(json.loads(one("select ai_embed('reg_embed','after-error',7)")) == vector('after-error', 7))
case('HTTP permanent failure and next-query recovery', permanent_http_error)

def json_length_documents():
    sql('drop table if exists json_length_test')
    sql('create table json_length_test(id int primary key, doc longtext, path varchar(40))')
    documents = [(1,'[1,2,3]','$'),(2,'{"a":[1,2],"b":4}','$.a'),
                 (3,None,'$'),(4,'[1]',None),(5,'{}','$.missing'),
                 (6,'true','$'),(7,'"hello"','$')]
    cursor.executemany('insert into json_length_test values(%s,%s,%s)', documents)
    expected = [(id,one('select json_length(%s,%s)',(doc,path))) for id,doc,path in documents]
    check(sql('select id,json_length(doc,path) from json_length_test order by id') == tuple(expected))
    check(sql('select id,json_length(doc) from json_length_test where id < 3 order by id') == ((1,3),(2,2)))
case('ordinary JSON_LENGTH batch matches scalar paths and NULL', json_length_documents)
case('ordinary JSON_LENGTH invalid JSON still errors', lambda: fail("select json_length('invalid')"))
case('ordinary JSON_LENGTH invalid path still errors', lambda: fail("select json_length('[1]','invalid')"))

report = {'cases': len(results), 'passed': sum(x['passed'] for x in results), 'results': results}
(OUT / 'integration-summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'cases': report['cases'], 'passed': report['passed']}, ensure_ascii=False), flush=True)
raise SystemExit(int(report['cases'] != report['passed']))
