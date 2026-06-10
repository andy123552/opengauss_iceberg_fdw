# openGauss-Iceberg 类型转换与操作符对齐详细设计

## 1. 设计目标

实现 Iceberg FDW 扫描链路中 openGauss、Iceberg、底层索引系统之间的数据类型转换和操作符语义对齐，首期支持：

- 整数族：openGauss `int2`/`int4`/`int8` <-> Iceberg `int`/`long`。
- `char`/字符串：openGauss `char(n)`/`varchar` <-> Iceberg `string`。
- `vector`：openGauss `vector(n)` <-> Iceberg `list<float>`，并对齐向量索引 metric。

设计重点是数据准确性，而不是尽可能多地下推。任何语义不确定的转换或操作符下推都必须退回本地 recheck。

本设计当前只覆盖 `SELECT`/scan 能力，不覆盖 INSERT/UPDATE/DELETE 写入能力。与 DML 相关的值编码、事务、delta write、rollback 等能力不在本文范围内。为了后续接入 Iceberg delta 表，本设计只在扫描状态、SDK scan request 和类型转换上下文中预留 delta 表扫描接口，确保未来能够读取 base data file 与 delete/delta 信息合并后的结果。

## 2. 总体架构

新增 `type_adapter` 与 `operator_adapter` 两个 FDW 内部模块，位于 FDW 主流程和团队提供的 Iceberg SDK 封装、catalog 元数据模块、索引系统接口之间。

```
openGauss planner/executor
        |
        v
iceberg_fdw callbacks
        |
        +-- catalog_adapter
        |     - relid -> Iceberg table identity
        |     - metadata_location/current snapshot/schema fields
        |     - vector logical metadata
        |
        +-- type_adapter
        |     - schema mapping
        |     - Arrow/Iceberg value -> Datum
        |     - Datum const/Param -> predicate value
        |     - Arrow -> Datum
        |
        +-- operator_adapter
        |     - Expr/OpExpr recognition
        |     - predicate normalization
        |     - index capability matching
        |     - recheck policy
        |
        +-- sdk_scan_adapter
        |     - build Iceberg scan request
        |     - pass projection/predicate/snapshot/delta options
        |     - receive Arrow batch
        |
        +-- index_adapter
        |     - scalar index capability
        |     - vector top-k capability
        |     - optional prefilter
        |
        v
Iceberg Java SDK wrapper / metadata catalog / index service
        |
        v
Iceberg metadata, data files, delete/delta files, indexes
```

模块边界：

| 模块 | 输入 | 输出 |
| --- | --- | --- |
| `catalog_adapter` | foreign table `relid`、server/user/table options | Iceberg table identity、`metadata_location`、current schema/snapshot、field id/type 摘要 |
| `type_adapter` | openGauss `TupleDesc`、catalog schema、Arrow schema/value、Datum 常量 | scan 列映射、Datum、标准 predicate value、错误、recheck 标志 |
| `operator_adapter` | openGauss expr tree、type mapping、index capabilities | 标准 scalar predicate、vector search request、remote/local qual 划分 |
| `sdk_scan_adapter` | table identity、projection、snapshot、remote scalar predicates、delta scan options | Iceberg SDK scan handle、Arrow batch |
| `index_adapter` | 标准 scalar predicate/vector search request、index capability | 底层索引查询请求、候选文件/row id/top-k 结果 |

### 2.1 与 Iceberg SDK 封装的关系

本文依赖 `6. fdw-iceberg-sdk-arrow-design.md` 中的 SDK/Arrow 封装。该封装对 FDW 暴露扫描接口：

```c
IcebergScan *iceberg_scan_open(MemoryContext cxt,
                                const char *metadata_path,
                                const char *storage_config,
                                const char **columns, int n_columns,
                                const char *filter,
                                ArrowSchema **out_schema);

int iceberg_scan_next(IcebergScan *scan,
                      ArrowArray **out_array,
                      ArrowSchema **out_schema);

void iceberg_scan_close(IcebergScan *scan);
```

类型/操作符设计对 SDK 封装的调用要求：

