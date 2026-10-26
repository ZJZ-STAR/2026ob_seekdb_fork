"""Native column pruning, COPY failure cleanup and privilege boundaries."""
import json, shutil, os
from pathlib import Path
import pymysql
w=Path('/home/PeterPan/2026ob');root=w/'tests/file-sql';data=root/'edges';data.mkdir(exist_ok=True)
fixtures=w/'src/seekdb/unittest/sql/engine/basic/file_sql/data'
for f in fixtures.glob('*.parquet'):shutil.copyfile(f,data/f.name)
results=[]
def run(name,q,expected=None,error=False):
    con=pymysql.connect(user='root',unix_socket=str(w/'run/seekdb-file-sql/run/sql.sock'),database='test',autocommit=True)
    result={'name':name,'sql':q}
    try:
        with con.cursor() as c:
            c.execute('set enable_file_sql=ON');c.execute(q);rows=c.fetchall()
            assert not error,'expected error'
            if expected is not None:assert rows==expected,(rows,expected)
            result.update(passed=True,rows=rows)
    except Exception as e:result.update(passed=error and isinstance(e,pymysql.MySQLError),error=str(e))
    finally:con.close()
    results.append(result);print(json.dumps(result,default=str,ensure_ascii=False),flush=True)
p=data/'projection_corrupt.parquet'
run('native Parquet column pruning',f"select count(*),sum(id) from read_parquet('{p}')",((7,28),))
run('referenced corrupt column fails',f"select amount from read_parquet('{p}')",error=True)
run('unsigned and all-null',f"select * from read_parquet('{data/'unsigned_null.parquet'}')",((4294967295,'18446744073709551615',None),(None,'1',None)))
csv=data/'simple.csv';csv.write_text('id,name\n1,a\n2,b\n')
for clause in ['id','name','*']:
    run('native rescan '+clause,f"select /*+ leading(a b) use_nl(b) */ a.id,b.{clause if clause!='*' else 'id'} from read_csv('{csv}') a join read_csv('{csv}') b on a.id=b.id order by a.id")
target=data/'failure.csv'
if target.exists():target.unlink()
run('COPY later-row error',f"copy (select id,json_extract(case when id=2 then 'bad' else '{{}}' end,'$') as doc from read_csv('{csv}')) to '{target}' (header)",error=True)
results.append({'name':'COPY did not publish partial target','passed':not target.exists() and not list(data.glob('.seekdb-copy-*'))})
outside=w/'tests/file-sql-sibling';outside.mkdir(exist_ok=True);(outside/'outside.csv').write_text('id\n9\n')
run('prefix sibling denied',f"select * from read_csv('{outside/'outside.csv'}')",error=True)
run('COPY prefix sibling denied',f"copy (select 1) to '{outside/'export.csv'}'",error=True)
link=data/'outside.csv'
if link.is_symlink():link.unlink()
link.symlink_to(outside/'outside.csv')
run('symlink sibling denied',f"select * from read_csv('{link}')",error=True)
con=pymysql.connect(user='root',unix_socket=str(w/'run/seekdb-file-sql/run/sql.sock'),database='test',autocommit=True)
try:
    with con.cursor() as c:
        c.execute('select 17 into @FSQLCOPY1ordinary');c.execute('select @FSQLCOPY1ordinary')
        results.append({'name':'ordinary INTO variable prefix preserved','passed':c.fetchone()==(17,)})
finally:con.close()
summary={'total':len(results),'passed':sum(r['passed'] for r in results),'failed':[r for r in results if not r['passed']]}
(w/'results/file-sql/native-edge-results.json').write_text(json.dumps({'summary':summary,'results':results},default=str,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(summary,default=str,ensure_ascii=False));raise SystemExit(int(bool(summary['failed'])))
