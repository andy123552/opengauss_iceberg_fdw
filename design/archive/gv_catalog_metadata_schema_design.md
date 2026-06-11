# openGauss Iceberg Catalog 元信息表设计

## 1. 文档目标

本文定义 openGauss 首期 Iceberg Catalog 元信息表设计，聚焦以下内容：

1. Iceberg 原生元信息的结构与边界。
2. 哪些元信息落表，哪些不落表。
3. 各表的建表语句、字段含义、与 Iceberg 元数据文件的对应关系。
4. 待确认项及其适用场景。

本文只讨论 Schema 设计，不展开升级机制、执行流程和运维细节。

## 2. 设计范围

本文覆盖以下元信息对象：

| 对象 | 设计范围 |
| --- | --- |
| Namespace | Namespace 标识及扩展属性 |
| Internal Table | 本地 relation 绑定表的目录记录、当前 metadata 指针和顶层高频摘要 |
| Schema | `schemas[]` 的版本与字段定义 |
| Partition Spec | `partition-specs[]` 的版本与字段定义 |
| Snapshot | `snapshots[]` 的高频摘要 |

首期 internal 场景采用 database 级 Catalog 隔离：一个 database 对应一个 internal catalog，`catalog_name` 固定为 `current_database()`。同一 database 内不支持多个 internal catalog。

首期不包含：

1. 审计体系。
2. 业务索引元信息。
3. Manifest / Data File 明细落表。
4. 完整回滚与历史恢复机制。

此外，本文在后文补充 Spark / PyIceberg 通过 JDBC Catalog 直连 openGauss 场景下的 external 目录表扩展设计。

## 3. 设计原则

1. `metadata.json` 及其引用链路是唯一权威元信息。
2. 数据库只缓存高频、稳定、结构化的摘要信息。
3. 能直接展开的高频结构优先展开，不保留整段 JSON。
4. Schema 设计优先围绕 Iceberg 原生元信息展开。
5. Internal / external 目录记录物理分表，避免在本地 relation 绑定模型中混入大量空字段。

## 4. Iceberg 元信息与缓存边界

### 4.1 文件链路

```mermaid
flowchart TB
    catalog["Catalog 表项<br/>namespace、table_name 与 metadata_location 的映射"]
    metadata["metadata.json"]
    current_snapshot["current-snapshot-id"]
    snapshots["snapshots[]"]
    snapshot["current snapshot"]
    manifest_list["manifest-list file"]
    manifests["manifest files"]
    data["data files"]
    delete["delete files"]

    catalog --> metadata
    metadata --> current_snapshot
    metadata --> snapshots
    current_snapshot --> snapshot
    snapshots --> snapshot
    snapshot --> manifest_list
    manifest_list --> manifests
    manifests --> data
    manifests --> delete
```

### 4.2 元信息分类

从 Schema 设计视角，可将 Iceberg 元信息分为以下几类：

| 分类 | 典型内容 | 设计处理方式 |
| --- | --- | --- |
| Catalog 对象元信息 | `catalog_name`、Namespace、Internal Table identifier、`metadata_location` 及兼容字段 | 独立落表 |
| 表级顶层摘要 | `table-uuid`、`location`、`last-column-id`、当前版本指针 | 缓存到 `tables_internal` |
| 高频版本结构 | `schemas[]`、`partition-specs[]`、`snapshots[]` | 拆分为结构化子表 |
| 开放或低频顶层信息 | `properties`、`sort-orders`、`refs`、`snapshot-log`、`metadata-log` | 不独立落表，保留在完整 metadata 中 |
| 下层文件链路信息 | Manifest List 内容、Manifest File、Data File、Delete File 明细 | 首期不落表 |

### 4.3 原生元信息落表去向总览（Internal 表）

