# Iceberg FDW Managed 表与全表扫描实现方案

## 1. 目标与边界

本文定义 openGauss Iceberg FDW 的首期可执行实现方案。方案采用 **managed Iceberg foreign table** 模式：用户只通过 openGauss 外表 DDL 创建和修改表元数据，FDW 负责把 openGauss 外表 catalog 与 Iceberg metadata 同步维护。

首期目标：

1. 支持 `CREATE FOREIGN TABLE` 创建 managed Iceberg 表。
2. 支持受控的 `ALTER FOREIGN TABLE` 修改 Iceberg schema。
3. 支持普通 `SELECT` 全表扫描，包含列裁剪、文件/分区剪枝请求、Arrow 到 openGauss tuple 转换。
4. 预留 delta 表扫描入口，delta 表是 openGauss 表，用于记录尚未合入 Iceberg metadata 的新鲜 IUD 数据。
5. 明确 `type_adapter`、`operator_adapter`、`catalog_adapter`、`sdk_scan_adapter`、`delta_scan_adapter` 的职责和调用点。

首期明确不支持：

- 不支持连接已有外部 Iceberg metadata 文件的 read-only external table。
- 不支持 MOR，不处理 Iceberg delete file 的读时合并语义。
- 不支持 `ANALYZE`。
- 不支持绕过 openGauss 外表 DDL 修改 Iceberg metadata。
- SDK 层只做 Iceberg 文件/分区剪枝和列裁剪；所有 SQL filter 在 openGauss 侧必须 recheck。

## 2. 总体架构

```
openGauss SQL
    |
    +-- CREATE/ALTER/DROP FOREIGN TABLE
    |       |
    |       v
    |   iceberg_ddl_hook
    |       |
    |       +-- catalog_adapter
    |       |     - 创建/修改 internal catalog 表记录
    |       |     - 维护 table_uuid、field_id、schema_id、metadata_location
    |       |
    |       +-- metadata_txn_tracker
    |             - 记录本事务待提交的 Iceberg metadata operation
    |
    +-- SELECT
            |
            v
        FDW callbacks
            |
            +-- catalog_adapter
            +-- type_adapter
            +-- operator_adapter
            +-- sdk_scan_adapter
            +-- delta_scan_adapter
            |
            v
        Iceberg SDK scan + openGauss delta table scan
            |
            v
        TupleTableSlot -> openGauss executor recheck quals
```

核心原则：

- openGauss 外表定义是 managed 表的用户入口。
- Iceberg metadata 是底层数据湖表的权威元数据。
- `catalog_adapter` 负责在两者之间维护绑定关系。
- 查询阶段信任由 DDL 创建出的列定义，不重复执行外部表兼容性校验。
- 查询阶段仍必须按 `field_id` 访问 Iceberg 数据，不能只依赖列顺序。

## 3. DDL 能力实现

### 3.1 DDL 语义

用户创建表：

```sql
CREATE FOREIGN TABLE public.orders_iceberg (
    order_id bigint NOT NULL,
    user_id integer,
    status varchar(32),
    embedding vector(768)
)
SERVER iceberg_srv
OPTIONS (
    namespace 'default',
    table_name 'orders'
);
```

该语句语义为：

1. 在 openGauss 中创建 foreign table。
2. 在 Iceberg catalog 中创建 managed Iceberg table。
3. 生成初始 `metadata.json`。
4. 把 openGauss `relid` 与 Iceberg `table_uuid`、`metadata_location`、`schema_id`、`field_id` 绑定。

不支持：

```sql
CREATE FOREIGN TABLE t()
SERVER iceberg_srv
OPTIONS (metadata_location 's3://.../v1.metadata.json');
```

即首期不提供“连接已有 Iceberg 表”的路径。

### 3.2 Hook 注册

`CREATE FOREIGN TABLE` 原生流程不会创建 Iceberg metadata。FDW 需要通过扩展库加载时注册 DDL hook 和 transaction hook。

openGauss 参考接口：

```c
extern THR_LOCAL PGDLLIMPORT ProcessUtility_hook_type ProcessUtility_hook;
extern void RegisterXactCallback(XactCallback callback, void *arg);
extern void UnregisterXactCallback(XactCallback callback, const void *arg);
```

扩展入口：

