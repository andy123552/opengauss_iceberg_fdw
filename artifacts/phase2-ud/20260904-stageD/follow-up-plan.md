# 阶段二待处理问题与合并执行计划

日期：2026-09-04  
适用范围：阶段二 v3 Deletion Vector MOR、当前四个待合入 PR 及阶段 D 验收

## 1. 为什么本轮没有 FDW PR

这不是“FDW 永远不需要修改”，而是当前增量的职责边界如下：

1. FDW 阶段一已经具备 `ForeignModify`、物理 `data_file + position` locator、
   `_delta` overlay 记录和 UPDATE after-image 透传。v3 的 SQL DML 仍沿用这条入口。
2. v1/v2/v3 的真正写入分流发生在 `iceberg_delta.flush`：它读取 Catalog 的
   `format-version`，再调用 v2 Position Delete 或 v3 DV RowDelta。Puffin、manifest、
   metadata CAS 和 watermark 不应在 FDW 中实现。
3. 阶段二设计明确禁止 FDW 编码 Puffin、构造 manifest 或直接更新
   `metadata_location`；因此 SDK/Bridge/Delta/Catalog 的当前改动可以不触碰 FDW。
4. 当前物理机 FDW 基线为 `f5aaf02`（`origin/main`），其代码确实包含 locator
   物化和 `ExecForeignUpdate/Delete`，但没有使用 `format_version` 的显式 gate，也
   没有把完整的 UUID、base snapshot、metadata hash 写入 modify private state。

因此，正确结论是：

- 对已经跑通的“v3 表 + FDW locator + Delta flush”窄路径，不需要为 Puffin/DV
  再改 FDW；
- 对阶段二设计中 B3 的 capability gate、基线锚定和私有状态契约，目前仍缺验证，
  不能因为没有 FDW PR 就宣称该部分完成；先按阶段 A 的门禁用黑盒测试确认，缺失时
  再创建 FDW PR。

## 2. 当前待处理问题