| 原生字段 | 处理方式 |
| --- | --- |
| `table-uuid` | 缓存到 `tables_internal.table_uuid` |
| `location` | 缓存到 `tables_internal.table_location` |
| `last-column-id` | 缓存到 `tables_internal.last_column_id` |
| `schemas` | 展开到 `table_schemas` |
| `schemas[].identifier-field-ids` | 不落表，保留在完整 metadata 中 |
| `current-schema-id` | 缓存到 `tables_internal.current_schema_id` |
| `partition-specs` | 展开到 `partition_specs` |
| `default-spec-id` | 缓存到 `tables_internal.default_spec_id` |
| `current-snapshot-id` | 缓存到 `tables_internal.current_snapshot_id` |
| `snapshots` | 高频摘要缓存到 `snapshots` |
| `snapshots[].summary.total-records` | 缓存到 `snapshots.total_records` |
| `snapshots[].parent-snapshot-id` | 不落表，保留在完整 metadata 中 |
| `snapshots[].sequence-number` | 不落表，保留在完整 metadata 中 |
| `snapshots[].first-row-id` | 不落表，保留在完整 metadata 中 |
| `format-version` | 不落表，保留在完整 metadata 中 |
| `last-updated-ms` | 不落表，按需识别 |
| `last-partition-id` | 不落表，按需识别 |
| `last-sequence-number` | 不落表，按需识别 |
| `sort-orders` / `default-sort-order-id` | 不落表，保留在完整 metadata 中 |
| `properties` | 不落表，保留在完整 metadata 中 |
| `snapshot-log` / `metadata-log` / `refs` | 不落表，保留在完整 metadata 中 |
| `statistics` / `partition-statistics` / `next-row-id` | 不落表，按需识别 |
| Manifest List 内容 / Manifest File / Data File / Delete File 明细 | 不落表，按需识别 |

说明：`previous_metadata_location` 属于 JDBC Catalog 兼容字段，不对应 `metadata.json` 原生字段，因此不在本节单列。Spark / PyIceberg 通过 JDBC Catalog 直连 openGauss 的 external 表扩展场景放在后文单独说明。

## 5. 表设计

| 表 | 简短说明 |
| --- | --- |
| `namespaces` | 保存 Namespace 标识及其扩展属性。 |
| `tables_internal` | 保存本地 relation 绑定表的目录记录、当前 metadata 指针和顶层高频摘要。 |
| `table_schemas` | 保存 `schemas[]` 的版本与字段定义。 |
| `snapshots` | 保存 `snapshots[]` 的高频摘要信息。 |
| `partition_specs` | 保存 `partition-specs[]` 的版本与字段定义。 |

### 5.1 `namespaces`

```sql
CREATE TABLE iceberg_catalog.namespaces (
    catalog_name TEXT NOT NULL,
    namespace    TEXT NOT NULL,
    properties   JSONB NOT NULL DEFAULT '{}'::JSONB,
    PRIMARY KEY (catalog_name, namespace),
    CHECK (jsonb_typeof(properties) = 'object')
);
```

`namespaces` 对应 Catalog 层 Namespace 对象，不对应 `metadata.json` 内部结构。

| 字段 | 类型 | 含义 | Iceberg 对应 |
| --- | --- | --- | --- |
| `catalog_name` | `TEXT` | Catalog 标识 | JDBC Catalog identifier |
| `namespace` | `TEXT` | Namespace 唯一标识 | Namespace name |
| `properties` | `JSONB` | Namespace 扩展属性 | Namespace properties |

说明：

1. `properties` 以 JSONB 对象保存 Namespace 级扩展属性，当前不拆分为属性明细表。
2. `catalog_name` 用于统一 internal 与 external 的 Namespace 维度；internal 主线采用 database 级 Catalog 隔离，固定使用 `current_database()` 作为 catalog 标识。
3. external 扩展场景由外部客户端按目标 catalog 写入 `catalog_name`。
4. 该字段语义与 `pg_lake.namespace_properties.catalog_name` 保持一致，但当前方案采用 JSONB 聚合存储，而非 key-value 行模型。

### 5.2 `tables_internal`

```sql
CREATE TABLE iceberg_catalog.tables_internal (
    relid                 REGCLASS NOT NULL,
    namespace             TEXT NOT NULL,
    table_name            TEXT NOT NULL,
    table_uuid            UUID NOT NULL,
    metadata_location     TEXT NOT NULL,
    previous_metadata_location TEXT,
    table_location        TEXT NOT NULL,
    last_column_id        INT NOT NULL,
    current_schema_id     INT,
    current_snapshot_id   BIGINT,
    default_spec_id       INT,
    PRIMARY KEY (namespace, table_name),
    UNIQUE (table_uuid),
    UNIQUE (relid)
);
```