- `operator_adapter` 输出的可下推 scalar predicate 必须序列化为 SDK 可理解的 Iceberg expression，而不是传递 openGauss OID 或表达式树。
- `type_adapter` 负责把 openGauss 常量 Datum 编码成 Iceberg expression 常量值，SDK 封装只消费 Iceberg 类型值。
- `type_adapter` 使用 `iceberg_scan_open` 返回的 `ArrowSchema` 校验 Arrow 实际类型与 catalog schema 是否一致，再初始化 Arrow 到 Datum converter。
- `sdk_scan_adapter` 每个 batch 调用 `iceberg_scan_next` 后，由 `type_adapter` 完成 Arrow 列式数据到 openGauss tuple slot 的逐行转换。
- SDK 如果因为 delete file 进入 MOR 行式回退路径，对 FDW 层仍必须输出同构 Arrow batch；`type_adapter` 不感知快路径/回退路径差异。

现有 `iceberg_scan_open` 的 `filter` 参数建议演进为结构化 scan request，至少包含：

```c
typedef struct IcebergFdwSdkScanRequest {
    const char *metadata_path;
    const char *storage_config;
    int64 snapshot_id;
    const int *projected_field_ids;
    int n_projected_fields;
    const IcebergFdwPredicate *predicates;
    int n_predicates;
    bool enable_delta_scan;
    const char *delta_scan_mode;
    const char *index_result_ref;
} IcebergFdwSdkScanRequest;
```

`enable_delta_scan` 和 `delta_scan_mode` 是预留字段。首期默认 `enable_delta_scan=false`，只读取当前 snapshot 的普通 scan 结果。后续支持 delta 表扫描时，该字段用于指示 SDK 读取 base data file 后叠加 position/equality delete 或团队提供的 delta-table scan 结果。

### 2.2 与 catalog 元数据模块的关系

本文依赖 `gv_catalog_metadata_schema_design.md` 中的 catalog 元信息模块。`catalog_adapter` 不重新解析全部 Iceberg metadata，而是优先从 catalog 表读取高频摘要：

- `tables_internal`：根据 `relid` 找到 `metadata_location`、`table_uuid`、`current_schema_id`、`current_snapshot_id`、`table_location`。
- `table_schemas`：读取当前 schema 的 `field_id`、`field_name`、`field_required`、`field_type`。
- `snapshots`：读取当前 snapshot 的 `snapshot_id`、`schema_id`、`manifest_list` 和统计摘要。
- 字段级 vector metadata：若 catalog 表已扩展 logical type/dimension/metric 字段，优先读取；否则从 Iceberg schema properties 或 metadata 文件中读取。

类型/操作符设计需要 catalog 模块提供以下接口：

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
    int vector_dim;
    char *vector_metric_default;
} IcebergCatalogFieldInfo;

