# Iceberg FDW 索引扫描适配实现方案

## 1. 目标与边界

本文定义 openGauss Iceberg FDW 在现有 managed full scan 流程上接入 Iceberg vector index scan 的实现方案。

目标：

1. 对上层计划形态仍表现为 `ForeignScan`。
2. FDW 在规划阶段识别 Vector Search 下传的 top-k 向量查询信息。
3. FDW 调用 bridge index discovery 接口判断目标列是否存在可用索引。
4. 命中索引时只生成一条 index-backed `ForeignPath`；未命中时只生成一条 full-scan `ForeignPath`。
5. 执行阶段根据 `ForeignScan.fdw_private` 在 full scan adapter 和 index scan adapter 之间分流。
6. 删除 `enable_index_scan` 表选项，不再提供手工开关。

非目标：

- FDW 不实现索引构建、索引存储、ANN 算法、回表读取或低层索引维护逻辑。
- 首期不做 full scan path 与 index scan path 的 cost 竞争。
- 首期不做 delta 表与 index scan 的全局 top-k merge；存在未合并 delta 时应保守回退 full scan。
- 首期不把 `_distance` 作为用户可见列暴露。
- 首期不设计标量 prefilter 传入 index scan；标量 filter 继续按现有 full scan/operator adapter 规则处理。

## 2. 与现有 full scan 流程的关系

现有 full scan 流程：

```text
GetForeignPlan
    -> 构造 full-scan scan options JSON
    -> ForeignScan.fdw_private

BeginForeignScan
    -> catalog_adapter 查询 table_info
    -> sdk_scan_adapter 打开 bridge full scan
    -> delta_scan_adapter 可选打开 delta scan

IterateForeignScan
    -> sdk_scan_adapter 从 Arrow batch materialize TupleTableSlot
    -> base scan 结束后切换 delta scan
```

索引扫描适配后：

```text
Vector Search planner
    -> 向 FDW 下传必要 top-k 查询信息

GetForeignPaths
    -> 判断 top-k 信息是否完整
    -> 调 bridge index discovery 判断目标列是否有可用索引
    -> 只生成一条 ForeignPath:
       - 命中索引: ann_index
       - 未命中索引: full_scan

GetForeignPlan
    -> 序列化 scan method 和必要执行参数
    -> ForeignScan.fdw_private

BeginForeignScan
    -> ann_index: index_scan_adapter 打开 bridge index scan
    -> full_scan: 现有 sdk_scan_adapter 打开 full scan

IterateForeignScan
    -> 两种 scan adapter 都 materialize 到 TupleTableSlot
```

因此，本设计是在现有 `sdk_scan_adapter` 旁边新增 `index_scan_adapter`，并扩展 FDW plan private / scan state，而不是替换 full scan。

## 3. Bridge Index 接口约定

### 3.1 列名版接口

本项目要求 bridge index 接口改为使用列名，而不是 Iceberg `field_id`。

FDW 侧不再向 index discovery / scan 接口传：

```text
vector_field_id
projection field_id list
```

而是传：

```text
vector_column_name
projection column name list
```

这样 index scan 与现有 full scan 的投影语义保持一致：

- full scan：按顶层列名投影。
- index scan：按顶层列名定位向量列和投影列。
- bridge 内部负责根据 Iceberg schema 将列名解析成稳定 field id。

FDW 仍可保留 catalog field id 能力用于 DDL/schema 管理，但 index scan planning 不依赖 FDW 自行计算 field id。

### 3.2 Index Discovery

bridge 需要提供按列名判断索引是否存在的 discovery 接口，语义等价于当前 `match_index`：

```c
match_index(
    table,
    vector_column_name,
    metric,
    snapshot
) -> nullable index descriptor
```

约定：

- 命中：返回 index descriptor，其中包含 `index_name`、metric、dim、built snapshot 等。
- 未命中：返回 OK + NULL descriptor，未命中不是错误。
- catalog/table 解析失败、snapshot 冲突、index 状态异常：返回错误。

FDW 用该接口决定是否走 index scan。

### 3.3 Index Scan

bridge index scan 需要支持按列名输入：

```c
scan_begin(
    table,
    snapshot,
    index_name,
    vector_column_name,
    query_vector,
    query_vector_len,
    metric,
    k,
    fetch_k,
    projection_column_names
) -> Arrow batches
```

