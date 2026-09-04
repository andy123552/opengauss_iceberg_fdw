# 阶段二阶段 D：待合入 PR 全量检视记录

日期：2026-09-04  
检视方式：物理机 `opengauss-ad` 使用 `/home/ad/.local/bin/gh`，对 PR head、diff、历史 review 和检查状态进行回读。

## 检视范围

| 仓库 | PR | HEAD | 状态 |
| --- | ---: | --- | --- |
| iceberg-rust-datainfra | [#15](https://github.com/DataInfraLab/iceberg-rust-datainfra/pull/15) | `61404d2d` | OPEN，Rust checks queued |
| iceberg-rust-bridge | [#112](https://github.com/DataInfraLab/iceberg-rust-bridge/pull/112) | `20dc91e5` | OPEN，无 checks |
| iceberg_delta | [#28](https://github.com/DataInfraLab/iceberg_delta/pull/28) | `f0de2074` | OPEN，无 checks |
| DataInfra-devtest | [#66](https://github.com/DataInfraLab/DataInfra-devtest/pull/66) | `c4a76d49` | OPEN，无 checks |

检视时参考了已合入 PR 中关于行级 locator、分区 delete、FFI ownership、NULL out、错误副作用、混合 I/U/D 和测试不得只改 expected 的历史意见。

## 已提交的 review 意见

- SDK #15：P1——负的 `content_offset/content_size_in_bytes` 被 `ok()` 静默转换为 `None`，可能把非法 Puffin DV 误分类为 Parquet Position Delete；P2——DV reader 未校验 snapshot/sequence 属性。
- Bridge #112：P1——Puffin 先于 RowDelta/CAS 写出，失败时无清理或 orphan 登记；P1——分区表缺失 `partition_json` 时仍使用空 Struct，可能先产生物理文件再失败。
- Delta #28：P2——需补同一 data file 多位置/重复位置去重、分区多文件和故障后 orphan/watermark 保留的黑盒证据。
- Devtest #66：P2——当前只验证 SQL 可见性，未核对 metadata/manifest/Puffin footer；INSERT→UPDATE（insert-origin）仍未支持，应显式关联后续任务。

上述意见已分别以 `gh pr review --comment` 发布，GitHub 回读确认作者为 `andy123552`、状态为 `COMMENTED`。当前没有声明这些意见已修复，也没有关闭 PR。

## 检视结论

- 四个 PR 均显示 `MERGEABLE`，但 SDK CI 仍处于 queued/pending，其他三个 PR 没有 checks，不能据此宣称通过全量验收。
- 物理机组件构建和阶段 C 定向回归已通过。阶段 D 默认全量看护在补齐 rustup Cargo 路径后为 81 PASS、2 个既有权限输出差异、1 个预期 XFAIL、40 个 opt-in SKIP；详见同目录 `full-watchdog.md`。
- 黑盒 DV 元数据、CAS/并发、故障恢复和覆盖率仍是阶段 D 后续工作；在这些 review 意见闭环且 SDK CI 完成前，不应合入或将阶段 D 标记为全部通过。
