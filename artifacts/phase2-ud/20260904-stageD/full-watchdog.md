# 阶段二阶段 D：全量看护回归记录

日期：2026-09-04  
主机：`opengauss-ad`  
仓库根目录：`/data/ad/stack/data_infra`  
测试仓：`/data/ad/stack/data_infra/test/DataInfra-devtest`  
执行命令：`./run_all.sh`（默认套件，84 个用例）

## 环境校正

首次执行时物理机 `PATH` 未包含 rustup toolchain，fixture 脚本回退到不存在的
`/home/ad/.cargo/bin/cargo`，导致 5 个 FDW 用例拿不到 metadata。未改动代码，
仅在第二次执行中显式加入：

```text
PATH=/home/ad/.rustup/toolchains/1.96-aarch64-unknown-linux-gnu/bin:$PATH
CARGO_BIN=/home/ad/.rustup/toolchains/1.96-aarch64-unknown-linux-gnu/bin/cargo
```

校正后 `scan_2x2`、`parallel_scan_2x2`、`vector_ann_sdk`、`vector_scalar_first`、
`vectorsearch_null` 的 fixture 均可生成并通过。

## 结果

```text
passed=81 failed=2 xfailed=1 xpassed=0 skipped=40
```

- 81 个用例通过，包含全部阶段 C MOR/v3 用例（09--14）、Catalog 维护用例（05--07）
  以及 FDW 分区、索引、向量和并行扫描用例。
- `catalog/04_permission`、`catalog/04_permission_table`：仅输出中的授权者从
  期望的 `@CURRENT_USER@` 变为物理机用户 `ad`，属于既有权限输出归一化问题，
  与本阶段 PR 无关。
- `delta/08_delta_flush_error`：按现有 `xfail` 规则记录为 XFAIL；故障注入后
  overlay 行仍可见（当前实现/断言约束尚未改变），未将其误报为通过。
- 40 个 opt-in 用例按配置跳过（S3 cache、S3 delta、runtime-control 等），不属于
  默认本地套件。

结果文件：物理机
`/data/ad/stack/data_infra/test/DataInfra-devtest/test/results/summary.txt`，
差异文件：`results/regression.diff`。

## 阶段 D 判定

在正确 Rust 环境下，本阶段相关功能没有新增回归；全量套件仍不能标记为“零失败”，
因为保留了 2 个已知权限输出差异和 1 个明确 XFAIL。黑盒 Puffin/DV 元数据字段、
CAS/故障清理及 insert-origin UPDATE 仍按 PR review 意见作为后续工作。