`tables_internal` 对应一张本地 relation 绑定 Iceberg 表的顶层 `metadata.json` 摘要，以及 Catalog 层表标识。

| 字段 | 类型 | 含义 | Iceberg 对应 |
| --- | --- | --- | --- |
| `relid` | `REGCLASS` | 本地 openGauss relation 标识 | 无 |
| `namespace` | `TEXT` | 表所属 Namespace | Table identifier.namespace |
| `table_name` | `TEXT` | 表名 | Table identifier.name |
| `table_uuid` | `UUID` | 表稳定唯一标识 | `metadata.json.table-uuid` |
| `metadata_location` | `TEXT` | 当前 metadata 文件指针 | Catalog pointer |
| `previous_metadata_location` | `TEXT` | 上一版本 metadata 文件指针 | JDBC Catalog 兼容字段 |
| `table_location` | `TEXT` | 表根路径 | `metadata.json.location` |
| `last_column_id` | `INT` | 当前最大列 ID | `metadata.json.last-column-id` |
| `current_schema_id` | `INT` | 当前 Schema 指针 | `metadata.json.current-schema-id` |
| `current_snapshot_id` | `BIGINT` | 当前 Snapshot 指针 | `metadata.json.current-snapshot-id` |
| `default_spec_id` | `INT` | 当前默认 Partition Spec 指针 | `metadata.json.default-spec-id` |

说明：

1. `relid` 是 internal 表与本地 openGauss relation 的绑定标识，必须由受控 DDL、drop 或 unregister 路径维护，避免 relation 被绕过删除或重建后出现悬空引用。
2. 参照 `pg_lake`，`tables_internal` 不保存 `catalog_name`，internal catalog 在兼容视图中固定投影为 `current_database()`。
3. `previous_metadata_location` 用于对齐 JDBC Catalog 表形状；`current_schema_id`、`current_snapshot_id`、`default_spec_id` 为当前版本指针，对应版本记录的存在性由写入事务保证，不在本表中额外建立复合外键。
4. `last_column_id` 用于维护 Iceberg 字段 ID 分配边界，支撑首期加列场景。

### 5.3 `table_schemas`

```sql
CREATE TABLE iceberg_catalog.table_schemas (
    table_uuid       UUID NOT NULL,
    schema_id        INT NOT NULL,
    field_position   INT NOT NULL,
    field_id         INT NOT NULL,
    field_name       TEXT NOT NULL,
    field_required   BOOLEAN NOT NULL,
    field_type       TEXT NOT NULL,
    field_doc        TEXT,
    PRIMARY KEY (table_uuid, schema_id, field_position),
    UNIQUE (table_uuid, schema_id, field_id),
    FOREIGN KEY (table_uuid)
        REFERENCES iceberg_catalog.tables_internal(table_uuid)
        ON DELETE CASCADE,
    CHECK (field_position >= 0)
);
```

`table_schemas` 对应 `metadata.json.schemas[]` 中的单个 Schema 对象。完整对象通常包含：`type`、`schema-id`、`identifier-field-ids`、`fields[]`；其中 `fields[]` 包含：`id`、`name`、`required`、`type`、`doc`。

| 字段 | 类型 | 含义 | Iceberg 对应 |
| --- | --- | --- | --- |
| `table_uuid` | `UUID` | 所属表 | `metadata.json.table-uuid` |
| `schema_id` | `INT` | Schema 版本 ID | `metadata.json.schemas[].schema-id` |
| `field_position` | `INT` | 字段顺序 | `metadata.json.schemas[].fields[]` 顺序 |
| `field_id` | `INT` | 字段稳定 ID | `metadata.json.schemas[].fields[].id` |
| `field_name` | `TEXT` | 字段名 | `metadata.json.schemas[].fields[].name` |
| `field_required` | `BOOLEAN` | 必填标记 | `metadata.json.schemas[].fields[].required` |
| `field_type` | `TEXT` | 类型字符串表示 | `metadata.json.schemas[].fields[].type` |
| `field_doc` | `TEXT` | 字段说明 | `metadata.json.schemas[].fields[].doc` |

