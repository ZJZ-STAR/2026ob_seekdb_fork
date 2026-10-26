"""SQL regression tests run against the native seekdb executor."""
import base64, datetime, decimal, json, math, shutil
from pathlib import Path
import pymysql

w=Path('/home/PeterPan/2026ob'); repo=w/'src/seekdb'; root=w/'tests/file-sql'; data=root/'regression-data'
data.mkdir(parents=True,exist_ok=True); results=[]
def connect():
    con=pymysql.connect(user='root',unix_socket=str(w/'run/seekdb-file-sql/run/sql.sock'),database='test',autocommit=True)
    with con.cursor() as c:c.execute('set enable_file_sql=ON');c.execute('set ob_query_timeout=120000000')
    return con
def sql(q):
    con=connect()
    try:
        with con.cursor() as c:c.execute(q);return c.fetchall(),[d[0] for d in c.description] if c.description else []
    finally:con.close()
def same(a,b):
    if isinstance(a,bytes):a=a.decode('utf-8')
    if isinstance(b,(list,tuple)):return isinstance(a,(list,tuple)) and len(a)==len(b) and all(same(x,y) for x,y in zip(a,b))
    if isinstance(b,float):return a is not None and math.isclose(float(a),b,rel_tol=1e-10,abs_tol=1e-10)
    return a==b
def check(name,q,expected=None,error=False,columns=None,predicate=None):
    result={'name':name,'sql':q}
    try:
        rows,meta=sql(q);assert not error,'expected SQL to fail'
        if expected is not None:assert same(rows,expected),(rows,expected)
        if columns is not None:assert meta==columns,(meta,columns)
        if predicate:assert predicate(rows),rows
        result.update(passed=True,rows=rows,columns=meta)
    except Exception as e:result.update(passed=error and isinstance(e,pymysql.MySQLError),error=str(e))
    results.append(result);print(json.dumps(result,default=str,ensure_ascii=False),flush=True)
def file(name,body):
    p=data/name
    with p.open('w',encoding='utf-8',newline='') as f:f.write(body)
    return p
def csv(p,options=''):return "read_csv('"+str(p)+"'"+options+")"
def js(p,fn='read_json'):return fn+"('"+str(p)+"')"

sql("set global secure_file_priv='"+str(root)+"'")
for f in (repo/'tools/deploy/mysql_test/test_suite/file_sql/data').iterdir():
    if f.suffix=='.b64':(data/'simple.parquet').write_bytes(base64.b64decode(f.read_bytes()))
    else:shutil.copyfile(f,data/f.name)
for f in (repo/'unittest/sql/engine/basic/file_sql/data').glob('*.parquet'):shutil.copyfile(f,data/f.name)
a=data/'basic.csv';j=data/'basic.jsonl';p=data/'simple.parquet'
check('ordinary SQL',"select 2+3,upper('abc')",[(5,'ABC')])
check('CSV star',f'select * from {csv(a)} order by id',columns=['id','name','score','active','day','created','note','id__2'],predicate=lambda r:len(r)==3 and r[0][1]=='Alice' and r[0][6]=='hello,\nworld')
check('CSV aggregate',f'select count(*),sum(id),round(avg(score),2),min(id),max(score),sum(active) from {csv(a)}',[(3,6,91.92,1,98.5,2)])
check('expressions WHERE ORDER LIMIT',f'select id*10 as k,upper(name) as n from {csv(a)} f where score>88 order by score desc limit 1',[(10,'ALICE')],columns=['k','n'])
check('GROUP HAVING',f'select active,count(*),sum(id) from {csv(a)} group by active having count(*)>1',[(1,2,4)])
check('alias',f'select f.id from {csv(a)} as f where f.id=2',[(2,)])
check('named options',f"select count(*) from read_csv('{a}',header=true,sample_size=-1)",[(3,)])
for opts in [',sample_size=10',',unknown=true',',header=true,header=false',",'xml'",",delimiter='xx'"]:
    check('invalid options '+opts,f'select * from {csv(a,opts)}',error=True)
