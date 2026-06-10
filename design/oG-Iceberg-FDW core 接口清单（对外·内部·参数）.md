# openGauss Iceberg FDW core 接口清单（对外 · 内部 · 参数）

本文在结构图 `fdw_layer_abstract_overview.svg` 与数据流图 `fdw_layer_dataflow.svg` 的基础上，逐条列出 FDW core 的对外交互接口与内部调用关系，并给出每个接口的主要字段 / 参数。状态约定如下，与两图配色一致：

- <span style="color:#16a34a">**稳定**</span>：第一阶段确定实现。
- <span style="color:#9333ea">**预留**</span>：阶段二 / 三（分区向量索引、位置回表、delta 合并）相关接口或字段，首期仅留形态。
- <span style="color:#ea580c">**待定**</span>：需与相关方确认或在评审中决定。

未标注者默认 <span style="color:#16a34a">稳定</span>。文中签名为示意，以对齐 openGauss FdwRoutine 与下游 reader 边界为准，不锁定具体实现。

---

## 1. 对外接口（FDW core ↔ 外部组件）

FDW core 对外有六个边界：优化器 / 执行器、查询计划结构、系统目录（OPTIONS）、Catalog 元信息表、下游 Reader，以及阶段三的分区向量索引层（<span style="color:#9333ea">预留</span>）。对象存储不直接对接（经 Reader）。

### 1.1 与优化器 / 执行器：FdwRoutine 回调

FDW 经 `handler` 返回的 `FdwRoutine` 向内核注册回调，内核在规划期与执行期回调它们。这是 FDW 对外最主要的接口面。

| 回调 | 签名（主要参数） | 输出 | 状态 |
| --- | --- | --- | --- |
| `GetForeignRelSize` | `(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)` | 写 `baserel->rows` | 稳定 |
| `GetForeignPaths` | `(root, baserel, foreigntableid)` | `add_path(baserel, ForeignPath)` | 稳定 |
| `GetForeignPlan` | `(root, baserel, foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan)` | `ForeignScan *`（含 `fdw_private`） | 稳定 |
| `BeginForeignScan` | `(ForeignScanState *node, int eflags)` | 设 `node->fdw_state` | 稳定 |
| `IterateForeignScan` | `(ForeignScanState *node)` | `TupleTableSlot *` | 稳定 |
| `ReScanForeignScan` | `(ForeignScanState *node)` | 重置游标 | 稳定 |
| `EndForeignScan` | `(ForeignScanState *node)` | 释放资源 | 稳定 |
| `ExplainForeignScan` | `(ForeignScanState *node, ExplainState *es)` | 追加 EXPLAIN 项 | 稳定 |
| `VecIterateForeignScan` | `(VectorBatch *batch, ForeignScanState *node)` | `VectorBatch *` | <span style="color:#9333ea">预留</span> |
| 索引扫描路径登记 | 在 `GetForeignPaths` 内追加一条 ForeignPath / CustomScan | 多路径择优 | <span style="color:#9333ea">预留</span> |

注册入口：`iceberg_fdw_handler()` 返回 `FdwRoutine`；`outer_plan` 为 openGauss 特有参数。

### 1.2 与查询计划结构（解析 / 分析的产物）

FDW core **不直接对接 parser**；SQL 经内核 `parser + analyzer` 形成查询树后，优化器把外表的计划结构传入回调。FDW 只读取其中如下字段：

| 来源结构 | 读取字段 | 用途 | 状态 |
| --- | --- | --- | --- |
| `RelOptInfo *baserel` | `baserestrictinfo`（`List<RestrictInfo>`） | 谓词来源（拆 remote / local） | 稳定 |
| `RelOptInfo *baserel` | `reltarget->exprs` | 投影列来源（算 retrievedAttrs） | 稳定 |
| `RelOptInfo *baserel` | `fdw_private`（regt: 暂存 ident） | 跨规划回调复用表身份 | 稳定 |
| `GetForeignPlan` 入参 | `tlist`、`scan_clauses` | 目标列与最终扫描谓词 | 稳定 |
| `PlannerInfo *root` | 选择率计算上下文 | `clauselist_selectivity` 估行 | 稳定 |
| topk 形态 | `ORDER BY <-> :q LIMIT k` 对应的 PathKey / 表达式 | 解析为 indexSpec | <span style="color:#9333ea">预留</span> |

`baserel`（含 `RestrictInfo`）入三个规划回调；`tlist / scan_clauses` 仅入 `GetForeignPlan`。

### 1.3 与系统目录：OPTIONS 与 validator