说明：

1. `field_type` 保存 Iceberg 字段类型的字符串表示，当前不对复杂类型继续递归拆表。
2. `table_schemas` 只展开顶层字段定义，不保存 `identifier-field-ids`。
3. 默认每个 Schema 至少包含一条实际字段记录，不为空 Schema 单独设置占位记录。

### 5.4 `snapshots`

```sql
CREATE TABLE iceberg_catalog.snapshots (
    table_uuid          UUID NOT NULL,
    snapshot_id         BIGINT NOT NULL,
    schema_id           INT,
    timestamp_ms        BIGINT NOT NULL,
    manifest_list       TEXT,
    total_records       BIGINT,
    PRIMARY KEY (table_uuid, snapshot_id),
    FOREIGN KEY (table_uuid)
        REFERENCES iceberg_catalog.tables_internal(table_uuid)
        ON DELETE CASCADE
);
```

`snapshots` 对应 `metadata.json.snapshots[]` 中的单个 Snapshot 对象。完整对象通常包含：`sequence-number`、`snapshot-id`、`schema-id`、`parent-snapshot-id`、`timestamp-ms`、`manifest-list`、`summary`。

| 字段 | 类型 | 含义 | Iceberg 对应 |
| --- | --- | --- | --- |
| `table_uuid` | `UUID` | 所属表 | `metadata.json.table-uuid` |
| `snapshot_id` | `BIGINT` | Snapshot ID | `metadata.json.snapshots[].snapshot-id` |
| `schema_id` | `INT` | Snapshot 对应的 Schema 版本 | `metadata.json.snapshots[].schema-id` |
| `timestamp_ms` | `BIGINT` | 生成时间 | `metadata.json.snapshots[].timestamp-ms` |
| `manifest_list` | `TEXT` | Manifest List 路径 | `metadata.json.snapshots[].manifest-list` |
| `total_records` | `BIGINT` | 当前 Snapshot 的总记录数摘要 | `metadata.json.snapshots[].summary.total-records` |

说明：

1. `schema_id` 对应 Iceberg Snapshot 原生对象中的可选 `schema-id`。
2. 该字段用于保留 Snapshot 与 Schema 版本之间的关联信息。
3. `schema_id` 不参与 Snapshot 主键，仅作为历史 Schema 关联和后续分析能力的结构化摘要。

### 5.5 `partition_specs`

```sql
CREATE TABLE iceberg_catalog.partition_specs (
    table_uuid      UUID NOT NULL,
    spec_id         INT NOT NULL,
    field_position  INT NOT NULL,
    field_id        INT,
    source_id       INT,
    field_name      TEXT,
    transform       TEXT,
    PRIMARY KEY (table_uuid, spec_id, field_position),
    UNIQUE (table_uuid, spec_id, field_id),
    FOREIGN KEY (table_uuid)
        REFERENCES iceberg_catalog.tables_internal(table_uuid)
        ON DELETE CASCADE,
    CHECK (
        (field_position = -1
            AND field_id IS NULL
            AND source_id IS NULL
            AND field_name IS NULL
            AND transform IS NULL)
        OR
        (field_position >= 0
            AND field_id IS NOT NULL
            AND source_id IS NOT NULL
            AND field_name IS NOT NULL
            AND transform IS NOT NULL)
    )
);
```

`partition_specs` 对应 `metadata.json.partition-specs[]` 中的单个 Partition Spec 对象。完整对象通常包含：`spec-id`、`fields[]`；其中 `fields[]` 包含：`source-id`、`field-id`、`name`、`transform`。

| 字段 | 类型 | 含义 | Iceberg 对应 |
| --- | --- | --- | --- |
| `table_uuid` | `UUID` | 所属表 | `metadata.json.table-uuid` |
| `spec_id` | `INT` | Partition Spec 版本 ID | `metadata.json.partition-specs[].spec-id` |
| `field_position` | `INT` | 字段顺序；`-1` 表示版本占位记录 | `metadata.json.partition-specs[].fields[]` 顺序 |
| `field_id` | `INT` | Partition Field ID | `metadata.json.partition-specs[].fields[].field-id` |
| `source_id` | `INT` | 源列 ID | `metadata.json.partition-specs[].fields[].source-id` |
| `field_name` | `TEXT` | 分区字段名 | `metadata.json.partition-specs[].fields[].name` |
| `transform` | `TEXT` | 分区变换表达式 | `metadata.json.partition-specs[].fields[].transform` |

