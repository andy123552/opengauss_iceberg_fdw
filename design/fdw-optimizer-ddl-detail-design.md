# Iceberg FDW 优化器与外表 DDL 详细设计

## 1. 设计范围

本文补充 Iceberg FDW 在 openGauss FDW 侧的两个实现面：

1. 优化器部分：`GetForeignRelSize`、`GetForeignPaths`、`GetForeignPlan` 的代价估算、路径选择和计划生成。
2. 外表 DDL 部分：`CREATE SERVER`、`CREATE USER MAPPING`、`CREATE FOREIGN TABLE` 的 option 约束、列定义约束和 openGauss 列类型到 Iceberg 类型的映射。

本文只覆盖普通外表扫描路径。索引扫描、DML 写入、事务提交、delta 表合并和底层 Iceberg metadata/index/delta 实现由其他设计文档和团队接口覆盖。

首期明确不支持 `ANALYZE`：

- 不实现 `AnalyzeForeignTable`。
- 不依赖 `pg_class.reltuples`、`pg_statistic` 或 openGauss 本地采样统计。
- 不在规划期访问对象存储、metadata.json、manifest、Parquet footer。
- 代价估算只读取 catalog 元信息表中已经落表的摘要字段；catalog 没有的统计值只能使用固定默认值，不触发额外 I/O。

## 2. 依赖接口与数据来源

优化器只依赖三类输入：

| 来源 | 字段 | 用途 |
| --- | --- | --- |
| 外表 OPTIONS | `namespace`、`table_name`、`snapshot_id`、`enable_index_scan` | 定位 Iceberg 表、固定 snapshot、控制是否生成索引路径 |
| SERVER OPTIONS | `catalog_type`、`catalog_uri`、`warehouse` | 定位 catalog 和仓库 |
| Catalog 元信息表 | `tables_internal`、`table_schemas`、`snapshots` | 表指针、当前 schema、当前 snapshot、行数摘要、列映射 |

当前代码骨架已在 `iceberg_fdw/src/iceberg_fdw.cpp` 中声明以下 option 名称：

| 层级 | OPTION | 必填 | 说明 |
| --- | --- | --- | --- |
| server | `catalog_type` | 是 | catalog 后端类型，例如 `pg_native`、`rest`。首期实现以团队 adapter 支持的值为准。 |
| server | `catalog_uri` | 否 | catalog 服务地址或本地 catalog 标识。`pg_native` 可为空或由 adapter 解释。 |
| server | `warehouse` | 是 | Iceberg warehouse 根路径或存储命名空间。 |
| user mapping | `username` | 否 | catalog 或对象存储访问用户名。 |
| user mapping | `password` | 否 | catalog 或对象存储访问密码。 |
| foreign table | `namespace` | 是 | Iceberg namespace。首期只支持单级或已由 catalog adapter 标准化后的 namespace 字符串。 |
| foreign table | `table_name` | 是 | Iceberg 表名。 |
| foreign table | `snapshot_id` | 否 | 固定读取指定 snapshot；为空时读取 catalog 当前 snapshot。 |
| foreign table | `enable_index_scan` | 否 | 预留索引路径开关，首期普通扫描可忽略或只接受 `false`。 |

说明：早期设计文档中出现过 `catalog_kind`、`catalog_schema` 命名；实现文档以当前扩展骨架中的 `catalog_type`、`catalog_uri`、`warehouse` 为准。若后续代码改名，应同步迁移本文和 validator。

## 3. Catalog 统计边界

代价估算读取的 catalog 字段必须是普通表字段，不能通过规划期临时解析 Iceberg 文件链路获得。

### 3.1 必需字段

| 表 | 字段 | 说明 |
| --- | --- | --- |
| `iceberg_catalog.tables_internal` | `relid` | 绑定 openGauss 外表 OID，internal 表场景用于反查 Iceberg 表。 |
| `iceberg_catalog.tables_internal` | `namespace`、`table_name` | Iceberg 表身份。 |
| `iceberg_catalog.tables_internal` | `table_uuid` | 关联 schema、snapshot 的稳定表 ID。 |
| `iceberg_catalog.tables_internal` | `metadata_location` | 执行期 reader 打开 Iceberg 表的入口。 |
| `iceberg_catalog.tables_internal` | `current_schema_id` | 当前 schema 版本。 |
| `iceberg_catalog.tables_internal` | `current_snapshot_id` | 当前 snapshot。 |
| `iceberg_catalog.snapshots` | `total_records` | 当前 snapshot 总行数摘要，来自 `snapshots[].summary.total-records`。 |