外表身份与存储配置以 OPTIONS 持久化在系统目录，FDW 经访问函数读取。

| 接口 | 主要字段 / 参数 | 用途 | 状态 |
| --- | --- | --- | --- |
| `iceberg_fdw_validator(List *options, Oid catalog)` | 按层校验 OPTIONS 合法性 | DDL 期校验 | 稳定 |
| `GetForeignTable(foreigntableid)->options` | `namespace`、`table_name` | 表身份 | 稳定 |
| `GetForeignServer(serverid)->options` | `catalog_kind`、`catalog_schema` | Catalog 后端定位 | 稳定 |
| `GetUserMapping(...)->options` / server options | `endpoint`、`region`、`access_key` 等 | 组装 `storage_config` | 稳定 |

### 1.4 与 Catalog 元信息表（只读点查）

规划期经 `systable` / SPI 点查库内元信息表，仅取指针与摘要，不访问对象存储。

| 接口 | 入参 | 取用字段 | 使用回调 | 状态 |
| --- | --- | --- | --- | --- |
| `IcebergGetTableIdentity(foreigntableid)` | 外表 OID | → `(namespace, table_name, catalog_schema)` | `GetForeignRelSize` | 稳定 |
| `IcebergCatalogResolveTable(ident)` | `(namespace, table_name)` | `tables`: `metadata_location`、`current_snapshot_id`、`current_schema_id`、`table_uuid`、`table_location` | `GetForeignPlan` | 稳定 |
| `IcebergGetTableCardinality(ident)` | `table_uuid` + `current_snapshot_id` | `snapshots`: `total_records` | `GetForeignRelSize` | <span style="color:#ea580c">待定</span>（见下·表基数来源） |
| 文件清单缓存查询 | `(table_uuid, snapshot_id)` | `data_files(file_path, …)` | reader / 规划期 | <span style="color:#ea580c">待定</span>（见下·文件清单缓存） |

`schema_json`（`table_schemas`）与 `partition_specs` 由 reader 解析，FDW 不直接消费。

**<span style="color:#ea580c">待定</span> · 表基数来源**

- 原因：估行依赖当前 snapshot 的 `total_records`。该值是 snapshot summary 字段，当前 Catalog 是否将其拍平为 `snapshots` 普通列尚未确定；若未落列，规划期取基数须回退读 `metadata.json`，与"规划期不访问对象存储"冲突。落列与否属元信息表设计范畴，非 FDW 单方决定。
- 可选项：(a) `snapshots` 增列持久化 → 规划期纯点查、零对象存储 I/O，需上游改表；(b) 规划期回退读 `metadata.json` → 不改表但引入规划期 I/O；(c) 首期用固定 / 统计选择率，暂不依赖精确基数。
- 倾向：优先 (a) 并与元数据负责方确认；落列前以 (c) 兜底。

**<span style="color:#ea580c">待定</span> · 文件清单缓存 `data_files`**

- 原因：FDW 主要对象存储开销在 **manifest 展开**（snapshot → 数据文件清单）。"执行期产出文件规划"下，每次 `BeginForeignScan`、每次 `ReScan` 重复展开，单查询内随 ReScan 次数放大对象存储读。是否由 Catalog 按 snapshot 缓存 `data_files(table_uuid, snapshot_id, file_path, …)`、把展开降到"每 snapshot 首次访问"，属元信息表设计范畴，需与负责方协商。
- 安全性：snapshot 文件集不可变。提交新 snapshot 时指针前移，FDW 自然读新 snapshot（首次未命中、之后命中），旧行按容量裁剪，无写时失效逻辑。
- 可选项：(a) 引入缓存表 → 重复扫描与索引回表的对象存储代价大降，增一张表与维护成本；(b) 不缓存，靠 Reader 内部 / 进程级缓存吸收；(c) 不缓存，接受重复展开（首期最简）。
- 倾向：(a) 可让"执行期产出 + 缓存"与"规划期产出"两种时机代价趋同；首期先 (c)，作为阶段二 / 三高频回表的性能底座再引入 (a)。

### 1.5 与下游 Reader / SDK

reader 对 FDW 是不透明句柄，接口为打开 / 取批 / 关闭三段，外加位置读取（预留）。FDW 只传 open 所需元数据，文件规划、列投影 / 行过滤、delete 应用均在 reader 内。

