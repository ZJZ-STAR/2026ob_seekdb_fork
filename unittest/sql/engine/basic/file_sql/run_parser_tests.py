import json, shlex, subprocess
from pathlib import Path
repo=Path('/home/PeterPan/2026ob/src/seekdb')
out=Path('/home/PeterPan/2026ob/results/file-sql/parser')
out.mkdir(parents=True,exist_ok=True)
entries=json.loads((repo/'build_release/compile_commands.json').read_text())
entry=next(e for e in entries if 'unity_ob_sql_ob_sql_engine_expr_13' in e['file'])
command=shlex.split(entry['command'])
target=out/'test_file_sql_parser.o'
command[command.index('-o')+1]=str(target)
command[command.index('-c')+1]=str(repo/'unittest/sql/parser/test_file_sql_parser.cpp')
command+=['-Wno-error','-O0','-g0']
subprocess.run(command,cwd=entry['directory'],check=True)
directory=repo/'build_release/src/observer'
link=shlex.split((directory/'CMakeFiles/seekdb.dir/link.txt').read_text())
link=[s for s in link if not s.startswith('CMakeFiles/ob_main.dir/')]
link[link.index('-o')+1]=str(out/'parser-tests')
link += [str(target),'-lgtest_main','-lgtest']
subprocess.run(link,cwd=directory,check=True)
run=subprocess.run([str(out/'parser-tests'),'--gtest_output=xml:'+str(out/'results.xml')],cwd=out,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,universal_newlines=True)
(out/'output.log').write_text(run.stdout)
print(run.stdout)
raise SystemExit(run.returncode)
