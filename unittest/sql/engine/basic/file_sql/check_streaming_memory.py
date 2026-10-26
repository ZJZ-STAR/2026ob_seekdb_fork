"""Measure native aggregate memory while scanning a large CSV."""
import json, time
from pathlib import Path
import pymysql
w=Path('/home/PeterPan/2026ob');data=w/'tests/file-sql/memory';data.mkdir(exist_ok=True)
path=data/'large.csv';rows=1000000
with path.open('w') as f:
    f.write('id,payload\n')
    for i in range(rows):f.write(str(i)+','+'x'*128+'\n')
pid=int((w/'run/seekdb-file-sql.pid').read_text())
def rss():
    for line in (Path('/proc')/str(pid)/'status').read_text().splitlines():
        if line.startswith('VmRSS:'):return int(line.split()[1])*1024
before=rss();started=time.monotonic()
con=pymysql.connect(user='root',unix_socket=str(w/'run/seekdb-file-sql/run/sql.sock'),database='test',autocommit=True,read_timeout=180)
try:
    with con.cursor() as c:
        c.execute('set enable_file_sql=ON');c.execute('set ob_query_timeout=180000000')
        c.execute("select count(*),sum(id) from read_csv('"+str(path)+"')")
        result=c.fetchone();assert result==(rows,rows*(rows-1)//2),result
finally:con.close()
evidence={'rows':rows,'input_bytes':path.stat().st_size,'server_rss_before':before,'server_rss_after':rss(),
          'elapsed_seconds':round(time.monotonic()-started,3),'result':[str(x) for x in result]}
evidence['rss_delta']=evidence['server_rss_after']-before
(w/'results/file-sql/streaming-memory.json').write_text(json.dumps(evidence,indent=2)+'\n')
print(json.dumps(evidence))
