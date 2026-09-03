# 阶段二 U/D 开发与验收执行计划

版本：v2.0  
更新时间：2026-09-03  
设计依据：design/iceberg-fdw-update-delete-phase2-v3-dv-mor-design.md  
测试依据：design/iceberg-fdw-update-delete-phase2-v3-dv-mor-test-plan.md  
经验依据：design/阶段一开发测试问题与修复经验指南.md

本文是阶段二唯一执行清单。后续按阶段 0 -> A -> B -> C -> D 顺序执行；每个复选项都必须记录实际 commit、命令、日志、PASS/FAIL/SKIP。不能用部分 SQL 通过、旧 .so、默认版本或只看 EXPLAIN 代替完整验收。

## 1. 目标和完成定义

目标：在不破坏阶段一 v1/v2 行为的前提下，为 format v3 表实现 Deletion Vector（DV）MOR 的 SQL UPDATE/DELETE、显式 flush、row lineage、并发校验和外部 reader 可见性。

完成定义：

- [ ] v1 拒绝 row-level U/D；v2 继续使用 Position Delete；v3 只写 Puffin deletion-vector-v1。
- [ ] DV writer/reader、旧 DV 合并、遗留 position delete 合并和每 data file 单一 DV 全部通过。
- [ ] DELETE、UPDATE、RETURNING、NULL/residual、连续 mutation、insert-origin、混合 I/U/D 和 partition 全部通过。
- [ ] UPDATE 的 DV 与 after-image data file 位于同一 RowDelta snapshot；row lineage 和 sequence 正确。
- [ ] rollback/savepoint、跨会话、rewrite/delete/data conflict、Catalog CAS、故障恢复和幂等重试通过。
- [ ] Puffin footer、CRC、properties、manifest offset/size/cardinality、partition/spec 和 sequence 三方一致。
- [ ] ABI ownership、NULL out、错误 schema、错误 JSON 和 runtime 边界通过 C ABI 测试。
- [ ] clean build、阶段一回归、阶段二回归、全量 devtest、coverage 全部完成。
- [ ] 未具备的并发或既有 delete file 能力显式记录 SKIP、原因和后续任务。

## 2. 仓库交付边界

| 仓库 | 交付物 |
| --- | --- |
| iceberg-rust-datainfra | DV model、Puffin 编解码、DV merge、v3 RowDelta、row lineage、inspection/unit tests |
| iceberg-rust-bridge | v3 row-delta ABI、错误映射、ownership、runtime 串行化、C ABI tests |
| iceberg_delta | v3 flush 编排、按 data file 分组、watermark、重试、partition/lineage 透传 |
| iceberg_fdw | v3 capability gate、ForeignModify 和 locator/private state 透传 |
| openGauss-Catalog | lock/commit 基线对齐和必要 SQL API |
| DataInfra-devtest | 阶段二 SQL、fixture runner、Puffin/manifest black-box、全量 schedule |
| 根仓 | 设计、测试方案、阶段记录；不 vendor 依赖仓 |

## 3. 阶段 0：基线和规格核对

状态：已完成（2026-09-03；基线记录见 `artifacts/phase2-ud/20260903-stage0/baseline.md`）

### 0.1 物理机和依赖仓

- [ ] 登录 opengauss-ad，确认 gh、git、cargo、ad-build 路径。
- [ ] 对 SDK、Bridge、Index、Catalog、FDW、Delta、Devtest 执行 fetch origin/fork --prune。
- [ ] 记录远端 tip、默认分支、当前分支、HEAD、工作树状态。
- [ ] 隔离历史冲突 rebase 工作树，建立基于最新 origin/main 的干净构建目录。
- [ ] 确认阶段一 PR #14、#107、#111、#45、#48、#26、#27、#62、#65 已合入。
- [ ] 检查 make/gcc/cc1 遗留进程和 free -h。

### 0.2 构建和基线回归

~~~bash
cd /data/ad
BUILD_JOBS=2 INSTALL_JOBS=2 /data/ad/tools/bin/ad-build rust fdw catalog delta verify

cd /data/ad/stack/data_infra/plugins/iceberg_delta/test/integrate
./run.sh
~~~

- [ ] 确认安装产物时间对应本次构建，没有旧 .so 回退。
- [ ] 阶段一用例全部通过；权限 expected normalization 差异独立记录。
- [ ] 输出 artifacts/phase2-ud/<run-id>/baseline.md。

### 0.3 规格和接口矩阵

- [ ] 对照 Apache Iceberg/Puffin 官方规范核对 DV blob、manifest 字段、row lineage 和 v3 禁止新 position delete。
- [ ] 对照 Java RowDelta 列出 Rust SDK 的 validation/commit 范围。
- [ ] 对照阶段一 C ABI 确定 request/result、status、ownership、free、versioning。
- [ ] 明确 Catalog 是 metadata_location 唯一发布者，create_table stub 不用于真实 fixture。
- [ ] 输出已有接口、新接口、明确不实现项和兼容策略。

## 4. 阶段 A：SDK DV 核心和 inspection