IcebergCatalogTableInfo *iceberg_catalog_get_table_info(Oid relid);
List *iceberg_catalog_get_fields(const IcebergCatalogTableInfo *table);
```

`type_adapter` 以 `field_id` 建立 openGauss attribute 与 Iceberg 字段的稳定映射；字段名只用于兼容、日志和 EXPLAIN。schema 或 snapshot 变化时，FDW 在 `BeginForeignScan` 阶段重新加载 catalog 信息并重建 mapping。

### 2.3 与索引系统的关系

索引系统以 Iceberg 底层数据和自身类型系统为准，不直接理解 openGauss Datum。`operator_adapter` 与 `type_adapter` 共同把 openGauss 表达式转换为索引系统可消费的标准请求：

- 标量过滤：`field_id + logical_type + op + encoded_value`。
- 向量 top-k：`field_id + metric + query_vector + limit + prefilter_predicates`。
- recheck 策略：标记结果是否需要 openGauss 本地 qual 复核。

索引系统返回候选文件、候选 row id、候选 top-k 或其他团队定义的 scan token。FDW 不直接解释底层索引结构，只把返回结果作为 `index_result_ref` 传给 SDK scan adapter，或在 FDW 层按 row identity 过滤 Arrow batch。首期若不接入索引，所有可扫描数据由 SDK 普通 scan 返回，openGauss 执行器完成排序和 LIMIT。

### 2.4 FDW scan 调用时序

规划阶段：

1. `GetForeignRelSize` 调用 `catalog_adapter`，通过 `relid` 读取 table/schema/snapshot 摘要。
2. `GetForeignRelSize` 调用 `type_adapter`，用 openGauss `TupleDesc` 与 catalog field 信息建立 `field_id` 级映射。
3. `GetForeignPaths` 调用 `operator_adapter`，把 `baserestrictinfo`、pathkeys、LIMIT 上下文拆成 remote predicate、vector top-k request 和 local recheck qual。
4. `GetForeignPaths` 调用 `index_adapter` 查询能力与代价；如果索引能力不足，退回 SDK 普通 scan path。
5. `GetForeignPlan` 把 projection、remote predicate、local recheck qual、snapshot id、可选 index result ref 写入 FDW private plan。

执行阶段：

1. `BeginForeignScan` 重新加载 catalog 快照信息，校验 plan 中的 schema/snapshot 与当前执行上下文是否兼容。
2. `BeginForeignScan` 初始化 `type_adapter` converter，并构造 `IcebergFdwSdkScanRequest`。
3. 如使用索引，先调用 `index_adapter` 获得候选 token 或 top-k 结果，再把 `index_result_ref` 传入 SDK scan request。
4. `BeginForeignScan` 调用 SDK 封装打开 scan；SDK 返回 Arrow schema 后，`type_adapter` 做二次校验。
5. `IterateForeignScan` 循环读取 Arrow batch，并逐行转换为 openGauss slot；local recheck qual 由 openGauss 执行器或 FDW scan 节点保留执行。
6. `EndForeignScan` 关闭 SDK scan handle，释放 batch/context/index token。

接口约束：

- `catalog_adapter` 是 schema/snapshot 的权威入口，`type_adapter` 不直接扫描 catalog 表。
- `operator_adapter` 不直接调用 SDK；它只产出标准 predicate/search request。
- `sdk_scan_adapter` 不解释 openGauss 表达式；它只接收已经标准化的 Iceberg predicate、projection、snapshot 和 delta scan options。
- `index_adapter` 不接收 openGauss Datum；所有值都必须先经过 `type_adapter` 编码。

## 3. 核心数据结构

### 3.1 类型描述

```c
typedef enum IcebergFdwLogicalType {
    ICEBERG_FDW_TYPE_INT16,
    ICEBERG_FDW_TYPE_INT32,
    ICEBERG_FDW_TYPE_INT64,
    ICEBERG_FDW_TYPE_STRING,
    ICEBERG_FDW_TYPE_VECTOR_FLOAT32
} IcebergFdwLogicalType;

typedef enum IcebergFdwCoercionPolicy {
    ICEBERG_FDW_COERCE_EXACT,
    ICEBERG_FDW_COERCE_RECHECK_REQUIRED,
    ICEBERG_FDW_COERCE_REJECT
} IcebergFdwCoercionPolicy;

typedef struct IcebergFdwTypeMapping {
    AttrNumber attnum;
    int iceberg_field_id;
    char *iceberg_name;

    Oid pg_type;
    int32 pg_typmod;
    Oid pg_collation;

    IcebergFdwLogicalType logical_type;
    int vector_dim;
    bool nullable;

    IcebergFdwCoercionPolicy read_policy;
    IcebergFdwCoercionPolicy const_encode_policy;
    IcebergFdwCoercionPolicy predicate_policy;
} IcebergFdwTypeMapping;
```

设计要求：

- `iceberg_field_id` 是列身份主键。
- `attnum` 只用于 openGauss tuple slot 位置。
- `iceberg_name` 只用于日志、EXPLAIN 和兼容缺少 field id 的数据文件。
- `pg_typmod` 对 `vector(n)`、`char(n)`、`varchar(n)` 必须生效。
- `predicate_policy` 可比 `read_policy` 更保守。
- `const_encode_policy` 只服务 scan 谓词常量和向量 query 参数，不代表 DML 写入能力。

### 3.2 标准值

```c
typedef struct IcebergFdwValue {
    IcebergFdwLogicalType type;
    bool is_null;
    union {
        int16 int16_value;
        int32 int32_value;
        int64 int64_value;
        struct {
            const char *data;
            int len;
        } string_value;
        struct {
            int dim;
            const float *values;
        } vector_value;
    } v;
} IcebergFdwValue;
```

约束：

- 字符串值用长度而不是 `\0` 结束判断。
- vector 元素必须是 float32，不能含 NaN/Inf。
- vector 不允许元素级 NULL。
- `is_null=true` 时 union 内容无效。

### 3.3 标准操作符

```c
typedef enum IcebergFdwOperator {
    ICEBERG_FDW_OP_EQ,
    ICEBERG_FDW_OP_NE,
    ICEBERG_FDW_OP_LT,
    ICEBERG_FDW_OP_LE,
    ICEBERG_FDW_OP_GT,
    ICEBERG_FDW_OP_GE,
    ICEBERG_FDW_OP_IS_NULL,
    ICEBERG_FDW_OP_IS_NOT_NULL,

    ICEBERG_FDW_OP_VECTOR_L2,
    ICEBERG_FDW_OP_VECTOR_DOT,
    ICEBERG_FDW_OP_VECTOR_COSINE,
    ICEBERG_FDW_OP_VECTOR_L1
} IcebergFdwOperator;

