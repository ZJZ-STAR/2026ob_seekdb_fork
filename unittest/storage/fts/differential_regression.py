#!/usr/bin/env python3
"""Record an unmodified server's results, then compare an optimized server.

Uses a dedicated fts_perf_regression database, not the scoring workload.
Requires PyMySQL. SQL errors are compared by code; TOKENIZE objects include
frequencies/doc_len, MATCH comparisons include IDs and relevance scores.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import json
import os
from pathlib import Path
import random
import threading

import pymysql


def canonical(value):
    if isinstance(value, Decimal):
        return {'decimal': str(value)}
    if isinstance(value, str):
        try:
            return canonical(json.loads(value))
        except (ValueError, TypeError):
            return value
    if isinstance(value, dict):
        return {k: canonical(v) for k, v in sorted(value.items())}
    if isinstance(value, (tuple, list)):
        # SQL row ordering is preserved; only the JSON token array is a set.
        return [canonical(v) for v in value]
    if isinstance(value, float):
        return round(value, 10)
    return value


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--golden', type=Path, required=True)
    parser.add_argument('--record', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    def connect():
        con = pymysql.connect(user='root', unix_socket=args.socket, charset='utf8mb4', autocommit=True)
        with con.cursor() as cur:
            cur.execute('SET ob_query_timeout=100000000000')
        return con

    con = connect()
    database = 'fts_perf_regression_' + str(os.getpid())
    observations = []

    def observe(name, sql, params=None, connection=con):
        with connection.cursor() as cur:
            try:
                # Populate range statistics used by BM25's document-frequency
                # estimate before comparing numeric scores across fresh indexes.
                if '/natural/' in name:
                    for _ in range(2):
                        cur.execute(sql, params)
                        cur.fetchall()
                cur.execute(sql, params)
                result = canonical(cur.fetchall())
            except pymysql.MySQLError as error:
                result = {'error': error.args[0]}
        item = {'name': name, 'result': result}
        return item

    fixed = ['', ' ', '\t\n\r', None, 'a', '1', 'hello HELLO Hello world',
             '中华人民共和国 数据库 全文索引 自然语言处理', '研究生命起源 南京市长江大桥',
             '一二三四五六七八九十百千万', '12个 3.14米 2026年 10公斤',
             'ＡＢＣ１２３ abc-def abc_def c++ a@b.example',
             '中文 English 한국어 カタカナ ひらがな 😀𠀀', 'é É Straße İstanbul naïve CAFÉ',
             'the THE and An is of to a i', '\0hello\0world\0',
             '数据库全文索引 ' * 220, 'alpha BETa, gamma! 123 ' * 220,
             '中' * 999 + ' 文', '中' * 1000 + ' 文', '中' * 1001 + ' 文',
             '中' * 2001 + ' 文', ' ' * 1100 + 'tail']
    rng = random.Random(20261027)
    atoms = ['OceanBase', '数据库', '全文', '索引', '自然语言', '研究生', '命中', '23',
             '三百', '公里', 'abc-def', 'ABC', 'c++', 'école', '😀', 'the', 'AND',
             'カタカナ', '한국어', '.', ',', '\t', '\n', ' ', '!', '；']
    corpus = fixed + [''.join(rng.choices(atoms, k=rng.randint(1, 80))) for _ in range(120)]
    configs = [('ik', []), ('ik', [{'output': 'all'}]),
               ('ik', [{'output': 'all'}, {'additional_args': [{'ik_mode': 'max_word'}]}]),
               ('beng', []), ('beng', [{'output': 'all'}]),
               ('beng', [{'output': 'all'}, {'additional_args': [{'min_token_size': 1}, {'max_token_size': 84}]}]),
               ('space', [{'output': 'all'}]), ('ngram', [{'output': 'all'}])]
    # Concurrent readers must not share mutable parser state. Run both modes.
    barrier = threading.Barrier(8)

    def worker(index):
        connection = connect()
        barrier.wait(timeout=30)
        values = []
        try:
            for i in range(40):
                text = corpus[(i * 7 + index) % len(corpus)]
                config = json.dumps([{'output': 'all'}, {'additional_args': [{'ik_mode': 'smart' if i % 2 else 'max_word'}]}])
                values.append(observe('thread/%d/%d' % (index, i), 'SELECT TOKENIZE(%s,\'ik\',%s) AS tokens',
                                      (text, config), connection))
        finally:
            connection.close()
        return values

    with ThreadPoolExecutor(max_workers=8) as pool:
        for batch in pool.map(worker, range(8)):
            observations.extend(batch)

    for i, text in enumerate(corpus):
        for j, (name, config) in enumerate(configs):
            observations.append(observe('token/%d/%d' % (i, j), 'SELECT TOKENIZE(%s,%s,%s) AS tokens',
                                        (text, name, json.dumps(config))))
    for charset in ['utf8mb4', 'utf16', 'utf16le', 'gbk', 'latin1']:
        for collation in (['utf8mb4_bin', 'utf8mb4_general_ci', 'utf8mb4_unicode_ci']
                          if charset == 'utf8mb4' else [None]):
            value = 'CONVERT(%s USING %s)' % ('%s', charset)
            if collation:
                value += ' COLLATE ' + collation
            for name in ['ik', 'beng', 'space', 'ngram']:
                observations.append(observe('charset/%s/%s/%s' % (charset, collation, name),
                                            "SELECT TOKENIZE(" + value + ",%s,'[{\"output\":\"all\"}]')",
                                            ('Alpha 中文 123 thé STRASSE', name)))
    # Malformed UTF-8 and property errors must retain their error/skip behavior.
    for raw in ['80', 'c0af', 'e4b8', 'f4908080', '6162ff6364', 'ed a0 80'.replace(' ', '')]:
        for name in ['ik', 'beng']:
            observations.append(observe('invalid/%s/%s' % (raw, name),
                                        "SELECT HEX(CAST(TOKENIZE(CONVERT(UNHEX(%s) USING utf8mb4),%s) AS CHAR))", (raw, name)))
    with con.cursor() as cur:
        cur.execute('CREATE DATABASE ' + database)
        cur.execute('USE ' + database)
    docs = [(1, '数据库 全文索引', 'hello world database database'),
            (2, '自然语言处理', 'the quick brown fox HELLO'),
            (3, '全文查询 数据库', 'database performance audit'),
            (4, '一二三 123公里', 'HELLO, punctuation! café'),
            (5, '', ''), (6, None, None)]
    docs += [(i, '数据库 文档%d ' % i + ' '.join(rng.choices(atoms[:9], k=12)),
              ' '.join(rng.choices(['hello', 'world', 'database', 'performance', 'audit', 'and'], k=16)))
             for i in range(7, 207)]
    queries = ['数据库', '全文 索引', 'OceanBase 数据库', 'missingunique', 'hello', 'HELLO hello',
               'database performance audit', 'the and', 'hello database', '123', 'école']
    for name in ['ik', 'beng']:
        for mode in (['smart', 'max_word'] if name == 'ik' else ['default']):
            table = name + '_' + mode
            with con.cursor() as cur:
                cur.execute('CREATE TABLE ' + table + '(id INT PRIMARY KEY, title TEXT, body TEXT)')
                cur.executemany('INSERT INTO ' + table + ' VALUES(%s,%s,%s)', docs)
                props = " PARSER_PROPERTIES=(ik_mode='%s')" % mode if name == 'ik' else ''
                cur.execute('CREATE FULLTEXT INDEX ft ON ' + table + '(title,body) WITH PARSER ' + name + props)

            def match_stage(stage):
                # BM25 uses optimizer estimates for total document count.
                # Refresh both versions at the same stage, avoiding an async
                # auto-statistics refresh changing IDF halfway through DML tests.
                with con.cursor() as cur:
                    cur.execute('ANALYZE TABLE ' + table)
                    cur.fetchall()
                for i, query in enumerate(queries):
                    observations.append(observe('%s/%s/natural/%d' % (table, stage, i),
                        'SELECT id,MATCH(title,body) AGAINST(%s) score FROM ' + table +
                        ' WHERE MATCH(title,body) AGAINST(%s) ORDER BY id', (query, query)))
                for i, query in enumerate(['+hello -world', 'hello database', '+database +performance',
                                           'missingunique', 'hello hello', '"hello world"']):
                    observations.append(observe('%s/%s/boolean/%d' % (table, stage, i),
                        'SELECT id,MATCH(title,body) AGAINST(%s IN BOOLEAN MODE) score FROM ' + table +
                        ' WHERE MATCH(title,body) AGAINST(%s IN BOOLEAN MODE) ORDER BY id', (query, query)))
                observations.append(observe('%s/%s/limit' % (table, stage),
                    "SELECT id FROM " + table + " WHERE MATCH(title,body) AGAINST('database 数据库') ORDER BY id LIMIT 7 OFFSET 3"))
                observations.append(observe('%s/%s/rank' % (table, stage),
                    "SELECT id, MATCH(title,body) AGAINST('database hello') score FROM " + table +
                    " WHERE MATCH(title,body) AGAINST('database hello') ORDER BY score DESC,id LIMIT 12"))
                match = "MATCH(title,body) AGAINST('database hello')"
                extra = [
                    'SELECT id FROM {t} WHERE {m} ORDER BY id',
                    'SELECT COUNT(*),SUM(id) FROM {t} WHERE {m}',
                    'SELECT id FROM {t} WHERE {m} AND id BETWEEN 10 AND 90 ORDER BY id',
                    'SELECT id FROM {t} WHERE {m} ORDER BY id LIMIT 0',
                    'SELECT id FROM {t} WHERE {m} ORDER BY id LIMIT 7 OFFSET 20',
                    'SELECT MOD(id,5),COUNT(*) FROM {t} WHERE {m} GROUP BY MOD(id,5) ORDER BY 1',
                    'SELECT id,({m}>0),({m} IS TRUE) FROM {t} WHERE {m} ORDER BY id',
                    'SELECT id,{m}+1 FROM {t} WHERE {m} ORDER BY id',
                    'SELECT id FROM {t} WHERE {m}>0 ORDER BY id',
                    'SELECT id FROM {t} WHERE {m} AND {m}>0.00001 ORDER BY id',
                    'SELECT id FROM {t} WHERE {m} AND {m}>0.25 ORDER BY id',
                    'SELECT id FROM {t} WHERE {m} AND {m}>1 ORDER BY id',
                    'SELECT id FROM {t} WHERE {m} AND {m}>100 ORDER BY id',
                    'SELECT SUM({m}),MAX({m}) FROM {t} WHERE {m}',
                    'SELECT id FROM {t} WHERE {m} GROUP BY id HAVING MAX({m})>0.00001 ORDER BY id',
                    'SELECT id,ROW_NUMBER() OVER(ORDER BY {m} DESC,id) FROM {t} WHERE {m} ORDER BY id',
                    'SELECT id FROM {t} WHERE {m} ORDER BY {m} DESC,id LIMIT 10',
                    'SELECT id FROM {t} WHERE {m} AND id IN(SELECT id FROM {t} WHERE id<15) ORDER BY id',
                    "SELECT id FROM {t} WHERE MATCH(title,body) AGAINST('database hello' IN BOOLEAN MODE) ORDER BY id",
                    "SELECT id FROM {t} WHERE MATCH(title,body) AGAINST('the and') ORDER BY id",
                    "SELECT id FROM {t} WHERE MATCH(title,body) AGAINST('missingunique') ORDER BY id",
                    "SELECT id FROM {t} WHERE MATCH(title,body) AGAINST('') ORDER BY id",
                    'SELECT id FROM {t} WHERE MATCH(title,body) AGAINST(NULL) ORDER BY id',
                ]
                for i, sql in enumerate(extra):
                    observations.append(observe('%s/%s/score_use/%d' % (table, stage, i), sql.format(t=table, m=match)))

            match_stage('built')
            with con.cursor() as cur:
                cur.execute('UPDATE ' + table + " SET title='新文档 数据库',body='hello replacement' WHERE id IN(1,7,8)")
                cur.execute('DELETE FROM ' + table + ' WHERE id IN(2,9)')
                cur.execute('INSERT INTO ' + table + " VALUES(300,'全文索引','database hello audit')")
                cur.execute('START TRANSACTION')
                cur.execute('DELETE FROM ' + table)
                cur.execute('ROLLBACK')
            match_stage('dml')

    # Check full ID streams on the 20,000-row benchmark as auxiliary evidence.
    for name, cols, query in [('cn', 'title,content', '全文索引 分词器'),
                              ('beng', 'title_en,content_en', 'database performance audit'),
                              ('mixed', 'title,content', 'OceanBase 稳定 database'),
                              ('limit', 'content', '倒排索引 tokenizer')]:
        observations.append(observe('large/' + name,
            'SELECT id FROM fts_large_bench.docs WHERE MATCH(' + cols + ') AGAINST(%s) ORDER BY id', (query,)))
    con.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(observations, ensure_ascii=False, indent=2) + '\n')
    if args.record:
        args.golden.write_text(json.dumps(observations, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps({'recorded': len(observations), 'errors': sum(isinstance(x['result'], dict) for x in observations)}))
    else:
        expected = json.loads(args.golden.read_text())
        failures = [{'name': a['name'], 'expected': b['result'], 'actual': a['result']}
                    for a, b in zip(observations, expected) if a != b]
        assert len(expected) == len(observations)
        summary = {'total': len(observations), 'passed': len(observations) - len(failures), 'failures': failures}
        args.output.with_suffix('.summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps({k: v for k, v in summary.items() if k != 'failures'}))
        if failures:
            print(json.dumps(failures[:3], ensure_ascii=False)[:4000])
            raise SystemExit(1)


if __name__ == '__main__':
    main()