`total_records` 是普通扫描路径的唯一必需行数统计。若当前 snapshot 没有 `total_records`，FDW 不读取 metadata.json 兜底，改用固定默认行数。

### 3.2 可选增强字段

若 catalog 后续把 Iceberg snapshot summary 中的字段拍平为普通列，优化器可以使用这些字段改进 cost，但必须保持字段缺失时可退化：

| 可选字段 | Iceberg 来源 | 用途 |
| --- | --- | --- |
| `total_data_files` | `summary.total-data-files` | 估算文件打开和 task 数量成本。 |
| `total_file_size_in_bytes` | `summary.total-file-size-in-bytes` | 估算顺序读取成本。 |
| `total_delete_files` | `summary.total-delete-files` | 估算 MOR/delete merge 附加成本。 |
| `partition_count` | catalog 派生 | 分区裁剪后的 task 成本修正。 |

首期 `gv_catalog_metadata_schema_design.md` 已明确 `snapshots.total_records`；文件数、字节数和 delete 文件数暂未作为必需字段。实现时应把这些字段封装在 catalog adapter 返回结构中，用 `has_xxx` 标志区分“真实为 0”和“字段不可用”。

## 4. 优化器数据结构

规划三回调之间使用 `RelOptInfo.fdw_private` 暂存 `IcebergFdwPlanState`。该对象只在规划期有效，不进入 `ForeignScan.fdw_private`。

```c
typedef struct IcebergFdwCatalogStats {
    bool has_total_records;
    double total_records;

    bool has_total_data_files;
    double total_data_files;

    bool has_total_file_size;
    double total_file_size;

    bool has_total_delete_files;
    double total_delete_files;
} IcebergFdwCatalogStats;

typedef struct IcebergFdwPlanState {
    IcebergFdwOptions options;
    char *table_uuid;
    char *metadata_location;
    int current_schema_id;
    int64 current_snapshot_id;
    List *field_mappings;          /* Iceberg field id <-> openGauss attnum */
    IcebergFdwCatalogStats stats;
    double rows_before_filter;
    double rows_after_filter;
    Cost startup_cost;
    Cost total_cost;
} IcebergFdwPlanState;
```

`ForeignScan.fdw_private` 只保存执行期需要且可序列化的数据：

```c
enum IcebergFdwPrivateIndex {
    IcebergFdwPrivScanEntry,       /* metadata_location, snapshot_id, schema_id */
    IcebergFdwPrivRetrievedAttrs,  /* List<int> attno */
    IcebergFdwPrivPushedFilter,    /* serialized Iceberg predicate, nullable */
    IcebergFdwPrivPlanInfo         /* rows/cost/snapshot for EXPLAIN, optional */
};
```

## 5. 代价估算

### 5.1 基本原则

代价估算目标不是精确模拟 Iceberg reader，而是让 openGauss 在多个本地/外表路径之间有稳定、可解释的比较依据。

原则：

1. `rows` 以 catalog 的 `total_records` 为基数；缺失时使用固定默认值。
2. 不支持 `ANALYZE`，不读取本地统计直方图，不按本地采样修正选择率。
3. 谓词选择率只使用 catalog adapter 明确返回的统计能力；没有列级统计时，普通扫描路径不因谓词下推降低物理扫描 cost。
4. 下推谓词可以降低 `baserel->rows` 的估计输出行数，但普通扫描仍可能需要读取同一个 snapshot 的全部候选文件。若没有分区/文件裁剪统计，I/O cost 不随普通列谓词下降。
5. 规划期不访问对象存储。任何需要 manifest 或 parquet footer 的估算都留给 reader 或后续 catalog 缓存增强。

### 5.2 默认常量