typedef enum IcebergFdwRecheckMode {
    ICEBERG_FDW_RECHECK_NONE,
    ICEBERG_FDW_RECHECK_LOCAL_QUAL,
    ICEBERG_FDW_RECHECK_REFINE_VECTOR
} IcebergFdwRecheckMode;
```

## 4. 类型映射策略

### 4.1 整数族

首期支持 openGauss `int2`、`int4`、`int8` 与 Iceberg `int`、`long` 的映射。

| openGauss | Iceberg | 读策略 | 常量编码策略 | 谓词策略 |
| --- | --- | --- | --- | --- |
| `INT2OID` | `int` | Iceberg int32 读入后检查 int16 范围，用 `Int16GetDatum` | `DatumGetInt16` 后提升为 int32 | 强下推，常量按 int32 编码 |
| `INT4OID` | `int` | `Int32GetDatum` | `DatumGetInt32` | 强下推 |
| `INT8OID` | `long` | `Int64GetDatum` | `DatumGetInt64` | 强下推 |
| `INT2OID` | `long` | Iceberg int64 读入后检查 int16 范围 | `DatumGetInt16` 后提升为 int64 | 强下推，常量按 int64 编码 |
| `INT4OID` | `long` | Iceberg int64 读入后检查 int32 范围 | `DatumGetInt32` 后提升为 int64 | 强下推，常量按 int64 编码 |

拒绝或受限场景：

- `INT8OID` -> Iceberg `int` 是范围收缩，默认拒绝。只有显式用户配置允许且谓词常量在 int32 范围内时，才能作为候选谓词编码；谓词默认不强下推。
- Iceberg `long` -> openGauss `int2`/`int4` 需要逐值范围检查。为避免扫描中途才失败，建表/映射阶段默认拒绝；若用户显式声明字段值域受控，可允许 scan，但仍保留范围错误。
- Iceberg `int` -> openGauss `int8` 是安全提升，可支持，但默认建议把 openGauss 外表列声明为 `int4` 以反映真实 Iceberg schema。

索引 predicate：

- `=`, `!=`, `<`, `<=`, `>`, `>=`, `IS NULL` 对精确匹配和安全提升场景可强下推。
- 常量类型不等于列类型时，先按 openGauss coercion 规则归一化，再按 Iceberg 目标类型编码。
- `int2` 映射 Iceberg `int` 时，索引端看到的是 int32；FDW 负责保证 openGauss 侧读回仍满足 int16。

### 4.2 char / 字符串

首期支持：

| openGauss 类型 | Iceberg 类型 | 读策略 | 常量编码策略 | 谓词策略 |
| --- | --- | --- | --- | --- |
| `BPCHAROID` | `string` | 读入后按 openGauss `bpchar` typmod 规范化 | 按 openGauss `bpchar` 常量语义编码 | `=` 候选下推 + recheck |
| `VARCHAROID` | `string` | 按 typmod 校验 | 按 typmod 校验 | `=` 可配置强下推，默认 recheck |
| `CHAROID` | 不默认映射 | 拒绝 | 拒绝 | 拒绝 |

`bpchar` 处理：

- 读入 Iceberg `string` 后调用 openGauss 类型输入/typmod 路径或等价函数，保证长度和 blank padding 语义由 openGauss 决定。
- 编码 `bpchar` 常量时要明确是否保留尾部空格。建议保留 openGauss Datum 转文本后的语义值，不做额外 trim。
- `bpchar = const` 只作为 candidate predicate 下推，最终保留原 `OpExpr` 在本地执行器 recheck。

`varchar` 处理：

- 读入时超过 typmod 应报错，而不是截断。
- 谓词常量编码时同样报错。
- 若 `attcollation` 是 binary/C 语义，且索引系统声明字符串比较也是字节序，可强下推 `=` 和范围谓词。
- 其他 collation 默认候选下推或不下推。

编码策略：

- FDW 内部标准字符串统一 UTF-8。
- openGauss 数据库编码为 UTF-8 时直接传递。
- 非 UTF-8 数据库编码首期拒绝字符串索引强下推；读取和谓词常量编码可以通过编码转换函数实现后再打开。

### 4.3 vector

首期映射：

| openGauss | Iceberg | Arrow/Parquet 建议 | 约束 |
| --- | --- | --- | --- |
| `vector(n)` | `list<float>` | FixedSizeList<Float32> 优先 | 维度必须等于 `n` |
| `vector` 无 typmod | `list<float>` | List<Float32> | 读首批后可缓存维度；索引列必须有固定维度 |

列级 metadata 建议：

```text
iceberg.field.<field-id>.logical_type = vector
iceberg.field.<field-id>.vector.dimension = N
iceberg.field.<field-id>.vector.element_type = float32
iceberg.field.<field-id>.vector.metric.default = l2
```

读入规则：

1. Iceberg/Arrow 值为 NULL：openGauss 值为 NULL。
2. list 元素类型必须为 float32；float64 首期拒绝，避免精度变化。
3. list 不允许 null element。
4. 维度必须大于 0 且不超过 `VECTOR_MAX_DIM=16000`。
5. 若 openGauss typmod 指定维度，实际维度必须相等。
6. 元素必须有限，拒绝 NaN/Inf。
7. 分配 openGauss `Vector` varlena，并用 float32 填充。

谓词/查询向量编码规则：

1. detoast 查询向量 `Datum` 为 `Vector*`。
2. 读取 `dim` 和 `x[]`。
3. 按 Iceberg field id 编码为 vector query value。
4. 同样校验维度和有限性。

## 5. 操作符识别与下推

### 5.1 planner 输入形态

`operator_adapter` 需要识别：

- `RestrictInfo` 中的 `OpExpr`。
- `NullTest`。
- `ScalarArrayOpExpr`，后续可支持 `IN`，首期可拒绝。
- `SortGroupClause`/pathkeys 中的 vector distance order by。
- `Limit`，vector top-k 必须有 LIMIT 才下推为 ANN/top-k。

### 5.2 标量谓词矩阵

| 类型 | 操作符 | 下推级别 | 本地 recheck |
| --- | --- | --- | --- |
| int/long | `=` | 强下推 | 否 |
| int/long | `<`, `<=`, `>`, `>=` | 强下推 | 否 |
| int/long | `!=` | 强下推或残差 | 建议是，取决于索引能力 |
| int/long | `IS NULL` | 强下推 | 否 |
| varchar | `=` | 默认候选下推 | 是 |
| varchar | 范围 | 默认不下推 | 是 |
| bpchar | `=` | 候选下推 | 是 |
| bpchar | 范围 | 不下推 | 是 |

强下推的前提：

- 常量类型与列类型经过 openGauss coercion 后确定。
- 操作符 OID 与类型 OID 在白名单中。
- 对应 Iceberg/index 类型完全匹配。
- collation 和比较语义一致。

### 5.3 vector top-k 矩阵

| SQL | openGauss 函数 | 索引 metric | 排序语义 | 下推条件 |
| --- | --- | --- | --- | --- |
| `ORDER BY v <-> q LIMIT k` | `l2_distance` | `l2` | distance ASC | 可下推 |
| `ORDER BY v <#> q LIMIT k` | `negative_inner_product` | `dot` / `inner_product` | similarity DESC 等价于 negative ASC | 可下推 |
| `ORDER BY v <=> q LIMIT k` | `cosine_distance` | `cosine` | distance ASC | 非零向量可下推 |
| `ORDER BY v <+> q LIMIT k` | `l1_distance` | `l1` | distance ASC | 索引声明支持才下推 |