说明：

1. `partition_specs` 保存 Partition Spec 版本及其字段展开结果。
2. `field_position = -1` 表示无分区或空 Spec 场景下的版本占位记录，该值不是 Iceberg 原生字段。
3. 实际分区字段使用 `field_position >= 0`，并按原始 `fields[]` 顺序保存。
4. 当前默认 Spec 由 `tables_internal.default_spec_id` 记录，本表不重复保存默认标记。

### 5.6 兼容视图

为兼容 JDBC Catalog 形状，可在当前主线物理表之上暴露以下两张兼容视图。视图不引入新的物理存储，仅对现有表做兼容层映射。

```sql
CREATE OR REPLACE VIEW iceberg_catalog.iceberg_tables AS
SELECT
    current_database()::TEXT AS catalog_name,
    namespace AS table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.tables_internal;

CREATE OR REPLACE VIEW iceberg_catalog.iceberg_namespace_properties AS
SELECT
    n.catalog_name,
    n.namespace,
    p.key AS property_key,
    p.value AS property_value
FROM iceberg_catalog.namespaces n
CROSS JOIN LATERAL jsonb_each_text(n.properties) AS p(key, value);
```

说明：

1. `iceberg_tables` 暴露 JDBC Catalog 兼容形状所需的基础字段。
2. `iceberg_namespace_properties` 将 `namespaces.properties` 展开为行式属性视图；Namespace 列表以 `namespaces` 基表为准，空属性 Namespace 不依赖该视图返回。
3. 主线 `iceberg_tables` 默认仅覆盖 internal 表；`iceberg_namespace_properties` 统一按 `catalog_name + namespace` 暴露 Namespace 属性。

## 6. External 扩展设计

本章讨论 Spark / PyIceberg 通过 JDBC Catalog 直连 openGauss 的扩展场景。该场景不改变前文 internal 主线建模，仅额外补充 external 目录表与兼容视图。

### 6.1 `tables_external`

```sql
CREATE TABLE iceberg_catalog.tables_external (
    catalog_name               TEXT NOT NULL,
    namespace                  TEXT NOT NULL,
    table_name                 TEXT NOT NULL,
    metadata_location          TEXT NOT NULL,
    previous_metadata_location TEXT,
    PRIMARY KEY (catalog_name, namespace, table_name),
    FOREIGN KEY (catalog_name, namespace)
        REFERENCES iceberg_catalog.namespaces(catalog_name, namespace)
        ON DELETE RESTRICT
);
```

`tables_external` 对应仅通过当前 Catalog 管理、但没有本地 openGauss relation 绑定的外部表目录记录。典型场景包括 Spark / PyIceberg 通过 JDBC Catalog 创建的表。

| 字段 | 类型 | 含义 | Iceberg 对应 |
| --- | --- | --- | --- |
| `catalog_name` | `TEXT` | external catalog 标识 | JDBC Catalog identifier |
| `namespace` | `TEXT` | 表所属 Namespace | Table identifier.namespace |
| `table_name` | `TEXT` | 表名 | Table identifier.name |
| `metadata_location` | `TEXT` | 当前 metadata 文件指针 | Catalog pointer |
| `previous_metadata_location` | `TEXT` | 上一版本 metadata 文件指针 | JDBC Catalog 兼容字段 |

说明：

1. `tables_external` 只保存 external catalog 的目录记录和 metadata 指针，不建立本地 openGauss relation 绑定。
2. `table_uuid`、`table_location`、`current_schema_id`、`current_snapshot_id`、`default_spec_id` 等表级摘要不进入本表，需要时从 `metadata_location` 指向的完整 `metadata.json` 解析。
3. 参照 `pg_lake`，external 目录记录使用独立 `catalog_name` 标识；统一视图过滤 `catalog_name = current_database()` 的 external 记录，避免与 internal catalog 冲突。