scan 输出：

- Arrow schema 为投影列，加可选内部 `_distance` 列。
- FDW materialize TupleTableSlot 时只填充用户表列。
- `_distance` 首期不暴露给 SQL 层。

## 4. Vector Search 下传信息

索引需要的信息按 Vector Search 层下传设计。FDW 不从 SQL 文本重新解析完整 top-k 查询，只消费 Vector Search 已识别出的最小必要信息。

### 4.1 必要信息

Vector Search 层向 FDW 下传：

```c
typedef struct IcebergFdwVectorTopKInfo {
    bool valid;
    AttrNumber vector_attnum;
    char *vector_column_name;
    float *query_vector;
    uint32 query_vector_len;
    IcebergFdwVectorMetric metric;
    uint32 k;
} IcebergFdwVectorTopKInfo;
```

字段说明：

- `valid`：是否为可交给 FDW 尝试 index scan 的 vector top-k 查询。
- `vector_attnum`：openGauss 外表中的向量列 attnum，便于与投影/类型信息对齐。
- `vector_column_name`：传给 bridge index discovery/scan 的列名。
- `query_vector`：距离表达式中的 q 值。
- `query_vector_len`：q 的维度。
- `metric`：距离类型，首期至少支持 L2。
- `k`：top-k 数量。

不下传的信息：

- 不下传 `enable_index_scan`。
- 不下传 field id。
- 不下传 metadata location；FDW 可自行从 catalog adapter 获取。
- 不下传完整 SQL 表达式；Vector Search 层已完成识别。
- 不下传冗余 cost hint；首期 FDW 只做“有索引则走索引”的单路径选择。

### 4.2 Metric 映射

距离算子用于 Vector Search 层确定 metric，并抽取 q。

示例：

```sql
ORDER BY c_vector <-> '[1,0,0]'::vector
LIMIT 3
```

下传给 FDW：

```text
vector_column_name = "c_vector"
query_vector       = [1.0, 0.0, 0.0]
metric             = L2
k                  = 3
```

FDW 不需要知道原始距离算子 OID，只需要拿到 metric 枚举。

## 5. 优化器生成计划部分

本节是优化器侧实现范围，可独立分配。

### 5.1 删除 enable_index_scan

从 FDW 中移除 `enable_index_scan`：

1. 从 `IcebergFdwOptions` 删除字段。
2. 从 `valid_options[]` 删除 table option。
3. 从 `icebergGetOptions()` 删除解析逻辑。
4. 从 SQL/regression/design 文档中删除使用说明。

删除后，是否尝试 index scan 完全由查询形态和 bridge discovery 结果决定。

### 5.2 Planner 私有结构

新增 planner 阶段结构：

```c
typedef enum IcebergFdwScanMethod {
    ICEBERG_FDW_SCAN_FULL,
    ICEBERG_FDW_SCAN_ANN_INDEX
} IcebergFdwScanMethod;

typedef enum IcebergFdwVectorMetric {
    ICEBERG_FDW_VECTOR_METRIC_L2
} IcebergFdwVectorMetric;

typedef struct IcebergFdwIndexPathInfo {
    bool valid;
    char *index_name;
    char *vector_column_name;
    float *query_vector;
    uint32 query_vector_len;
    IcebergFdwVectorMetric metric;
    uint32 k;
    uint32 fetch_k;
} IcebergFdwIndexPathInfo;
```

`fetch_k` 首期可等于 `k`。后续若 Vector Search 层需要 local exact refine，可约定传入更大的 fetch_k。

### 5.3 GetForeignRelSize

保持轻量：

- 可继续设置保守 `baserel->rows`。
- 不在这里打开 index handle。
- 不在这里调用 bridge discovery。

原因：index scan 选择依赖 Vector Search 下传 top-k 信息，通常在路径生成阶段更合适。

### 5.4 GetForeignPaths

`GetForeignPaths` 负责单路径选择。

流程：

