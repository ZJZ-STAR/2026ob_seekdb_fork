"""Start/restart only the isolated File SQL validation instance."""
import os, signal, subprocess, time
from pathlib import Path
import pymysql

workspace=Path('/home/PeterPan/2026ob')
base=workspace/'run/seekdb-file-sql'
pid_file=workspace/'run/seekdb-file-sql.pid'
logs=workspace/'logs/file-sql'
logs.mkdir(parents=True,exist_ok=True)
if pid_file.exists():
    pid=int(pid_file.read_text())
    process=Path('/proc')/str(pid)
    if process.exists() and (process/'stat').read_text().split()[2]!='Z':
        assert process.stat().st_uid==os.getuid()
        assert ('--base-dir='+str(base)).encode() in (process/'cmdline').read_bytes().split(b'\0')
        os.kill(pid,signal.SIGTERM)
        for _ in range(45):
            if not process.exists() or (process/'stat').read_text().split()[2]=='Z':break
            time.sleep(1)
        else:raise RuntimeError('File SQL instance did not stop')
command=[str(workspace/'src/seekdb/build_release/src/observer/seekdb'),'--nodaemon',
         '--base-dir='+str(base),'--data-dir='+str(workspace/'data/seekdb-file-sql'),
         '--redo-dir='+str(workspace/'data/seekdb-file-sql/redo'),
         '--parameter=mysql_port_mode=random','--parameter=rpc_port=12885',
         '--parameter=cpu_count=4','--parameter=memory_budget=4G',
         '--parameter=datafile_maxsize=10G','--parameter=log_disk_size=2G']
with (logs/'startup.log').open('w') as out:
    child=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
pid_file.write_text(str(child.pid)+'\n')
for _ in range(55):
    if child.poll() is not None:raise RuntimeError('File SQL instance exited')
    try:
        connection=pymysql.connect(user='root',unix_socket=str(base/'run/sql.sock'),connect_timeout=1)
        with connection.cursor() as cursor:
            cursor.execute('set global ob_query_timeout=120000000')
            cursor.execute('select 1')
        connection.close()
        print('File SQL instance ready',flush=True)
        break
    except pymysql.MySQLError:time.sleep(1)
else:raise RuntimeError('File SQL instance not ready')