实现应集中定义默认常量，便于测试和 EXPLAIN 解释：

| 常量 | 建议值 | 用途 |
| --- | --- | --- |
| `ICEBERG_FDW_DEFAULT_ROWS` | `1000` | catalog 无 `total_records` 时的默认表行数。 |
| `ICEBERG_FDW_DEFAULT_FILE_COUNT` | `1` | catalog 无文件数时的默认 task 数。 |
| `ICEBERG_FDW_DEFAULT_ROW_WIDTH` | 使用 `baserel->reltarget->width`，为空时 `32` | 估算字节成本。 |
| `ICEBERG_FDW_REMOTE_STARTUP_COST` | `10` | 打开 reader、解析 plan entry、建立 Arrow schema 的固定成本。 |
| `ICEBERG_FDW_FILE_STARTUP_COST` | `1` | 每个 data file/task 的附加成本。 |
| `ICEBERG_FDW_TUPLE_CPU_COST` | 复用 `cpu_tuple_cost` 或固定 `0.01` | 输出 tuple 转换成本。 |

默认值不是统计信息，不能写回 catalog，也不能让用户误以为执行过 `ANALYZE`。

### 5.3 行数估算

`GetForeignRelSize` 的行数计算：

```c
base_rows = stats.has_total_records ? stats.total_records : ICEBERG_FDW_DEFAULT_ROWS;
base_rows = clamp_row_est(Max(base_rows, 1.0));

remote_selectivity = IcebergCatalogEstimateSelectivity(remote_conds, stats);
if (!remote_selectivity.available)
    remote_selectivity.value = IcebergDefaultPredicateSelectivity(remote_conds);

rows_after_filter = clamp_row_est(base_rows * remote_selectivity.value);
baserel->rows = rows_after_filter;
```

选择率来源优先级：

| 优先级 | 来源 | 说明 |
| --- | --- | --- |
| 1 | catalog adapter 返回的分区/文件裁剪结果 | 只有当 catalog 已有可点查摘要时使用。 |
| 2 | catalog adapter 返回的列级 distinct/min/max/null fraction | 当前未要求，后续可扩展。 |
| 3 | 固定启发式 | 无统计时使用，例如等值 `0.1`、范围 `0.333`、其他 `0.5`。 |
| 4 | 无可下推谓词 | `1.0`。 |

固定启发式只影响估算 rows，不代表真实统计；EXPLAIN 中应标记为 `heuristic`。

### 5.4 Cost 公式

普通扫描路径 cost 分为固定打开成本、文件/task 成本、字节读取成本、tuple 转换成本：

```c
file_count = stats.has_total_data_files ? stats.total_data_files
                                        : ICEBERG_FDW_DEFAULT_FILE_COUNT;

if (stats.has_total_file_size)
    bytes_to_read = stats.total_file_size;
else
    bytes_to_read = base_rows * estimated_row_width;

startup_cost = ICEBERG_FDW_REMOTE_STARTUP_COST
             + file_count * ICEBERG_FDW_FILE_STARTUP_COST;

run_cost = bytes_to_read / ICEBERG_FDW_COST_BYTES_PER_UNIT
         + rows_after_filter * ICEBERG_FDW_TUPLE_CPU_COST;

if (stats.has_total_delete_files)
    run_cost += stats.total_delete_files * ICEBERG_FDW_DELETE_FILE_COST;

total_cost = startup_cost + run_cost;
```

约束：

- `bytes_to_read` 只使用 catalog 可得的 `total_file_size` 或 `base_rows * width` 估算。
- 没有分区/文件裁剪统计时，普通列谓词不降低 `bytes_to_read`。
- `rows_after_filter` 可降低 tuple 转换成本，因为 fewer rows 需要返回给 openGauss。
- delete 文件成本只在 catalog 有 `total_delete_files` 时启用。

### 5.5 `GetForeignRelSize`

职责：

1. 解析 FDW options。
2. 从 catalog 解析表、schema、snapshot 和行数摘要。
3. 校验 openGauss 外表列定义与 Iceberg schema 是否兼容。
4. 写 `baserel->rows`。
5. 把 `IcebergFdwPlanState` 放入 `baserel->fdw_private`。