```text
1. 构造默认 path info:
   scan_method = full_scan

2. 从 Vector Search 层读取 IcebergFdwVectorTopKInfo。

3. 如果 top-k info 不完整:
   生成 full_scan ForeignPath
   return

4. 查询 catalog_adapter:
   - namespace
   - table_name
   - current_snapshot_id
   - table_location 或 warehouse 所需信息

5. 检查 delta 状态:
   如果存在未合并 delta 表或 delta scan 需要参与:
       生成 full_scan ForeignPath
       return

6. 调 index_scan_adapter 的 planner discovery 包装:
   match_index(table, vector_column_name, metric, snapshot)

7. 如果 match_index 未命中:
   生成 full_scan ForeignPath
   return

8. 如果 match_index 命中:
   生成 ann_index ForeignPath
   path private 保存 IcebergFdwIndexPathInfo
```

首期只调用 discovery，不调用 `estimate_access`。因为策略是“有索引则走索引”，不做 full scan 与 index scan cost 竞争。

### 5.5 只生成一条 ForeignPath

必须遵守：

- 命中索引：只生成一条 `ann_index` path。
- 未命中索引：只生成一条 `full_scan` path。
- 不同时生成 full path 和 index path。

cost 设置建议：

- full scan：沿用当前成本。
- ann_index：可设置较低 startup/total cost，保证该单 path 进入后续计划。

因为只有一条 path，cost 首期主要用于满足 optimizer API，不承担路径竞争决策。

### 5.6 GetForeignPlan

`GetForeignPlan` 负责把选中的 scan method 和执行所需参数写入 `ForeignScan.fdw_private`。

建议继续使用可 `copyObject` 的 `String` 节点，主 scan options JSON 增加：

full scan：

```json
{
  "scan_method": "full",
  "projection": ["c_integer", "c_vector"],
  "snapshot_id": 123
}
```

index scan：

```json
{
  "scan_method": "ann_index",
  "index_name": "idx_c_vector",
  "vector_column_name": "c_vector",
  "metric": "l2",
  "query_vector": [1.0, 0.0, 0.0],
  "k": 3,
  "fetch_k": 3,
  "projection": ["c_integer", "c_vector"],
  "snapshot_id": 123
}
```

注意：

- `projection` 仍使用列名，与 full scan 一致。
- index scan 的 projection 必须包含上层需要输出的列。
- 如果 Vector Search 层仍需要本地 refine，projection 应包含 vector 列。
- `snapshot_id` 使用 catalog 当前 snapshot 或用户显式指定 snapshot。

### 5.7 Local Qual 与 Vector Search 的关系

`ForeignScan` 仍可保留现有 local quals / recheck 机制。

对于 index scan：

- vector top-k 本身由 index scan 执行。
- Vector Search 上层是否继续 refine 由 Vector Search 层决定。
- 普通标量 qual 首期不传入 index scan prefilter。
- 如果存在标量 qual，FDW 可继续保留为 local qual，保证正确性。

### 5.8 EXPLAIN

`ExplainForeignScan` 增加输出：

full scan：

```text
Iceberg FDW: bridge full scan
```

index scan：

```text
Iceberg FDW: bridge index scan
Index Name: idx_c_vector
Vector Column: c_vector
Metric: L2
TopK: 3
FetchK: 3
```

不要输出 query vector 全量内容，避免 EXPLAIN 过长。可以输出维度：

```text
Query Vector Dim: 768
```

## 6. 执行器计划执行部分

本节是执行器侧实现范围，可独立分配给另一个实现者。

### 6.1 扩展 Scan State

当前 `IcebergFdwScanState` 只保存 full scan 相关 `IcebergSdkScan *sdk_scan`。

新增：

```c
typedef struct IcebergFdwScanState {
    ...
    IcebergFdwScanMethod scan_method;
    IcebergIndexScan *index_scan;
    char *index_name;
    char *vector_column_name;
    uint32 topk;
    uint32 fetch_k;
    uint32 query_vector_len;
    ...
} IcebergFdwScanState;
```

`sdk_scan` 与 `index_scan` 同一时刻只应有一个非空。

### 6.2 新增 index_scan_adapter

新增文件建议：

```text
iceberg_fdw/src/index_scan_adapter.cpp
```

对外接口建议：

