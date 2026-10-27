# 全文索引性能优化

验证日期：2026-10-27。开发分支：`fts-performance`，基于 `cb268d1ea3cb4c455fa8614e808c0d07b8b88fe4`。本题单独提交，不合并到 `2026-competition`。

同机两轮完整基准的三类等权综合耗时下降 **12.91%**，构建、分词、查询分别下降 **14.50%、4.82%、19.40%**。全文索引基准、数据生成器、计时、评分、基线和 CI 工作流均未修改。

## 瓶颈依据与修改

### IK 字典初始化和逐字符处理

原 `ObIKFTParser::init_dict()` 每次创建解析器，都为 main/quan/stop 三个内置字典加载缓存句柄、建立范围字典包装和分配相应内存。底层 DAT 已有缓存，但这些包装工作仍随文档和 TOKENIZE 调用重复。内置字典内容在进程运行期间不变，适合复用。

`ObFTDictHub` 现在延迟创建一个完整的内置 IK 字典快照，持有三个字典及其 DAT 缓存句柄。互斥锁保护首次构建，acquire/release 原子发布保证读者只看到完整对象；失败时释放部分对象并允许重试。解析器借用只读字典，分词状态和临时结果仍属于各自解析器。快照在所有使用者停止后销毁，且 `deinit_fts()` 提前到字典缓存和全局 KV 缓存销毁之前，避免固定句柄晚于缓存释放。

原 `ObIKCharUtil::classify()` 按字符类型多次解码同一个字符，现在只解码一次，再按相同顺序判定类型。原分词循环中的各处理器重复读取相同上下文，现在一次取得字符、长度和类型，传入既有处理接口。没有改变字符边界、处理器顺序、词典、停用词或 smart/max_word 规则。

相关文件：`src/storage/fts/dict/ob_ft_dict_hub.{h,cpp}`、`src/storage/fts/ob_ik_ft_parser.{h,cpp}`、`src/storage/fts/ik/ob_ik_char_util.h`、`src/observer/ob_server.cpp`。

### 只需要命中数量的查询

原执行计划对 `SELECT COUNT(*) ... WHERE MATCH(...) AGAINST(...)` 仍读取文档长度、文档/词项统计并计算 BM25。执行计划中的 `calc_relevance` 和调用链表明，标量计数不消费这些数值。

优化器只为单表、无子查询、单一标量 `COUNT(*)` 输出，且自然语言 MATCH 作为直接布尔条件、其他表达式不消费列或 MATCH 值的语句关闭 relevance。使用表达式访问器检查 WHERE 之外的相关作用域，再检查各 WHERE 条件，避免遗漏 HAVING、排序、窗口或其他评分消费者。Boolean mode、数值阈值、评分投影、普通行输出及隐式/显式相关度排序仍执行原路径。

自然语言模式中，存在倒排 posting 意味着词频为正，而当前 BM25 实现将 IDF 下限设为正 epsilon。因此 posting 并集与此处的正分命中布尔条件等价。优化只省去评分和为评分提供数据的访问，仍扫描完整倒排、去重文档并执行计数；不缓存查询结果，也没有针对词语、数据量或基准标签的特殊处理。

代码生成和检索迭代器允许不请求 relevance/document length；常规计数使用 DAAT 合并。超过原有词项阈值进入 TAAT 时，也能以集合存在性处理重复文档，保留完整结果、空查询和重扫语义。数值估计只用于分配容量，不限制扫描条数。

相关文件：`src/sql/optimizer/ob_log_plan.cpp`、`src/sql/code_generator/ob_tsc_cg_service.cpp`、`src/sql/das/iter/ob_das_iter_utils.cpp`、`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`、`src/storage/retrieval/ob_{text_daat,text_taat,sparse_taat,text_retrieval_token}_iter.cpp`。

## 完整基准结果

同一台服务器：Intel Xeon Gold 5218 @ 2.30GHz，双路、共 64 逻辑 CPU，Linux 5.15.0-67-generic。两个版本均使用 CMake release 构建，实例参数 `cpu_count=4`、`memory_budget=4G`、`log_disk_size=2G`、`datafile_maxsize=10G`；未绑定独占物理 CPU。

停止本项目主实例后，在同一测试实例、数据目录上依次运行原版 1 → 优化版 1 → 原版 2 → 优化版 2，串行执行。每轮均重新导入 20,000 篇文档、每批 500 行；分词 3,000 次、查询 200 次、3 组采样、30 次预热，`SKIP_LOAD=0`，每轮新建报告文件。四轮之后均用 SIGUSR1 正常关闭，进程退出码均为 0，最后恢复优化版主实例。原始启动参数、负载和二进制哈希见各轮 environment JSON。