伪代码：

```c
static void
icebergGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    IcebergFdwPlanState *fdw_state = palloc0(sizeof(IcebergFdwPlanState));

    icebergGetOptions(foreigntableid, &fdw_state->options);
    IcebergValidateRequiredOptions(&fdw_state->options);

    table = IcebergCatalogResolveTable(foreigntableid, &fdw_state->options);
    schema = IcebergCatalogLoadSchema(table.table_uuid, table.current_schema_id);
    stats = IcebergCatalogLoadSnapshotStats(table.table_uuid, EffectiveSnapshot(table, options));

    fdw_state->field_mappings = IcebergBuildFieldMappings(foreigntableid, schema);
    IcebergValidateForeignTableColumns(foreigntableid, fdw_state->field_mappings);

    fdw_state->rows_before_filter = IcebergBaseRows(stats);
    fdw_state->rows_after_filter = IcebergEstimateRows(root, baserel, stats);
    baserel->rows = fdw_state->rows_after_filter;

    baserel->fdw_private = fdw_state;
}
```

错误处理：

- option 缺失或非法：DDL validator 能提前发现的在 DDL 时报错；运行期仍要防御。
- catalog 找不到表：规划时报 `ERRCODE_FDW_TABLE_NOT_FOUND` 或项目统一错误码。
- 外表列和 Iceberg schema 不兼容：规划时报错，避免执行期转换失败。

### 5.6 `GetForeignPaths`

职责：

1. 使用 `IcebergFdwPlanState` 中的 rows/cost 创建普通 `ForeignPath`。
2. 首期只生成一个普通扫描路径。
3. `enable_index_scan` 为 true 时，如果索引 adapter 尚未接入，应记录不生成索引路径的原因，不 silently 生成错误路径。

伪代码：

```c
static void
icebergGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    IcebergFdwPlanState *fdw_state = (IcebergFdwPlanState *) baserel->fdw_private;

    IcebergEstimatePlainScanCost(root, baserel, fdw_state,
                                 &fdw_state->startup_cost,
                                 &fdw_state->total_cost);

    path = create_foreignscan_path(root,
                                   baserel,
                                   baserel->rows,
                                   fdw_state->startup_cost,
                                   fdw_state->total_cost,
                                   NIL,       /* pathkeys */
                                   NULL,      /* required_outer */
                                   NIL);      /* fdw_private for path */
    add_path(baserel, (Path *) path);
}
```

路径选择规则：

| 场景 | 处理 |
| --- | --- |
| 普通 SELECT | 生成普通扫描路径。 |
| 有 WHERE 谓词 | 仍生成普通扫描路径；可下推谓词影响 rows，是否影响 I/O cost 取决于 catalog 是否提供裁剪统计。 |
| 有 ORDER BY | 首期普通扫描路径不提供 pathkeys，排序由 openGauss 上层 Sort 处理。 |
| 有 LIMIT | 首期普通扫描路径不因 LIMIT 降低 cost，除非 reader 明确支持 limit pushdown 且 catalog adapter 能估算收益。 |
| `enable_index_scan=true` | adapter 未接入前不生成索引路径；接入后另行比较索引 path 与 plain path。 |

## 6. 计划生成

`GetForeignPlan` 把优化器选中的 `ForeignPath` 固化为 `ForeignScan` 节点。

### 6.1 谓词拆分

输入 `scan_clauses` 是 `RestrictInfo` 列表。FDW 应把它拆为：

| 分类 | 去向 | 示例 |
| --- | --- | --- |
| remote quals | 序列化到 `IcebergFdwPrivPushedFilter` | `col = const`、`col > const`、`col IS NULL`、安全的 `IN` |
| local quals | `make_foreignscan` 的 qual 参数 | 函数表达式、collation 不一致的字符串范围比较、类型收缩可能丢精度的谓词 |

判断依据应复用 `type-operator-conversion-design.md` 中的类型/操作符策略。任何不能证明 Iceberg 与 openGauss 等价的表达式都必须留在 local qual。

### 6.2 投影列计算

`retrievedAttrs` 必须包含：

