#!/usr/bin/env python3
"""Independent membership oracle across rowkey formats, index-back and rescan."""
import argparse
import json
import os
from pathlib import Path
import pymysql

parser = argparse.ArgumentParser(__doc__)
parser.add_argument('--socket', required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
con = pymysql.connect(user='root', unix_socket=args.socket, charset='utf8mb4', autocommit=True)
results = []
database = 'fts_membership_' + str(os.getpid())
with con.cursor() as cur:
    cur.execute('SET ob_query_timeout=100000000000')
    cur.execute('CREATE DATABASE ' + database)
    cur.execute('USE ' + database)
    definitions = [
        ('signed_key', 'id BIGINT PRIMARY KEY, shard INT', ''),
        ('unsigned_key', 'id BIGINT UNSIGNED PRIMARY KEY, shard INT', ''),
        ('composite_key', 'id BIGINT, shard INT, PRIMARY KEY(id,shard)', ''),
        ('heap_table', 'id BIGINT, shard INT', ' ORGANIZATION=HEAP'),
        ('partitioned', 'id BIGINT PRIMARY KEY, shard INT', ' PARTITION BY HASH(id) PARTITIONS 4'),
    ]
    for table, columns, suffix in definitions:
        cur.execute('CREATE TABLE ' + table + '(' + columns + ', body TEXT,payload VARCHAR(80))' + suffix)
        docs = {i: (' '.join(word for n, word in enumerate(['hello', 'database', 'audit']) if i % (n+2) == 0),
                    '取行验证-%d' % i) for i in range(1, 302)}
        cur.executemany('INSERT INTO ' + table + ' VALUES(%s,%s,%s,%s)',
                        [(i, i % 4, body, payload) for i, (body, payload) in docs.items()])
        cur.execute('CREATE FULLTEXT INDEX ft ON ' + table + '(body) WITH PARSER beng')

        def check(name, sql, expected, params=None):
            try:
                cur.execute(sql, params)
                actual = list(cur.fetchall())
                passed = actual == expected
            except pymysql.MySQLError as error:
                actual = {'error': error.args[0], 'message': str(error)}
                passed = False
            results.append({'name': table + '/' + name, 'passed': passed,
                            'expected': expected, 'actual': actual})

        for stage in ['built', 'dml']:
            for text, terms in [('hello', {'hello'}), ('hello database', {'hello', 'database'}),
                                ('hello hello database', {'hello', 'database'}),
                                ('missingunique', {'missingunique'}), ('the and', set()), ('', set())]:
                ids = [i for i, (body, _) in sorted(docs.items()) if terms & set(body.split())]
                predicate = 'MATCH(body) AGAINST(%s)'
                check(stage + '/ids/' + text, 'SELECT id FROM ' + table + ' WHERE ' + predicate + ' ORDER BY id',
                      [(i,) for i in ids], (text,))
                check(stage + '/count/' + text, 'SELECT COUNT(*) FROM ' + table + ' WHERE ' + predicate,
                      [(len(ids),)], (text,))
                check(stage + '/index_back/' + text,
                      'SELECT id,body,payload FROM ' + table + ' WHERE ' + predicate + ' ORDER BY id LIMIT 11 OFFSET 7',
                      [(i, docs[i][0], docs[i][1]) for i in ids[7:18]], (text,))
            long_query = 'hello ' + ' '.join('uniqueterm%04d' % i for i in range(300))
            ids = [i for i, (body, _) in sorted(docs.items()) if 'hello' in body.split()]
            check(stage + '/over_256_terms', 'SELECT id FROM ' + table + ' WHERE MATCH(body) AGAINST(%s) ORDER BY id',
                  [(i,) for i in ids], (long_query,))
            # A subquery execution repeatedly opens/rescans the text iterator.
            check(stage + '/correlated',
                  'SELECT n.id,(SELECT COUNT(*) FROM ' + table + ' d WHERE MATCH(d.body) AGAINST(%s) AND d.id<=n.id) '
                  'FROM ' + table + ' n WHERE n.id IN(3,11,29) ORDER BY n.id',
                  [(i, sum(j <= i for j in ids)) for i in [3, 11, 29]], ('hello',))
            if stage == 'built':
                cur.execute('UPDATE ' + table + " SET body='database replacement' WHERE id IN(2,4,6)")
                for i in [2, 4, 6]:
                    docs[i] = ('database replacement', docs[i][1])
                cur.execute('DELETE FROM ' + table + ' WHERE id IN(8,12)')
                for i in [8, 12]:
                    del docs[i]
                cur.execute('INSERT INTO ' + table + " VALUES(500,0,'hello audit','新增行')")
                docs[500] = ('hello audit', '新增行')
con.close()
summary = {'total': len(results), 'passed': sum(r['passed'] for r in results), 'results': results}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({'total': summary['total'], 'passed': summary['passed'],
                  'failures': [r['name'] for r in results if not r['passed']]}))
raise SystemExit(int(summary['total'] != summary['passed']))
