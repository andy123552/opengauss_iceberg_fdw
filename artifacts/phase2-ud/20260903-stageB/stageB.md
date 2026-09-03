# 阶段二 Stage B 执行记录

日期：2026-09-03  
主机：`opengauss-ad`  
仓库根目录：`/data/ad/stack/data_infra`

## 交付范围

- Bridge：为 format v3 RowDelta 增加 `deletion_vector_inputs` 请求路径。调用方只传递 `data_file_path`、物理位置和分区信息，由 Rust SDK 的 `PuffinWriter` 生成 deletion-vector-v1，并构造带 referenced data file、offset、length、cardinality 的 delete DataFile；保留 v2 Position Delete 兼容路径。
- Delta：按 `_delta_op` 解析并按 data file 聚合位置；v2 继续写 Position Delete，v3 将位置输入交给 Bridge 生成 DV；UPDATE 在同一个 RowDelta 中提交 DV 与 after-image。
- FDW/Catalog：本阶段未改代码。现有 FDW locator/ForeignModify 链路和 Catalog metadata CAS 接口可直接承载该请求；新增 v3 写入逻辑位于 Bridge/Delta 边界。
- Devtest：更新 `delta/12_phase1_ud_e2e.sql`，v3 场景覆盖同一 flush 中的 UPDATE + DELETE，验证 flush 推进 snapshot 并清理 overlay；保留 v1 拒绝和 v2 既有断言，expected 仅同步受影响的行数。

## 提交和 PR

| 仓库 | commit | PR |
| --- | --- | --- |
| `iceberg-rust-bridge` | `20dc91e` | [#112](https://github.com/DataInfraLab/iceberg-rust-bridge/pull/112) |
| `iceberg_delta` | `f0de207` | [#28](https://github.com/DataInfraLab/iceberg_delta/pull/28) |
| `DataInfra-devtest` | `0874f10` | [#66](https://github.com/DataInfraLab/DataInfra-devtest/pull/66) |

三个 PR 已推送到 `andy123552` fork，GitHub 当前未配置可报告的检查项（`gh pr checks` 显示 no checks reported），不能据此宣称 CI 通过。

## 构建和回归

执行：

```text
/data/ad/tools/bin/ad-build rust fdw catalog delta verify
```

结果：`component-rust`、`component-fdw`、`component-catalog`、`component-delta`、`component-verify` 全部 `OK`；最终安装产物来自本次构建。

定向回归（显式使用本次构建的 `GAUSSHOME` 和 Bridge `.so`）：

```text
delta/09_mor_full
delta/09_mor_state_matrix
delta/10_delta_vector_flush
delta/10_test_types_and_delete
delta/11_partition_flush
delta/12_phase1_ud_e2e
```

结果：`passed=6 failed=0 xfailed=0 xpassed=0 skipped=0`。

Bridge 额外执行 `cargo check --offline`，结果通过；Delta `git diff --check` 通过。

## 阶段边界

当前 SDK 扫描器仍有 Puffin DV reader TODO，v3 外部 reader 可见性、既有 DV/Position Delete 合并和黑盒 Puffin 验收属于 Stage C。本阶段只验收 v3 写入、RowDelta 提交、snapshot 推进和 overlay 清理，不把尚未实现的读取能力误报为完成。