```c
PG_MODULE_MAGIC;

extern "C" void _PG_init(void);
extern "C" void _PG_fini(void);

static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

void
_PG_init(void)
{
    prev_ProcessUtility_hook = ProcessUtility_hook;
    ProcessUtility_hook = iceberg_ProcessUtility;
    RegisterXactCallback(iceberg_xact_callback, NULL);
}

void
_PG_fini(void)
{
    ProcessUtility_hook = prev_ProcessUtility_hook;
    UnregisterXactCallback(iceberg_xact_callback, NULL);
}
```

部署约束：

- `_PG_init()` 只有动态库加载到 backend 时才执行。
- 需要通过 openGauss 支持的 preload 机制加载扩展库，或保证执行 DDL 的会话已经加载 `iceberg_fdw` 动态库。
- `CREATE EXTENSION iceberg_fdw` 只负责创建 SQL 对象和内部 catalog 表，不应被视为所有会话永久挂 hook 的机制。

### 3.3 CREATE FOREIGN TABLE 流程

`iceberg_ProcessUtility` 识别 `CreateForeignTableStmt`，仅处理 server 所属 FDW 为 `iceberg_fdw` 的语句。

流程：

1. 解析 server options：`catalog_type`、`catalog_uri`、`warehouse`。
2. 解析 table options：`namespace`、`table_name`。
3. 拒绝外部只读路径相关 option，例如 `metadata_location`、`path`。
4. 校验 openGauss 列定义是否能映射到 Iceberg 类型。
5. 计算 Iceberg table location。
6. 调用 `standard_ProcessUtility` 或前序 hook，创建 openGauss foreign table。
7. 通过 `RangeVarGetRelid` 获取新建外表 `relid`。
8. 调用 catalog 模块创建 managed table 记录。
9. 调用 catalog 模块注册 schema 与 field id。
10. 记录本事务待提交的 `ICEBERG_METADATA_OP_CREATE_TABLE`。

建议接口：

```c
typedef struct IcebergCatalogCreateTableRequest {
    Oid relid;
    const char *catalog_type;
    const char *catalog_uri;
    const char *warehouse;
    const char *namespace_name;
    const char *table_name;
    const char *table_location;
    TupleDesc tuple_desc;
    List *column_defs;
} IcebergCatalogCreateTableRequest;

typedef struct IcebergCatalogCreateTableResult {
    char *table_uuid;
    char *metadata_location;
    int current_schema_id;
    int64 current_snapshot_id;
} IcebergCatalogCreateTableResult;

bool iceberg_catalog_create_managed_table(
    const IcebergCatalogCreateTableRequest *request,
    IcebergCatalogCreateTableResult *result);

void iceberg_catalog_register_schema_from_tupledesc(
    Oid relid,
    const char *table_uuid,
    int schema_id,
    TupleDesc tuple_desc);

void iceberg_metadata_track_create_table(
    Oid relid,
    const IcebergCatalogCreateTableResult *result);
```

DDL 期类型映射失败必须在 openGauss foreign table 创建前报错。

### 3.4 ALTER FOREIGN TABLE 流程

首期只允许通过 `ALTER FOREIGN TABLE` 修改 managed Iceberg 表 schema。任何直接修改 Iceberg metadata 文件或外部 catalog 的方式都不纳入首期。

支持的最小 ALTER 集合：

| 操作 | 支持 | Iceberg 行为 |
| --- | --- | --- |
| `ADD COLUMN` | 是 | 分配新的 `field_id`，生成新 schema |
| `DROP COLUMN` | 是，建议首期可先禁用 | Iceberg schema 删除字段，保留历史 field id |
| `RENAME COLUMN` | 是 | 保持 `field_id` 不变，更新字段名 |
| `ALTER COLUMN TYPE` | 仅安全提升 | 生成新 schema，拒绝可能丢精度的修改 |
| `SET/DROP NOT NULL` | 受限 | 与 Iceberg required/optional 对齐 |
| 修改 table options | 受限 | 仅允许 FDW 声明支持的选项 |

流程：

1. DDL hook 识别 `AlterTableStmt`，确认目标表是 `iceberg_fdw` managed foreign table。
2. 把 ALTER 子命令转换成 `IcebergCatalogSchemaChange`。
3. 对每个子命令做 DDL 期校验。
4. 调用原生 `ProcessUtility` 修改 openGauss catalog。
5. 重新读取目标表 `TupleDesc`。
6. 调用 catalog 模块写入新的 schema 版本。
7. 记录本事务待提交的 `ICEBERG_METADATA_OP_ALTER_SCHEMA`。

