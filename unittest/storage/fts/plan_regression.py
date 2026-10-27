#!/usr/bin/env python3
"""Assert membership-only plans elide BM25 while score consumers retain it."""
import argparse
import json
from pathlib import Path
import pymysql

parser = argparse.ArgumentParser(__doc__)
parser.add_argument('--socket', required=True)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--optimized', action='store_true')
args = parser.parse_args()
connection = pymysql.connect(user='root', unix_socket=args.socket, charset='utf8mb4', autocommit=True)
table = 'fts_large_bench.docs'
match = "MATCH(title_en,content_en) AGAINST('database performance')"
cases = [
    ('count', False, 'SELECT COUNT(*) FROM {t} WHERE {m}'),
    ('implicit_score_order', True, 'SELECT id FROM {t} WHERE {m}'),
    ('implicit_score_limit', True, 'SELECT id FROM {t} WHERE {m} LIMIT 20'),
    ('ids_explicit_order', True, 'SELECT id FROM {t} WHERE {m} ORDER BY id'),
    ('limit_explicit_order', True, 'SELECT id FROM {t} WHERE {m} ORDER BY id LIMIT 20'),
    ('extra_filter', True, 'SELECT /*+ INDEX(docs fti_en_beng) */ id FROM {t} WHERE {m} AND id<100 ORDER BY id'),
    ('aggregate', True, 'SELECT MOD(id,5),COUNT(*) FROM {t} WHERE {m} GROUP BY MOD(id,5)'),
    ('project_score', True, 'SELECT id,{m} FROM {t} WHERE {m}'),
    ('project_boolean', True, 'SELECT id,({m} IS TRUE) FROM {t} WHERE {m}'),
    ('order_score', True, 'SELECT id FROM {t} WHERE {m} ORDER BY {m} DESC LIMIT 20'),
    ('numeric_threshold', True, 'SELECT id FROM {t} WHERE {m}>0.25'),
    ('additional_threshold', True, 'SELECT id FROM {t} WHERE {m} AND {m}>0.25'),
    ('score_aggregate', True, 'SELECT SUM({m}) FROM {t} WHERE {m}'),
    ('score_having', True, 'SELECT id FROM {t} WHERE {m} GROUP BY id HAVING MAX({m})>0.25'),
    ('score_window', True, 'SELECT id,ROW_NUMBER() OVER(ORDER BY {m} DESC) FROM {t} WHERE {m}'),
    ('boolean_mode', True, "SELECT id FROM {t} WHERE MATCH(title_en,content_en) AGAINST('+database -audit' IN BOOLEAN MODE)"),
]
results = []
with connection.cursor() as cursor:
    for name, expected, sql in cases:
        cursor.execute('EXPLAIN ' + sql.format(t=table, m=match))
        plan = '\n'.join(str(row[0]) for row in cursor.fetchall())
        want = expected if args.optimized else True
        passed = (('calc_relevance=true' in plan or 'has_functional_lookup=true' in plan)
                  and 'calc_relevance=false' not in plan) if want else 'calc_relevance=false' in plan
        results.append({'name': name, 'passed': passed, 'expected_calc_relevance': want, 'plan': plan})
connection.close()
summary = {'total': len(results), 'passed': sum(x['passed'] for x in results), 'results': results}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({'total': summary['total'], 'passed': summary['passed']}))
raise SystemExit(int(summary['total'] != summary['passed']))