下表先分别对两轮原版/优化版的每项耗时取算术平均，再按 `(原版−优化版)/原版` 计算下降率。三类内部取平均，再对三类等权平均，未将九项直接平均。

| 指标 | 原版平均 | 优化版平均 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 24.8535 s | 20.2205 s | 18.64% |
| build_ik_content_sec | 16.6025 s | 13.9845 s | 15.77% |
| build_beng_en_sec | 10.6780 s | 9.7070 s | 9.09% |
| tokenize_ik_avg_ms | 0.18030 ms | 0.16330 ms | 9.43% |
| tokenize_beng_avg_ms | 0.09175 ms | 0.09155 ms | 0.22% |
| query_cn_avg_ms | 2.75085 ms | 2.05130 ms | 25.43% |
| query_beng_avg_ms | 3.91355 ms | 2.97220 ms | 24.05% |
| query_mixed_avg_ms | 3.13860 ms | 2.32665 ms | 25.87% |
| query_limit_avg_ms | 3.52825 ms | 3.44855 ms | 2.26% |

IK 和计数查询与修改方向吻合。BENG 分词 0.22% 接近波动，未修改其分词算法；不能将所有 BENG 构建变化归因于 IK 修改。LIMIT 保留相关度排序，没有关闭评分。

四类查询命中数均为 8001、11000、7332、20，四轮相同。正确性另有完整输出验证，见下一节。

本机原版和优化版用官方固定基线计算都已达到 100/100，说明本机与固定基线环境存在差异。此分数不能证明本次修改使耗时减半；本次可归因的同机对比为上述 **12.91%**，正式成绩以统一 CI 为准。

原始数据：[两轮对比](fts-performance/comparison.json)、[四轮退出记录](fts-performance/paired-runs.json)、[原版 1](fts-performance/paired-before-1.txt)、[优化版 1](fts-performance/paired-after-1.txt)、[原版 2](fts-performance/paired-before-2.txt)、[优化版 2](fts-performance/paired-after-2.txt)。同目录还保留四轮固定基线评分和环境 JSON。

## 功能验证

最终 release 编译通过。新增用例及复现命令见 [FTS 回归说明](../../unittest/storage/fts/README.md)，结果见 [验证摘要](fts-performance/validation-summary.json)。

| 验证 | 结果与覆盖 |
| --- | --- |
| C++ | 4/4；穷举 1,112,064 个 Unicode 标量值与原分类算法比较，非法/截断输入、不支持的排序规则、未初始化字典和重复销毁 |
| 原版/优化版 SQL 差分 | 1760/1760；TOKENIZE 完整输出、词频/文档长度、两种 IK 模式及其他解析器、字符边界、预期错误、并发冷初始化、完整命中 ID、评分、阈值、排序、窗口、聚合和事务 DML |
| 独立集合结果 oracle | 原版和优化版均 200/200；有符号/无符号/复合主键、无主键表、分区表、回表、DML、相关重扫、超过 256 个查询词项 |
| 执行计划与消费者边界 | 16/16；只有符合条件的标量计数关闭 relevance，评分及其他消费者保留 |
| 官方 mysqltest 与新增固定用例 | tokenize_function、parser_properties、simple_query、cn_word、score_elision 共 5/5；新增期望结果从原版录制 |
| 生命周期 | 四次完整 benchmark 后 SIGUSR1 正常退出，均为 0 |

差分测试在每个 DML 阶段 ANALYZE，并对自然语言评分查询预执行两次后记录第三次，使原有基于范围估计的词项统计稳定。记录后的全部 ID 和评分逐项比较，评分保留 10 位小数，没有以命中数替代结果核对。

原版另通过 `create_fts_index_afterward`、`create_table_with_fts_index`。原版 `basic_dml` 在第 1598 行获取 rowkey-doc 辅助表元数据时得到 `No such row`，随后变量替换产生 SQL 1064；已在验证摘要保留原始失败，未修改原有测试期望来掩盖它。该完整用例未计入本次通过数量。

## 复现

使用同配置的专用实例，对原版和本分支分别构建并重启；完整官方基准命令如下，连接参数按实际 Socket/端口调整：

```bash
export MYSQL='mysql -h127.0.0.1 -P2881 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2881 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

正确性差分须先在原版记录 golden，再在本分支比较；并发冷初始化检查应在实例刚启动后运行。所有 SQL 回归在专用测试库执行。原版二进制 SHA-256 为 `51081a3001d0c8f728fc0191d2decbb99ab74b7790c309ad13d643dbee703f49`，本次实测优化二进制为 `17a452158a5aecb7e838eacf52c68ff4bac37b6b55a471f1c256872b972f7adb`。二进制在提交前构建，版本字符串不代表最终提交号；源码/测试文件哈希见同目录 `source-manifest.json`。
