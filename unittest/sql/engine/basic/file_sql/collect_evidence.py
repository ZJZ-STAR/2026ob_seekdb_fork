"""Record source/binary hashes and the final test results."""
import hashlib, json, subprocess, xml.etree.ElementTree as ET
from pathlib import Path
w=Path('/home/PeterPan/2026ob');r=w/'src/seekdb';out=w/'results/file-sql'
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()
paths=[]
for command in [['git','diff','--name-only','-z'],['git','ls-files','--others','--exclude-standard','-z']]:
    paths+=subprocess.check_output(command,cwd=r).decode().split('\0')
sources={p:digest(r/p) for p in sorted(set(paths)-{''})}
binary=r/'build_release/src/observer/seekdb'
def gtest(path):
    tree=ET.parse(path).getroot()
    return {k:int(tree.get(k,'0')) for k in ['tests','failures','errors','disabled']}
reader_log=(out/'reader-sanitized.log').read_text()
manifest={
    'branch':subprocess.check_output(['git','branch','--show-current'],cwd=r).decode().strip(),
    'build_base_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r).decode().strip(),
    'build_command':'source env/activate.sh; bash build.sh release --make -j16 seekdb',
    'binary':{'path':str(binary),'bytes':binary.stat().st_size,'sha256':digest(binary)},'sources':sources,
    'parser':gtest(out/'parser/results.xml'),'reader':gtest(out/'reader-results.xml'),
    'sanitizers':{'address':True,'undefined':True,'leak_detection':True,'clean':not any(s in reader_log for s in ['ERROR: AddressSanitizer','runtime error:','ERROR: LeakSanitizer'])},
    'mysqltest':json.loads((out/'mysqltest/summary.json').read_text()),
    'sql_regression':json.loads((out/'sql-summary.json').read_text()),
    'native_edges':json.loads((out/'native-edge-results.json').read_text())['summary'],
    'ai_compatibility':json.loads((out/'ai-compatibility/summary.json').read_text()),
    'streaming':json.loads((out/'streaming-memory.json').read_text())}
(out/'build-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in manifest.items() if k!='sources'},ensure_ascii=False))