状态：已完成（2026-09-03；SDK PR #15）

### A1. DV/Puffin

- [ ] 实现非负 64 位 position 的 Roaring bitmap。
- [ ] 拒绝负数和最高位为 1 的位置。
- [ ] 实现 deletion-vector-v1 Puffin header、length、magic、CRC、footer。
- [ ] 写入 referenced-data-file、cardinality、snapshot-id=-1、sequence-number=-1。
- [ ] 校验 manifest content、file_format、offset、size 和 record_count。
- [ ] 增加 0/1/多位置、重复、逆序、2^32 边界和大位置单测。

### A2. 合并和 RowDelta

- [ ] 读取同一 data file 的既有 DV。
- [ ] 读取并合并升级遗留 position delete。
- [ ] 对重复位置保持幂等，保证每个 data file 一个 DV。
- [ ] 校验 table format、UUID、base snapshot、metadata location、partition/spec。
- [ ] UPDATE after-image 继承 _row_id，更新 _last_updated_sequence_number。
- [ ] 空操作返回 no_op；缺失文件、错误 schema、错误 partition 返回确定性错误。
- [ ] 生成合法 delete manifest、snapshot summary、sequence 和 lineage。

### A3. Fixture、inspection 和单测

- [ ] fixture 生成 v1/v2/v3、已有 DV、遗留 position delete、多文件/row group。
- [ ] inspection 输出 metadata、manifest、Puffin footer/blob、partition、sequence、row lineage。
- [ ] 运行 cargo test --locked 和 release tests。
- [ ] 运行 formatter/locked 检查并记录真实命令。
- [ ] 记录 stageA.md、fixture JSON 和覆盖率基线。

通过标准：SDK 单测全部通过；inspection 能独立重读 Puffin/manifest；错误路径没有文件副作用。

## 5. 阶段 B：Bridge、Delta、FDW、Catalog 集成

状态：已完成（2026-09-03；详见 `artifacts/phase2-ud/20260903-stageB/stageB.md`）

### B1. Bridge v3 ABI

- [ ] 设计版本化 request JSON：format_version、delete_mode、table UUID、base snapshot/location、targets、validation、request_id。
- [ ] 新增 v3 row_delta_commit ABI；调用方不得传 Puffin descriptor。
- [ ] 所有 NULL out、NULL request、错误 JSON 在 SDK/IO 前检查。
- [ ] 统一 nested string ownership；头文件、Rust 注释、free 函数和测试一致。
- [ ] 映射 InvalidArgument、Unsupported、Conflict、NotFound、IO。
- [ ] 使用串行化 runtime 入口，禁止调用方嵌套 block_on。
- [ ] C ABI 覆盖 0/1/多文件、partition、重复 request_id、错误 schema。

### B2. Delta flush

- [ ] 读取 Catalog format version 和基线 metadata/snapshot。
- [ ] 按 _delta_op 分流 I/D/U，按 data file 聚合 positions。
- [ ] 传递真实 partition/spec、data sequence，不伪造 DV descriptor。
- [ ] v2 调用 position-delete，v3 只调用 DV。
- [ ] 同一 RowDelta 提交 DV 和 UPDATE after-image。
- [ ] CAS 成功后推进 watermark、清理 overlay；失败保留 overlay。
- [ ] request_id、batch watermark 和 orphan candidate 支持幂等重试。

### B3. FDW

- [ ] planner/private state 序列化 format version、UUID、base snapshot、metadata hash、locator resno、relation id。
- [ ] executor 消费行级 data_file_path + physical_position，保留真实物理属性号。
- [ ] residual qual 在 openGauss 本地复核，不能关闭 filter。
- [ ] capability 不完整时在 flush 前失败，不产生湖端副作用。
- [ ] RETURNING、NULL、新增列和 partition 完成 fdw_private -> overlay -> bridge 链路。
- [ ] fake bridge 断言真实 request 字段，不只检查 EXPLAIN。

### B4. Catalog

- [ ] lock_table 返回 UUID、format version、current snapshot、metadata location。
- [ ] commit_table 用 UUID + old metadata location 做唯一 CAS。
- [ ] CAS 失败返回 Conflict，不删除物理文件。
- [ ] CAS 成功更新 snapshot/location 后才清理 watermark/overlay。
- [ ] Catalog 不解析 Puffin、不直接删除对象存储。

通过标准：Bridge/Delta/FDW/Catalog 定向测试通过；trace 显示完整 request/validation；v2 旧回归无变化；clean build 通过。

## 6. 阶段 C：端到端、并发和故障验收

状态：TODO

### C1. 版本和 DV 主路径

- [ ] v1 gate 拒绝且无副作用。
- [ ] v2 Position Delete 回归通过且无 DV。
- [ ] v3 basic DELETE 单文件、单 DV、外部 reader 可见。
- [ ] capability 缺失时返回 Unsupported，overlay 保留。
- [ ] 已有 DV、遗留 position delete、多文件/row group 合并通过。

### C2. U/D、schema 和 lineage