schema=[('id','BIGINT','NO',None,None,None),('name','VARCHAR','NO',None,None,None),('score','DOUBLE','NO',None,None,None),('active','BOOLEAN','NO',None,None,None),('day','DATE','YES',None,None,None),('created','DATETIME','YES',None,None,None),('note','VARCHAR','NO',None,None,None),('id__2','BIGINT','NO',None,None,None)]
check('DESCRIBE',f'describe select * from {csv(a)}',schema,columns=['column_name','column_type','null','key','default','extra'])
check('DESCRIBE projection',f'describe select name as person,id+1 as next_id from {csv(a)}',predicate=lambda r:r[0][0]=='person' and r[1][0]=='next_id')
check('native EXPLAIN',f'explain select id from {csv(a)}',predicate=lambda r:'JSON_TABLE' in str(r) or 'FILE_SCAN' in str(r))
check('JSON union',f'select id,name,score,active,day,note from {js(j)} order by id',[(1,'Alice',98.5,1,None,None),(2,'Bob',None,0,datetime.date(2026,8,2),None),(3,'Chen',90.0,1,None,'ok')])
check('NDJSON alias',f'select count(*) from {js(j,"read_ndjson")}',[(3,)])
check('JSON format',f"select count(*) from read_json('{j}',format='newline_delimited',sample_size=-1)",[(3,)])
check('JSON aggregates',f'select count(score),sum(id),avg(score),min(id),max(id) from {js(j)}',[(2,6,94.25,1,3)])
check('Parquet fixture',f"select count(*),min(c1),max(c1),sum(c1) from read_parquet('{p}')",[(7,0,6,21)])
for name in ['flat_v1_none.parquet','flat_v1_snappy.parquet','flat_v2_none.parquet','flat_v2_snappy.parquet','flat_plain.parquet','flat_int96.parquet']:
    source=f"read_parquet('{data/name}')"
    check('Parquet aggregate '+name,f'select count(*),sum(id),sum(amount),count(amount),sum(flag) from {source}',[(7,28,14.0,6,3)])
    check('Parquet strings '+name,f'select id,label from {source} where id in(2,4,7) order by id',[(2,'中文'),(4,None),(7,'')])
    check('Parquet dates '+name,f'select day,created from {source} where id=1',[(datetime.date(2026,1,1),datetime.datetime(2026,1,1,12,30,4,123456))])
check('Parquet many pages',f"select count(*),sum(id) from read_parquet('{data/'many_pages.parquet'}')",[(256,32640)])
check('Parquet nested',f"select * from read_parquet('{data/'nested.parquet'}')",error=True)
check('glob',f"select substring_index(file,'/',-1) from glob('{data}/*.jsonl') order by file",predicate=lambda r:('basic.jsonl',) in r)
check('glob empty',f"select count(*) from glob('{data}/no-such-*')",[(0,)])
left=file('left.csv','id,name\n1,A\n2,B\n3,C\n4,D\n');right=file('right.jsonl','{"id":2,"v":20}\n{"id":2,"v":21}\n{"id":4,"v":40}\n')
for hint in ['', '/*+ leading(a b) use_nl(b) */']:
    check('INNER '+hint,f'select {hint} a.id,a.name,b.v from {csv(left)} a inner join {js(right)} b on a.id=b.id order by a.id,b.v',[(2,'B',20),(2,'B',21),(4,'D',40)])
    check('LEFT '+hint,f'select {hint} a.id,b.v from {csv(left)} a left join {js(right)} b on a.id=b.id order by a.id,b.v',[(1,None),(2,20),(2,21),(3,None),(4,40)])
check('CSV-Parquet JOIN',f"select a.id,b.c1 from {csv(left)} a join read_parquet('{p}') b on a.id=b.c1 order by a.id",[(1,1),(2,2),(3,3),(4,4)])
check('self JOIN',f'select a.id,b.name from {csv(left)} a join {csv(left)} b on a.id=b.id where a.id<3 order by a.id',[(1,'A'),(2,'B')])
for name,body,opts,expected in [
    ('noheader.csv','1,a\n2,b\n',',header=false',[(1,'a'),(2,'b')]),
    ('null.csv','id,v\n1,NA\n2,"NA"\n3,""\n',",nullstr='NA'",[(1,None),(2,'NA'),(3,'')]),
    ('bom.csv','\ufeffid,name\n1,中文\n','',[(1,'中文')]),
    ('nulls.csv','n\n\n\n','',[(None,),(None,)]),
    ('boundary.csv','v\n9223372036854775807\n-9223372036854775808\n','',[(9223372036854775807,),(-9223372036854775808,)]),
    ('overflow.csv','v\n9223372036854775808\n-9223372036854775809\n','',[('9223372036854775808',),('-9223372036854775809',)]),
    ('allvarchar.csv','id,active\n1,true\n2,false\n',',all_varchar=true',[('1','true'),('2','false')])]:
    check('CSV '+name,f'select * from {csv(file(name,body),opts)}',expected)
