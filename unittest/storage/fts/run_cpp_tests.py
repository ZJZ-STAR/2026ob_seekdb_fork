#!/usr/bin/env python3
"""Compile FTS regression tests using an existing CMake release build."""
import json
from pathlib import Path
import shlex
import subprocess

repo = Path(__file__).resolve().parents[3]
build = repo / 'build_release'
dest = build / 'fts-regression'
dest.mkdir(exist_ok=True)
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(e for e in entries if 'unity_ob_storage_ob_storage_ik_0' in e['file'])
command = shlex.split(entry['command'])
obj = dest / 'test_ik_char_util.o'
command[command.index('-o') + 1] = str(obj)
command[command.index('-c') + 1] = str(Path(__file__).resolve().with_name('test_ik_char_util.cpp'))
command += ['-O2', '-g0']
subprocess.run(command, cwd=entry['directory'], check=True)
directory = build / 'src/observer'
command = shlex.split((directory / 'CMakeFiles/seekdb.dir/link.txt').read_text())
command = [x for x in command if not x.startswith('CMakeFiles/ob_main.dir/')]
command[command.index('-o') + 1] = str(dest / 'fts-tests')
command += [str(obj), '-lgtest_main', '-lgtest']
subprocess.run(command, cwd=str(directory), check=True)
run = subprocess.run([str(dest / 'fts-tests'), '--gtest_output=xml:' + str(dest / 'results.xml')],
                     cwd=str(dest), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
(dest / 'output.txt').write_text(run.stdout)
print(run.stdout, flush=True)
raise SystemExit(run.returncode)