- [ ] UPDATE 同一 snapshot 同时含 DV + after-image。
- [ ] RETURNING、NULL/residual、0 行 DML 通过。
- [ ] UPDATE -> UPDATE -> DELETE 折叠通过。
- [ ] INSERT -> UPDATE 只写最终 data row。
- [ ] INSERT -> DELETE 不写湖端文件。
- [ ] 混合 I/U/D 同一 flush 原子提交。
- [ ] partition/spec、schema evolution、MOR UNION targetlist 连续性通过。
- [ ] row lineage 和 sequence 与 metadata/Parquet 一致。

### C3. 事务、冲突和恢复

- [ ] 外层 rollback、savepoint、跨会话隔离通过。
- [ ] data-file rewrite/expire 与 flush 冲突返回 Conflict。
- [ ] conflicting data/delete validation 实际启用且 trace 可见。
- [ ] 两个 flush 同 old metadata 的 CAS 冲突仅允许一个成功。
- [ ] CAS 失败者不清理 overlay/watermark、不删除物理文件。
- [ ] DV writer、manifest validation、RowDelta、CAS 前后、cleanup 六个 fault point 都能重试。
- [ ] 最终最多一个逻辑 DV/RowDelta，未发布文件登记 orphan。

### C4. 黑盒验收

- [ ] 读取 metadata JSON、delete manifest、Puffin footer/blob、data manifest。
- [ ] 校验 referenced-data-file、offset、size、cardinality、CRC、partition/spec、sequence。
- [ ] 校验 baseline 和新 snapshot 的读结果。
- [ ] 校验移除 data file 后 DV entry 不残留。
- [ ] 记录外部 reader 版本和命令。

通过标准：测试方案 P0/P1 全部 PASS；P2 每项有真实 PASS 或明确 SKIP；生成 stageC.md 和完整 metadata/Puffin 清单。

## 7. 阶段 D：全量回归、覆盖率和 PR 收尾

状态：TODO

### D1. 全量回归

- [ ] 执行 ad-build rust fdw catalog delta verify。
- [ ] 运行阶段一 run.sh、10~14 号脚本和阶段二 20_run_phase2_v3_dv.sh。
- [ ] 运行 DataInfra-devtest 默认 watchdog。
- [ ] 运行 opt-in S3/MinIO/重启/并发 schedule，不能把跳过当通过。
- [ ] permission normalization 差异单独记录，不修改 expected 掩盖阶段二结果。
- [ ] 验证加载的是本次 commit 产物。

### D2. 覆盖率

- [ ] SDK DV/merge/Puffin/validation/lineage 函数有目标覆盖率。
- [ ] Bridge status、NULL out、ownership、error 分支全覆盖。
- [ ] Delta op 分流、partition、watermark、retry 全覆盖。
- [ ] FDW planner/executor/private state/RETURNING/residual 全覆盖。
- [ ] Catalog lock/CAS/conflict 全覆盖。
- [ ] 生成 HTML/LCOV 或项目认可格式，列出未覆盖函数和原因。
- [ ] P0/P1 未覆盖代码必须补用例或修复；未实现 P2 登记 SKIP。

### D3. PR 和合入

- [ ] 各仓 feature 分支 rebase 最新 base。
- [ ] 只提交目标代码、测试和必要文档。
- [ ] PR 描述使用中文，写清问题、根因、修改原因、测试命令、结果和剩余限制。
- [ ] 逐条回复 review，附复现和回归证据。
- [ ] push fork，回读 PR head。
- [ ] 执行 gh pr checks；红灯必须修复后再更新。
- [ ] 合入后 fetch main，重跑最小 smoke 和全量 watchdog。
- [ ] issue/PR 关闭前确认没有把 SKIP 或独立测试框架问题误标为完成。

### D4. 最终 summary

在 artifacts/phase2-ud/<run-id>/summary.md 记录：

- [ ] 主机、warehouse、端口、时间；
- [ ] 所有仓库 SHA、构建命令和产物；
- [ ] 每个测试 ID 的 PASS/FAIL/SKIP、SQLSTATE 和错误文本；
- [ ] snapshot、metadata location、manifest、Puffin、row lineage；
- [ ] bridge trace、ownership/错误路径；
- [ ] 并发时序、CAS 行数、watermark、overlay、orphan；
- [ ] 覆盖率报告和未覆盖函数；
- [ ] 已知独立问题及 issue；
- [ ] 阶段二完成结论和阶段三候选项。

## 8. 失败处理闭环

~~~text
1. 保存原始日志和输入 fixture
2. 定位到仓库 -> 函数/ABI -> 输入/快照
3. 写最小复现用例
4. 修复代码或测试契约
5. 运行定向测试
6. clean build
7. 阶段二定向全量
8. 阶段一回归和默认 watchdog
9. 更新 PR、summary 和剩余风险
~~~

禁止用修改 expected、关闭 filter、默认 format version、旧 .so、脏 worktree 或未记录 commit 掩盖问题；没有真实 backtrace 不能把普通 ERROR 当作 core。