请求结构建议：

```c
typedef struct IcebergFdwVectorSearch {
    int field_id;
    IcebergFdwOperator metric;
    IcebergFdwValue query_vector;
    int limit;
    bool approximate_ok;
    IcebergFdwRecheckMode recheck_mode;
    List *prefilter_predicates;
    List *postfilter_quals;
} IcebergFdwVectorSearch;
```

规则：

- vector top-k 必须携带 `LIMIT`；无 LIMIT 时不下推 ANN，只保留普通排序。
- 查询向量必须是常量或 stable 参数，可在执行期绑定。
- 查询向量维度必须与列维度一致。
- `cosine` 查询向量不能是零向量；索引系统也必须声明如何处理存量零向量。
- 如果索引返回近似结果，EXPLAIN 必须显示 approximate/refine 状态。
- 如果标量谓词不能 prefilter，则作为 postfilter/local qual；top-k 的正确性会受 postfilter 影响，必须在 EXPLAIN 中标出。

### 5.4 操作符白名单

实现时维护一张静态/动态白名单。静态表用于首期快速落地，动态校验用于防止 OID 变化。

```c
typedef struct IcebergFdwOperatorRule {
    Oid left_type;
    Oid right_type;
    const char *operator_name;
    IcebergFdwOperator op;
    bool requires_binary_collation;
    bool supports_index_exact;
    bool requires_recheck;
} IcebergFdwOperatorRule;
```

