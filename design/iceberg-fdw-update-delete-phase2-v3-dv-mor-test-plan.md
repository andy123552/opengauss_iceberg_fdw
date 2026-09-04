# Iceberg FDW 更新删除第二阶段：format v3 Deletion Vector MOR 测试方案

版本：v2.0（与阶段二设计同步）  
更新时间：2026-09-03  
适用设计：[阶段二 DV MOR 设计](iceberg-fdw-update-delete-phase2-v3-dv-mor-design.md)

## 1. 目的、范围和完成定义

本方案验证 v3 Deletion Vector（DV）从 SQL DML 到外部 Iceberg reader 的完整闭环，不把 SQL 查询结果正确当作 DV 正确的充分条件。

受测链路：

~~~text
FDW ForeignModify
  -> 行级 BaseRowLocator
  -> _delta mutation overlay
  -> 显式 flush
  -> SDK 合并 DV/遗留 position delete
  -> Puffin deletion-vector-v1
  -> v3 delete manifest + after-image data file
  -> RowDelta validation
  -> Catalog metadata_location CAS
  -> watermark / overlay 清理
  -> 外部 v3 reader
~~~

阶段二完成必须满足：

- v1 拒绝 row-level U/D，v2 保持阶段一 Position Delete 路径，v3 只写 DV；
- 单 data file、跨 data file、跨 row group、已有 DV、遗留 position delete 均正确合并；
- DELETE、UPDATE、连续 mutation、insert-origin、混合 I/U/D、NULL、residual、RETURNING 正确；
- Puffin blob、manifest entry、partition、sequence、row lineage 一致；
- rollback/savepoint、跨会话、故障恢复、幂等重试、rewrite/append/CAS 冲突可验证；
- 全量构建、C ABI 内存/错误路径、全量 devtest 和覆盖率通过；
- 尚未具备的并发或既有 delete file 能力必须显式记录 SKIP。

## 2. 环境、基线和执行入口

### 2.1 物理机基线

所有依赖仓测试在 opengauss-ad 执行：

~~~text
/data/ad/stack/data_infra
├── deps/iceberg-rust-datainfra
├── deps/iceberg-rust-bridge
├── deps/iceberg-index
├── plugins/openGauss-Catalog
├── plugins/iceberg_fdw
├── plugins/iceberg_delta
└── test/DataInfra-devtest
~~~

开始测试前，所有触及仓库执行 git fetch origin --prune 和 git fetch fork --prune，记录远端 tip、本地基线、PR head 和工作树状态。不得使用冲突 rebase 工作树或历史 PR 分支作为隐式依赖。

### 2.2 构建和测试命令

~~~bash
cd /data/ad
BUILD_JOBS=2 INSTALL_JOBS=2 /data/ad/tools/bin/ad-build rust fdw catalog delta verify

cd /data/ad/stack/data_infra/plugins/iceberg_delta/test/integrate
PORT=5434 WAREHOUSE=file:///tmp/iceberg_phase2_v3_warehouse ./20_run_phase2_v3_dv.sh

cd /data/ad/stack/data_infra/test/DataInfra-devtest
export PATH=/home/ad/.rustup/toolchains/stable-aarch64-unknown-linux-gnu/bin:$PATH
./test/run_all.sh
./test/run_all.sh --coverage
~~~

所有长命令写入带 run-id 的日志；记录仓库 SHA、编译产物时间、warehouse、server log、fixture 和结果摘要。clean build 失败时不得回退到旧 .so。

## 3. 统一 fixture 和黑盒工具

### 3.1 表和文件布局

fixture 由 Rust SDK/Bridge 构造，不允许 SQL 伪造 locator。至少创建：

| 表 | format | 目的 |
| --- | ---: | --- |
| phase2_ns.events_v1 | 1 | v1 gate |
| phase2_ns.events_v2 | 2 | 阶段一回归和 v2 兼容 |
| phase2_ns.events_v3 | 3 | DV 主路径 |
| phase2_ns.events_v3_partitioned | 3 | partition/spec/row lineage |

v3 baseline 至少包含两个 data file、每个文件至少两个 row group。fixtures.json 记录 table UUID、format、schema/spec、baseline snapshot、metadata location、data file、partition、position、sequence 和 row lineage。snapshot ID 和 position 使用无损 64 位整数读取，禁止通过 IEEE-754 double 解析。

### 3.2 预置 delete 状态

生成两类 v3 fixture：

1. events_v3_existing_dv：A 已有 DV {1}；
2. events_v3_legacy_pos：B 仍引用 v2 position delete {0}，表 metadata 已升级到 v3。

预置状态必须由 SDK inspection 验证，不能只在 expected 中声明。

### 3.3 Inspection 最小输出