| 接口 | 签名 | FDW 传入 | 返回 | 状态 |
| --- | --- | --- | --- | --- |
| 打开 | `iceberg_scan_open(MemoryContext cxt, const char *metadata_location, StorageConfig *storage_config, const char **columns, int n_columns, const char *filter, ArrowSchema *out_schema)` | 扫描入口、凭证、投影列名、下推谓词串（可空） | `IcebergScan *` + 出参 `out_schema`（建 converters 用） | 稳定 |
| 取批 | `iceberg_scan_next(IcebergScan *scan, ArrowArray *out_array, ArrowSchema *out_schema)` | — | `int nrows`（0=结束）+ 出参 Arrow 列批 | 稳定 |
| 关闭 | `iceberg_scan_close(IcebergScan *scan)` | — | — | 稳定 |
| 位置读取 | `iceberg_read_by_locator(IcebergScan *scan, const Locator *loc, const char **cols, int n_cols, ArrowArray *out)` | `(file_path, position)`、投影列 | 目标行 Arrow 批 | <span style="color:#9333ea">预留</span> |

`filter` 范围：AND 合取的 `col OP const` / `col IS [NOT] NULL` / `col IN (...)`，仅非嵌套列、可映射类型。

**<span style="color:#ea580c">待定</span> · `pushedFilter` 序列化格式**

- 原因：编码格式取决于 Reader 内置解析能力，而 Reader 未定（见下）；SDK 路线复用其 `ExpressionParser` 最省事，自研路线自定义更可控。两者联动，故格式待定。
- 可选项：(a) Iceberg `ExpressionParser` JSON → 与 SDK 对齐、表达力完整，绑定 SDK；(b) 最小 DSL `[{field, op, type, value}]` → 简单、Reader 无关，需双边各实现一份解析、表达力受限。
- 倾向：随 Reader 选型确定——SDK 路线用 (a)，自研路线用 (b)。

**<span style="color:#ea580c">待定</span> · Reader 选型**

- 原因：取数后端的语言 / 运行时直接决定性能（虚拟机冷启动与 GC）、集成方式（cdylib / JNI / 子进程）、MOR 与剪枝能力、编译门槛。各候选成熟度与门槛差异大，需专题对比与小规模验证后确定，属全组质疑点而非可单方拍板项。
- 可选项：Rust（iceberg-rust，原生 cdylib + C FFI，无虚拟机）、自研 C/C++（同进程、最可控、工作量大）、C++ 官方（iceberg-cpp，C++23 / GCC 14+ 门槛高）、C++ 社区（iceberg-cxx，支持 DV、缺分区层剪枝）、Java（iceberg-java + arrow，JNI + JVM、GC、依赖包大）、DuckDB（duckdb-iceberg，独立进程、运维较重）。
- 倾向：以 Rust 与自研 C 为两条主候选，C++23 路线列为高风险候选。

### 1.6 与分区向量索引层（阶段三，<span style="color:#9333ea">预留</span>）

分区向量索引层由其他成员实现（内部结构见 `fdw_layer_abstract_overview.svg` ④ 与 `索引查询.png`）。它是阶段三带索引扫描的对外组件：FDW 下发向量检索请求、接收候选行定位，再经 Reader 回表。FDW 不感知索引内部（段交集、seg blob 读取、READY / BUILDING 段处理、reconcile by `_row_id`）。

| 接口 | 签名 | FDW 传入 | 返回 | 状态 |
| --- | --- | --- | --- | --- |
| 索引检索 | `iceberg_index_search(IndexHandle *idx, const Vector *Q, Metric metric, int k_prime, const PartitionMask *mask, IndexSnapshot S, Candidate **out, int *n_out)` | indexSpec（下表） | 候选数组 `Candidate[]` | <span style="color:#9333ea">预留</span> |

**输入契约 `indexSpec`（FDW → 索引）**

| 字段 | 含义 | 来源 |
| --- | --- | --- |
| `S` | index snapshot / 索引版本 | snapshot.statistics 的 index-manifest-list |
| `Q` | 查询向量 | `ORDER BY emb <-> :q` |
| `metric` | 距离度量（L2 / cosine / ip） | 算子 / 列定义 |
| `k` | 最终 top-k | `LIMIT k` |
| `k'` | over-fetch，实际检索候选数（`k' ≥ k`） | 由 `k` 派生（系数 / 启发式） |
| `partition_mask` | 选中分区 | 分区谓词 + Catalog partition spec |

`k'` 多取候选用于抵消近似召回（ANN）、删除扣减（delete / DV）、delta 未入索引、回表精确重排四处损耗，`reconcile` 后裁回 `k`。

**输出契约 `Candidate`（索引 → FDW）**