### 6.2 兼容视图扩展

如启用 external 扩展，可将 `iceberg_tables` 视图扩展为同时暴露 `tables_internal` 与 `tables_external`。

```sql
CREATE OR REPLACE VIEW iceberg_catalog.iceberg_tables AS
SELECT
    current_database()::TEXT AS catalog_name,
    namespace AS table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.tables_internal
UNION ALL
SELECT
    catalog_name,
    namespace AS table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.tables_external e
WHERE catalog_name <> current_database()::TEXT;
```

说明：

1. `iceberg_tables` 在 external 扩展场景下同时暴露 internal 与 external 目录记录。
2. `iceberg_namespace_properties` 继续复用主线定义，并统一按 `catalog_name + namespace` 暴露 internal 与 external 的 Namespace 属性。
3. 兼容视图中 internal catalog 固定投影为当前数据库名；当 external 记录的 `catalog_name = current_database()` 时，该记录在视图层被过滤。
4. 如需对外暴露到固定 Schema，可在部署阶段再将视图发布到目标 Schema。
5. 如需兼容 external Catalog 的写入、更新和删除目录记录，可在 `iceberg_tables` 视图上通过 `INSTEAD OF` trigger 将 DML 分发到 `tables_external`。

可参考如下触发器定义形态：

```sql
CREATE TRIGGER external_catalog_modifications_trg
INSTEAD OF INSERT OR UPDATE OR DELETE
ON iceberg_catalog.iceberg_tables
FOR EACH ROW
EXECUTE FUNCTION iceberg_catalog.external_catalog_modification();
```

触发函数需要至少实现以下约束：

1. `INSERT` 时检查 `NEW.catalog_name <> current_database()`；`UPDATE` 时检查 `OLD.catalog_name <> current_database()` 且 `NEW.catalog_name <> current_database()`；`DELETE` 时检查 `OLD.catalog_name <> current_database()`。
2. `INSERT`、`UPDATE`、`DELETE` 仅分发到 `tables_external`。
3. 触发器只维护 external 目录记录，不修改 `tables_internal` 及其高频缓存表。

## 7. internal / external 能力范围

### 7.1 Internal 表目标支持范围

| 对外接口能力（按 Iceberg Catalog 语义） | openGauss | Spark / PyIceberg（JDBC Catalog） |
| --- | --- | --- |
| Namespace: list / load | 支持 | 支持 |
| Table: list / load / exists | 支持 | 支持 |
| Table: create / drop | 支持，目录记录落入 `tables_internal` | 不支持 |
| Table: commit | 支持，当前 Schema 变更范围仅包含加列 | 不支持 |
| 数据读取，非 REST Catalog API | 支持，作为本地 relation 查询 | 支持，通过 Catalog 读取 `metadata_location` 后按 Iceberg 标准读取 |

### 7.2 External 表目标支持范围

| 对外接口能力（按 Iceberg Catalog 语义） | openGauss | Spark / PyIceberg（JDBC Catalog） |
| --- | --- | --- |
| Namespace: list / load | 支持，列表以 `namespaces` 为准，属性通过 `iceberg_namespace_properties` 查看 | 支持 |
| Table: list / load / exists | 支持，通过 `iceberg_tables` 查看目录记录 | 支持 |
| Table: create / drop | 支持目录记录维护，通过兼容视图和 trigger 分发到 `tables_external`；仅允许 `catalog_name != current_database()` | 支持，目录记录落入 `tables_external` |
| Table: commit | 不支持作为本地提交方 | 支持 |
| 数据读取，非 REST Catalog API | 默认不直接支持；如显式创建外表或其他读取入口，可支持查询 | 支持，按 Iceberg 标准读取 |

说明：

1. Internal 表面向 openGauss 本地表能力，支持 relation 绑定和高频元信息缓存。
2. External 表面向 Catalog 目录兼容能力，仅用于记录表标识和 metadata 指针，不缓存 `table_uuid`、`table_location` 和当前版本摘要。
3. openGauss 可通过兼容视图维护 external 目录记录，但仅处理 `catalog_name != current_database()` 的 external catalog；当前不作为 external 表的数据写入方和 Schema 提交方。
4. External 表如需在 openGauss 中查询数据，需要后续单独建立读取入口；本文不展开该部分设计。
5. 以上表格按 Iceberg Catalog 语义描述当前 schema 的目标支持范围，不等同于完整运行时实现状态。

