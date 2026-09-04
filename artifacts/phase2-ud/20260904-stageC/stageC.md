# 阶段二阶段 C：端到端验收记录

日期：2026-09-04  
主机：`opengauss-ad`（`/data/ad`）  
warehouse：`file:///data/ad/stack/data_infra/tmp/dt_test_warehouse_<pid>`  
构建：`/data/ad/tools/bin/ad-build rust fdw catalog delta verify`

## 本阶段完成内容

1. SDK 增加 Puffin `deletion-vector-v1` 扫描读取：按 manifest 中的
   `content_offset/content_size_in_bytes` 定位 blob，校验
   `referenced-data-file`，并将 DV 合并到扫描过滤器。没有 blob 范围的旧
   Parquet Position Delete 仍走原路径。
2. `12_phase1_ud_e2e` 增加 v3 flush 后真实表扫描断言：UPDATE after-image
   可见，DELETE 行不会重新出现。
3. `13_phase1_ud_edges` 将过时的“v3 flush 必须拒绝”改为 DV 成功路径，检查
   snapshot 推进、删除结果和 overlay 清理。
4. 新增 `14_phase2_v3_mutation_matrix`，覆盖同一基准行连续 UPDATE、混合
   UPDATE+DELETE 以及 flush 后无旧镜像残留。

## 验证结果

- SDK：`cargo test -p iceberg --lib --offline` —— **1411 passed, 0 failed**。
- 组件构建：`ad-build rust fdw catalog delta verify` —— **通过**，产物已安装到
  `mppdb_temp_install`，没有使用旧 `.so` 回退。
- 阶段二重点回归（12/14）：
  `09_mor_full`、`09_mor_state_matrix`、`10_delta_vector_flush`、
  `10_test_types_and_delete`、`11_partition_flush`、`12_phase1_ud_e2e`、
  `13_phase1_ud_edges`、`14_phase2_v3_mutation_matrix` —— **8/8 PASS**。
- 维护/错误路径回归：`catalog/05_phase1_maintenance_e2e`、
  `catalog/06_phase2_expire_snapshots_smoke`、
  `catalog/07_phase2_expire_snapshots_e2e` —— **3/3 PASS**。
- `delta/08_delta_flush_error` 仍按测试仓 `xfail/known_failures` 记为 **XFAIL**。
  当前失败文本为 phase-1 故障后 MOR 视图仍能看到保留在 overlay 中的 50 行；这
  是该旧测试把“湖端基础数据行数”与“MOR 合并视图行数”混用造成的测试语义问题，
  不是本阶段 DV reader 引入的回归，后续应改为检查 snapshot/metadata 未推进。

## 尚未完成 / 明确 SKIP

- INSERT→UPDATE（insert-origin）仍返回 `UPDATE 0`：FDW 修改扫描目前只扫描已发布
  Iceberg data file，不会把 delta-only 行物化为可更新 locator。该能力属于阶段 C2
  的后续修复项，不能宣称已覆盖。
- 已有 DV 与遗留 Position Delete 的真实多文件黑盒合并、并发 CAS/冲突、故障点
  重试、Puffin footer/CRC 清单和覆盖率报告尚未完成；需在阶段 C 后续或阶段 D 登记
  为独立任务，不能用当前 8/8 回归替代。

## 发布记录

- SDK：`61404d2d`，PR [iceberg-rust-datainfra#15](https://github.com/DataInfraLab/iceberg-rust-datainfra/pull/15)
- Devtest：`c4a76d4`，PR [DataInfra-devtest#66](https://github.com/DataInfraLab/DataInfra-devtest/pull/66)
- Bridge：PR [iceberg-rust-bridge#112](https://github.com/DataInfraLab/iceberg-rust-bridge/pull/112)
- Delta：PR [iceberg_delta#28](https://github.com/DataInfraLab/iceberg_delta/pull/28)