| 字段 | 含义 |
| --- | --- |
| `locator` | `_row_id` 或 `(file_path, position)` |
| `dist` | 与 `Q` 的距离 |
| `source` / `segment` | 候选来源段（base / delta 标记），供 reconcile |
| `delete_mask` | 删除可见性标记（对应 DV / delete file） |

**调用形态**：规划期 `GetForeignPaths` 追加索引扫描路径（ForeignPath / CustomScan，§1.1），将 indexSpec 编入 `fdw_private`（§2.1）；执行期由该路径触发 `iceberg_index_search`，得候选后经 Reader `iceberg_read_by_locator`（§1.5）回表整行，再与 delta 扫描结果归并（§2.4）。

**边界**：indexSpec 的解析下发、candidates 的接收、回表与 delta 归并归 FDW；段交集、seg blob 读取、段状态与 reconcile 归索引模块。

### 1.7 与对象存储

FDW 不直接读对象存储；`metadata.json` / `manifest` / `*.parquet` / `delete file` 均由 reader 经 `storage_config` 访问。此处仅为边界说明，无 FDW 直接接口。

---

## 2. 内部接口与调用关系（FDW core 内部）

内部分四类：规划→执行的 `fdw_private` 契约、规划回调内部的暂存与 helper、执行态结构、回调与 reader 的调用关系。

### 2.1 规划 → 执行契约：`fdw_private` 编解码

规划回调与执行回调不共享内存，唯一通道是挂在 `ForeignScan` 节点上、可 `copyObject` / 序列化的 `fdw_private`（一个按固定枚举顺序装配的 `List`）。

| 枚举位 | 内容 | 生成（编码） | 消费（解码） | 状态 |
| --- | --- | --- | --- | --- |
| `IcebergPrivScanEntry` | `{ metadata_location, snapshot_id, schema_id }` | `GetForeignPlan` | `BeginForeignScan` | 稳定 |
| `IcebergPrivRetrievedAttrs` | 投影列 `List<attno>` | `GetForeignPlan` | `BeginForeignScan` | 稳定 |
| `IcebergPrivPushedFilter` | 下推谓词序列化串（可空） | `GetForeignPlan` | `BeginForeignScan` | 稳定 |
| `IcebergPrivIndexSpec` | `{ S, Q, metric, k, k', partition_mask }`（详见 §1.6） | `GetForeignPlan` | `BeginForeignScan` | <span style="color:#9333ea">预留</span> |
| `IcebergPrivLocator` | `_row_id` 或 `(file_path, position)` 形态 | 索引路径 | 位置回表 | <span style="color:#9333ea">预留</span> |

约束：`fdw_private` 不得携带 reader 句柄、已打开的文件、内存上下文等执行期资源；不可下推的 local 谓词以 `make_foreignscan` 的 `qual` 参数下传，不进 `fdw_private`。

### 2.2 规划回调内部：暂存与 helper 调用链

| 接口 / 暂存 | 输入 → 输出 | 调用方 | 状态 |
| --- | --- | --- | --- |
| `baserel->fdw_private`（ident 暂存） | `GetForeignRelSize` 写 → `GetForeignPlan` 读 | 跨规划回调复用，免重复解析 | 稳定 |
| `classifyConditions(scan_clauses)` | `List<RestrictInfo>` → `(remote_conds, local_conds)` | `GetForeignPlan` | 稳定 |
| `is_foreign_expr(expr)` | 表达式 → bool（能否远端等价求值） | `classifyConditions` | 稳定 |
| `SerializeFilter(remote_conds)` | 谓词 → `pushedFilter` 串 | `GetForeignPlan` | 稳定 |
| `BuildRetrievedAttrs(baserel)` | `reltarget + local_conds` → `List<attno>` | `GetForeignPlan` | 稳定 |
| `extract_actual_clauses(local_conds, false)` | → `local_exprs`（节点 qual） | `GetForeignPlan` | 稳定 |
| `make_foreignscan(tlist, local_exprs, scanrelid, fdw_exprs, fdw_private, …)` | 组装计划节点 | `GetForeignPlan` | 稳定 |
| `ParseTopK(root, baserel)` | PathKey / 表达式 → `indexSpec` | `GetForeignPlan`（索引路径） | <span style="color:#9333ea">预留</span> |

### 2.3 执行态结构 `IcebergScanState`（Begin / Iterate / End 共享）

`BeginForeignScan` 建立、`node->fdw_state` 持有，是执行期回调间的内部接口载体。