建议接口：

```c
typedef enum IcebergCatalogSchemaChangeKind {
    ICEBERG_SCHEMA_ADD_COLUMN,
    ICEBERG_SCHEMA_DROP_COLUMN,
    ICEBERG_SCHEMA_RENAME_COLUMN,
    ICEBERG_SCHEMA_ALTER_TYPE,
    ICEBERG_SCHEMA_ALTER_NULLABILITY
} IcebergCatalogSchemaChangeKind;

typedef struct IcebergCatalogSchemaChange {
    IcebergCatalogSchemaChangeKind kind;
    AttrNumber attnum;
    char *old_name;
    char *new_name;
    Oid old_pg_type;
    Oid new_pg_type;
    int32 new_typmod;
    bool new_not_null;
} IcebergCatalogSchemaChange;

bool iceberg_catalog_apply_schema_changes(
    Oid relid,
    TupleDesc new_tuple_desc,
    List *schema_changes,
    int *new_schema_id,
    char **new_metadata_location);

void iceberg_metadata_track_alter_schema(
    Oid relid,
    int new_schema_id,
    const char *new_metadata_location);
```

### 3.5 DROP FOREIGN TABLE 流程

首期建议实现为 catalog unregister，不删除远端对象存储中的历史数据文件。

流程：

1. DDL hook 识别 `DropStmt`。
2. 找出目标 relation 是否为 managed Iceberg foreign table。
3. 调用原生 `ProcessUtility` 删除 openGauss foreign table。
4. 调用 catalog 模块删除 internal catalog 绑定记录。
5. 记录本事务待提交的 drop metadata operation 或 cleanup operation。

是否删除远端 metadata/data 文件应作为后续能力，不在首期默认执行。

### 3.6 Transaction Hook

DDL hook 不直接把 Iceberg metadata 视为已提交事实，而是记录本事务变更。

```c
static void
iceberg_xact_callback(XactEvent event, void *arg)
{
    switch (event) {
        case XACT_EVENT_PRE_COMMIT:
            iceberg_metadata_commit_pending_changes();
            break;
        case XACT_EVENT_ABORT:
            iceberg_metadata_abort_pending_changes();
            break;
        case XACT_EVENT_PRE_PREPARE:
            if (iceberg_metadata_has_pending_changes())
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("prepared transaction with Iceberg metadata changes is not supported")));
            break;
        default:
            break;
    }
}
```

`iceberg_metadata_commit_pending_changes()` 调用 catalog 模块：

```c
bool iceberg_catalog_commit_metadata_changes(
    const IcebergMetadataCommitRequest *request,
    IcebergMetadataCommitResult *result);
```

该接口由团队 catalog 模块负责：

- 生成或更新 Iceberg `metadata.json`。
- 写入对象存储。
- 更新 `tables_internal.metadata_location`。
- 更新 `previous_metadata_location`。
- 写入 `table_schemas`、`snapshots`、`partition_specs` 等摘要表。
- 保证失败时返回明确错误，使 openGauss 事务回滚。

## 4. Catalog Adapter

### 4.1 职责

`catalog_adapter` 是 FDW 查询和 DDL 的元数据入口。首期 managed-only 模式下，它不需要支持外部 metadata path 注册。

主要能力：

- 根据 `relid` 找到 managed Iceberg 表。
- 返回当前 `table_uuid`、`metadata_location`、`table_location`、`current_schema_id`、`current_snapshot_id`。
- 返回当前 schema 的字段列表。
- 返回 snapshot 行数摘要。
- 处理 DDL 创建、schema 变更和 metadata commit。

### 4.2 查询期接口

```c
typedef struct IcebergCatalogTableInfo {
    Oid relid;
    char *namespace_name;
    char *table_name;
    char *table_uuid;
    char *metadata_location;
    char *table_location;
    int current_schema_id;
    int64 current_snapshot_id;
} IcebergCatalogTableInfo;

typedef struct IcebergCatalogFieldInfo {
    int field_id;
    char *field_name;
    char *field_type;
    bool field_required;
    int field_position;
    char *logical_type;
    int vector_dim;
    char *vector_element_type;
} IcebergCatalogFieldInfo;

typedef struct IcebergCatalogStats {
    bool has_total_records;
    double total_records;
    bool has_total_data_files;
    double total_data_files;
    bool has_total_file_size;
    double total_file_size;
} IcebergCatalogStats;

bool iceberg_catalog_get_table_info(Oid relid, IcebergCatalogTableInfo *out);
List *iceberg_catalog_get_fields(const char *table_uuid, int schema_id);
bool iceberg_catalog_get_snapshot_stats(
    const char *table_uuid,
    int64 snapshot_id,
    IcebergCatalogStats *out);
```

