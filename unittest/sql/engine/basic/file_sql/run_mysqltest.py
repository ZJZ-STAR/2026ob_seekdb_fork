"""Run the competition mysqltest body with local fixture deployment."""
import base64, json, os, shutil, subprocess
from pathlib import Path
import pymysql

w=Path('/home/PeterPan/2026ob');repo=w/'src/seekdb';suite=repo/'tools/deploy/mysql_test/test_suite/file_sql'
out=w/'results/file-sql/mysqltest';out.mkdir(parents=True,exist_ok=True)
base=w/'tests/file-sql/official';data=base/'data_for_mysqltest_file_sql';data.mkdir(parents=True,exist_ok=True)
for f in data.iterdir():
    if f.is_file() or f.is_symlink():f.unlink()
for f in (suite/'data').iterdir():
    if f.suffix=='.b64':(data/'simple.parquet').write_bytes(base64.b64decode(f.read_bytes()))
    else:shutil.copyfile(f,data/f.name)
socket=str(w/'run/seekdb-file-sql/run/sql.sock')
con=pymysql.connect(user='root',unix_socket=socket,autocommit=True)
with con.cursor() as c:c.execute("set global secure_file_priv='/home/PeterPan/2026ob/tests/file-sql'")
con.close()
body=(suite/'t/file_sql_basic.test').read_text()
body=body.replace('--source mysql_test/test_suite/file_sql/include/copy_file_sql_data.inc','let $OBSERVER_DIR = '+str(base)+';')
body=body.replace('--source mysql_test/test_suite/file_sql/include/remove_file_sql_data.inc','')
test=out/'file_sql_basic.test';test.write_text(body)
expected=out/'file_sql_basic.result';shutil.copyfile(suite/'r/mysql/file_sql_basic.result',expected)
config=out/'client.cnf'
with os.fdopen(os.open(str(config),os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as f:f.write('[client]\nuser=root\nsocket='+socket+'\n')
command=[str(repo/'deps/3rd/u01/obclient/bin/mysqltest'),'--defaults-file='+str(config),'--database=test',
         '--test-file='+str(test),'--result-file='+str(expected),'--logdir='+str(out),'--tmpdir='+str(out),'--max-connect-retries=1']
try:run=subprocess.run(command,cwd=repo/'tools/deploy/mysql_test',stdout=subprocess.PIPE,stderr=subprocess.STDOUT,universal_newlines=True)
finally:config.unlink()
(out/'output.log').write_text(run.stdout)
result={'test':'file_sql_basic','exit_code':run.returncode,'fixture_setup':'local deployment; SQL body unchanged','output':run.stdout}
(out/'summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result,ensure_ascii=False))
raise SystemExit(run.returncode)