| 优先级 | 仓库/PR | 问题 | 处理结果 |
| --- | --- | --- | --- |
| P1 | [SDK #15](https://github.com/DataInfraLab/iceberg-rust-datainfra/pull/15) | `content_offset/content_size_in_bytes` 的负值被 `try_from(...).ok()` 静默变成 `None`，可能误分类为 Position Delete；DV reader 还缺 snapshot/sequence 属性校验 | 修复输入校验并补单测 |
| P1 | [Bridge #112](https://github.com/DataInfraLab/iceberg-rust-bridge/pull/112) | Puffin 先写后 CAS，失败路径没有清理或 orphan 登记；分区表缺 `partition_json` 时使用空 Struct | 写前校验、失败清理/登记、补 fault test |
| P2 | [Delta #28](https://github.com/DataInfraLab/iceberg_delta/pull/28) | 多位置/重复位置/多分区文件的 DV 聚合缺黑盒证据；CAS/RowDelta 失败后的 orphan/watermark 语义未验证 | 补聚合、并发和失败恢复测试 |
| P2 | [Devtest #66](https://github.com/DataInfraLab/DataInfra-devtest/pull/66) | 现有用例主要断言 SQL 可见性，未检查 manifest/Puffin footer；INSERT→UPDATE（insert-origin）仍不支持 | 增加 metadata black-box；不把未支持能力伪装成通过 |
| 环境 | Devtest runner | fixture helper 依赖 Cargo 在 `PATH` 中；物理机默认 PATH 找不到 `/home/ad/.cargo/bin/cargo` | 固化 rustup Cargo 环境或 helper 自动探测 |
| 已知独立项 | Catalog | 两个权限用例授权者显示为物理用户 `ad` | 保留记录，不修改 expected 掩盖问题 |
| 已知 XFAIL | Delta | `delta/08_delta_flush_error` 故障注入后 overlay 可见性仍不满足旧断言 | 保持 XFAIL，另立修复任务 |

## 3. 合并后的执行步骤

### 阶段 A：基线、环境与 FDW 门禁一次完成

1. 在 `opengauss-ad` 对 SDK、Bridge、Delta、FDW、Catalog、Devtest fetch 最新
   upstream/fork，核对当前 PR head 和基线；不得覆盖未提交修改。
2. 固化 Devtest 的 Rust 环境（优先 source `$HOME/.cargo/env`；否则按 rustup
   toolchain 自动选择 `cargo`，并让 helper 与 runner 使用同一目录）。
3. 运行 v1/v2/v3 的 FDW DML 定向用例，检查 `fdw_private -> locator -> overlay ->
   flush`，同时验证：v1 拒绝、v2 仍走 Position Delete、v3 只走 DV。
4. 增加一个私有状态/能力缺失测试：若 FDW 必须在 flush 前拒绝却当前没有 gate，
   则在 FDW 创建独立 PR；若 Delta 已能在所有入口可靠拒绝且 FDW 不持有该状态，
   记录“无需 FDW 代码变更”的证据并关闭该分支。

交付物：FDW 是否需要修改的结论、环境修复提交（如需）、阶段 A 定向结果。

### 阶段 B：SDK 与 Bridge 核心修复合并处理

1. SDK 严格拒绝负值、零值、半缺失的 DV manifest 范围；校验
   `referenced-data-file`、cardinality、snapshot/sequence 约束；补 malformed Puffin
   和 reader 单元测试。
2. Bridge 在写 Puffin 前验证分区表必需的 `partition_json`；RowDelta/CAS 失败时
   清理临时文件或登记可追踪 orphan，保证不会产生未发布且无记录的对象；补 CAS、
   IO、分区和重复 request fault tests。
3. 在物理机运行 SDK 全量单测、Bridge/FDW/Catalog/Delta 组件构建；只把目标修改
   推回现有 PR，不混入 cleancode 或无关格式化。

交付物：PR #15、#112 的更新 head、单测/构建日志、对 review 意见的逐条回复。

### 阶段 C：Delta 聚合、恢复与 Devtest 黑盒合并处理

1. Delta 增加同一 data file 多位置、重复位置、多 data file/多 partition 的黑盒
   验证，确认每个 data file 一个逻辑 DV、cardinality 去重、partition/spec 正确。
2. 注入 RowDelta/CAS 前后失败，验证 snapshot 不推进、overlay/watermark 保留、
   orphan 可追踪、重试不重复提交。
3. Devtest 增加 metadata/manifest/Puffin footer 检查：DV blob 类型、引用 data file、
   offset/size/cardinality、sequence/partition；保留 INSERT→UPDATE 为明确的
   unsupported/后续 issue，不修改 expected 伪造通过。

交付物：PR #28、#66 的更新 head，黑盒 inspection 结果和失败恢复日志。

### 阶段 D：全量验收、覆盖率与 PR 闭环

1. 使用正确 Rust 环境执行 `ad-build rust fdw catalog delta verify`，再执行阶段一、
   阶段二定向用例和 Devtest 默认全量；最后按资源条件执行 opt-in S3/重启/并发套件。
2. 生成 SDK、Bridge、Delta、FDW、Catalog 覆盖率，P0/P1 未覆盖分支必须补用例或
   修复；P2 暂不能运行的场景登记原因和 issue。
3. 对四个 PR rebase 最新 base，更新中文描述（问题、根因、修改理由、测试结果、
   剩余限制），逐条回复本阶段 review；修复后重新回读 diff、head、评论和 CI。
4. 只有 P1 意见关闭、SDK CI 完成、黑盒元数据检查通过、全量结果中仅剩已登记的
   权限差异/XFAIL/SKIP 时，才可将阶段二标记为完成并进入合入顺序。

## 4. 当前验收门槛

- 已完成：组件构建、阶段 C 定向回归、正确环境下默认全量 81 PASS。
- 尚未完成：四个 PR 的 review 修复复检、黑盒 Puffin/manifest、CAS/orphan 故障
  测试、覆盖率、SDK CI、FDW gate 是否需要代码变更的最终判定。
- 不得把 Cargo 环境修正、权限输出差异或 XFAIL 当成功能修复，也不得通过修改
  expected 文件隐藏新失败。