### 4.3 field id 映射

首期不单独建设 `field_id_map` 表。理由：

- 表由 openGauss 外表 DDL 创建。
- schema 只能通过受控 `ALTER FOREIGN TABLE` 修改。
- `table_schemas` 已保存当前 schema 的 `field_name`、`field_position`、`field_id`。
- `RENAME COLUMN` 同步修改 openGauss attname 与 Iceberg field name，`field_id` 保持不变。

查询期 `type_adapter` 用 `TupleDesc` 的列名在当前 `table_schemas` 中匹配 `field_id`。由于 DDL 受控，该映射被视为可信。

后续如果支持连接外部 Iceberg 表、跨系统 schema evolution、隐藏列或复杂 drop/rename 历史，再增加独立 `field_id_map(relid, attnum, field_id)` 表。

## 5. Type Adapter

### 5.1 职责

`type_adapter` 负责三类工作：

1. DDL 期：把 openGauss 列类型映射为 Iceberg schema 字段类型。
2. 规划期：建立 `attnum -> field_id`、`attnum -> logical_type` 的扫描映射。
3. 执行期：把 Arrow/Iceberg 值转换为 openGauss `Datum`，填入 `TupleTableSlot`。

managed-only 模式下，查询期不再做“openGauss 外表是否兼容外部 Iceberg schema”的完整校验；外表列类型在 DDL 期已经校验并生成 Iceberg metadata，查询期只做轻量一致性检查：

- `relid` 是否存在 internal catalog 记录。
- 当前 schema 是否能找到所有投影列对应的 `field_id`。
- SDK 返回的 Arrow schema 是否与 scan request 中的投影列数量和基础类型一致。

### 5.2 类型映射

首期支持：

| openGauss 类型 | Iceberg 类型 | DDL 行为 | 执行期转换 |
| --- | --- | --- | --- |
| `smallint` / `int2` | `int` | 允许 | Iceberg int32 读回后检查 int16 范围 |
| `integer` / `int4` | `int` | 允许 | `Int32GetDatum` |
| `bigint` / `int8` | `long` | 允许 | `Int64GetDatum` |
| `varchar(n)` | `string` | 允许，记录 typmod | 读回时按 typmod 校验长度 |
| `varchar` | `string` | 允许 | 文本 Datum |
| `text` | `string` | 允许 | 文本 Datum |
| `char(n)` / `bpchar` | `string` | 允许但谓词保守 | 按 openGauss bpchar 语义构造 Datum |
| `vector(n)` | `list<float>` | 允许，必须有固定维度 | Arrow `FixedSizeList<Float32>` 或等价结构转 openGauss vector |

首期拒绝：

- `boolean`
- `float4`、`float8` 标量列
- `numeric`、`decimal`
- `date`、`time`、`timestamp`
- `uuid`
- `bytea` / binary
- openGauss 数组
- Iceberg `struct`、`map`、嵌套 `list`

说明：

- `float4` 可作为 `vector` 元素支撑类型，但不作为普通标量列类型开放。
- `float8` 可作为向量距离表达式返回类型知识储备，但首期本文不展开向量搜索执行链路，也不开放普通标量列。

### 5.3 数据结构

```c
typedef enum IcebergFdwLogicalType {
    ICEBERG_FDW_TYPE_INT16,
    ICEBERG_FDW_TYPE_INT32,
    ICEBERG_FDW_TYPE_INT64,
    ICEBERG_FDW_TYPE_STRING,
    ICEBERG_FDW_TYPE_VECTOR_FLOAT32
} IcebergFdwLogicalType;

typedef struct IcebergFdwColumnMapping {
    AttrNumber attnum;
    int field_id;
    char *field_name;
    Oid pg_type;
    int32 pg_typmod;
    Oid pg_collation;
    IcebergFdwLogicalType logical_type;
    bool nullable;
    int vector_dim;
} IcebergFdwColumnMapping;
```

### 5.4 调用点