1. `reltarget` 中需要返回的列。
2. local qual 中需要 recheck 的列。
3. 后续 row locator 或 DML 需要的隐藏列，首期普通扫描不启用。

列映射以 Iceberg field id 为权威。`retrievedAttrs` 在 plan 中保存 openGauss attno；执行期 `BeginForeignScan` 再通过 field mapping 转为 Iceberg field id 或列名传给 reader。

### 6.3 `fdw_private` 内容

计划节点中的 `fdw_private` 建议包含：

| 枚举位 | 内容 | 说明 |
| --- | --- | --- |
| `IcebergFdwPrivScanEntry` | `metadata_location`、`snapshot_id`、`schema_id`、`table_uuid` | 执行期打开 reader 的入口。 |
| `IcebergFdwPrivRetrievedAttrs` | `List<int>` | 投影和 local recheck 所需 attno。 |
| `IcebergFdwPrivPushedFilter` | `String` 或 `NULL` | 下推谓词的结构化序列化结果。 |
| `IcebergFdwPrivPlanInfo` | rows/cost/统计来源 | EXPLAIN 展示用，不参与执行正确性。 |

`fdw_private` 不能保存 catalog adapter 指针、reader 句柄、内存上下文或未序列化 C++ 对象。

### 6.4 伪代码

```c
static ForeignScan *
icebergGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
                      ForeignPath *best_path, List *tlist,
                      List *scan_clauses, Plan *outer_plan)
{
    IcebergFdwPlanState *fdw_state = (IcebergFdwPlanState *) baserel->fdw_private;

    scan_clauses = order_qual_clauses(root, scan_clauses);

    remote_conds = NIL;
    local_conds = NIL;
    IcebergClassifyConditions(root, baserel, scan_clauses,
                              fdw_state->field_mappings,
                              &remote_conds, &local_conds);

    local_exprs = extract_actual_clauses(local_conds, false);
    pushed_filter = IcebergSerializeRemoteConds(remote_conds);

    retrieved_attrs = IcebergBuildRetrievedAttrs(root, baserel, tlist, local_exprs);

    scan_entry = IcebergBuildScanEntry(fdw_state->metadata_location,
                                       EffectiveSnapshot(fdw_state),
                                       fdw_state->current_schema_id,
                                       fdw_state->table_uuid);

    fdw_private = IcebergBuildScanPrivate(scan_entry,
                                          retrieved_attrs,
                                          pushed_filter,
                                          IcebergBuildPlanInfo(fdw_state));

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

### 6.5 EXPLAIN 输出

`ExplainForeignScan` 应输出：

| 字段 | 示例 |
| --- | --- |
| `Iceberg Namespace` | `default` |
| `Iceberg Table` | `orders` |
| `Iceberg Snapshot` | `123456789` |
| `Iceberg Schema ID` | `3` |
| `Catalog Rows` | `1000000` |
| `Rows Estimate Source` | `catalog.total_records` 或 `default` |
| `Cost Estimate Source` | `catalog snapshot summary` 或 `default constants` |
| `Remote Filter` | 序列化谓词摘要 |
| `Local Recheck` | local qual 数量 |
| `Analyze Support` | `false` |

## 7. 外表 DDL 设计

### 7.1 建表示例

```sql
CREATE EXTENSION iceberg_fdw;

CREATE SERVER iceberg_srv
  FOREIGN DATA WRAPPER iceberg_fdw
  OPTIONS (
    catalog_type 'pg_native',
    catalog_uri 'iceberg_catalog',
    warehouse 's3://warehouse'
  );

CREATE USER MAPPING FOR CURRENT_USER
  SERVER iceberg_srv
  OPTIONS (
    username 'reader',
    password 'secret'
  );