~~~text
table_uuid
format_version
base/current snapshot_id
metadata_location
data file path / partition / sequence
delete manifest: content, file_format, referenced_data_file,
  content_offset, content_size_in_bytes, record_count
Puffin: blob type, referenced-data-file, cardinality,
  snapshot-id, sequence-number, CRC/footer range
row lineage: _row_id, _last_updated_sequence_number
~~~

## 4. P0：版本门禁和 ABI 安全

### P0-01：v1 拒绝 U/D flush

~~~sql
DELETE FROM phase2_ns.events_v1 WHERE id = 1;
SELECT iceberg_delta.flush_table('phase2_ns', 'events_v1');
~~~

预期：返回 feature-not-supported；不写 Puffin、DV、position delete 或 metadata；overlay 按事务规则保留或回滚。

### P0-02：v2 保持 Position Delete

对 events_v2 执行 DELETE/UPDATE 并 flush。

预期：结果与阶段一一致，产生 POSITION_DELETES Parquet，不产生 DV；阶段一原有回归无回归。

### P0-03：v3 不得新增 position delete

对 events_v3 执行 DELETE/flush，trace 必须出现 format_version=3 和 delete_mode=deletion-vector。

预期：capability 完整时进入 DV；能力缺失时在写文件前返回 feature-not-supported；不得降级调用 position-delete writer。

### P0-04：C ABI NULL 和 ownership

覆盖 NULL request、NULL out、malformed JSON、版本/模式不匹配、locator batch 0/1/多行、正常释放和重复释放。

预期：无效输入在任何 SDK/IO 副作用前返回 InvalidArgument；嵌套字符串只能由约定的 batch/result free 释放；ASan/UBSan 无错误。

## 5. P1：DV 写入、合并和读取

### P1-01：单 data file DELETE

DELETE A 中一行并 flush。断言 DV bitmap、manifest content/file_format、referenced_data_file、Puffin offset/size/cardinality、Catalog CAS 和 overlay 清理。baseline snapshot 仍能读旧行，新 snapshot 看不到该行。

### P1-02：已有 DV 合并和重复位置幂等

预置 A 的 DV {1}，本批命中 {1,2}，重复 flush 相同 request_id。

预期：结果为 {1,2}，每个 data file 只有一个 DV；重复位置不增加 cardinality，不产生第二个逻辑 snapshot。

### P1-03：遗留 position delete 合并

v3 表 B 的旧 position delete 为 {0}，本批命中 position 1。

预期：新 DV 为 {0,1}；不新增第二个 position delete；外部 reader 不复活或重复删除行。

### P1-04：多文件、多 row group

一次 DELETE 命中 A/B 两个文件和不同 row group。

预期：每个 data file 最多一个 DV；positions 是原始物理位置，不受过滤后行数影响；manifest、partition、sequence 一一对应。

### P1-05：UPDATE、after-image 和 row lineage

~~~sql
UPDATE phase2_ns.events_v3
   SET status = 'done', note = NULL
 WHERE id = 3
 RETURNING id, status, note;
SELECT iceberg_delta.flush_table('phase2_ns', 'events_v3');
~~~

预期：同一 RowDelta snapshot 包含 id=3 的 DV 和 after-image data file；after-image 继承原 _row_id，更新 sequence 正确；flush 前后 reader 结果符合快照语义。

### P1-06：连续 mutation、insert-origin 和混合操作

覆盖 base UPDATE -> UPDATE -> DELETE、INSERT -> UPDATE、INSERT -> DELETE、同一 flush 的 INSERT + UPDATE + DELETE。

预期：base 行最终只产生一个 DV 位；INSERT -> UPDATE 只写最终 data row；INSERT -> DELETE 不生成湖端 data file/DV；混合操作按 _delta_op 分流并原子提交。

### P1-07：NULL、residual 和 0 行 DML

覆盖 NULL 谓词、length/upper residual、NULL marker/locator 和 0 行 UPDATE/DELETE。

预期：residual 由 openGauss 本地复核；_pos 与 selection 同步；NULL 返回可恢复错误且 overlay 保留；0 行不写 Puffin、不推进 snapshot。

### P1-08：partition 和 schema evolution

至少两个 partition 执行 DELETE、同 partition UPDATE 和跨 partition UPDATE。

预期：DV manifest partition/spec 正确，after-image 落入目标 partition；schema 演进不破坏 UNION/targetlist；SQL、manifest、Puffin、外部 reader 一致。

## 6. P1：事务、并发和故障恢复

### P1-09：rollback、savepoint、跨会话

验证外层 rollback、ROLLBACK TO SAVEPOINT、未提交 mutation 隔离、提交后新事务可见性和 watermark 时机。

### P1-10：data-file rewrite 冲突

会话 A 固定 locator 后暂停，会话 B rewrite/expire 目标 data file，恢复 A flush。