| 函数 | 使用能力 |
| --- | --- |
| DDL hook create/alter | `iceberg_type_build_iceberg_schema_from_tupledesc`，生成 Iceberg field type |
| `GetForeignRelSize` | `iceberg_type_build_column_mappings`，构造规划期列映射 |
| `GetForeignPlan` | 根据投影列生成 `projected_field_ids` |
| `BeginForeignScan` | 初始化 Arrow converter |
| `IterateForeignScan` | Arrow batch 行转 Datum/slot |

建议接口：

```c
List *iceberg_type_build_column_mappings(
    Relation rel,
    List *catalog_fields);

bool iceberg_type_pg_column_to_iceberg_field(
    const Form_pg_attribute attr,
    Oid pg_type,
    int32 pg_typmod,
    IcebergCatalogFieldInfo *out_field);

bool iceberg_type_arrow_row_to_slot(
    const IcebergFdwScanState *state,
    ArrowArray *array,
    ArrowSchema *schema,
    int row_index,
    TupleTableSlot *slot);
```

## 6. Operator Adapter

### 6.1 职责

本文只描述基础扫描路径，仍保留 `operator_adapter`，用于：

- 识别可传给 SDK 做文件/分区剪枝的简单谓词。
- 生成 SDK filter expression。
- 保留所有原始 qual 作为 openGauss local recheck。

关键约束：

- SDK filter 只用于剪枝，不作为最终正确性依据。
- `GetForeignPlan` 传给 `make_foreignscan` 的 local quals 必须包含全部原始 `scan_clauses`。
- 即便谓词被下推给 SDK，扫描返回后仍由 openGauss 执行器 recheck。

### 6.2 谓词下推范围

可尝试下推到 SDK 剪枝：

| 类型 | 操作符 | 下推用途 | local recheck |
| --- | --- | --- | --- |
| int/long | `=` `<` `<=` `>` `>=` | 文件/分区剪枝 | 必须 |
| int/long | `IS NULL` / `IS NOT NULL` | 文件/分区剪枝 | 必须 |
| varchar/text | `=` | 保守剪枝 | 必须 |
| bpchar | `=` | 一般不下推，除非语义明确 | 必须 |

不下推：

- 字符串范围比较。
- 非 binary collation 相关比较。
- 函数表达式、volatile expression。
- `IN`、`LIKE`、正则、复杂 boolean 组合，首期可全部留本地。
- vector 距离、top-k、向量相似度。

### 6.3 数据结构

```c
typedef enum IcebergFdwOperator {
    ICEBERG_FDW_OP_EQ,
    ICEBERG_FDW_OP_LT,
    ICEBERG_FDW_OP_LE,
    ICEBERG_FDW_OP_GT,
    ICEBERG_FDW_OP_GE,
    ICEBERG_FDW_OP_IS_NULL,
    ICEBERG_FDW_OP_IS_NOT_NULL
} IcebergFdwOperator;

typedef struct IcebergFdwPredicate {
    int field_id;
    IcebergFdwLogicalType type;
    IcebergFdwOperator op;
    IcebergFdwValue value;
    bool sdk_pruning_only;
} IcebergFdwPredicate;
```

`sdk_pruning_only` 首期恒为 `true`。

### 6.4 调用点

| 函数 | 使用能力 |
| --- | --- |
| `GetForeignRelSize` | 可选：估算谓词选择率时识别简单条件 |
| `GetForeignPlan` | 拆分 SDK pruning predicate 与 local recheck quals |
| `ExplainForeignScan` | 输出 SDK pruning filter 与 local recheck 数量 |

建议接口：

```c
typedef struct IcebergFdwQualClassification {
    List *sdk_predicates;      /* IcebergFdwPredicate */
    List *local_exprs;         /* Expr，包含全部原始 quals */
    char *serialized_filter;   /* SDK 可消费表达式 */
} IcebergFdwQualClassification;

void iceberg_operator_classify_scan_clauses(
    PlannerInfo *root,
    RelOptInfo *baserel,
    List *scan_clauses,
    List *column_mappings,
    IcebergFdwQualClassification *out);
```

## 7. 全表扫描 FDW 流程

### 7.1 规划期状态

`RelOptInfo.fdw_private` 保存规划期临时状态，不进入执行计划。

```c
typedef struct IcebergFdwPlanState {
    IcebergFdwOptions options;
    IcebergCatalogTableInfo table_info;
    IcebergCatalogStats stats;
    List *column_mappings;        /* IcebergFdwColumnMapping */
    List *sdk_predicates;         /* IcebergFdwPredicate */
    char *serialized_sdk_filter;
    double rows_before_filter;
    double rows_after_filter;
    Cost startup_cost;
    Cost total_cost;
} IcebergFdwPlanState;
```

