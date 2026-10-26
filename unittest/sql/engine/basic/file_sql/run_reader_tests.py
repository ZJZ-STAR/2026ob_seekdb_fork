"""Compile and run the bounded file readers with sanitizers."""
import os, subprocess
from pathlib import Path
r=Path('/home/PeterPan/2026ob/src/seekdb');out=Path('/home/PeterPan/2026ob/results/file-sql')
out.mkdir(parents=True,exist_ok=True);deps=r/'deps/3rd/usr/local/oceanbase/deps/devel'
command=['g++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
         '-I',str(r/'src'),'-I',str(deps/'include'),str(r/'unittest/sql/engine/basic/test_file_scan_utils.cpp'),
         '-L',str(deps/'lib'),'-lgtest_main','-lgtest','-lpthread','-o',str(out/'reader-sanitized')]
subprocess.run(command,check=True)
env=os.environ.copy();env['FILE_SQL_FIXTURES']=str(Path(__file__).resolve().parent/'data');env['ASAN_OPTIONS']='detect_leaks=1'
run=subprocess.run([str(out/'reader-sanitized'),'--gtest_output=xml:'+str(out/'reader-results.xml')],env=env,
                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT,universal_newlines=True)
(out/'reader-sanitized.log').write_text(run.stdout);print(run.stdout);raise SystemExit(run.returncode)
