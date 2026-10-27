"""Compare same-machine complete runs using the competition category weights."""
import argparse
import json
from pathlib import Path
from statistics import mean

parser = argparse.ArgumentParser(__doc__)
parser.add_argument('--before', nargs='+', type=Path, required=True)
parser.add_argument('--after', nargs='+', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()

def read(path):
    values = {}
    for line in path.read_text().splitlines():
        if ':' not in line:
            continue
        key, value = line.split(':', 1)
        try:
            values[key.strip()] = float(value.strip())
        except ValueError:
            pass
    expected = dict(rows=20000, batch=500, rounds=3000, query_rounds=200, samples=3, warmup=30, skip_load=0)
    assert all(values.get(k) == v for k, v in expected.items()), (path, 'configuration mismatch')
    return values

before = [read(p) for p in args.before]
after = [read(p) for p in args.after]
categories = {
    'build': ['build_ik_all_sec', 'build_ik_content_sec', 'build_beng_en_sec'],
    'tokenize': ['tokenize_ik_avg_ms', 'tokenize_beng_avg_ms'],
    'query': ['query_cn_avg_ms', 'query_beng_avg_ms', 'query_mixed_avg_ms', 'query_limit_avg_ms'],
}
metrics, improvements = {}, {}
for category, keys in categories.items():
    for key in keys:
        old = mean(v[key] for v in before)
        new = mean(v[key] for v in after)
        metrics[key] = {'before': old, 'after': new, 'improvement': (old-new)/old}
    improvements[category] = mean(metrics[key]['improvement'] for key in keys)
for key in ['query_cn_hits', 'query_beng_hits', 'query_mixed_hits', 'query_limit_hits']:
    assert len({v[key] for v in before+after}) == 1, (key, 'hit count changed')
result = {'before_files': [str(p) for p in args.before], 'after_files': [str(p) for p in args.after],
          'metrics': metrics, 'category_improvements': improvements,
          'mean_category_improvement': mean(improvements.values())}
args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(result, ensure_ascii=False, indent=2))