### 7.2 `GetForeignRelSize`

职责：

1. 解析 options。
2. 调用 `iceberg_catalog_get_table_info(relid)`。
3. 调用 `iceberg_catalog_get_fields(table_uuid, current_schema_id)`。
4. 调用 `iceberg_type_build_column_mappings`。
5. 调用 `iceberg_catalog_get_snapshot_stats`。
6. 设置 `baserel->rows`。
7. 保存 `IcebergFdwPlanState` 到 `baserel->fdw_private`。

不做：

- 不访问对象存储。
- 不解析 metadata.json。
- 不执行外部 Iceberg schema 兼容性校验。

### 7.3 `GetForeignPaths`

首期只生成一个普通 foreign scan path。

```c
static void
icebergGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    IcebergFdwPlanState *fdw_state = baserel->fdw_private;

    add_path(baserel,
        (Path *) create_foreignscan_path(root,
                                         baserel,
                                         baserel->rows,
                                         fdw_state->startup_cost,
                                         fdw_state->total_cost,
                                         NIL,
                                         NULL,
                                         NIL));
}
```

基础扫描路径不生成额外 path，不处理 pathkeys，不做 LIMIT pushdown。

### 7.4 计划节点私有信息

`ForeignScan.fdw_private` 必须是可序列化 `List`。首期建议把复杂结构序列化为 JSON 字符串，避免直接塞 C 指针。

```c
enum IcebergFdwPrivateIndex {
    IcebergFdwPrivScanEntry = 0,
    IcebergFdwPrivProjection,
    IcebergFdwPrivSdkFilter,
    IcebergFdwPrivPlanInfo,
    IcebergFdwPrivDeltaScan
};
```

`IcebergFdwPrivScanEntry` JSON：

```json
{
  "relid": 12345,
  "table_uuid": "...",
  "metadata_location": "s3://warehouse/t/metadata/00000.metadata.json",
  "snapshot_id": 1001,
  "schema_id": 0
}
```

`IcebergFdwPrivProjection` JSON：

```json
{
  "attrs": [
    {"attnum": 1, "field_id": 1, "field_name": "order_id"},
    {"attnum": 2, "field_id": 2, "field_name": "user_id"}
  ]
}
```

`IcebergFdwPrivSdkFilter` JSON：

```json
{
  "predicates": [
    {"field_id": 2, "op": "eq", "type": "int", "value": 10}
  ],
  "pruning_only": true
}
```

`IcebergFdwPrivDeltaScan` JSON：

```json
{
  "enabled": true,
  "delta_relation": "iceberg_delta.orders_iceberg_delta",
  "base_snapshot_id": 1001
}
```

### 7.5 `GetForeignPlan`

流程：

1. 从 `baserel->fdw_private` 取 `IcebergFdwPlanState`。
2. 调用 `extract_actual_clauses(scan_clauses, false)` 得到全部 local exprs。
3. 调用 `operator_adapter` 生成 SDK pruning filter。
4. 根据 `tlist` 和 local quals 计算需要读取的列。
5. 用 `field_id` 生成投影信息。
6. 构造 `fdw_private`。
7. 调用 `make_foreignscan`，local quals 传入全部原始 quals。

伪代码：

```c
static ForeignScan *
icebergGetForeignPlan(...)
{
    IcebergFdwPlanState *fdw_state = baserel->fdw_private;

    List *local_exprs = extract_actual_clauses(scan_clauses, false);

    IcebergFdwQualClassification quals;
    iceberg_operator_classify_scan_clauses(root, baserel, scan_clauses,
                                           fdw_state->column_mappings, &quals);

    List *retrieved_attrs = iceberg_build_retrieved_attrs(root, baserel,
                                                          tlist, local_exprs);

    List *fdw_private = iceberg_build_fdw_private(fdw_state,
                                                  retrieved_attrs,
                                                  quals.serialized_filter);

    return make_foreignscan(tlist,
                            local_exprs,
                            baserel->relid,
                            NIL,
                            fdw_private,
                            NIL,
                            NIL,
                            outer_plan);
}
```

### 7.6 执行期状态