启动或首次使用时通过系统缓存校验：

- `OpernameGetOprid` 找 operator OID。
- `pg_operator` 校验左右类型和 `oprcode`。
- `pg_opclass`/`pg_opfamily` 校验索引 metric 与 opclass 名称。

不要只靠字符串匹配 `<->`、`=`。

## 6. 转换 API 设计

### 6.1 初始化 schema mapping

```c
List *iceberg_type_build_mappings(Relation rel,
                                  const List *catalog_fields,
                                  ArrowSchema *arrow_schema);
```

职责：

- 遍历 openGauss foreign table `TupleDesc`。
- 根据 `catalog_adapter` 返回的 field 信息找 Iceberg field id。
- 如果 `arrow_schema` 非空，校验 SDK 实际输出 schema 与 catalog schema 一致。
- 读取 `pg_type` 元数据。
- 校验 Iceberg 类型兼容性。
- 为每列生成 `IcebergFdwTypeMapping`。

失败示例：

- openGauss `int8` 对应 Iceberg `int`：默认报错，避免范围收缩。
- openGauss `int4` 对应 Iceberg `long`：允许安全提升，但读回时检查 int32 范围。
- openGauss `vector(768)` 对应 Iceberg `list<double>`：报错。
- openGauss `bpchar(10)` 对应 Iceberg `string`：允许，但 predicate recheck。

### 6.2 Iceberg/Arrow 到 Datum

```c
Datum iceberg_type_to_datum(
    const IcebergFdwTypeMapping *mapping,
    const IcebergFdwValue *value,
    bool *isnull);
```

实现：

- `INT16`：检查范围后 `Int16GetDatum(value->v.int16_value)`。
- `INT32`：`Int32GetDatum(value->v.int32_value)`。
- `INT64`：`Int64GetDatum(value->v.int64_value)`。
- `STRING`：调用 openGauss 字符类型输入函数，传入 `pg_typmod` 和 collation。
- `VECTOR_FLOAT32`：分配 `Vector`，设置 varlena size、`dim`、`x[]`。

内存：

- 返回的 varlena 必须在当前 per-tuple 或 batch memory context 中。
- batch 完成后统一释放。

### 6.3 Datum 常量到 Iceberg/Index 值

```c
bool iceberg_const_datum_to_value(
    const IcebergFdwTypeMapping *mapping,
    Datum datum,
    bool isnull,
    IcebergFdwValue *out,
    char **error);
```

