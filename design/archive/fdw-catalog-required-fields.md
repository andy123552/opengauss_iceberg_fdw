# Iceberg FDW Catalog 必需字段

本文从 `fdw-optimizer-ddl-detail-design.md` 中单独抽出优化器和外表 DDL 依赖的 catalog 必需字段，便于 catalog adapter、type adapter、operator adapter 和 scan executor 对齐接口边界。

代价估算读取的 catalog 字段必须是普通表字段，不能通过规划期临时解析 Iceberg 文件链路获得。

| 表                                       | 字段                           | 使用方                                                     | 说明                                                                                                                     |
| --------------------------------------- | ---------------------------- | ------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `iceberg_catalog.tables_internal`       | `relid`                      | FDW DDL / planner                                       | 绑定 openGauss 外表 OID，internal 表场景用于反查 Iceberg 表。                                                                        |
| `iceberg_catalog.tables_internal`       | `namespace`、`table_name`     | `catalog_adapter` / planner                             | Iceberg 表身份，用于按 foreign table options 定位 catalog 表项。                                                                   |
| `iceberg_catalog.tables_internal`       | `table_uuid`                 | `catalog_adapter` / `type_adapter` / `operator_adapter` | 关联 `table_schemas`、`snapshots`、`partition_specs` 等子表的稳定表 ID；同时作为索引能力查询和 schema/snapshot 一致性校验的表级身份。                    |
| `iceberg_catalog.tables_internal`       | `metadata_location`          | reader / scan executor                                  | 执行期 reader 打开 Iceberg 表的入口。                                                                                            |
| `iceberg_catalog.tables_internal`       | `table_location`             | reader / DML 预留                                         | 表根路径，用于构造数据、delete/delta 或后续写入目标路径；普通只读扫描可不直接消费，但 catalog 解析结果应保留。                                                     |
| `iceberg_catalog.tables_internal`       | `current_schema_id`          | `type_adapter` / `operator_adapter`                     | 当前 schema 版本；用于加载字段映射、校验外表定义，并保证 predicate/vector request 绑定到正确 schema。                                                |
| `iceberg_catalog.tables_internal`       | `current_snapshot_id`        | planner / reader / index                                | 当前 snapshot；用于估行、普通扫描、索引可见性判断和执行期 snapshot 固定。                                                                         |
| `iceberg_catalog.table_schemas`         | `schema_id`、`field_position` | `type_adapter`                                          | 选取当前 schema 的字段列表，并保留列顺序，用于 openGauss `attnum` 与 Iceberg field 的映射。                                                    |
| `iceberg_catalog.table_schemas`         | `field_id`                   | `type_adapter` / `operator_adapter` / index             | Iceberg 字段稳定 ID；predicate、projection、vector search 和索引能力匹配均应使用 field id，而不是只依赖字段名。                                     |
| `iceberg_catalog.table_schemas`         | `field_name`                 | `type_adapter` / EXPLAIN                                | 与 openGauss 外表列名匹配，兼容日志、错误消息和 EXPLAIN 输出。                                                                              |
| `iceberg_catalog.table_schemas`         | `field_required`             | `type_adapter` / `operator_adapter`                     | nullable 语义；用于读路径 NULL 校验、`IS NULL`/`IS NOT NULL` 谓词判断和本地 recheck 策略。                                                  |
| `iceberg_catalog.table_schemas`         | `field_type`                 | `type_adapter` / `operator_adapter`                     | Iceberg 物理类型；用于 openGauss 类型映射、常量编码、谓词下推安全性判断。vector 列的物理类型首期应为 `list<float>` 或等价 Arrow `FixedSizeList<Float32>`。      |
| `iceberg_catalog.table_schemas` 或字段级属性表 | `logical_type`               | `type_adapter` / `operator_adapter`                     | 字段逻辑类型扩展。vector 列应显式标记为 `vector`；普通 Iceberg 类型为空或按原生类型处理。                                                              |
| `iceberg_catalog.table_schemas` 或字段级属性表 | `vector_dim`                 | `type_adapter` / `operator_adapter` / index             | vector 固定维度；用于校验 openGauss `vector(n)` typmod、查询向量维度和向量索引可用性。                                                          |
| `iceberg_catalog.table_schemas` 或字段级属性表 | `vector_element_type`        | `type_adapter`                                          | vector 元素类型；首期必须为 `float32`，避免 `list<double>` 等精度语义变化被静默接受。                                                            |
| `iceberg_catalog.table_schemas` 或字段级属性表 | `vector_metric_default`      | `operator_adapter` / index                              | vector 默认 metric，例如 `l2`、`dot`、`cosine`、`l1`；用于 `<->`、`<#>`、`<=>`、`<+>` 与底层索引 metric 对齐。                               |
| `iceberg_catalog.snapshots`             | `snapshot_id`、`schema_id`    | planner / reader / index                                | 标识 snapshot 及其 schema 版本，用于固定扫描版本、校验索引 snapshot 可见性和 time-travel 预留。                                                   |
| `iceberg_catalog.snapshots`             | `manifest_list`              | reader / scan executor                                  | 当前 snapshot 的 manifest list 入口；若 reader 只消费 `metadata_location`，该字段可作为 catalog 缓存和诊断信息，但不能与 `current_snapshot_id` 不一致。 |
| `iceberg_catalog.snapshots`             | `total_records`              | optimizer                                               | 当前 snapshot 总行数摘要，来自 `snapshots[].summary.total-records`。                                                              |

`total_records` 是普通扫描路径的唯一必需行数统计。若当前 snapshot 没有 `total_records`，FDW 不读取 metadata.json 兜底，改用固定默认行数。

`table_uuid` ：是 `table_schemas`、`snapshots`、`partition_specs` 等 catalog 子表的关联键，并且对应 Iceberg `metadata.json.table-uuid`，比 `relid`、`namespace/table_name` 更适合作为跨 rename、rebind 和索引元数据的一致性身份。如果后续实现确认所有子表都改用 `relid` 或 `(namespace, table_name)` 作为关联键，且不需要对齐 Iceberg 原生 table uuid、外部索引表身份和 external catalog 兼容视图时，则可以替换`table_uuid`。