| 字段 | 类型 | 含义 | 生成 → 使用 | 状态 |
| --- | --- | --- | --- | --- |
| `rel` / `tupdesc` | `Relation` / `TupleDesc` | 外表与元组描述 | Begin → Iterate | 稳定 |
| `retrievedAttrs` | `List<int>` | 投影 attno | Begin 解码 → 转列名 | 稳定 |
| `reader` | `IcebergReaderState*` | reader 句柄 + converters | Begin → Iterate / End | 稳定 |
| `tuples / num_tuples / next_tuple / eof_reached` | 批缓冲 | 逐批读入、逐行返回的状态机 | Iterate / fetch_more_data / ReScan | 稳定 |
| `batch_cxt / temp_cxt` | `MemoryContext` | 每批回收 / 单行分配 | Begin 建 → End 释放 | 稳定 |
| `locator_cursor` | — | 位置回表游标 | 位置回表 | <span style="color:#9333ea">预留</span> |

### 2.4 回调 ↔ reader 调用关系

| 回调 | 调用 reader | 传入参数 | 说明 | 状态 |
| --- | --- | --- | --- | --- |
| `BeginForeignScan` | `iceberg_scan_open` | `metadata_location`、`storage_config`、`columns`、`filter` | 由 ScanEntry 解码 + OPTIONS 组装；reader 内部 PlanFiles | 稳定 |
| `IterateForeignScan` → `fetch_more_data` | `iceberg_scan_next` | scan 句柄 | 取 Arrow 批；FDW 经 `converters` 转 HeapTuple | 稳定 |
| `EndForeignScan` | `iceberg_scan_close` | scan 句柄 | 关闭并释放上下文 | 稳定 |
| `ReScanForeignScan` | `close` + `open` | 同 Begin 入参 | 无原地重绕，关旧开新 | 稳定 |
| 位置回表 | `iceberg_read_by_locator` | `locator`、投影列 | 索引候选 → 整行；与 delta 合并 | <span style="color:#9333ea">预留</span> |

辅助转换接口（FDW 侧）：

| 接口 | 输入 → 输出 | 调用方 | 状态 |
| --- | --- | --- | --- |
| `AttnosToColumnNames(tupdesc, retrievedAttrs)` | attno → Iceberg 列名 `columns` | Begin | 稳定 |
| `BuildStorageConfig(server_opts, usermap_opts)` | OPTIONS → `storage_config` | Begin | 稳定 |
| `BuildConverters(ArrowSchema)` | Arrow 列类型 → `converters` 函数表 | Begin | 稳定 |
| `ArrowBatchToHeapTuples(array, converters, tupdesc, batch_cxt)` | Arrow 批 → `HeapTuple[]` | fetch_more_data | 稳定 |
| `ArrowBatchToVectorBatch(array, …)` | Arrow 批 → `VectorBatch` | VecIterate | <span style="color:#9333ea">预留</span> |

### 2.5 批缓存状态机（热路径内部接口）

`IterateForeignScan` 每次返回一行，reader 每次返回整批，二者经 `IcebergScanState` 的批缓冲字段衔接：

```
Iterate:
  if next_tuple >= num_tuples: fetch_more_data()      // 批耗尽再取
  if num_tuples == 0: return ExecClearTuple(slot)      // reader 结束
  return ExecStoreTuple(tuples[next_tuple++], slot)    // local qual 由外壳 ExecQual 施加

fetch_more_data:
  MemoryContextReset(batch_cxt)                         // 回收上一批
  nrows = iceberg_scan_next(reader->scan, &array, &schema)
  tuples = ArrowBatchToHeapTuples(array, converters, tupdesc, batch_cxt)
  num_tuples = nrows; next_tuple = 0
```

`ReScanForeignScan` 复用同组字段：`close + open`，批缓冲归零，下次 `Iterate` 从首个 task 重读。

---

## 3. 待定与预留项汇总

| <span style="color:#ea580c">待定</span>项 | 位置 |
| --- | --- |
| 表基数来源 | §1.4 |
| 文件清单缓存 `data_files` | §1.4 |
| `pushedFilter` 序列化格式 | §1.5 |
| Reader 选型 | §1.5 |

| <span style="color:#9333ea">预留</span>项 | 位置 |
| --- | --- |
| 分区向量索引层接口 `iceberg_index_search` | §1.6 |
| indexSpec / locator 字段 | §1.6 / §2.1 |
| `iceberg_read_by_locator` | §1.5 / §2.4 |
| `VecIterateForeignScan` / `ArrowBatchToVectorBatch` | §1.1 / §2.4 |
| 索引扫描路径登记 | §1.1 / §2.2 |
| delta 合并 | §2.4 |