用途限定：

- 常量 predicate 下推。
- vector query 参数绑定。
- 执行期 stable/Param 参数绑定。

规则：

- NULL 直接设置 `out->is_null=true`。
- 字符串必须使用 openGauss 输出函数或类型专用函数，避免绕过 typmod/collation 语义。
- vector 通过 `DatumGetVector` detoast，复制为索引系统可持有的 float32 buffer。
- 该 API 不作为 INSERT/UPDATE 写入编码接口；未来支持 DML 时需要单独设计写入路径的事务、幂等和错误恢复策略。

### 6.4 Predicate 编码

```c
typedef struct IcebergFdwPredicate {
    int field_id;
    IcebergFdwOperator op;
    IcebergFdwValue value;
    IcebergFdwRecheckMode recheck;
} IcebergFdwPredicate;
```

向 index adapter 输出时必须包含：

- field id。
- logical type。
- operator。
- value。
- recheck mode。
- collation policy。

## 7. 索引能力协商

index adapter 需要暴露能力声明：

```c
typedef struct IcebergIndexTypeCapability {
    IcebergFdwLogicalType type;
    bool supports_eq;
    bool supports_range;
    bool supports_is_null;
    bool supports_vector_l2;
    bool supports_vector_dot;
    bool supports_vector_cosine;
    bool supports_vector_l1;
    bool exact;
    bool supports_prefilter;
    bool supports_postfilter;
    bool returns_distance;
    bool returns_similarity;
} IcebergIndexTypeCapability;
```

规划阶段决策：

1. 根据列类型和 operator 找到标准 predicate。
2. 查 index capability。
3. 若能力 exact 且语义一致，生成 index path 且不 recheck。
4. 若能力 candidate only，生成 index path 并保留 local qual。
5. 若能力不支持，不下推。

## 8. Delta 表扫描预留

当前范围不支持 DML，但需要为后续读取 delta 表预留接口。delta 表扫描指 FDW scan 能够读取一个逻辑 Iceberg snapshot 中 base data file 与 delete/delta 信息合并后的可见结果，输出仍然是普通 Arrow batch。

预留设计：

```c
typedef enum IcebergFdwDeltaScanMode {
    ICEBERG_FDW_DELTA_SCAN_OFF,
    ICEBERG_FDW_DELTA_SCAN_SDK_MOR,
    ICEBERG_FDW_DELTA_SCAN_TEAM_DELTA_API
} IcebergFdwDeltaScanMode;

typedef struct IcebergFdwDeltaScanOptions {
    IcebergFdwDeltaScanMode mode;
    int64 snapshot_id;
    bool apply_position_delete;
    bool apply_equality_delete;
    const char *delta_scan_token;
} IcebergFdwDeltaScanOptions;
```

模块职责：

- `catalog_adapter` 提供当前 snapshot、schema id、manifest list 摘要；必要时提供 delete/delta 相关 metadata 指针。
- `sdk_scan_adapter` 接收 `IcebergFdwDeltaScanOptions`，决定走 SDK MOR 路径还是团队提供的 delta scan API。
- `type_adapter` 只处理最终 Arrow batch 到 Datum 的转换，不感知某行来自 base file 还是 delta 合并结果。
- `operator_adapter` 继续只负责 scan predicate 和 vector top-k 请求，不生成 DML mutation。
- `index_adapter` 如果返回 row identity 或候选文件，需要标明该结果是否已应用 delta 可见性；未应用时必须交给 SDK/delta scan 层做可见性过滤。

正确性要求：

- 首期默认 `ICEBERG_FDW_DELTA_SCAN_OFF`。
- 一旦开启 delta scan，FDW 不能把未应用 delete/equality delete 的中间结果直接返回给 openGauss。
- delta scan 输出 schema 必须与 `type_adapter` 在 `BeginForeignScan` 建立的 field id/type mapping 一致。
- 如果索引结果基于旧 snapshot，必须拒绝与当前 snapshot 混用，除非索引系统显式声明 snapshot 一致性。

## 9. 测试计划

### 9.1 整数族

