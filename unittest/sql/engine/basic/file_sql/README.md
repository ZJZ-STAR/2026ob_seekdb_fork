# File SQL

`read_csv`、`read_json`／`read_ndjson`、`read_parquet` 和 `glob` 接入 seekdb 的表函数计划。读取器输出行，WHERE、表达式、GROUP BY、HAVING、排序、LIMIT 和 JOIN 由 seekdb 执行。`COPY` 接入原生 SELECT INTO 执行链路。

```sql
SET GLOBAL secure_file_priv = '/home/PeterPan/2026ob/tests/file-sql';
SET SESSION enable_file_sql = ON;

SELECT active, COUNT(*), AVG(score)
FROM read_csv('/home/PeterPan/2026ob/tests/file-sql/data/basic.csv',
              header = true, sample_size = -1)
GROUP BY active;

DESCRIBE SELECT * FROM read_json('/home/PeterPan/2026ob/tests/file-sql/data/basic.jsonl');

SELECT a.id, a.name, b.score
FROM read_csv('/home/PeterPan/2026ob/tests/file-sql/data/basic.csv') a
LEFT JOIN read_json('/home/PeterPan/2026ob/tests/file-sql/data/basic.jsonl') b
  ON a.id = b.id;

COPY (SELECT id, name FROM read_csv('/home/PeterPan/2026ob/tests/file-sql/data/basic.csv'))
TO '/home/PeterPan/2026ob/tests/file-sql/data/export.csv'
(FORMAT CSV, HEADER, NULLSTR 'NULL');
```

## 入口与选项

| 入口 | 选项 |
| --- | --- |
| `read_csv(path, ...)` | `header`、`delimiter`／`delim`、`quote`、`escape`、`nullstr`、`all_varchar`、`sample_size=-1` |
| `read_json(path, ...)`／`read_ndjson(path, ...)` | `format='newline_delimited'`／`'ndjson'`／`'jsonl'`、`sample_size=-1` |
| `read_parquet(path)` | Footer Schema、多个 Row Group、PLAIN／Dictionary、Data Page V1／V2、未压缩／Snappy、INT96、列裁剪 |
| `glob(pattern)` | 返回一列 `file`，按路径排序；用于文件发现 |
| `COPY table [(columns)] TO path [(options)]` | `FORMAT CSV`、`HEADER [true/false]`、`DELIMITER`／`DELIM`、`QUOTE`、`ESCAPE`、`NULLSTR`；也支持 `WITH (...)` |
| `COPY (SELECT ...) TO path [(options)]` | 同上；执行完整的原生 SELECT 计划 |

CSV 默认有表头、逗号分隔、双引号包围和双引号转义，未加引号的空字段为 NULL。加引号的空字符串与 NULL 分开处理。重复表头依次补 `__2`、`__3` 后缀。UTF-8 BOM、LF、CRLF、引号内换行和中文均可读取。

JSONL 按首次出现顺序建立字段并集，缺失值补 NULL，拒绝嵌套对象、数组、重复键和非法标量。JSON 字符串与数字冲突时宽化为 VARCHAR；合法的日期字符串可推断为 DATE／DATETIME。

CSV／JSONL 全量推断 Schema，不缓存文件查询计划。类型包括 NULL、BIGINT、DOUBLE、BOOLEAN、VARCHAR、DATE、DATETIME。BIGINT 与 DOUBLE 合并为 DOUBLE，DATE 与 DATETIME 合并为 DATETIME，其余冲突合并为 VARCHAR。超出有符号 64 位范围的整数保留为 VARCHAR。DATETIME 精度来自实际小数位数，Parquet 时间精度来自 Schema。

## 执行链路

1. `sql_parser_mysql_mode.y` 产生 `T_FILE_TABLE_EXPRESSION` 和 COPY 的 SELECT／INTO 节点。
2. `ObDMLResolver::resolve_file_table_item` 检查开关和路径，推断列名、类型、可空性，建立原生表函数列表达式。
3. `ObLogJsonTable` 携带 `OB_FILE_TABLE_TYPE`，原生优化器构造过滤、聚合、排序和 JOIN。文件表未引用列可以删除。
4. `ObJsonTableOp::open_file_scan` 重校验路径和文件指纹；`next_file_row` 将读取值写入 seekdb Datum。
5. `ob_file_scan_utils.h` 的 `Reader` 负责扫描、批量取数和重扫；CSV／JSONL 保存当前记录，Parquet 保存当前页与字典。
6. `ObSelectIntoOp` 执行 COPY 子计划，`CsvExport` 写入同目录临时文件，成功后通过 `renameat2(RENAME_NOREPLACE)` 发布。

Schema 描述符只携带路径、选项和文件指纹。数据行不保存在解析树或执行计划中。

## 安全与资源

`enable_file_sql` 默认 OFF。Schema 推断、执行打开和 COPY 都检查当前 `secure_file_priv`。路径经真实路径校验后，逐级使用目录文件描述符和 `openat(O_NOFOLLOW)` 打开；输入仅使用只读文件描述符，拒绝 FIFO 等特殊文件。

文件变化检测结合设备号、inode、大小、mtime／ctime、流式内容指纹和 inotify。计划执行前重新校验，扫描与重扫继续检查。glob 监听发现目录的变化。

| 对象 | 上限 |
| --- | --- |
| 列数 | 1024 |
| CSV／JSONL 列名 | 256 字节 |
| 单条 CSV／JSONL 记录 | 16 MiB |
| Parquet Footer／单个页面 | 16 MiB |
| 展开后的字典页面 | 64 MiB |
| Parquet 扫描状态／批次 | 128 MiB |
| 单批行数 | 1024 |
| glob 匹配文件 | 10000 |
| Thrift 嵌套深度／节点数 | 16／200000 |

COPY 目标已存在时失败；查询或写入失败时删除本次临时文件。带引号、分隔符、换行、空字符串和 NULL 文本的输出可由相同选项的 `read_csv` 读回。

## 验证命令

学校服务器工作区为 `/home/PeterPan/2026ob`。先加载 `env/activate.sh`，再进入 `src/seekdb`。

```bash
nice -n 10 bash build.sh release --make -j16 seekdb
ulimit -n 65536
python3 unittest/sql/engine/basic/file_sql/restart_instance.py
python3 unittest/sql/engine/basic/file_sql/run_parser_tests.py
python3 unittest/sql/engine/basic/file_sql/run_reader_tests.py
python3 unittest/sql/engine/basic/file_sql/run_mysqltest.py
python3 unittest/sql/engine/basic/file_sql/test_file_sql.py
python3 unittest/sql/engine/basic/file_sql/test_native_edges.py
```

`run_mysqltest.py` 使用官方 `file_sql_basic.test` 的 SQL 与结果文件；夹具部署和清理改由本地脚本完成。结果保存到 `/home/PeterPan/2026ob/results/file-sql/`。数据库使用独立的 `run/seekdb-file-sql` 和 `data/seekdb-file-sql`。

读取器单测位于 `unittest/sql/engine/basic/test_file_scan_utils.cpp`，Parser 单测位于 `unittest/sql/parser/test_file_sql_parser.cpp`。Parquet 夹具由 `generate_fixtures.py` 使用 Apache Arrow 生成，已随测试提交；SQL 回归直接连接 seekdb。

Parquet 实现依据：[文件格式](https://parquet.apache.org/docs/file-format/)、[数据页](https://parquet.apache.org/docs/file-format/data-pages/)、[编码](https://parquet.apache.org/docs/file-format/data-pages/encodings/)、[Thrift 定义](https://github.com/apache/parquet-format/blob/master/src/main/thrift/parquet.thrift)。