```c
typedef struct IcebergFdwScanState {
    Relation rel;
    Oid relid;
    TupleDesc tuple_desc;

    IcebergCatalogTableInfo table_info;
    int64 snapshot_id;
    int schema_id;

    List *column_mappings;
    List *projected_attrs;
    int *projected_field_ids;
    int n_projected_fields;

    char *sdk_filter_json;
    IcebergSdkScan *iceberg_scan;
    ArrowArray *current_array;
    ArrowSchema *current_schema;
    int current_row;
    int current_batch_rows;

    bool delta_scan_enabled;
    DeltaScanHandle *delta_scan;
    bool reading_delta;

    MemoryContext scan_cxt;
    MemoryContext batch_cxt;
} IcebergFdwScanState;
```

### 7.7 `BeginForeignScan`

流程：

1. 解析 `fdw_private`。
2. 打开 relation，读取 `TupleDesc`。
3. 调用 `iceberg_catalog_get_table_info(relid)`。
4. 校验 `table_uuid` 与计划一致。
5. 加载当前 schema 字段并重建 column mappings。
6. 构造 SDK scan request。
7. 调用 `sdk_scan_adapter` 打开 Iceberg scan。
8. 如果 delta scan enabled，初始化 `delta_scan_adapter`。
9. 创建 batch memory context。

SDK scan request：

```c
typedef struct IcebergSdkScanRequest {
    const char *metadata_location;
    int64 snapshot_id;
    int schema_id;
    const int *projected_field_ids;
    int n_projected_fields;
    const char *filter_json;
    bool filter_is_pruning_only;
    bool enable_mor;
} IcebergSdkScanRequest;
```

首期：

- `enable_mor=false`。
- 如果 SDK 发现 Iceberg snapshot 含 delete file，直接报错：`MOR is not supported`。

### 7.8 `IterateForeignScan`

返回顺序：

1. 先扫描 Iceberg base snapshot。
2. Iceberg 扫描 EOF 后，扫描 delta openGauss 表。
3. 两者都 EOF 后返回空 slot。

伪代码：

```c
static TupleTableSlot *
icebergIterateForeignScan(ForeignScanState *node)
{
    IcebergFdwScanState *state = node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

    for (;;) {
        if (!state->reading_delta) {
            if (iceberg_next_base_row(state, slot))
                return slot;

            state->reading_delta = true;
            continue;
        }

        if (state->delta_scan_enabled &&
            iceberg_next_delta_row(state, slot))
            return slot;

        return ExecClearTuple(slot);
    }
}
```

`iceberg_next_base_row`：

1. 当前 Arrow batch 还有行，则调用 `type_adapter` 转 slot。
2. 当前 batch 耗尽，则释放 batch context。
3. 调用 `iceberg_sdk_scan_next` 获取下一批。
4. EOF 则返回 false。

### 7.9 `ReScanForeignScan`

首期简单关闭并重新打开 SDK scan：

1. `iceberg_sdk_scan_close`。
2. `delta_scan_close`。
3. 清理 batch context。
4. 使用保存的 request 重新执行 `BeginForeignScan` 初始化逻辑。

### 7.10 `EndForeignScan`

释放：

- SDK scan handle。
- Arrow batch。
- delta scan handle。
- scan memory context。

### 7.11 `ExplainForeignScan`

输出：

| 字段 | 示例 |
| --- | --- |
| `Iceberg Table` | `default.orders` |
| `Iceberg Table UUID` | `...` |
| `Iceberg Metadata` | `s3://.../metadata.json` |
| `Iceberg Snapshot` | `1001` |
| `Iceberg Schema ID` | `0` |
| `Projection Field IDs` | `1,2,3` |
| `SDK Filter` | JSON 摘要 |
| `Filter Mode` | `pruning only, local recheck required` |
| `Delta Scan` | `enabled` / `disabled` |
| `MOR Support` | `false` |

## 8. SDK Scan Adapter

### 8.1 职责

`sdk_scan_adapter` 封装 Iceberg SDK + Arrow C Data Interface。

能力：

- 根据 `metadata_location` 和 `snapshot_id` 打开 Iceberg 表。
- 按 `field_id` 做列裁剪。
- 接收 SDK filter，用于 Iceberg 文件/分区剪枝。
- 返回 Arrow batch。
- 检测 MOR/delete file 并在首期报错。

### 8.2 接口