for name,body in [('bad_count.csv','a,b\n1\n'),('bad_quote.csv','a\n"x'),('bad_after.csv','a\n"x"z\n')]:check(name,f'select * from {csv(file(name,body))}',error=True)
bad=data/'bad_utf8.csv';bad.write_bytes(b'a\n\xff\n');check('invalid UTF8',f'select * from {csv(bad)}',error=True)
conflict=file('conflict.jsonl','{"v":1}\n{"v":"2"}\n{"v":null}\n');check('JSON conflict',f'select v from {js(conflict)}',[('1',),('2',),(None,)])
for name,body in [('nested.jsonl','{"v":{}}\n'),('array.jsonl','{"v":[]}\n'),('duplicate.jsonl','{"v":1,"v":2}\n'),('bad.jsonl','{"v":1.}\n')]:check(name,f'select * from {js(file(name,body))}',error=True)
dynamic=file('dynamic.csv','old\n1\n');check('schema first',f'select * from {csv(dynamic)}',[(1,)],columns=['old']);file('dynamic.csv','new\n2\n');check('schema refreshed',f'select * from {csv(dynamic)}',[(2,)],columns=['new'])
check('missing file',f'select * from {csv(data/"missing.csv")}',error=True)
for q in ["select * from read_csv('/etc/passwd')","select * from glob('/etc/*')",f"select * from read_csv('{data}/../../../env/activate.sh')"]:check('path denied',q,error=True)
escape=data/'escape.csv'
if escape.is_symlink():escape.unlink()
escape.symlink_to('/etc/passwd');check('symlink denied',f'select * from {csv(escape)}',error=True)
con=connect()
with con.cursor() as c:
    c.execute('set enable_file_sql=OFF')
    try:c.execute(f'select * from {csv(a)}');results.append({'name':'disabled rejects files','passed':False})
    except pymysql.MySQLError as e:results.append({'name':'disabled rejects files','passed':e.args[0]==1235,'error':str(e)})
    c.execute('select 123');results.append({'name':'ordinary SQL disabled','passed':c.fetchone()==(123,)})
con.close()
for name in ['export.csv','custom.csv','failed.csv','table.csv','columns.csv']:
    f=data/name
    if f.exists():f.unlink()
out=data/'export.csv';check('COPY query',f"copy (select id,name,score from {csv(a)} order by id) to '{out}' (format csv,header,nullstr 'NULL')",[])
check('COPY readback',f'select * from {csv(out)} order by id',[(1,'Alice',98.5),(2,'Bob',87.0),(3,'Chen',90.25)])
before=out.read_bytes() if out.exists() else b'';check('COPY existing target',f"copy (select 1) to '{out}'",error=True);results.append({'name':'target unchanged','passed':out.exists() and out.read_bytes()==before})
custom=data/'custom.csv';check('COPY dialect',f'''copy (select 1 as id,'a|b"c' as text_value,NULL as empty_value) to '{custom}' with (format csv,header true,delimiter '|',quote '"',escape '"',nullstr 'NA')''',[])
check('COPY dialect readback',f'''select * from read_csv('{custom}',delimiter='|',nullstr='NA')''',[(1,'a|b"c',None)])
check('COPY outside root',"copy (select 1) to '/tmp/seekdb-file-sql-outside.csv'",error=True)
failed=data/'failed.csv';check('COPY execution failure',f"copy (select id,json_extract(case when id=2 then 'bad' else '{{}}' end,'$') as doc from {csv(a)}) to '{failed}' (header)",error=True)
results.append({'name':'COPY failure cleanup','passed':not failed.exists() and not list(data.glob('.seekdb-copy-*'))})
check('create table','create table if not exists file_sql_native(id int primary key,name varchar(40))',[]);check('clear table','delete from file_sql_native',[]);check('insert table',"insert into file_sql_native values(1,'a'),(2,'b')",[])
check('COPY table',f"copy file_sql_native to '{data/'table.csv'}' (header)",[]);check('COPY table readback',f"select * from read_csv('{data/'table.csv'}') order by id",[(1,'a'),(2,'b')])
check('COPY columns',f"copy file_sql_native (name,id) to '{data/'columns.csv'}' (header)",[]);check('COPY columns readback',f"select * from read_csv('{data/'columns.csv'}') order by id",[('a',1),('b',2)])
check('ordinary table preserved','select count(*),sum(id) from file_sql_native',[(2,3)])
check('JSON_TABLE preserved',"select v from json_table('[1,2]','$[*]' columns (v int path '$')) j order by v",[(1,),(2,)])
summary={'total':len(results),'passed':sum(bool(r['passed']) for r in results),'failed':[r for r in results if not r['passed']]}
outdir=w/'results/file-sql';outdir.mkdir(parents=True,exist_ok=True)
(outdir/'sql-results.json').write_text(json.dumps(results,default=str,ensure_ascii=False,indent=2)+'\n');(outdir/'sql-summary.json').write_text(json.dumps(summary,default=str,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(summary,default=str,ensure_ascii=False),flush=True);raise SystemExit(int(bool(summary['failed'])))
