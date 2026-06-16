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
- 不支持 `ANALYZE`。
- 不支持绕过 openGauss 外表 DDL 修改 Iceberg metadata。
- 首期 FDW 不自行实现 Iceberg delete file/MOR 合并逻辑；Rust SDK `to_arrow()` 路径默认应用 position/equality delete。
- SDK 层使用 iceberg-rust Arrow 列式扫描接口，投影参数传顶层列名，谓词参数由桥接层转换为 iceberg-rust `Predicate`。

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
        bridge loader -> Iceberg Rust SDK batch scan + openGauss delta table check/scan
            |
            v
        TupleTableSlot -> openGauss executor optional local quals
```

核心原则：

- openGauss 外表定义是 managed 表的用户入口。
- Iceberg metadata 是底层数据湖表的权威元数据。
- `catalog_adapter` 负责在两者之间维护绑定关系。
- 查询阶段信任由 DDL 创建出的列定义，不重复执行外部表兼容性校验。
- 查询阶段调用 Rust SDK 时按当前顶层列名传递投影和谓词字段；FDW 先把扫描参数序列化为 JSON，再由 bridge 转换为 Iceberg Rust SDK 可识别的参数和 `Predicate`。

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
    ProcessUtility_hook = icebergProcessUtility;
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

`iceberg_fdw_handler` 返回的 `FdwRoutine` 需要挂接 `ValidateTableDef`，创建 managed 表时由 openGauss `CreateForeignTable` 流程调用该回调。`ProcessUtility` hook 主要用于 `DROP FOREIGN TABLE` 和 `DROP SERVER ... CASCADE` 的 catalog 清理。

流程：

1. 解析 server option `warehouse`。
2. 解析 table options `namespace`、`table_name`。
3. 拒绝外部只读路径相关 option，例如 `metadata_location`、`path`。
4. 校验 openGauss 列定义是否能映射到 Iceberg 类型。
5. 计算 Iceberg table location。
6. 调用 `ValidateTableDef` 路径完成 openGauss 外表创建前的校验和 managed table 记录写入。
8. 记录本事务待提交的 `ICEBERG_METADATA_OP_CREATE_TABLE`。

建议接口：

```c
typedef struct IcebergCatalogCreateTableRequest {
    Oid relid;
    const char *warehouse;
    const char *namespace_name;
    const char *table_name;
    const char *table_location;
    TupleDesc tuple_desc;
    List *column_mappings;
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
| `DROP COLUMN` | 是，按当前实现支持 | Iceberg schema 删除字段，保留历史 field id |
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
2. 找出目标 relation 是否为 managed Iceberg foreign table；`DROP SERVER ... CASCADE` 时先收集受影响的 managed foreign table。
3. 调用原生 `ProcessUtility` 删除 openGauss foreign table / server。
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
    char *field_type;       /* Iceberg 物理类型，例如 int/long/string/list<float> */
    bool field_required;
    int field_position;
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

### 4.3 field id 与 SDK 列名

首期不单独建设 `field_id_map` 表。理由：

- 表由 openGauss 外表 DDL 创建。
- schema 只能通过受控 `ALTER FOREIGN TABLE` 修改。
- `table_schemas` 已保存当前 schema 的 `field_name`、`field_position`、`field_id`。
- `RENAME COLUMN` 同步修改 openGauss attname 与 Iceberg field name，`field_id` 保持不变。

Iceberg metadata 仍需要 `field_id` 作为 schema evolution 的稳定列身份，catalog 继续维护该字段。但 FDW 调用 Rust SDK 的公开扫描接口时不传 `field_id`，只传当前 schema 顶层列名：

- 投影列：`columns`/`n_columns` 为列名数组。
- 谓词字段：filter 表达式中的字段名为当前顶层列名。
- SDK 在 `TableScanBuilder::select(...).build()` 内部按列名解析 field id，并使用 `ProjectionMask` 完成真正的 field id 投影。

因此查询期不再需要构建 `attnum -> field_id` 或独立 `field_id_map` 扫描映射。`type_adapter` 只需要从 `TupleDesc` 和受控 catalog 中拿到列名、openGauss 类型、typmod、collation 与 Iceberg 物理类型，用于构造 SDK 列名请求和 Arrow 转 Datum。

`table_schemas.field_type` 只保存 Iceberg 物理类型，不保存 openGauss 逻辑类型扩展信息。例如 openGauss `vector(n)` 在 Iceberg schema 中记录为 `list<float>`。向量列的“这是 vector 类型”和“维度是多少”由 openGauss 外表列类型 `VECTOROID` 与 typmod 管理，catalog 不再保存 `logical_type`、`vector_dim`、`vector_element_type` 一类字段。

后续如果支持连接外部 Iceberg 表、跨系统 schema evolution、隐藏列或复杂 drop/rename 历史，再评估是否需要独立 `field_id_map(relid, attnum, field_id)` 表；该表即便存在，也只服务 catalog 一致性和诊断，不作为 Rust SDK open 接口入参。

## 5. Type Adapter

### 5.1 职责

`type_adapter` 负责三类工作：

1. DDL 期：把 openGauss 列类型映射为 Iceberg schema 字段类型。
2. 规划期：建立 `attnum -> column_name`、`attnum -> pg_type/typmod` 与 Iceberg 物理字段类型的扫描映射。
3. 执行期：把 Arrow/Iceberg 值转换为 openGauss `Datum`，填入 `TupleTableSlot`。

managed-only 模式下，查询期不再做“openGauss 外表是否兼容外部 Iceberg schema”的完整校验；外表列类型在 DDL 期已经校验并生成 Iceberg metadata，查询期只做轻量一致性检查：

- `relid` 是否存在 internal catalog 记录。
- 当前 schema 是否能找到所有投影列对应的顶层列名。
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

向量类型约束：

- 不在 Iceberg 中创建自定义 logical type。
- 不在 FDW catalog 中保存 `attnum -> logical_type` 或 vector 维度映射。
- Iceberg schema 只记录 `list<float>` 作为物理存储类型。
- openGauss 外表列类型 `vector(n)` 是向量语义和维度的权威来源；DDL 期必须要求固定 typmod。
- 执行期通过 openGauss vector 类型接口构造/解析 Datum，不能依赖 Iceberg catalog 的 logical type 字段。

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
typedef struct IcebergFdwColumnMapping {
    AttrNumber attnum;
    char *column_name;
    Oid pg_type;
    int32 pg_typmod;
    Oid pg_collation;
    bool nullable;
    char *iceberg_field_type;    /* Iceberg 物理类型 */
} IcebergFdwColumnMapping;
```

`IcebergFdwColumnMapping` 不保存 logical type，也不保存 SDK 投影所需的 field id。标量转换和向量转换都从 `pg_type`、`pg_typmod`、`iceberg_field_type` 推导。对 `vector(n)` 而言，`pg_typmod` 保存维度，`iceberg_field_type` 固定为 `list<float>`。

### 5.4 调用点

| 函数 | 使用能力 |
| --- | --- |
| DDL hook create/alter | `iceberg_type_build_iceberg_schema_from_tupledesc`，生成 Iceberg field type |
| `GetForeignRelSize` | `iceberg_type_build_column_mappings`，构造规划期列映射 |
| `GetForeignPlan` | 根据投影列生成 `projected_column_names` |
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

- 识别可转换为 iceberg-rust `Predicate` 的简单谓词。
- 生成以当前顶层列名引用字段的 SDK filter expression。
- 对无法转换为 SDK filter 的谓词保留为 openGauss local qual。
- 首期也可保留全部原始 qual 作为防御性 recheck，但对已成功转换为 Rust SDK `Predicate` 的顶层标量谓词，正确性不再依赖本地 recheck。

关键约束：

- SDK filter 字段名必须是当前 Iceberg schema 的顶层列名，不传 field id。
- 桥接层负责把 FDW filter 表达式构造成 iceberg-rust `Predicate`，不是 SQL 字符串直传。
- Rust SDK 会用 filter 自动完成分区/文件/列统计/row-group 剪枝，并在 Arrow 读取路径执行行级过滤。
- 为降低初期风险，`GetForeignPlan` 可以继续把全部原始 `scan_clauses` 放入 local quals；后续确认桥接谓词覆盖完整后，可只保留未下推残差。

### 6.2 谓词下推范围

可尝试转换为 SDK `Predicate`：

| 类型 | 操作符 | SDK 行为 | local recheck |
| --- | --- | --- | --- |
| int/long | `=` `<` `<=` `>` `>=` | 剪枝 + 行级过滤 | 首期可保留防御性 recheck |
| int/long | `IS NULL` / `IS NOT NULL` | 剪枝 + 行级过滤 | 首期可保留防御性 recheck |
| varchar/text | `=` | 剪枝 + 行级过滤 | 首期可保留防御性 recheck |
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
    char *column_name;
    Oid pg_type;
    int32 pg_typmod;
    char *iceberg_field_type;
    IcebergFdwOperator op;
    char *literal_value;
    bool exact_in_sdk;
} IcebergFdwPredicate;

typedef struct IcebergFdwSdkFilter {
    List *predicates;        /* IcebergFdwPredicate* */
    bool exact_in_sdk;       /* rust Predicate 行级过滤成功时为 true */
} IcebergFdwSdkFilter;
```

`exact_in_sdk` 表示该 filter 已成功转换为 bridge 可解析的 iceberg-rust `Predicate` JSON，SDK 会在 Arrow 读取路径做行级精确过滤。首期是否仍把同一 qual 放进 local quals 是 FDW 防御策略，不代表 SDK 只能剪枝。

### 6.4 调用点

| 函数 | 使用能力 |
| --- | --- |
| `GetForeignRelSize` | 可选：估算谓词选择率时识别简单条件 |
| `GetForeignPlan` | 拆分 SDK predicate 与 local residual/recheck quals |
| `ExplainForeignScan` | 输出 SDK filter 与 local residual/recheck 数量 |

建议接口：

```c
typedef struct IcebergFdwQualClassification {
    List *sdk_predicates;      /* IcebergFdwPredicate */
    List *local_exprs;         /* Expr，首期可包含全部原始 quals，后续可仅保留残差 */
    IcebergFdwSdkFilter *sdk_filter;
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
    IcebergFdwSdkFilter *sdk_filter;
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

参考 openGauss 现有 FDW 实现，首期 `ForeignScan.fdw_private` 使用 `List` 传递执行期需要的信息。但当前实现不再放入裸 C 结构体指针，而是放入可复制的 `String` 节点，执行期再反序列化回运行态状态。

```c
enum IcebergFdwPrivateIndex {
    ICEBERG_FDW_PRIVATE_SCAN_OPTIONS_JSON = 0,
    ICEBERG_FDW_PRIVATE_PREDICATE_COUNT_JSON,
    ICEBERG_FDW_PRIVATE_FILTER_EXACT_JSON
};
```

当前 `fdw_private` 内容与实现一致：

```c
typedef struct IcebergFdwPrivateStrings {
    char *scan_options_json;
    char *predicate_count_json;
    char *filter_exact_json;
} IcebergFdwPrivateStrings;
```

其中 `scan_options_json` 内部包含：

- `projection`
- `snapshot_id`（若可解析）
- `batch_size`
- `case_sensitive`
- `row_group_filtering_enabled`
- `row_selection_enabled`
- `filter`

`filter` 是 bridge 可反序列化的 JSON，字段引用使用当前顶层列名。

### 7.5 `GetForeignPlan`

流程：

1. 从 `baserel->fdw_private` 取 `IcebergFdwPlanState`。
2. 调用 `operator_adapter` 生成 SDK filter。
3. 根据 `tlist` 和 local quals 计算需要读取的列。
4. 用当前顶层列名生成投影信息。
5. 序列化 `scan_options_json`、谓词计数和 exact 标志。
6. 调用 `make_foreignscan`，首期 local quals 可传入全部原始 quals 作为防御性 recheck。

伪代码：

```c
static ForeignScan *
icebergGetForeignPlan(...)
{
    IcebergFdwPlanState *fdw_state = baserel->fdw_private;

    IcebergFdwQualClassification quals;
    iceberg_operator_classify_scan_clauses(root, baserel, scan_clauses,
                                           fdw_state->column_mappings, &quals);
    char *scan_options_json = icebergSerializeBridgeScanOptionsJson(
        &fdw_state->options, quals.sdk_filter, icebergBuildProjectedColumns(fdw_state->column_mappings));
    char *predicate_count_json = psprintf("%d",
        quals.sdk_filter != NULL ? list_length(quals.sdk_filter->predicates) : 0);
    char *filter_exact_json = psprintf("%d",
        (quals.sdk_filter != NULL && quals.sdk_filter->predicates != NIL && quals.sdk_filter->exact_in_sdk) ? 1 : 0);
    List *fdw_private = list_make3(makeString(scan_options_json), makeString(predicate_count_json),
        makeString(filter_exact_json));

    return make_foreignscan(tlist,
                            quals.local_exprs,
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

    List *projected_columns;        /* IcebergFdwProjectedColumn* */
    char *bridge_scan_options_json;
    char *bridge_storage_config_json;
    int sdk_predicate_count;
    bool sdk_filter_exact;
    IcebergSdkScan *iceberg_scan;
    ArrowArray *current_array;
    ArrowSchema *current_schema;
    int current_row;
    int current_batch_rows;

    bool delta_scan_enabled;       /* 执行期确认存在 delta 表后置 true */
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
3. 读取 `metadata_location`、`table_name`、`table_uuid` 和当前 schema 信息。
4. 组装 `IcebergSdkScanRequest`，其中包含 `storage_config_json`、`metadata_location`、`table_name`、`scan_options_json` 和投影列名。
5. 调用 `sdk_scan_adapter` 打开 Iceberg scan。
6. 调用 `delta_scan_adapter` 在 openGauss 侧判断是否存在对应 delta 表；存在则初始化 delta scan handle。
7. 创建 scan memory context。

`BeginForeignScan` 不再重新查询 catalog 元数据。执行期所需的 `metadata_location`、`table_name`、`storage_config_json`、`scan_options_json` 和投影列名均由 `GetForeignPlan` / `icebergGetOptions()` 组合而来。若后续需要处理计划缓存失效或 schema 演进并发问题，应通过 catalog invalidation 或重新规划解决，而不是在 `BeginForeignScan` 中重新修正计划。

SDK scan request：

```c
typedef struct IcebergSdkScanRequest {
    const char *storage_config_json;
    const char *metadata_location;
    const char *table_name;
    const char *scan_options_json;
    List *projected_columns;
} IcebergSdkScanRequest;
```

请求约束：

- `storage_config_json` 由 FDW 侧构造。当前 file warehouse 使用 `{"storage_scheme":"fs","warehouse":"file://..."}`。
- `scan_options_json` 由 FDW 侧构造，bridge 负责解析出 projection、filter、batch size 等字段。
- `projected_columns` 只包含当前顶层列名；不传 field id。
- `filter` 在 `scan_options_json` 中以 JSON 形式传入，bridge 负责把它反序列化为 iceberg-rust `Predicate`。
- `snapshot_id` 的传递取决于 bridge 对应能力；当前实现优先通过 `metadata_location` 指向目标快照对应的 metadata。
- Iceberg delete file/MOR 由 Rust SDK `to_arrow()` 路径默认应用，FDW 不自行实现 MOR 合并逻辑。

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

首期简单关闭并重新打开 SDK scan 与 delta scan：

1. `iceberg_sdk_scan_close`。
2. `delta_scan_close`。
3. 清理 batch context。
4. 使用执行期状态中保存的 scan entry 和 projection 重新打开扫描句柄。

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
| `Projection Columns` | `order_id,status` |
| `SDK Filter` | predicate 摘要 |
| `Filter Mode` | `rust predicate exact; local recheck enabled/disabled` |
| `Delta Scan` | `enabled` / `disabled` |
| `MOR Support` | `provided by rust SDK` |

## 8. SDK Scan Adapter

### 8.1 职责

`sdk_scan_adapter` 封装 Iceberg Rust bridge + Iceberg SDK + Arrow C Data Interface。

能力：

- FDW 编译期只依赖一个公共 C ABI 头，例如 `iceberg_bridge_abi.h`，不直接依赖 bridge Rust 内部结构。
- 运行时通过 `dlopen()` / `dlsym()` 加载 `libiceberg_rust_bridge.so`。
- 根据 `storage_config_json`、`metadata_location` 和 `table_name` 打开 Iceberg 表。
- 按顶层列名做列裁剪，bridge 侧再由 Rust SDK 解析为 field id。
- 接收 `scan_options_json`，由 bridge 将其中的 `filter` JSON 反序列化为 iceberg-rust `Predicate`。
- 依赖 Rust SDK 自动完成分区/文件/列统计/row-group 剪枝和行级过滤。
- 通过 `scan_next` 按批取回 Arrow 数据，再由 FDW 转成 openGauss `TupleTableSlot`。
- Iceberg delete file/MOR 由 Rust SDK Arrow 读取路径应用；FDW 层只处理 openGauss delta 表叠加。

### 8.2 接口

```c
IcebergSdkScan *iceberg_sdk_scan_open(
    MemoryContext cxt,
    const IcebergSdkScanRequest *request);

int iceberg_sdk_scan_next(
    IcebergSdkScan *scan,
    ArrowArray **out_array,
    ArrowSchema **out_schema);

void iceberg_sdk_scan_release_batch(IcebergSdkScan *scan);
void iceberg_sdk_scan_close(IcebergSdkScan *scan);
```

实现边界：

- FDW 不需要在链接期持有 bridge `.so`。
- 运行时由 `icebergBridgeApi()` 通过 `ICEBERG_RUST_BRIDGE_SO` 或 `ICEBERG_RUST_BRIDGE_HOME` 解析动态库路径。
- `scan_open` / `scan_next` 走 batch 路径；`scan_next` 返回每一批的数据句柄，FDW 负责把 batch materialize 成 Arrow 数据再转 tuple。
- 临时的 stream 适配只用于验证或过渡，不作为首期正式方案。

### 8.3 SDK 约束

- bridge 的 `scan_open` 入口只接受 JSON 形式的扫描参数，不接受 SQL 字符串或 openGauss expression tree。
- `scan_options_json` 里的 `projection` 必须是当前顶层列名数组，不是 field id。
- `scan_options_json` 里的 `filter` 必须是 bridge 可反序列化的 predicate JSON；当前实现使用 `Binary`、`Unary` 和 `And` 结构。
- 已成功转换的 SDK filter 在 Rust Arrow 路径中执行行级过滤；本地 recheck 是首期防御策略或未下推残差处理，不是 Rust SDK 正确性的必要条件。
- Rust SDK 默认处理 Iceberg position/equality delete；这与 openGauss delta 表不是同一概念。

## 9. Delta Scan Adapter

### 9.1 语义

delta 表是 openGauss 表，用于记录尚未更新到 Iceberg metadata 的新鲜 IUD 数据。它不是 Iceberg MOR delete file，也不是 Iceberg metadata 中的 delete file。

因此 delta 扫描不需要读取 Iceberg metadata 或 Iceberg data file，也不需要走 SDK。FDW 在 base Iceberg scan 之外，只需要预留一个 openGauss 侧 delta 表判断和扫描入口。

查询时的完整语义后续应演进为：

```text
final result = Iceberg base snapshot scan overlay visible delta IUD rows
```

后续完整实现不能只是简单追加 delta insert 行，还需要根据 delta 表中的 update/delete 记录对 base snapshot 行做可见性覆盖。首期本文只要求在 `IterateForeignScan` 中预留接口：

1. 执行期判断目标外表是否存在对应 delta 表。
2. 如果存在，打开 openGauss delta 表 scan。
3. Iceberg base scan EOF 后，从 delta scan 返回可见行。
4. 复杂 IUD 覆盖、去重、删除遮蔽规则由后续 delta 表方案定义。

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

Oid iceberg_delta_lookup_relation(Oid base_relid);

DeltaScanHandle *iceberg_delta_scan_begin(
    MemoryContext cxt,
    Oid delta_relid,
    const DeltaScanRequest *request);

bool iceberg_delta_scan_next(
    DeltaScanHandle *handle,
    TupleTableSlot *slot);

void iceberg_delta_scan_rescan(DeltaScanHandle *handle);
void iceberg_delta_scan_end(DeltaScanHandle *handle);
```

### 9.3 预留字段

delta scan 不需要通过 `fdw_private` 传递计划项。执行期已经有 `relid`、`snapshot_id`、投影列和 openGauss snapshot，`BeginForeignScan` 可直接调用 `iceberg_delta_lookup_relation(relid)` 判断是否存在对应 delta 表；不存在则跳过 delta 阶段。实际 delta 表设计和 IUD 合并规则由后续 DML 方案补充。

## 10. 当前约束与错误策略

### 10.1 Managed-only 约束

- 表必须由 `CREATE FOREIGN TABLE ... SERVER iceberg_fdw` 创建。
- Iceberg metadata 只能由 FDW DDL hook 和事务 hook 修改。
- 不允许外部进程直接改同一张 managed 表的 metadata。
- 不支持 external read-only Iceberg 表。

### 10.2 扫描约束

- 本文只展开基础全表扫描路径。
- 不做 LIMIT pushdown。
- 不做 ORDER BY pushdown。
- SDK projection 和 filter 均按顶层列名传入，FDW 不向 Rust SDK 下传 field id。
- Rust SDK filter 可执行行级精确过滤；首期可保留本地 recheck 作为防御层。
- Iceberg delete file/MOR 由 Rust SDK Arrow 路径处理；FDW 不自行实现 Iceberg MOR。

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
8. 实现 `GetForeignPlan` 的 projection、JSON scan options、SDK filter、local recheck、fdw_private。
9. 实现 bridge ABI 头和 SDK scan adapter 的 runtime `dlopen` / `scan_next` batch 读取。
10. 实现 Arrow 到 slot 的 `type_adapter` 转换。
11. 预留 delta scan adapter 并接入 `IterateForeignScan` 阶段切换。
12. 实现 `ALTER FOREIGN TABLE` 的受控 schema evolution。
13. 补充 `EXPLAIN` 输出和错误路径测试。
