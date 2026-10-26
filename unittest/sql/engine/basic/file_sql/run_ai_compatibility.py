"""Run the two existing AI mysqltests on the new File SQL binary."""
import json, os, shutil, subprocess
from pathlib import Path
w=Path('/home/PeterPan/2026ob');repo=w/'src/seekdb';out=w/'results/file-sql/ai-compatibility';out.mkdir(parents=True,exist_ok=True)
config=out/'client.cnf'
with os.fdopen(os.open(str(config),os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as f:f.write('[client]\nuser=root\nsocket='+str(w/'run/seekdb-file-sql/run/sql.sock')+'\n')
results=[]
try:
    for test in ['ai_function_enhancement','ai_prompt']:
        suite=repo/'tools/deploy/mysql_test/test_suite/ai_function';expected=out/(test+'.result');shutil.copyfile(suite/'r/mysql'/(test+'.result'),expected)
        command=[str(repo/'deps/3rd/u01/obclient/bin/mysqltest'),'--defaults-file='+str(config),'--database=test','--test-file='+str(suite/'t'/(test+'.test')),'--result-file='+str(expected),'--logdir='+str(out),'--tmpdir='+str(out),'--max-connect-retries=1']
        run=subprocess.run(command,cwd=repo/'tools/deploy/mysql_test',stdout=subprocess.PIPE,stderr=subprocess.STDOUT,universal_newlines=True)
        (out/(test+'.log')).write_text(run.stdout);result={'test':test,'exit_code':run.returncode,'output':run.stdout};results.append(result);print(json.dumps(result),flush=True)
finally:config.unlink()
(out/'summary.json').write_text(json.dumps(results,indent=2)+'\n');raise SystemExit(int(any(r['exit_code'] for r in results)))
