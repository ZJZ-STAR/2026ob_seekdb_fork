#!/usr/bin/env python3
"""Collect checksums and passing test records for the isolated kernel build."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
from datetime import datetime, timezone
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run_kernel_tests.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
baseline = '248daab26650470be6a29ece5d8ef4d7b27873d6'
integration = '--integration' in sys.argv

def git(*args):
    return subprocess.check_output(['git'] + list(args), cwd=str(runner.repo), universal_newlines=True).strip()

def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()

official = json.loads((runner.output / 'mysqltest/summary.json').read_text())
regression = json.loads((runner.output / 'mysqltest/ai_function-summary.json').read_text())
faults = json.loads((runner.output / 'fault-integration.json').read_text())
unit = ET.parse(str(runner.output / 'cpp/results.xml')).getroot()
assert len(official) == 5 and all(t['exit_code'] == 0 for t in official)
assert len(regression) == 2 and all(t['exit_code'] == 0 for t in regression)
assert len(faults) == 11 and all(t['passed'] for t in faults)
assert unit.get('tests') == '10' and unit.get('failures') == '0'
names = set(git('diff', '--name-only', baseline).splitlines())
names.update(git('ls-files', '--others', '--exclude-standard').splitlines())
sources = {}
for name in sorted(names):
    if name.startswith(('src/', 'unittest/')) and '__pycache__' not in name:
        path = runner.repo / name
        if path.is_file():
            sources[name] = {'sha256': digest(path), 'bytes': path.stat().st_size}
file_sql = None
if integration:
    root = runner.output / 'file-sql'
    scripts = json.loads((root / 'integration-summary.json').read_text())
    assert len(scripts) == 5 and all(t['exit_code'] == 0 for t in scripts)
    parser = ET.parse(str(root / 'parser/results.xml')).getroot()
    reader = ET.parse(str(root / 'reader-results.xml')).getroot()
    sql = json.loads((root / 'sql-summary.json').read_text())
    edges = json.loads((root / 'native-edge-results.json').read_text())['summary']
    assert parser.get('tests') == '2' and parser.get('failures') == '0'
    assert reader.get('tests') == '17' and reader.get('failures') == '0'
    assert sql['total'] == sql['passed'] == 91 and not sql['failed']
    assert edges['total'] == edges['passed'] == 12 and not edges['failed']
    file_sql = {'scripts': scripts, 'parser_tests': 2, 'sanitized_reader_tests': 17,
                'sql_regression': sql, 'native_edges': edges}
logs = runner.output / 'evidence-logs'
logs.mkdir(exist_ok=True)
log_files = [('ai-column-integration-build.log', 'build.txt'),
             ('ai-column-integration-ai.log', 'all-tests.txt'),
             ('ai-column-integration-faults.log', 'fault-tests.txt'),
             ('ai-column-integration-file-sql.log', 'file-sql.txt'),
             ('ai-column-integration-generation.log', 'generation.txt')] if integration else [
             ('ai-column-build.log', 'build.txt'), ('ai-column-all-tests.log', 'all-tests.txt'),
             ('ai-column-fault-tests.log', 'fault-tests.txt')]
for source, target in log_files:
    shutil.copyfile(runner.workspace / 'logs' / source, logs / target)
manifest = {'collected_at': datetime.now(timezone.utc).isoformat(), 'baseline': baseline,
            'commit': git('rev-parse', 'HEAD'), 'branch': git('branch', '--show-current'),
            'dirty': bool(git('status', '--porcelain', '--', 'src', 'unittest')),
            'binary': str(runner.binary), 'binary_sha256': digest(runner.binary),
            'instance': str(runner.base), 'socket': str(runner.socket),
            'unit_tests': 10, 'official_mysqltests': official, 'regression_mysqltests': regression,
            'integration_scenarios': faults, 'source_files': sources}
if integration:
    preserved = ['5257435b2a3cd676a90c337f79e7bb22c0f8d50e',
                 'aebf6d1a8a1c4224157de33aa559e6e669d9b077']
    for commit in preserved:
        subprocess.run(['git', 'merge-base', '--is-ancestor', commit, 'HEAD'], cwd=str(runner.repo), check=True)
    manifest['preserved_commits'] = preserved
    manifest['file_sql'] = file_sql
    manifest['author'] = git('show', '-s', '--format=%an <%ae>', 'HEAD')
    manifest['committer'] = git('show', '-s', '--format=%cn <%ce>', 'HEAD')
name = 'integration-build-manifest.json' if integration else 'build-manifest.json'
(runner.output / name).write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({key: manifest[key] for key in ['commit', 'dirty', 'unit_tests', 'binary_sha256']}, indent=2))