```c
typedef struct IcebergIndexScan IcebergIndexScan;

typedef struct IcebergIndexScanRequest {
    const char *catalog_uri;
    const char *warehouse;
    const char *storage_config_json;
    const char *namespace_name;
    const char *table_name;
    const char *snapshot_id;
    const char *index_name;
    const char *vector_column_name;
    const float *query_vector;
    uint32 query_vector_len;
    IcebergFdwVectorMetric metric;
    uint32 k;
    uint32 fetch_k;
    List *projected_columns;
} IcebergIndexScanRequest;

extern bool iceberg_index_match_available(
    MemoryContext cxt,
    const IcebergIndexMatchRequest *request,
    char **out_index_name);

extern IcebergIndexScan *iceberg_index_scan_open(
    MemoryContext cxt,
    const IcebergIndexScanRequest *request);

extern int iceberg_index_scan_next(
    IcebergIndexScan *scan,
    ArrowArray **out_array,
    ArrowSchema **out_schema);

extern bool iceberg_index_scan_materialize_row(
    IcebergIndexScan *scan,
    TupleTableSlot *slot);

extern void iceberg_index_scan_release_batch(IcebergIndexScan *scan);
extern void iceberg_index_scan_close(IcebergIndexScan *scan);
```

实现要求：

- `dlopen` 与 full scan 使用同一个 `libiceberg_rust_bridge.so`。
- 加载 index ABI 符号。
- 启动时校验 index ABI version。
- 所有 bridge 错误转为 openGauss `ereport(ERROR)`，包含 operation 名称和 bridge error message。
- 遵守 Arrow C Data Interface release 规则。
- `scan_next_batch` 输出的 `_distance` 不 materialize 到用户 slot。

### 6.3 BeginForeignScan

流程：

```text
1. 解析 ForeignScan.fdw_private scan options JSON。
2. 查询 table_info。
3. 构造 projection。
4. 构造 storage/catalog config。
5. switch scan_method:
   - full:
       调 iceberg_sdk_scan_open
   - ann_index:
       调 iceberg_index_scan_open
6. delta scan:
   - full scan 可沿用现有 delta_scan_adapter
   - index scan 首期不打开 delta scan
```

如果执行期发现 `ann_index` 计划但 delta scan 必须参与，应报错或回退 full scan。建议首期在 planner 阶段已经回退，因此执行期遇到该情况报错即可，避免 EXPLAIN 与执行行为不一致。

### 6.4 IterateForeignScan

执行逻辑：

```text
if scan_method == full:
    沿用现有 sdk_scan_adapter + delta_scan_adapter 逻辑

if scan_method == ann_index:
    while index_scan_next 有 batch:
        materialize 当前行到 TupleTableSlot
        return slot
    return ExecClearTuple(slot)
```

index scan 首期不切换 delta scan。

### 6.5 ReScanForeignScan

full scan：

- 沿用现有 full scan rescan/重开策略。

index scan：

- 如果 bridge 支持 `scan_rescan`，调用它并重置 batch/row cursor。
- 如果 adapter 初期未封装 `scan_rescan`，可关闭并重新 `scan_open`，但需要保留原始 request 参数。

### 6.6 EndForeignScan

清理顺序：

1. 若 `index_scan != NULL`，调用 `iceberg_index_scan_close`。
2. 若 `sdk_scan != NULL`，调用 `iceberg_sdk_scan_close`。
3. 若 `delta_scan != NULL`，调用 `iceberg_delta_scan_end`。
4. 删除 scan memory context。

所有 close/free 函数必须 NULL safe。

## 7. Plan Private JSON 建议格式

首期可保持当前 `fdw_private = list_make3(...)` 结构，但建议把第一个 JSON 扩展成统一 scan options。

```json
{
  "scan_method": "ann_index",
  "snapshot_id": 123,
  "projection": [
    "c_integer",
    "c_vector"
  ],
  "index": {
    "index_name": "idx_c_vector",
    "vector_column_name": "c_vector",
    "metric": "l2",
    "query_vector": [1.0, 0.0, 0.0],
    "k": 3,
    "fetch_k": 3
  }
}
```

full scan：

```json
{
  "scan_method": "full",
  "snapshot_id": 123,
  "projection": [
    "c_integer",
    "c_vector"
  ]
}
```

执行器解析时：

- `scan_method` 缺失时按 `full` 处理，兼容旧计划。
- `scan_method = ann_index` 但 `index` 信息缺失时应报 FDW error。
- `query_vector` 解析失败、维度为 0、`k == 0` 时应报 FDW error。

