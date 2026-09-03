# Phase 2 v3 DV/MOR 阶段 0 基线记录

执行日期：2026-09-03  
执行主机：`opengauss-ad`  
执行根目录：`/data/ad/stack/data_infra`

## 依赖基线

| 组件 | 分支 | 基线提交 |
| --- | --- | --- |
| Rust SDK | `codex/phase2-dv-sdk` | `9bb514bb9325ca238e84714b2f1395f7adafcfdb` |
| Rust Bridge | `codex/phase2-dv-bridge` | `0b068d6568c8660a7b8c88ee3d23aee876e890f8` |
| Index | `codex/phase2-dv-index` | `4fe9aa167d7504cb8352907e2ad82075484805f6` |
| Catalog | `codex/phase2-dv-catalog` | `b1c480650c98f96710a4ad8a1e65473ca9071552` |
| FDW | `codex/phase2-dv-fdw` | `f5aaf021a94ac13b39e6c58ce305412a68475268` |
| Delta | `codex/phase2-dv-delta` | `5302fef57ecf76ea86b35200899180d9b262943b` |
| Devtest | `codex/phase2-dv-devtest` | `dc18ddb87cf80041910eb1c67d8afadcf32b5a21` |

所有工作树均从对应最新 `origin/main`（Delta 为 `origin/master`）创建，创建时工作区干净。

## 构建与回归

- `/data/ad/tools/bin/ad-build rust fdw catalog delta verify`：通过（`BUILD_RC=0`）。
- 阶段一集成回归：通过（`PHASE1_RC=0`）。
- 阶段一覆盖的 hook、delta flush、MOR、SDK fixture、基础/高级/边界 U/D、事务与故障、元数据、NULL overlay、分区 U/D 均通过。
- 已知启动提示 `The core dump path is an invalid directory` 未触发崩溃，属于环境提示，不影响本次回归。

日志：物理机 `/tmp/phase2_stage0_build_20260903.log`、`/tmp/phase2_stage0_phase1_20260903.log`。

## 阶段 A API 矩阵

| 能力 | 基线状态 | 阶段 A 结果 |
| --- | --- | --- |
| Roaring 64 位删除向量 | SDK 已有内存模型和合并迭代 | 增加公开 API 与范围校验 |
| Puffin 通用读写 | SDK 已支持通用 Blob | 增加 DV Blob 写入及读取校验 |
| `deletion-vector-v1` framing | 基线缺失 | 实现长度、魔数、Roaring portable、CRC-32 |
| DV 属性 | 基线无专用构造 | 写入 `referenced-data-file`、`cardinality`，snapshot/sequence 为 -1 |
| v3 RowDelta | 基线拒绝全部 v3 row delete | 增加 `add_deletion_vectors`，拒绝 v3 新 position delete，并校验 Puffin/引用/offset/length |
| v2 兼容 | 已有 position-delete API | 保持原接口和 v2 行为；v2 使用 DV 时明确报不支持 |

规范依据：Apache Iceberg Format Specification 与 Puffin Specification 的 deletion-vector-v1 定义。

## 阶段 A 定向测试

- `cargo test -p iceberg delete_vector::tests:: --offline`：8 passed。
- `cargo test -p iceberg puffin::writer::tests::test_write_and_read_deletion_vector_blob --offline`：1 passed。
- `cargo check -p iceberg --offline`：通过。

