"""Generate small Parquet fixtures with Apache Arrow; queries run in seekdb."""
from pathlib import Path
from datetime import date, datetime
import pyarrow as pa
import pyarrow.parquet as pq

out=Path(__file__).resolve().parent/'data'
out.mkdir(parents=True,exist_ok=True)
table=pa.table({
    'id':pa.array([1,2,3,4,5,6,7],type=pa.int64()),
    'amount':pa.array([1.5,None,3.25,-4.0,5.5,0.0,7.75],type=pa.float64()),
    'flag':[True,False,None,True,False,True,False],
    'label':['alpha','中文','alpha',None,'comma,value','say "hi"',''],
    'day':pa.array([date(2026,1,1),None,date(2024,2,29),date(1970,1,1),date(1969,12,31),date(2000,1,1),date(9999,12,31)],type=pa.date32()),
    'created':pa.array([datetime(2026,1,1,12,30,4,123456),None,datetime(2000,1,1),datetime(1970,1,1),datetime(1969,12,31,23,59,59),datetime(2024,2,29),datetime(2026,10,26)],type=pa.timestamp('us')),
})
for version in ['1.0','2.0']:
    for codec in ['NONE','snappy']:
        pq.write_table(table,out/('flat_v'+version[0]+'_'+codec.lower()+'.parquet'),row_group_size=3,
                       data_page_version=version,compression=codec,use_dictionary=True,
                       version='1.0',data_page_size=128)
pq.write_table(table,out/'flat_plain.parquet',row_group_size=3,compression='NONE',use_dictionary=False,version='1.0')
pq.write_table(table,out/'flat_int96.parquet',row_group_size=3,compression='snappy',use_deprecated_int96_timestamps=True)
pq.write_table(pa.table({'id':pa.array(range(256),type=pa.int64()),'text':['same']*256}),out/'many_pages.parquet',
               row_group_size=100,data_page_size=128,write_batch_size=16,compression='snappy')
pq.write_table(pa.table({'nested':[[1,2],None,[3]]}),out/'nested.parquet')
plain=(out/'flat_plain.parquet').read_bytes()
metadata=pq.ParquetFile(out/'flat_plain.parquet').metadata
offset=metadata.row_group(0).column(1).data_page_offset
corrupt=bytearray(plain);corrupt[offset]=0xff
(out/'projection_corrupt.parquet').write_bytes(corrupt)
pq.write_table(pa.table({'small':pa.array([4294967295,None],type=pa.uint32()),'large':pa.array([18446744073709551615,1],type=pa.uint64()),'empty':pa.nulls(2)}),out/'unsigned_null.parquet',compression='NONE',use_dictionary=False)
print('Generated',len(list(out.glob('*.parquet'))),'Parquet fixtures')