预期：validation 返回 Conflict；overlay 保留；不删除 Puffin/metadata，不按业务键静默重放；重新 scan 后显式重试。

### P1-11：delete/data conflict

构造 conflicting delete file 和 data file，确认 validation flags、base snapshot 和 referenced files 真实透传到 SDK/Bridge trace。

### P1-12：Catalog CAS 冲突

两个 flush 使用同一 old metadata_location，只有一个 CAS 成功。

预期：失败者不清理 overlay/watermark，不执行 delete-after-CAS；未发布文件登记为 orphan candidate。

### P1-13：故障矩阵和幂等重试

依次在 DV writer、manifest validation、RowDelta commit、CAS 前、CAS 后/watermark 前、overlay cleanup 注入失败。

预期：CAS 前 snapshot 不变；CAS 后按 recovery 规则处理；重试最多一个逻辑 DV/RowDelta；所有 orphan 可追踪。

## 7. 阶段 C 已执行映射（2026-09-04）

| 测试 ID | DataInfra-devtest 用例 | 结果 |
| --- | --- | --- |
| P0-01/P0-02/P0-03 | `delta/12_phase1_ud_e2e`、`delta/13_phase1_ud_edges` | PASS |
| P1-01/P1-05 | `delta/12_phase1_ud_e2e`（v3 flush 后外部扫描可见性） | PASS |
| P1-06 | `delta/14_phase2_v3_mutation_matrix`（连续 UPDATE、UPDATE→DELETE、混合操作） | PASS |
| P1-08 | `delta/11_partition_flush`、`delta/13_phase1_ud_edges` | PASS |
| 维护回归 | `catalog/05_phase1_maintenance_e2e`、`06_phase2_expire_snapshots_smoke`、`07_phase2_expire_snapshots_e2e` | PASS |

以下场景尚未具备可宣称的通过证据：INSERT→UPDATE（FDW 尚未为 delta-only 行生成
可用 locator）、已有 DV/遗留 Position Delete 的多文件黑盒合并、并发/CAS 冲突、完整
故障点重试及 Puffin/manifest inspection。必须在后续阶段执行或明确登记 SKIP，不能以
上述重点回归替代。

## 8. P2：兼容、边界和性能

- 移除带 DV 的 data file 后，metadata 不再引用该 DV；Puffin 可由后续 orphan 清理。
- 覆盖 position=0、2^32-1、2^32 和最大允许正 64 位位置；非法负数和最高位为 1 拒绝。
- 使用大表和稀疏 DV 验证 bitmap、Puffin 大小、扫描耗时和并发读；性能结果不能替代正确性。
- 既有 equality delete、branch/tag/reference、S3/MinIO、NotFound 幂等按实际能力执行；缺少 fixture 或 API 时标记 SKIP。

## 9. 覆盖率和三方验收断言

### 8.1 Rust SDK

覆盖 DV 编解码、Roaring 64-bit 分片、Puffin header/footer/CRC/properties、旧 DV + position delete merge、版本 gate、非法位置和 validation。

### 8.2 Bridge C ABI

覆盖 JSON、版本/模式校验、NULL out、副作用前检查、ownership、多文件/partition/sequence、runtime 串行化和 request_id 幂等。

### 8.3 FDW / Delta / Catalog

覆盖 format gate、fdw_private 全链路、residual/recheck、RETURNING、overlay 折叠、watermark、CAS、orphan 和 bridge trace。

### 8.4 运行摘要

每次运行在 artifacts/phase2-ud/<run-id>/summary.md 记录各仓 SHA、构建命令、每个 case 的 PASS/FAIL/SKIP、SQL/RETURNING、snapshot、metadata location、manifest、Puffin、row lineage、trace、server log、orphan、重试和覆盖率。

## 10. 失败判定和禁止做法

以下情况必须判定 FAIL：

- 只验证 SELECT/EXPLAIN，不检查 Puffin/manifest；
- v3 走 position-delete fallback；
- 用业务键、row ID 或过滤后行号伪造 locator；
- 关闭 filter 规避定位错误；
- 通过删除 expected 断言隐藏功能失败；
- CAS 失败后仍删除物理文件；
- 使用旧 .so 或未记录 commit 的产物；
- 将普通 ERROR 日志当作 core，或把 crash 归因于没有 backtrace 的猜测；
- 未执行的并发、既有 delete file 或分区高阶场景标为 PASS。

## 11. 最终验收顺序

~~~text
SDK unit + inspection
  -> Bridge C ABI / ownership / error tests
  -> FDW + Delta targeted SQL
  -> Puffin / manifest / metadata black-box
  -> transaction / concurrency / fault matrix
  -> v2 regression
  -> full devtest watchdog
  -> coverage and summary
~~~
