#!/usr/bin/env python3
"""Reuse the competition CMake toolchain and link libraries for utility tests."""
import json
from pathlib import Path
import shlex
import subprocess

root = Path('/home/PeterPan/2026ob/src/seekdb')
build = root / 'build_release'
out = Path('/home/PeterPan/2026ob/results/ai-functions/cpp')
out.mkdir(parents=True, exist_ok=True)
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(x for x in entries if 'unity_ob_sql_ob_sql_engine_expr_13' in x['file'])
command = shlex.split(entry['command'])
objects = []
for name in ['ob_expr_ai_prompt_test.cpp', 'test_ob_openai_utils.cpp']:
    source = root / 'unittest/sql/engine/expr' / name
    target = out / (name + '.o')
    arguments = command.copy()
    arguments[arguments.index('-o') + 1] = str(target)
    arguments[arguments.index('-c') + 1] = str(source)
    arguments += ['-Wno-error', '-O0', '-g0']
    subprocess.run(arguments, cwd=entry['directory'], check=True)
    objects.append(str(target))
directory = build / 'src/observer'
link = shlex.split((directory / 'CMakeFiles/seekdb.dir/link.txt').read_text())
link = [x for x in link if not x.startswith('CMakeFiles/ob_main.dir/')]
link[link.index('-o') + 1] = str(out / 'ai-utils-tests')
link += objects + ['-lgtest_main', '-lgtest']
subprocess.run(link, cwd=str(directory), check=True)
run = subprocess.run([str(out / 'ai-utils-tests'), '--gtest_output=xml:' + str(out / 'results.xml')],
    cwd=str(out), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
(out / 'output.txt').write_text(run.stdout)
print(run.stdout)
raise SystemExit(run.returncode)