```c
typedef struct IcebergSdkScan IcebergSdkScan;

IcebergSdkScan *iceberg_sdk_scan_open(
    MemoryContext cxt,
    const IcebergSdkScanRequest *request,
    ArrowSchema **out_schema);

int iceberg_sdk_scan_next(
    IcebergSdkScan *scan,
    ArrowArray **out_array,
    ArrowSchema **out_schema);

void iceberg_sdk_scan_release_batch(IcebergSdkScan *scan);
void iceberg_sdk_scan_close(IcebergSdkScan *scan);
```

### 8.3 SDK 约束

- SDK 返回的 batch 必须只包含 requested projection。
- SDK filter 只用于剪枝，不能省略 openGauss recheck。
- SDK 不负责执行 SQL 语义完整过滤。
- SDK 发现 delete file/MOR 场景时返回明确错误码。

## 9. Delta Scan Adapter

### 9.1 语义

delta 表是 openGauss 表，用于记录尚未更新到 Iceberg metadata 的新鲜 IUD 数据。它不是 Iceberg MOR delete file，也不是 Iceberg metadata 中的 delete file。

查询时需要预留：

```text
final result = Iceberg base snapshot scan overlay visible delta IUD rows
```

也就是说，后续完整实现不能只是简单追加 delta insert 行，还需要根据 delta 表中的 update/delete 记录对 base snapshot 行做可见性覆盖。首期可以只定义接口，不实际合并复杂 IUD 语义；但扫描流程中必须预留 delta scan 阶段。

### 9.2 接口

```c
typedef struct DeltaScanHandle DeltaScanHandle;

typedef struct DeltaScanRequest {
    Oid base_relid;
    int64 base_snapshot_id;
    List *projected_attrs;
    List *column_mappings;
    Snapshot og_snapshot;
} DeltaScanRequest;

bool iceberg_delta_scan_available(Oid base_relid);

DeltaScanHandle *iceberg_delta_scan_begin(
    MemoryContext cxt,
    const DeltaScanRequest *request);

bool iceberg_delta_scan_next(
    DeltaScanHandle *handle,
    TupleTableSlot *slot);

void iceberg_delta_scan_rescan(DeltaScanHandle *handle);
void iceberg_delta_scan_end(DeltaScanHandle *handle);
```

### 9.3 预留字段

`ForeignScan.fdw_private` 中的 `IcebergFdwPrivDeltaScan` 记录：

- 是否启用 delta scan。
- delta relation 标识。
- base snapshot id。
- 投影列。

实际 delta 表设计和 IUD 合并规则由后续 DML 方案补充。

## 10. 当前约束与错误策略

### 10.1 Managed-only 约束

- 表必须由 `CREATE FOREIGN TABLE ... SERVER iceberg_fdw` 创建。
- Iceberg metadata 只能由 FDW DDL hook 和事务 hook 修改。
- 不允许外部进程直接改同一张 managed 表的 metadata。
- 不支持 external read-only Iceberg 表。

### 10.2 扫描约束

- 只支持全表扫描路径。
- 不做 LIMIT pushdown。
- 不做 ORDER BY pushdown。
- SDK filter 只剪枝，所有 filter 都本地 recheck。
- 不支持 MOR；检测到 Iceberg delete file 时报错。

### 10.3 DDL 约束

- 不支持 openGauss 默认值、check、unique、primary key 自动映射到 Iceberg。
- 不支持系统列、生成列、表达式列映射。
- 不支持外部 metadata path 注册。
- 不支持两阶段提交中的 Iceberg metadata 变更。

## 11. 实现顺序

1. 增加 DDL hook 和 transaction hook 骨架。
2. 增加 internal catalog adapter 接口和 catalog 表写入流程。
3. 实现 `CREATE FOREIGN TABLE` managed 表创建。
4. 实现 DDL 期类型映射和 Iceberg schema 生成。
5. 实现 metadata pending operation 跟踪和 `PRE_COMMIT` 提交。
6. 实现 `GetForeignRelSize` 读取 catalog 摘要和 column mappings。
7. 实现 `GetForeignPaths` 普通 scan path。
8. 实现 `GetForeignPlan` 的 projection、SDK filter、local recheck、fdw_private。
9. 实现 SDK scan adapter 的 open/next/close。
10. 实现 Arrow 到 slot 的 `type_adapter` 转换。
11. 预留 delta scan adapter 并接入 `IterateForeignScan` 阶段切换。
12. 实现 `ALTER FOREIGN TABLE` 的受控 schema evolution。
13. 补充 `EXPLAIN` 输出和错误路径测试。