## 8. Catalog 与 Snapshot

FDW 仍通过 `catalog_adapter` 读取 managed table binding：

- `namespace`
- `table_name`
- `metadata_location`
- `table_location`
- `current_snapshot_id`

用途分离：

- full scan 使用 `metadata_location` 打开 metadata file。
- index discovery/scan 使用 `namespace + table_name + snapshot` 定位表与索引。
- 如果用户指定 table option `snapshot_id`，index discovery/scan 应使用该 snapshot。
- 如果未指定，则使用 `tables_internal.current_snapshot_id`。

snapshot 规则：

- index descriptor 必须覆盖请求 snapshot。
- bridge 返回 snapshot conflict 时，规划阶段回退 full scan或报错；建议规划阶段 discovery conflict 回退 full scan，执行阶段 conflict 报错。

## 9. Delta 表策略

首期索引只覆盖 Iceberg base snapshot，不覆盖 openGauss delta 表中的未合并 IUD。

因此：

- planner 阶段若发现 delta 表存在且可能有可见数据，直接选择 full scan。
- index scan path 不打开 delta scan。
- 后续可扩展为：

```text
base index candidates
    + delta exact vector scan
    + local top-k merge/refine
```

该扩展不在本文首期范围内。

## 10. 错误处理与回退策略

规划阶段可回退 full scan：

- Vector Search 未下传 top-k 信息。
- q/k/metric 不完整。
- 目标列不是 vector 列。
- bridge discovery 返回 OK + no index。
- metric 不支持。
- delta 表需要参与。
- index discovery 依赖的 bridge 库不可用时，可选择回退 full scan并在 EXPLAIN 中不显示 index。

执行阶段应报错：

- 计划是 `ann_index`，但 `scan_begin` 返回 `NOT_FOUND`。
- 计划是 `ann_index`，但 snapshot conflict。
- index ABI version 不匹配。
- query vector 维度与索引维度不匹配。
- bridge scan 返回 Arrow schema 与 openGauss tuple descriptor 不兼容。

原则：

- 规划阶段可以保守选择 full scan。
- 执行阶段不要静默把已选择的 index plan 改成 full scan，避免执行行为与 EXPLAIN 不一致。

## 11. 测试计划

### 11.1 优化器测试

1. 无 vector top-k：生成 full scan。
2. vector top-k 但无索引：生成 full scan。
3. vector top-k 且有索引：生成 index scan。
4. metric 不支持：生成 full scan或报预期错误。
5. delta 存在：生成 full scan。
6. `enable_index_scan` option 被删除，使用该 option 创建表应报 invalid option。

EXPLAIN 断言：

```text
Iceberg FDW: bridge full scan
```

或：

```text
Iceberg FDW: bridge index scan
Index Name: ...
Vector Column: ...
Metric: L2
TopK: ...
```

### 11.2 执行器测试

1. index scan 返回 top-k 行。
2. index scan projection 只 materialize 用户列，不暴露 `_distance`。
3. projection 包含 vector 列时 Vector Search 上层可继续 refine。
4. 空结果返回 0 行。
5. `scan_rescan` 后结果一致。
6. `scan_begin` 返回 `NOT_FOUND` / `CONFLICT` 时错误信息清晰。

## 12. 推荐任务拆分

优化器部分：

1. 删除 `enable_index_scan` option。
2. 定义 Vector Search -> FDW top-k info 下传接口。
3. 在 `GetForeignPaths` 中实现单路径选择。
4. 调 index discovery 判断索引是否存在。
5. 在 `GetForeignPlan` 中序列化 `scan_method` 和 index request。
6. 扩展 `ExplainForeignScan`。
7. 补 EXPLAIN/regression 测试。

执行器部分：

1. 新增 `index_scan_adapter.cpp`。
2. 扩展 `iceberg_bridge_abi.h` 或新增 index ABI header。
3. 实现 index ABI 动态加载和 ABI version 校验。
4. 实现 index scan open/next/materialize/rescan/close。
5. 扩展 `IcebergFdwScanState`。
6. 在 `BeginForeignScan` / `IterateForeignScan` / `ReScanForeignScan` / `EndForeignScan` 中按 scan method 分流。
7. 补 index scan 执行测试。