## 8. 与 `pg_lake` 元信息 Schema 的对比

### 8.1 总体差异

| 维度 | `pg_lake` | 当前方案 |
| --- | --- | --- |
| 表目录建模 | 基础设计即包含 `tables_internal` / `tables_external` 分离 | 主线为 `tables_internal`；Spark / PyIceberg 通过 JDBC Catalog 直连 openGauss 时扩展 `tables_external` |
| Namespace 属性 | `namespace_properties` 行式存储 | 使用 `namespaces.properties` JSONB，并引入 `catalog_name` 维度；兼容视图展开后对齐 key-value 视图形态 |
| 表当前指针 | 保留 `metadata_location`，并保留 `previous_metadata_location` | 保留 `metadata_location`，并保留 `previous_metadata_location` |
| 本地表绑定 | 直接依赖 `regclass` | 保留 `relid` 作为本地 relation 标识 |
| 结构缓存重点 | 更偏 relation / runtime / 文件级管理 | Internal 表缓存 Catalog 顶层摘要与高频结构；External 表仅保存目录指针 |
| 同名 internal / external 冲突处理 | 视图层隐藏 `catalog_name = current_database()` 的 external 记录 | 视图层隐藏 `catalog_name = current_database()` 的 external 记录 |

### 8.2 结论

`pg_lake` 更偏向“本地 relation 生命周期 + 文件级 runtime 管理”，其基础设计即包含 internal / external 分表。当前方案主线聚焦 internal 表的 Catalog 顶层摘要与高频结构缓存，external 表作为 Spark / PyIceberg 通过 JDBC Catalog 直连 openGauss 的扩展能力引入。

## 9. 与原有 openGauss 方案对比

### 9.1 `tables_internal`

| 差异项 | 原有 openGauss 方案 | 当前方案 |
| --- | --- | --- |
| `namespace` | 无 | 有 |
| `relid` | 无 | 有 |
| `previous_metadata_location` | 无 | 有 |
| `last_column_id` | 无 | 有 |
| `default_spec_id` | 无 | 有 |
| `last_updated_ms` | 有 | 无 |

### 9.2 `tables_external`

| 差异项 | 原有 openGauss 方案 | 当前方案 |
| --- | --- | --- |
| 独立 external 目录表 | 无 | 有 |
| `catalog_name` | 无 | 有 |
| 本地 relation 绑定 | 无 | 无 |
| 保存内容 | 无 | `metadata_location`、`previous_metadata_location` |

### 9.3 `table_schemas`

| 差异项 | 原有 openGauss 方案 | 当前方案 |
| --- | --- | --- |
| `table_name` | 有 | 无 |
| `field_position` | 无 | 有 |

### 9.4 `snapshots`

| 差异项 | 原有 openGauss 方案 | 当前方案 |
| --- | --- | --- |
| `table_name` | 有 | 无 |
| `total_records` | 无 | 有 |

### 9.5 `partition_specs`

| 差异项 | 原有 openGauss 方案 | 当前方案 |
| --- | --- | --- |
| `table_name` | 有 | 无 |
| `field_position` | 无 | 有 |

### 9.6 `manifest` / `datafile`

原有 openGauss 方案已下沉到 `manifest` 和 `datafile` 级别；当前方案首期仍收敛在 `tables_internal`、`tables_external`、`table_schemas`、`snapshots`、`partition_specs` 五类高频结构，不扩展到文件明细层。

## 10. 结论

1. `metadata.json` 仍然是唯一权威元信息。
2. 首期主线落表对象为 `namespaces`、`tables_internal`、`table_schemas`、`partition_specs`、`snapshots`。
3. 针对 Spark / PyIceberg 通过 JDBC Catalog 直连 openGauss 的扩展场景，可额外引入 `tables_external` 与兼容视图扩展。
4. `sort-orders`、`properties`、`refs`、`snapshot-log`、`metadata-log` 等内容保留在完整 metadata 中，不单独落表。