CREATE FOREIGN TABLE public.orders_iceberg (
  order_id bigint NOT NULL,
  user_id integer,
  status varchar(32),
  comment text,
  embedding vector(768)
)
SERVER iceberg_srv
OPTIONS (
  namespace 'default',
  table_name 'orders',
  enable_index_scan 'false'
);
```

### 7.2 Server option 约束

| OPTION | 约束 |
| --- | --- |
| `catalog_type` | 非空字符串；必须是 adapter 支持的枚举值。首期建议支持 `pg_native`，其他值只在 adapter 明确支持后放开。 |
| `catalog_uri` | 可空；非空时长度不得超过项目约定上限，不能包含控制字符。 |
| `warehouse` | 非空；由 storage adapter 校验 URI scheme，例如 `s3://`、`obs://`、`file://` 或团队定义路径。 |

### 7.3 User mapping option 约束

| OPTION | 约束 |
| --- | --- |
| `username` | 可空；若 catalog/storage adapter 要求认证，则与 `password` 一起校验。 |
| `password` | 可空；不在 EXPLAIN 或错误明文中输出。 |

### 7.4 Foreign table option 约束

| OPTION | 约束 |
| --- | --- |
| `namespace` | 必填；不能为空；首期不解析多级 namespace 数组，复杂 namespace 由 catalog adapter 标准化。 |
| `table_name` | 必填；不能为空。 |
| `snapshot_id` | 可选；必须能解析为正 `int64`；指定后读取固定 snapshot，且 catalog 中必须存在该 snapshot。 |
| `enable_index_scan` | 可选；只接受 `true`/`false`，默认 `false`。adapter 未接入时设为 `true` 应报错或在 EXPLAIN 中明确未生成索引路径。 |

Validator 只能校验 option 名称和基础格式；涉及 catalog 是否存在、snapshot 是否存在、列映射是否兼容的校验发生在规划期或专门的 `IMPORT FOREIGN SCHEMA`/注册流程中。

## 8. 外表列定义约束

### 8.1 通用约束

外表列必须满足：

1. 列名与 Iceberg 顶层字段名匹配；后续如支持 field id option，可从“按名匹配”升级为“按 field id 匹配”。
2. 列数量可以少于 Iceberg schema，表示只暴露子集；不能定义 Iceberg schema 中不存在的普通列。
3. 列顺序可以不同，执行期以 field id/field mapping 读取；首期若没有 field id 绑定，则应要求列名唯一并按名匹配。
4. openGauss `NOT NULL` 不能弱于 Iceberg `required`：Iceberg required 字段在外表中必须声明 `NOT NULL`，避免本地语义误导。
5. openGauss 可以把 Iceberg optional 字段声明为 nullable；不建议把 optional 字段声明为 `NOT NULL`，除非 catalog adapter 能证明当前数据无 NULL。
6. 不支持 openGauss default、check、unique、primary key 在 Iceberg 侧自动生效；这些约束不作为 Iceberg 写入约束。
7. 不支持系统列、生成列、表达式列映射到 Iceberg 普通字段。

### 8.2 支持类型范围

首期 DDL 只允许已经在类型转换设计中定义过的类型族：

| openGauss 外表列类型 | Iceberg 类型 | 读路径 | 谓词下推 |
| --- | --- | --- | --- |
| `smallint` / `int2` | `int` | 支持，读回时做 int16 范围校验。 | 支持安全常量；范围外拒绝下推。 |
| `integer` / `int4` | `int` | 支持。 | 支持。 |
| `bigint` / `int8` | `long` | 支持。 | 支持。 |
| `smallint` / `integer` | `long` | 支持安全提升。 | 支持安全提升。 |
| `bigint` | `int` | 默认不支持，除非显式兼容模式并做范围校验。 | 默认拒绝。 |
| `varchar(n)` | `string` | 支持；读回时校验长度。 | 等值可下推；范围/排序受 collation 限制。 |
| `varchar` | `string` | 支持。 | 等值可下推；范围/排序受 collation 限制。 |
| `text` | `string` | 支持。 | 等值可下推；范围/排序受 collation 限制。 |
| `char(n)` | `string` | 支持；注意空格填充语义。 | 默认只下推等值且需要 recheck；范围拒绝。 |
| `vector(n)` | `list<float>` | 支持；维度必须等于 `n`，元素不能为 NULL。 | 标量谓词不下推；向量 top-k 留给索引路径。 |

当前不支持的类型：