- `int2` 正负边界：`-32768`、`32767`。
- `int4` 正负边界：`-2147483648`、`2147483647`。
- `int8` 正负边界：`-9223372036854775808`、`9223372036854775807`。
- Iceberg `int` <-> openGauss `int2`/`int4`。
- Iceberg `long` <-> openGauss `int8`。
- openGauss `int2`/`int4` 安全提升到 Iceberg `long`。
- openGauss `int8` 映射 Iceberg `int` 默认报错。
- Iceberg `long` 读入 openGauss `int2`/`int4` 超范围报错。
- `=`, `<`, `<=`, `>`, `>=`, `IS NULL` 下推。
- 谓词常量编码边界：`int2`/`int4` 安全提升，`int8` 到 Iceberg `int` 默认拒绝。

### 9.2 char / 字符串

- `varchar(3)` 读入超长报错。
- `char(3)` 读入和常量比较尾部空格行为与 openGauss 本地表一致。
- `bpchar = const` 保留 local recheck。
- 非 binary collation 下范围谓词不强下推。
- NULL 与空字符串区分。

### 9.3 vector

- `vector(3)` 读取 `[1,2,3]`。
- 维度不一致报错。
- 空 vector、NaN、Inf 报错。
- list element null 报错。
- `<->`、`<#>`、`<=>` 识别为不同 metric。
- `ORDER BY v <-> q LIMIT k` 生成 vector search request。
- 无 LIMIT 的 vector order by 不下推 ANN。
- cosine 零向量拒绝下推。

### 9.4 recheck

- 字符串候选下推后，本地 recheck 能过滤掉语义不一致结果。
- ANN 近似结果 explain 标明 approximate。
- postfilter 场景 explain 标明 top-k 可能先取候选再过滤。

### 9.5 delta scan 预留

- 默认 scan request 中 `enable_delta_scan=false`。
- 开启 delta scan 选项但 SDK/catalog 未声明支持时，报清晰错误。
- SDK MOR 路径与普通路径输出同一 Arrow schema。
- 索引 snapshot 与 catalog current snapshot 不一致时拒绝使用索引结果。

## 10. 分阶段落地

### Phase 1: 类型映射基础

- 新增 `type_adapter` 文件。
- 实现 `INT2OID`、`INT4OID`、`INT8OID`、`BPCHAROID`、`VARCHAROID`、`VECTOROID` mapping。
- 实现 `IcebergFdwValue`。
- 实现读路径 Datum 构造。

### Phase 2: 谓词识别

- 新增 `operator_adapter` 文件。
- 识别 int 标量谓词。
- 识别字符串等值候选下推。
- 识别 vector top-k pathkey。

### Phase 3: index adapter 对接

- 定义 index capability。
- 将 predicate/vector search request 传给团队索引接口。
- explain 输出下推、recheck、metric、limit。

### Phase 4: delta scan 接口预留

- 在 scan request 中加入 `IcebergFdwDeltaScanOptions`。
- 将 catalog snapshot 信息传给 SDK scan adapter。
- 预留 index result snapshot 校验字段。
- 增加 delta scan 未启用/未支持时的错误路径测试。

## 11. 待确认问题

- 团队 Iceberg catalog 是否已为 vector 列记录固定维度和 logical type。
- 底层索引接口是否接收 `list<float>` 原值，还是要求独立 vector binary layout。
- 索引系统中 dot metric 返回的是相似度、内积距离，还是 openGauss 风格负内积。
- 字符串索引比较是否为 UTF-8 binary order。
- 是否需要首期支持 `text`。
- 是否允许 `vector` 无 typmod 建索引。
- delta scan 由 Iceberg SDK MOR 路径负责，还是由团队提供独立 delta-table scan API。
- delta scan 是否需要支持历史 snapshot，还是只支持 current snapshot。

## 12. 准确性默认值

首期默认策略：

- 整数族精确匹配和安全提升强下推；范围收缩默认拒绝。
- varchar/bpchar 读取支持，谓词默认 recheck。
- vector 只支持 `list<float>`，必须固定维度才能建索引。
- vector top-k 支持 l2/dot/cosine，l1 等索引系统声明后再打开。
- 所有未明确支持的类型、操作符和隐式 cast 都拒绝下推。