- `boolean`
- `float4`、`float8`
- `numeric`、`decimal`
- `date`、`time`、`timestamp`
- `uuid`
- `binary`、`bytea`
- Iceberg `struct`、`map`、嵌套 `list`
- openGauss 数组类型

这些类型可以在后续类型设计补齐后放开。首期 validator 或规划期 schema 校验应拒绝不支持的列类型。

### 8.3 NULL 与 required 对齐

| Iceberg 字段 | openGauss 外表列 | 处理 |
| --- | --- | --- |
| `required` | `NOT NULL` | 合法。 |
| `required` | nullable | 拒绝，避免 openGauss 侧认为可产生 NULL。 |
| optional | nullable | 合法。 |
| optional | `NOT NULL` | 首期拒绝或警告后拒绝；否则执行期遇到 NULL 会破坏 openGauss 约束语义。 |

### 8.4 typmod 约束

| 类型 | 约束 |
| --- | --- |
| `varchar(n)` | `n > 0`；读取 Iceberg `string` 时超过 `n` 报错，不截断。 |
| `char(n)` | `n > 0`；读取时按 openGauss char 语义填充/校验；谓词下推保守。 |
| `vector(n)` | `n > 0`；Iceberg `list<float>` 长度必须等于 `n`。 |

## 9. openGauss 与 Iceberg 类型映射表

| openGauss OID/类型 | Iceberg primitive/logical | 允许方向 | 说明 |
| --- | --- | --- | --- |
| `INT2OID` / `smallint` | `int` | read, predicate | Iceberg int32 到 int16 需要范围校验。 |
| `INT4OID` / `integer` | `int` | read, predicate | 精确主路径。 |
| `INT8OID` / `bigint` | `long` | read, predicate | 精确主路径。 |
| `INT2OID` / `INT4OID` | `long` | read, predicate | 安全提升；读回按 openGauss 目标范围校验。 |
| `INT8OID` | `int` | reject | 防止 int64 常量或读回值收缩溢出。 |
| `VARCHAROID` / `varchar(n)` | `string` | read, predicate | typmod 只在 openGauss 侧生效。 |
| `TEXTOID` / `text` | `string` | read, predicate | 字符串主路径。 |
| `BPCHAROID` / `char(n)` | `string` | read, limited predicate | 空格填充语义导致下推保守。 |
| `vector(n)` | `list<float>` | read, vector index | 需要 catalog 或索引 metadata 提供 fixed dimension。 |

## 10. 实现步骤

建议按以下顺序实现：

1. 扩展 validator：补齐必填 option、布尔和 int64 格式校验。
2. 增加 catalog adapter：实现按 `foreigntableid` 读取 `tables_internal`、`table_schemas`、`snapshots.total_records`。
3. 增加 schema validator：规划期校验外表列和 Iceberg schema 类型、nullable、typmod。
4. 实现 `IcebergFdwPlanState`：在 `GetForeignRelSize` 暂存表指针、schema、stats、rows。
5. 实现 cost helper：只使用 catalog stats 和默认常量。
6. 实现 `GetForeignPaths`：生成普通 scan path，记录 startup/total cost。
7. 实现 `GetForeignPlan`：拆分 remote/local qual、生成 projection、构造 `fdw_private`。
8. 实现 EXPLAIN：输出统计来源、snapshot、schema、remote/local 条件。
9. 增加 mock catalog 单元测试：覆盖无统计、有 `total_records`、固定 snapshot、不支持类型、nullable 不一致等场景。

## 11. 待确认项

| 项 | 影响 |
| --- | --- |
| `catalog_type` 首期枚举值 | 决定 validator 是否只接受 `pg_native`。 |
| catalog 是否继续只保证 `snapshots.total_records` | 决定 cost 是否能使用文件数和字节数。 |
| optional Iceberg 字段是否允许外表声明 `NOT NULL` | 影响 DDL 兼容性与执行期约束错误策略。 |
| `vector(n)` 类型 OID 和 typmod 读取方式 | 需要对齐 GaussVector 当前 vector 类型实现。 |
| 多级 namespace 表达方式 | 当前 option 为单字符串，是否需要支持 `a.b.c` 或 JSON 数组需与 catalog adapter 对齐。 |
