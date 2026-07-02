# Iceberg GC 与索引 Puffin 清理实现设计

## 1. 目标与边界

本文档定义 openGauss Iceberg 集成中的表维护能力，包括常规 Iceberg
垃圾回收和自定义索引 Puffin 文件清理。维护入口放在
`iceberg_catalog` 扩展中，FDW 扫描/执行路径不参与 GC。维护能力仅通过
`iceberg_catalog` SQL 函数暴露；本设计不引入常驻外部 maintenance 服务。
定时调度可由用户通过 cron、脚本或数据库任务调用 SQL 函数完成。

当前实现统一使用 `metadata_location` 管理表状态：

```text
openGauss iceberg_catalog
  -> tables_internal.metadata_location
  -> bridge 通过 metadata_location 加载 Iceberg table
  -> bridge 写出新的 metadata.json / 删除确认安全的物理文件
  -> iceberg_catalog 原子更新 metadata_location
```

不引入 bridge 直接连接 REST Catalog 并自行 `load_table()` /
`commit()` 的模式，避免 openGauss Catalog 和 bridge 同时成为
metadata pointer 的事实来源。

## 2. 实现阶段划分

当前开发按能力风险拆成两个阶段。

第一阶段：目录扫描型维护接口，不修改 Iceberg table 的当前 `metadata_location` 指针。

- `remove_orphan_files`：扫描 `<table_location>/data/`，清理不被当前有效 metadata 引用、且超过安全窗口的 data/delete 文件。
- `cleanup_old_metadata`：扫描 `<table_location>/metadata/`，只清理确认安全的旧 table metadata json 文件。
- `vacuum_index`：扫描索引文件集合，清理不被 retained snapshot/index registry 引用、且超过安全窗口的 index registry / artifact Puffin。

第一阶段可以先完成 SDK 与 bridge C ABI，再接入 catalog SQL。实现时仍必须保持 dry-run 默认开启、
安全窗口检查、删除前 `stat` 复查、fail closed 和统一 JSON 返回格式。

第二阶段：metadata rewrite 型维护接口。

- `expire_snapshots`：会生成新的 Iceberg metadata.json，返回 `new_metadata_location`，并要求 catalog 侧
  使用 CAS 发布当前 metadata pointer。
- 该阶段需要先明确 metadata 写入、CAS 失败中间态、物理文件删除顺序、catalog 事务回滚和并发写入语义。

组合入口：`iceberg_catalog.gc_table(...)` 只在独立接口稳定后作为 catalog 层同步编排 wrapper 实现，
不要求 bridge 或 `iceberg-index` 新增组合 C ABI / SDK。
## 3. 当前代码现状

检查基线：

- `iceberg-rust-bridge`: `010add624c89e89558903b8c5cd55da730eb9007`
- `iceberg-index`: `17a5b95bc31238947bc4b548e01cd076c0a5d71b`
- `Catalog`: `ebca1a8f2e5e0098aa3023cf0b5d464e9f4856b0`

当前 bridge 使用 `iceberg = 0.9.1`。该 Rust SDK 已有底层能力：

- `TableMetadataBuilder::remove_snapshots(...)`
- `TableMetadataBuilder::remove_statistics(...)`
- `Transaction::update_statistics().remove_statistics(...)`
- `FileIO::delete(...)` / `FileIO::delete_prefix(...)`
- manifest / snapshot / statistics metadata 读取能力

但当前没有直接可调用的高层 maintenance action：

- 没有完整 `expire_snapshots` action
- 没有完整 `remove_orphan_files` action
- bridge `main` 也没有对应 C ABI

bridge 远端分支 `origin/feat/orphan-file-cleanup` 已有
`iceberg_bridge_table_remove_orphan_data_files(...)` 原型实现，可作为
`remove_orphan_files` 当前实现基础。该分支只覆盖 `<table_location>/data/`
下 orphan data/delete 文件清理，不覆盖 snapshot expire 和索引 Puffin GC。

索引侧当前已有 by-metadata ABI：

- `iceberg_index_rs_build_index_by_metadata`
- `iceberg_index_rs_drop_index_by_metadata`
- `iceberg_index_rs_match_index_by_metadata`
- `iceberg_index_rs_search_vector_by_metadata`
- `iceberg_index_rs_search_scalar_by_metadata`

索引 registry 存储在 Iceberg `statistics-files` 指向的 Puffin 文件中，
segment artifact 存储在 `{table_location}/indices/` 下。当前
`drop_index_by_metadata` 只把 registry entry 标记为 `Dropped` 并写出新的
registry，不删除物理 artifact。`ArtifactStore.delete()` 存在，但没有被管理级
GC 流程调用。

## 4. 对外 SQL 接口

维护入口由 `iceberg_catalog` 提供。接口统一返回 `jsonb`，默认返回统计信息，
只有 `verbose => true` 时返回候选/删除文件明细。

### 4.1 expire_snapshots

```sql
SELECT iceberg_catalog.expire_snapshots(
    p_namespace text,
    p_table text,
    older_than interval DEFAULT interval '7 days',
    retain_last integer DEFAULT 1,
    snapshot_ids bigint[] DEFAULT NULL,
    dry_run boolean DEFAULT true,
    delete_files boolean DEFAULT true,
    include_index boolean DEFAULT false,
    verbose boolean DEFAULT false
);
```

语义：

- 过期符合策略的历史 snapshots。
- 必须保护当前 snapshot。
- 必须保护仍被 branch/tag/reference 引用的 snapshot。
- `retain_last` 按 snapshot 时间线至少保留最近 N 个 snapshot。
- `snapshot_ids` 为显式过期列表；即便显式指定，也不能删除当前 snapshot 或受保护引用。
- `dry_run = true` 时只计算将要更新的 metadata 和将要删除的文件，不写 metadata，不删物理文件。
- `dry_run = false` 时由 Catalog 编排三段式流程：bridge prepare 新 metadata、Catalog CAS 发布、
  CAS 成功后按 `delete_files` 决定是否调用 bridge delete-after-CAS 接口。
- `delete_files = true` 时，Catalog 只在 metadata CAS 成功后继续调用 bridge delete-after-CAS；
  `delete_files = false` 时只发布新 metadata，不触发物理文件删除。
- dry-run / prepare 阶段返回的文件候选只是预估报告；真实物理删除阶段必须重新基于
  `base_metadata_location` 和 `new_metadata_location` 计算删除候选。
- `include_index = true` 时，不改变 `expire_snapshots` 的底层语义；Catalog 仅在
  `expire_snapshots` 成功后继续组合调用 `vacuum_index`。默认不自动清索引。

物理删除范围必须保守，仅删除 100% 确认可安全删除的文件：

- 只被过期 snapshots 引用、且不被任何保留 snapshot 引用的 data files。
- 只被过期 snapshots 引用、且不被任何保留 snapshot 引用的 delete files。
- 只被过期 snapshots 引用、且不被任何保留 snapshot 引用的 manifest files。
- 已从新 table metadata 中移除的 statistics Puffin 文件。

当前实现不删除无法完整证明引用关系的文件，统一计入 `skipped`。

索引清理边界：

- `expire_snapshots` 不直接删除 index registry Puffin 或 index segment artifact。
- index registry Puffin / segment artifact 由 `vacuum_index` 独立清理。
- `include_index` 只是 Catalog 层组合开关，不把索引清理逻辑合并进
  `prepare_expire_snapshots` 或 `delete_expired_snapshot_files`。
- 如需类似 LanceDB `optimize()` 的一键维护体验，应通过 `gc_table(...)` 或
  `optimize_table(...)` 组合入口实现，而不是扩宽 Iceberg 语义明确的
  `expire_snapshots(...)`。

返回示例：

```json
{
  "operation": "expire_snapshots",
  "dry_run": true,
  "table": "ns.t",
  "expired_snapshot_ids": [1001, 1002],
  "protected_snapshot_ids": [1003],
  "candidate_file_count": 12,
  "candidate_bytes": 1048576,
  "deleted_file_count": 0,
  "deleted_bytes": 0,
  "skipped_file_count": 0,
  "failed": []
}
```
### 4.2 remove_orphan_files

```sql
SELECT iceberg_catalog.remove_orphan_files(
    p_namespace text,
    p_table text,
    older_than interval DEFAULT interval '7 days',
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

语义：

- 清理 table location 下不被当前有效 Iceberg metadata 引用的物理文件。
- 这是目录扫描型维护动作，和 `expire_snapshots` 分开实现。
- 默认安全窗口为 7 天，避免误删并发写入但尚未提交的文件。
- 当前实现只清理 `<table_location>/data/` 下的 orphan data/delete 文件。
- `metadata/` 目录、`indices/` 目录不由该接口清理。
- `indices/` 由 `vacuum_index` 清理。
- `metadata/` 目录属于高风险区域，包含 `metadata.json`、manifest-list、
  manifest 等表版本和文件引用链。`remove_orphan_files` 不负责清理这些文件，
  防止误删仍被回滚、time travel、并发读或外部 catalog 指针引用的 metadata。
  旧 table metadata json 由独立的 `cleanup_old_metadata()` 处理；manifest-list、
  manifest 和 Puffin 文件必须由理解对应引用关系的维护动作清理。

为什么会存在 orphan 文件：

- 写文件成功但 metadata commit 失败。
- 任务取消或进程崩溃。
- 并发提交冲突，失败方已经写出文件。
- metadata 更新成功后物理删除失败。
- compact/rewrite/drop index/rebuild index 留下旧物理文件。
- 测试脚本或手工操作遗留。

可复用 bridge `origin/feat/orphan-file-cleanup` 的现有思路：

- 验证 `metadata/*.metadata.json` 的 `table-uuid`，确认 table location 未混用。
- 遍历当前 metadata 中所有 snapshots 的 manifest list / manifest。
- 计算所有被任意有效 snapshot 引用的 data/delete 文件集合。
- 只扫描 `<table_location>/data/`。
- 只把超过 grace period 的未引用文件作为 orphan。
- execute 删除每个候选 orphan 前不强制重新加载最新 metadata；默认依赖足够长的 grace
  period 保护并发写入/未提交文件，并在删除前重新 `stat` 候选文件，复查 mtime 是否仍在
  安全窗口外。若文件已消失视为已清理。每候选 reload metadata 代价高，且无法彻底消除
  reload 与 delete 之间的 TOCTOU 窗口；如后续需要更强并发保证，应优先由 catalog 层提供
  表级 maintenance lock 或把 reload 作为可选增强策略。

table location 安全约束：

- cleanup 前必须检查 `<table_location>/metadata/` 下所有 `*.metadata.json`
  的 `table-uuid`。只要发现非当前表 UUID，必须 fail closed 并拒绝清理。
- 当前实现不强制实现 `table_location` 唯一性和包含关系校验；后续应在
  `create_table` 中检查不同表的 table location 不完全相同、也不存在父子前缀
  包含关系。
- 在该优化完成前，用户必须保证各 Iceberg 表的 `table_location` 互不重叠。

### 4.3 cleanup_old_metadata

```sql
SELECT iceberg_catalog.cleanup_old_metadata(
    p_namespace text,
    p_table text,
    older_than interval DEFAULT interval '7 days',
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

语义：

- 独立清理 `<table_location>/metadata/` 下旧 metadata 文件。
- 这是高风险维护动作，必须独立于 `remove_orphan_files`，默认 dry-run。
- 该接口独立实现，不作为 `remove_orphan_files` 的一部分隐式执行。
- 必须保护当前 `metadata_location` 指向的 metadata.json。
- 必须保护 Iceberg metadata log 中仍可能用于 rollback/history 的 metadata 文件。
- 必须保护最近 `retain_last` 个 metadata json，默认值由 Catalog 传入 100。
- 必须保护 7 天安全窗口内的 metadata json。
- 校验实现优先使用 current metadata log / previous metadata log / retain_last 构建
  known/protected set；known 文件不需要逐个解析内容。
- 目录扫描发现但不在 known/protected set 中的 unknown metadata json，必须解析内容并校验
  `table-uuid` 后才可进入候选删除集合。
- 不清理 manifest-list、manifest、statistics Puffin、index registry Puffin 或 index segment artifact。
- 无法证明不再被当前 metadata、metadata log 或 catalog pointer 引用的 metadata json 必须跳过。

`expire_snapshots` 后续逻辑应补充对该接口的组合调用能力：snapshot 过期并成功发布新
metadata 后，可以在安全窗口外调用 `cleanup_old_metadata()` 清理旧 metadata 文件，
但不能由 `remove_orphan_files` 隐式处理。

其他文件的清理归属：

- manifest-list / manifest：由 `delete_expired_snapshot_files` 在 snapshot expire CAS 成功后，
  基于 `base_metadata_location` / `new_metadata_location` 重新计算引用差集后清理。
- data files / delete files：由 `delete_expired_snapshot_files` 清理过期 snapshot 独占引用；
  由 `remove_orphan_files` 清理不在 metadata 引用链中的 orphan data files。
- Iceberg 原生 statistics Puffin：由 `delete_expired_snapshot_files` 在确认不被新 metadata
  引用后清理。
- index registry Puffin / index segment artifact：只由 `vacuum_index` 清理。

### 4.4 vacuum_index

```sql
SELECT iceberg_catalog.vacuum_index(
    p_namespace text,
    p_table text,
    index_name text DEFAULT NULL,
    older_than interval DEFAULT interval '7 days',
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

语义参考 LanceDB：

- `drop index` 只修改 metadata/registry，不同步删除物理 index artifact。
- 物理 index 文件由 `vacuum_index` 分阶段清理。
- 必须支持历史 index 查询，因此不能只保护 current snapshot。
- 必须保护所有未过期 / 可 time-travel 的 snapshot 对应的 registry Puffin 和 segment artifact。
- 必须保护 branch/tag/reference 仍可到达的 snapshot 对应索引文件。
- `index_name IS NULL` 时清理该表所有索引的可清理文件。
- `index_name` 指定时只清理该逻辑索引相关的可清理文件。

实现采用 mark-and-sweep：

- `live_index_files` 是从保留 snapshots、branch/tag/reference 和 registry 中标记出的必须保留文件。
- `actual_index_files` 是对象存储中实际扫描到、且可能属于该表索引的文件。
- `candidate_index_files = actual_index_files - live_index_files`，再经过安全窗口和 Puffin/归属校验后才可删除。

当前可删除范围：

- 已超过安全窗口，且不被任何保留 snapshot 的 registry 引用的 segment artifact。
- 已超过安全窗口，且不被任何保留 snapshot 的 table statistics 引用的 index registry Puffin。
- `Dropped` index 在所有保留 snapshot 中不可达后留下的 artifact。
- build/commit 失败留下的 `{table_location}/indices/` 下 orphan Puffin。

不得删除：

- 当前 snapshot 的 registry Puffin。
- 当前 snapshot registry 引用的所有 segment artifact。
- 任意未过期 snapshot registry 引用的所有 segment artifact。
- branch/tag/reference 保护的 snapshot registry 和 artifact。
- 最近 7 天内创建或修改的 index 文件。
- 无法解析或无法确认归属本表的 Puffin 文件。

## 5. Catalog 扩展实现

`iceberg_catalog` 负责 SQL 入口、权限检查和 metadata pointer 管理，
不在 C 侧解析 Iceberg manifest 或 Puffin registry。

### 5.1 Catalog 需要读取的信息

SQL 入口使用 `p_namespace` / `p_table` 定位表，再通过
`iceberg_catalog.tables_internal` 获取：

- `relid`
- `namespace`
- `table_name`
- `table_uuid`
- `metadata_location`
- `table_location`
- `current_snapshot_id`

Catalog 还需要构造 bridge storage handle 所需的 S3/FileIO 配置，保持和建表/扫描路径一致。

### 5.2 metadata_location 提交流程

所有会写新 Iceberg metadata 的维护动作必须走以下流程：

```text
1. Catalog 读取 tables_internal 当前 metadata_location 作为 base。
2. Catalog 调 bridge maintenance prepare 接口。
3. bridge 只基于 base metadata_location 生成候选新 metadata.json，返回：
   - base_metadata_location
   - new_metadata_location
   - table_uuid
   - current_snapshot_id
   - summary json
4. Catalog 在同一事务中 CAS 更新 tables_internal.metadata_location：
   WHERE relid = $relid AND metadata_location = $base_metadata_location
5. CAS 成功后，Catalog 再触发物理文件删除，或调用 bridge execute-delete 阶段。
6. CAS 失败时，不更新 catalog，不删除物理文件；bridge 已写出的未发布 metadata
   依赖后续 cleanup_old_metadata 在超过安全窗口后清理。
```

责任边界：

- bridge / Rust SDK 负责读取 base metadata、计算 Iceberg metadata update、写出候选
  `new_metadata_location`，并返回 prepare 结果。
- openGauss Catalog 负责当前 metadata pointer 的 CAS 发布，是唯一能更新
  `tables_internal.metadata_location` 的组件。
- bridge 不读取或修改 `tables_internal`，也不能绕过 openGauss Catalog 直接成为当前
  metadata pointer。
- 第一阶段维护接口不产生新 metadata，`new_metadata_location` 固定为 `null`；第二阶段
  `expire_snapshots` 才需要 prepare 新 metadata 并交由 Catalog CAS 发布。

### 5.3 同步执行与返回

不新增维护任务表，不引入维护队列，也不引入后台调度器。每个 SQL 函数同步执行本次
维护动作，并将执行摘要作为 `jsonb` 返回。调用方负责保存结果、审计、告警和重试。

失败重试不依赖 Catalog 内部 job 状态：`dry_run` 可重复执行；`execute` 返回
`failed` / `skipped` 后，由调用方根据返回 JSON 决定是否再次调用。

## 6. Bridge C ABI 设计

bridge 需要新增 maintenance ABI。所有接口接收 `metadata_location`，不接收 REST catalog URI。

### 6.1 通用返回

所有 maintenance ABI 返回 `IcebergBridgeString*`，内容为 JSON。统一返回格式用于让各维护 SQL
函数在审计、日志记录、自动化调度、错误处理和回归测试中具备一致的字段语义。统一格式不意味着
丢弃文件路径明细：`verbose=false` 时默认只返回统计字段，`verbose=true` 时返回候选/删除/跳过
文件明细。

```json
{
  "operation": "remove_orphan_files",
  "dry_run": true,
  "scope": "data_files_only",
  "base_metadata_location": "...",
  "new_metadata_location": null,
  "table_uuid": "...",
  "current_snapshot_id": 1003,

  "candidate_file_count": 0,
  "candidate_bytes": 0,
  "deleted_file_count": 0,
  "deleted_bytes": 0,
  "skipped_file_count": 0,
  "skipped_bytes": 0,
  "failed_file_count": 0,

  "candidates": [],
  "removed": [],
  "skipped": [],
  "failed": []
}
```

字段约定：

- `operation` 标识子操作名称。
- `scope` 由具体接口声明，例如 `data_files_only`、`metadata_json_only`、`indices`。
- `base_metadata_location` 是本次维护基于的 metadata 指针；不产生新 metadata 的第一阶段接口中，
  `new_metadata_location` 固定为 `null`。
- `candidate_file_count` / `candidate_bytes`：dry-run 时表示如果 execute 将尝试删除的文件；
  execute 时表示本次进入删除候选集合的文件。
- `deleted_file_count` / `deleted_bytes`：execute 实际删除成功，或删除前/删除时发现 NotFound 并
  视为已清理的文件；dry-run 时固定为 0。
- `skipped_file_count` / `skipped_bytes`：发现但不能删除的文件，例如安全窗口内、mtime 缺失、
  类型不明、路径越界或无法按 `index_name` 归属。
- `failed_file_count`：尝试删除但删除失败的文件数量。
- `failed` 始终返回明细，不受 `verbose` 控制；失败项至少包含 `path` 和 `error`，便于诊断和重试。
- `candidates` / `removed` / `skipped` 只在 `verbose=true` 时返回明细；`verbose=false` 时为空数组或省略。
- 文件明细统一使用对象，不使用字符串数组；明细项至少包含 `path` 和 `size`，能确认时补充
  `uri`、`last_modified`、`reason`、`index_name` 等字段。
- `remove_orphan_files` 现有 `orphan_files` 字段应在实现中归一化为 `candidates`，不保留兼容字段，
  避免和其他维护接口返回格式分裂。

错误分层：

- 请求级错误返回 `IcebergBridgeStatus != OK`，并通过 `IcebergBridgeError` 暴露；Catalog SQL
  函数应把这类错误作为 SQL ERROR 抛出，不再返回成功 JSON。请求级错误包括参数非法、
  storage 初始化失败、`metadata_location` 无法加载、metadata 解析失败、table UUID 校验失败、
  table/location 越界或无法建立可信 live set。
- 文件级删除错误不应中断整个维护动作。execute 过程中某个候选文件删除失败时，接口返回成功
  JSON，并在 `failed_file_count` 和 `failed` 明细中记录；调用方可根据返回 JSON 决定告警或重试。
- 无法证明安全但不属于请求级错误的文件必须计入 `skipped`，不能删除。例如 mtime 缺失、
  文件类型不明确、Puffin 类型无法确认、指定 `index_name` 时无法确认归属。
- NotFound 在删除前或删除时出现时按幂等成功处理，计入 `deleted_file_count` / `deleted_bytes`
  或单独在明细中标注 `reason=not_found`，不能作为请求级错误。
- dry-run 不执行删除，因此不会产生文件级删除失败；但仍应返回 `skipped` 以暴露安全性不足的候选。
### 6.2 expire snapshots ABI

```c
typedef struct {
  const char* table_namespace_json;
  const char* table_name;
  const char* base_metadata_location;
  const char* snapshot_ids_json;      /* nullable JSON int64 array */
  int64_t older_than_ms;              /* <= 0 means unset */
  int32_t retain_last;                /* <= 0 means default 1 */
  bool dry_run;
  bool verbose;
} IcebergBridgePrepareExpireSnapshotsRequest;

IcebergBridgeStatus iceberg_bridge_table_prepare_expire_snapshots(
    IcebergBridgeStorage* storage,
    const IcebergBridgePrepareExpireSnapshotsRequest* request,
    IcebergBridgeString** out,
    IcebergBridgeError** err);

typedef struct {
  const char* table_namespace_json;
  const char* table_name;
  const char* base_metadata_location;
  const char* new_metadata_location;
  bool dry_run;
  bool verbose;
} IcebergBridgeDeleteExpiredSnapshotFilesRequest;

IcebergBridgeStatus iceberg_bridge_table_delete_expired_snapshot_files(
    IcebergBridgeStorage* storage,
    const IcebergBridgeDeleteExpiredSnapshotFilesRequest* request,
    IcebergBridgeString** out,
    IcebergBridgeError** err);
```

三段式执行：

```text
1. bridge prepare:
   - 加载 base_metadata_location 指定的 table。
   - 按策略计算 expired / retained snapshot 集合。
   - 生成 remove snapshot metadata update。
   - dry-run 不写新 metadata，不删文件，new_metadata_location 返回 null。
   - execute 写出候选新 metadata，返回 base_metadata_location、new_metadata_location、
     expired/retained 摘要和删除候选统计。
   - prepare 返回的删除候选仅用于展示、审计和预估，不作为后续物理删除授权。
2. Catalog CAS:
   - WHERE relid = $relid AND metadata_location = $base_metadata_location
   - 成功后 tables_internal.metadata_location 指向 new_metadata_location。
   - 失败则结束，不删除物理文件。
3. bridge delete-after-CAS:
   - 只允许在 Catalog CAS 成功后调用。
   - 输入 base_metadata_location 和 new_metadata_location。
   - 重新加载新旧 metadata，计算 old refs - new refs 得到删除候选。
   - 不接收、不信任 prepare 阶段返回的候选列表作为删除输入。
   - 删除失败写入 failed，不回滚 metadata。
```

约束：

- prepare 接口不发布当前 metadata pointer。
- prepare 接口不删除 data/delete/manifest/statistics/index 文件。
- delete-after-CAS 接口不得自行修改 metadata pointer。
- delete-after-CAS 接口必须重新加载 `base_metadata_location` 与 `new_metadata_location`，
  校验二者属于同一 table UUID，并重新计算 `old_refs - new_refs`。prepare 返回的候选列表
  只能作为 dry-run/explain/report 信息，不能作为删除授权或删除输入。
- CAS 失败时，本次新写出的 metadata 不是 current metadata，不能调用 delete-after-CAS；
  未发布 metadata 由后续 `cleanup_old_metadata` 在安全窗口后清理。

### 6.3 remove orphan files ABI

直接对齐 `origin/feat/orphan-file-cleanup` 已有 C ABI 命名：

```c
IcebergBridgeStatus iceberg_bridge_table_remove_orphan_data_files(
    IcebergBridgeStorage* storage,
    const char* metadata_location,
    const IcebergBridgeTableIdent* table_ident,
    bool dry_run,
    uint64_t grace_period_seconds,
    IcebergBridgeString** out,
    IcebergBridgeError** err);
```

当前实际清理范围仍是 `<table_location>/data/`。bridge 当前 ABI 不带
`verbose` 参数，Catalog 可以在 SQL 层根据 `verbose` 对 JSON 结果做裁剪或透传。
文档和返回 JSON 中必须声明：

```json
{
  "scope": "data_files_only",
  "excluded_locations": ["metadata", "indices"]
}
```

### 6.4 index vacuum ABI

索引 vacuum 建议在 `iceberg-index` SDK 中实现核心逻辑，bridge 仅做 C ABI 转发。

```c
typedef struct {
  const char* table_namespace_json;
  const char* table_name;
  const char* metadata_location;
  const char* index_name;             /* NULL means all indexes */
  uint64_t grace_period_seconds;      /* default 7 days from catalog */
  bool dry_run;
  bool verbose;
} IcebergIndexVacuumByMetadataRequest;

IcebergBridgeStatus iceberg_index_rs_vacuum_index_by_metadata(
    IcebergBridgeStorage* storage,
    const IcebergIndexVacuumByMetadataRequest* request,
    IcebergBridgeString** out,
    IcebergBridgeError** err);
```


### 6.5 cleanup old metadata ABI

`cleanup_old_metadata` 是 metadata 目录专用维护接口，不能复用
`remove_orphan_files`。bridge C ABI 只接收 `metadata_location` 和策略参数，
不接收 REST catalog URI，也不在 bridge 侧自行发现 current pointer。

```c
typedef struct {
  const char* table_namespace_json;
  const char* table_name;
  const char* metadata_location;
  int64_t older_than_ms;              /* <= 0 means default 7 days */
  int32_t retain_last;                /* <= 0 means default 100 */
  bool dry_run;
  bool verbose;
} IcebergBridgeCleanupOldMetadataRequest;

IcebergBridgeStatus iceberg_bridge_table_cleanup_old_metadata(
    IcebergBridgeStorage* storage,
    const IcebergBridgeCleanupOldMetadataRequest* request,
    IcebergBridgeString** out,
    IcebergBridgeError** err);
```

实现要求：

- 加载 `metadata_location` 指定的 table，并把该 metadata 文件视为 current。
- 只扫描 `<table_location>/metadata/` 下的 table metadata json 文件。
- 不通过该接口删除 data/delete files、index artifact、statistics Puffin、manifest-list
  或 manifest 文件；这些文件分别由 `remove_orphan_files`、`vacuum_index` 或
  `expire_snapshots` 后续组合流程处理。
- dry-run 只返回候选和 skipped 统计，不删除物理文件。
- execute 删除每个候选 metadata json 前必须重新 stat 并复查安全窗口。
- 删除失败写入 `failed`，不影响其他候选文件的 best-effort 删除。
- 返回 JSON 的 `operation` 必须为 `cleanup_old_metadata`，`scope` 必须声明为
  `metadata_json_only`。

## 7. Rust SDK / iceberg-index 实现

### 7.1 常规 Iceberg maintenance

在 bridge 中新增 managed table maintenance 模块：

```text
src/services/managed_table/maintenance.rs
src/services/managed_table/expire_snapshots.rs
src/services/managed_table/orphan.rs
```

`expire_snapshots` 需要在 SDK 层同步拆成 prepare 和 delete-after-CAS 两个入口，bridge
只做 C ABI 参数转换和 JSON 透传，不能调用一个内部“一次完成写 metadata 和删文件”的 SDK
接口来伪装三段式。

建议 SDK 请求结构：

```rust
pub struct PrepareExpireSnapshotsRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub base_metadata_location: String,
    pub snapshot_ids: Option<Vec<i64>>,
    pub older_than_ms: Option<i64>,
    pub retain_last: i32,
    pub dry_run: bool,
    pub verbose: bool,
    pub file_io_config_json: String,
}

pub struct DeleteExpiredSnapshotFilesRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub base_metadata_location: String,
    pub new_metadata_location: String,
    pub dry_run: bool,
    pub verbose: bool,
    pub file_io_config_json: String,
}
```

SDK 行为：

```text
prepare_expire_snapshots:
  load table from base_metadata_location
  collect snapshots / refs / current snapshot
  compute retained snapshot set
  compute expired snapshot set
  collect referenced files for retained snapshots
  collect referenced files for expired snapshots
  delete_candidates = expired_refs - retained_refs
  write candidate new metadata with RemoveSnapshots / RemoveStatistics
  return base_metadata_location and new_metadata_location for catalog CAS publish
  return delete candidate summary/details only as report data

delete_expired_snapshot_files:
  load old table from base_metadata_location
  load new table from new_metadata_location
  verify table_uuid matches
  recompute old_refs - new_refs
  do not accept prepare candidate list as input
  best-effort delete delete_candidates
  return summary JSON
```

`DeleteExpiredSnapshotFilesRequest` 禁止包含文件候选列表字段。真实删除候选必须由 SDK
在 delete-after-CAS 阶段重新从 old/new metadata 计算，避免 prepare 输出被篡改、过期或
与实际 CAS 发布的 `new_metadata_location` 不一致。

引用文件集合至少包括：

- data files
- delete files
- manifest files
- manifest list files
- statistics Puffin files

`prepare_expire_snapshots` 不删除旧 metadata json，也不删除旧 snapshot 引用的物理文件。
旧 metadata json 由独立 `cleanup_old_metadata()` 负责。snapshot 过期后不再被新 metadata
引用的数据文件、delete file、manifest-list、manifest 和 statistics Puffin，只能在 Catalog
CAS 成功后由 `delete_expired_snapshot_files` 处理。

### 7.2 remove orphan files

复用 `origin/feat/orphan-file-cleanup` 的算法，但需要补齐：

- 统一 JSON 返回字段。
- `verbose` 控制是否返回文件列表。
- 默认 grace period 由 catalog 传入 7 天。
- 明确 scope 为 `data_files_only`。
- 目录扫描失败时 fail closed。
- cleanup 前校验 `<table_location>/metadata/` 下所有 metadata json 的
  `table-uuid`，发现其他表 UUID 时拒绝清理。
- 删除每个候选文件前重新 stat 并检查 grace period；默认不做每候选 metadata reload。
  并发 commit 安全主要依赖 7 天安全窗口。若未来引入表级 maintenance lock 或低成本
  catalog current-pointer 校验，可把 reload 最新 metadata 作为增强策略，但不是默认要求。

### 7.3 index vacuum

在 `iceberg-index` 中新增：

```text
crates/iceberg-index-abi/src/maintenance.rs
crates/iceberg-index-iceberg/src/index_vacuum.rs
```

核心数据结构：

```rust
pub struct MetadataIndexVacuumRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub metadata_location: String,
    pub index_name: Option<String>,
    pub grace_period_seconds: u64,
    pub dry_run: bool,
    pub verbose: bool,
    pub file_io_config_json: String,
}
```

算法使用 mark-and-sweep 模型，核心是三个集合：

- `live_index_files`：必须保留的索引文件集合。它来自当前 table metadata 中仍可达的
  snapshots、branch/tag/reference 和对应 index registry，表示仍可能服务当前查询、time travel
  查询或历史索引查询的 registry Puffin / segment artifact。
- `actual_index_files`：对象存储中实际存在的索引文件集合。它来自受控目录扫描和
  `statistics-files.statistics_path` 指向的 registry Puffin，表示当前存储里所有可能属于该表索引的文件。
- `candidate_index_files`：候选删除集合，初始值为 `actual_index_files - live_index_files`。
  它只表示“未被 live set 标记”，还不能直接删除，必须继续经过安全窗口、路径、Puffin 类型、
  table/index 归属等检查。

完整流程：

```text
1. load table from metadata_location
2. collect retained snapshot ids:
   - current snapshot
   - all snapshots still present in current metadata
   - branch/tag/reference reachable snapshots
3. build live_index_files:
   for each retained snapshot:
   - find registry Puffin from statistics-files for that snapshot
   - read SnapshotIndexRegistry
   - mark registry Puffin as live
   - mark all segment artifact_files as live
4. build actual_index_files:
   - list {table_location}/indices recursively
   - current index code writes registry Puffin through unique_registry_path(indices_dir, snapshot_id),
     so {table_location}/indices should cover both registry Puffin and segment artifact Puffin
   - additionally include any statistics_path that contains an index registry blob even if a future
     implementation stores registry Puffin outside indices/
5. candidate_index_files = actual_index_files - live_index_files
6. filter candidate_index_files by:
   - older than grace period
   - optional index_name match when registry/artifact can be attributed
   - file extension / Puffin sanity checks
   - path must stay inside allowed index cleanup roots
7. dry-run returns summary and verbose candidates; no physical delete
8. execute re-stats candidates, re-checks grace period and sanity checks, then deletes candidates best-effort through ArtifactStore/FileIO
```

集合用途：

- `live_index_files` 用于保护文件，任何出现在该集合中的文件都不能删除。
- `actual_index_files` 用于发现存储中的 registry/artifact 文件，覆盖正常提交、drop/rebuild
  后残留和失败构建遗留。
- `candidate_index_files` 用于生成 dry-run 报告和 execute 删除输入；execute 前仍需逐文件复查，
  不能仅凭集合差集直接删除。

retained snapshot 语义必须显式实现，不能只依赖当前 metadata 中的 current registry：

- 当前 snapshot 必须保留。
- 当前 table metadata 中仍存在、尚未被 `expire_snapshots` 移除的历史 snapshots 必须保留，
  以支持 time travel。
- branch/tag/reference 仍可达的 snapshot 必须保留，即便它不在普通 current lineage 上。
- 每个 retained snapshot 对应的 registry Puffin 和 registry 中引用的 segment artifacts 都必须
  进入 live set。

registry Puffin 覆盖范围：

- 根据当前 `iceberg-index` 代码，registry Puffin 由 `unique_registry_path(indices_dir, snapshot_id)`
  写入 `{table_location}/indices/`，并通过 Iceberg `statistics-files.statistics_path` 引用。
  因此当前实现扫描 `indices/` 可以覆盖 registry Puffin 和 segment artifact Puffin。
- 设计上仍应以 `statistics_path` 为权威来源。若后续索引设计或实现把 registry Puffin 放到
  `metadata/`、`statistics/` 或其他目录，`vacuum_index` 必须把这些 `statistics_path` 指向的
  registry Puffin 纳入 live/candidate 计算，不能只固定扫描 `indices/`。

Puffin / file sanity check：

- 候选文件扩展名或文件类型必须符合索引 artifact 约定，当前至少应接受 `.puffin`。
- 对 Puffin 文件应能读取 footer，并确认 blob type 属于 index registry 或 index artifact 类型；
  无法解析或类型不明确的文件应计入 `skipped`，不能删除。
- 指定 `index_name` 时，只有能从 registry/artifact 元数据确认归属的候选才允许删除。
- 文件路径必须严格位于允许的 index cleanup roots 中，禁止越界删除。

`index_name` 过滤策略：

- 能从 registry 中确认属于该 index 的 artifact 才按 index_name 精确清理。
- 对完全 orphan、无法归属某个 index 的文件，只有 `index_name IS NULL` 时才清理。
- 指定 `index_name` 时，不删除无法归属的 orphan 文件，计入 `skipped`。


### 7.4 cleanup old metadata

在 `iceberg-index` 中新增 metadata cleanup SDK，并由 bridge C ABI 调用该 SDK，
避免 bridge 和 SDK 各自维护一套 metadata 文件判断逻辑。

建议新增文件：

```text
crates/iceberg-index-abi/src/maintenance.rs
crates/iceberg-index-iceberg/src/metadata_cleanup.rs
```

核心请求结构：

```rust
pub struct MetadataCleanupOldMetadataRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub metadata_location: String,
    pub older_than_ms: i64,
    pub retain_last: i32,
    pub dry_run: bool,
    pub verbose: bool,
    pub file_io_config_json: String,
}

impl IndexEngine {
    pub fn cleanup_old_metadata_by_metadata(
        &self,
        request: &MetadataCleanupOldMetadataRequest,
    ) -> Result<String>;
}
```

核心算法：

```text
1. load table from metadata_location
2. resolve table_location and current metadata json path
3. build known/protected metadata json set from current table metadata:
   - current metadata_location
   - metadata log / previous metadata log entries retained by current metadata
   - latest retain_last metadata json files by version or timestamp
   - files inside grace period
4. list {table_location}/metadata recursively
5. classify actual metadata json files:
   - files in known/protected set are retained without content parsing
   - unknown files are parsed as TableMetadata and must pass table-uuid validation
6. candidate = validated_unknown_metadata_json_files - protected_metadata_json_files
7. skip files with missing mtime, parse failure, uuid mismatch, path outside metadata/
8. dry-run returns candidates/skipped/failed without deleting
9. execute re-stat each candidate, re-check grace period, then delete best-effort
```

保护规则：

- 必须保护当前 `metadata_location` 指向的 metadata json。
- 必须保护 metadata log / previous metadata log 中仍保留的 metadata json。
- 必须保护最近 `retain_last` 个 metadata json，默认值由 catalog 传入 100。
- 必须保护安全窗口内的文件，默认窗口由 catalog 传入 7 天。
- 对 current metadata log / previous metadata log / retain_last 命中的 known metadata 文件，
  可以直接按 protected/retained 处理，不需要逐个解析内容。
- 对目录扫描发现但不在 known/protected set 中的 unknown metadata json，必须解析为
  Iceberg `TableMetadata` 并校验 `table-uuid` 等于当前表 UUID；解析失败或 UUID 不匹配时
  计入 `skipped`，不得删除。
- 不能删除 manifest-list、manifest、statistics Puffin 或 index registry Puffin；本接口只处理
  table metadata json。
- 无法证明安全的文件计入 `skipped`，不得删除。

`cleanup_old_metadata` 明确不负责的文件必须交给对应维护动作：

- manifest-list / manifest 由 `delete_expired_snapshot_files` 清理。
- Iceberg 原生 statistics Puffin 由 `delete_expired_snapshot_files` 清理。
- index registry Puffin / index segment artifact 由 `vacuum_index` 清理。
- data files / delete files 由 `delete_expired_snapshot_files` 或 `remove_orphan_files` 清理。

返回 JSON 字段需要和通用 maintenance 返回保持一致，并额外包含：

```json
{
  "operation": "cleanup_old_metadata",
  "scope": "metadata_json_only",
  "base_metadata_location": "...",
  "retained_metadata_count": 0,
  "candidate_file_count": 0,
  "deleted_file_count": 0,
  "skipped_file_count": 0,
  "failed": [],
  "candidates": [
    {
      "path": "...",
      "uri": "...",
      "size": 0,
      "last_modified": 0,
      "reason": "..."
    }
  ],
  "removed": [],
  "skipped": []
}
```

## 8. 安全与并发语义

### 8.1 dry-run 与 execute

- `dry_run=true`: 不写 metadata，不删物理文件。
- `dry_run=false`: 第一阶段接口执行目录扫描和物理删除；第二阶段 `expire_snapshots`
  先 prepare 新 metadata，Catalog CAS 成功后再调用 delete-after-CAS 接口执行物理删除。
- 所有危险接口默认 `dry_run=true`。

### 8.2 并发控制边界

当前设计不引入维护任务表、维护队列、后台调度器或表级维护锁。任务队列用于异步调度和
重试，加锁用于串行化同一表的并发维护动作，二者是不同方案；本设计当前都不采用。

并发安全依赖：

- Catalog CAS 更新 `tables_internal.metadata_location`，避免覆盖并发提交后的 metadata pointer。
- `expire_snapshots` delete-after-CAS 阶段重新加载 base/new metadata 并重新计算删除候选。
- `remove_orphan_files`、`cleanup_old_metadata`、`vacuum_index` 依赖 7 天安全窗口、删除前 re-stat、
  路径/UUID/引用关系校验和幂等 NotFound 处理。
- CAS 失败时不删除物理文件；并发导致候选不再安全时计入 `skipped` 或返回请求级错误。

后续如果需要维护任务编排、异步重试、全局节流或维护锁，应作为新的方案单独设计，不作为
当前接口实现的隐含前提。

### 8.3 删除顺序

对于 snapshot expire：

```text
bridge 先写候选 new_metadata_location
Catalog CAS 发布 new_metadata_location
CAS 成功后调用 delete_expired_snapshot_files
delete_expired_snapshot_files 重新加载 base/new metadata 并计算 old_refs - new_refs
只删除重新计算后确认不再引用的物理文件
CAS 失败则不删除物理文件
```

对于 index vacuum / orphan cleanup：

```text
基于当前 metadata_location 计算 live set
只删除超过 7 天安全窗口的 orphan/candidate 文件
删除前重新 stat 并重查安全窗口
默认不为每个候选重新加载 metadata；并发 commit 安全主要依赖安全窗口
删除失败不回滚 metadata
返回 failed 列表供重试
```

### 8.4 失败语义

- bridge prepare 新 metadata 失败：返回请求级错误，不更新 catalog，不删除物理文件。
- Catalog CAS 失败：不更新 catalog，不删除物理文件；未发布 metadata 后续由
  `cleanup_old_metadata` 经过安全窗口后清理。
- 物理删除部分失败：返回成功 JSON，记录 `failed`，`failed_file_count > 0`。
- 无法确认归属或引用关系：不删除，计入 `skipped`。
- 发现 table location 混用或 table uuid 不一致：fail closed，拒绝清理。

### 8.5 路径与 URI 安全校验

所有会删除物理文件的维护动作都必须在删除前做路径/URI 安全校验。无法完成校验时必须
fail closed，作为请求级错误或逐文件 `skipped`，不得删除。

通用规则：

- 所有输入和候选路径必须解析为规范 URI，不允许只做字符串拼接或简单前缀匹配。
- scheme、bucket/authority、normalized path 必须分别比较；`s3://bucket-a/x` 不能因为字符串前缀
  误判为 `s3://bucket-a-evil/x` 的父目录。
- table root 比较必须使用带结尾分隔符的 normalized prefix。例如 `s3://b/tbl/` 可以包含
  `s3://b/tbl/data/a.parquet`，不能包含 `s3://b/tbl2/data/a.parquet`。
- 删除目标路径必须位于该接口允许的 cleanup roots 内：
  - `remove_orphan_files`: `{table_location}/data/`
  - `cleanup_old_metadata`: `{table_location}/metadata/` 下 table metadata json
  - `delete_expired_snapshot_files`: 由 old/new metadata 引用差集确认的 data/delete/manifest/
    manifest-list/statistics Puffin 路径
  - `vacuum_index`: `{table_location}/indices/` 和经 `statistics-files.statistics_path`
    确认为 index registry 的 Puffin 及其 segment artifact
- URI decode/normalize 后如果出现 `..`、空 path segment、重复 slash 导致的歧义、大小写不一致
  或 backend 不支持的 path 表达，应计入 `skipped` 或请求级错误。
- 删除前必须重新 stat 候选文件并重查安全窗口；mtime 缺失或 stat 结果无法判断时计入 `skipped`。
- table UUID 校验失败、候选文件归属到其他 table、或同一 cleanup root 内发现混用 table UUID 时，
  必须 fail closed，拒绝本次清理。
- NotFound 在删除前或删除时出现按幂等成功处理，但返回明细中应标记 `reason=not_found`。

## 9. 实现工作清单

### 9.1 Catalog SQL 与 bridge orphan 对接

- 在 `iceberg_catalog` 增加 `remove_orphan_files(p_namespace, p_table, ...)`
  SQL 函数。
- 对接 bridge `iceberg_bridge_table_remove_orphan_data_files` ABI。
- 默认 7 天 grace period。
- 返回 jsonb。
- 只清理 `<table_location>/data/`。
- cleanup 前校验 table UUID；table_location 唯一性和包含关系校验作为
  `create_table` 后续优化。
- 不新增维护任务表，不引入维护队列或后台调度器；SQL 函数同步返回本次执行 JSON。

### 9.2 expire_snapshots

- 第二阶段实现；涉及写新 metadata 和 Catalog CAS，不属于第一阶段目录扫描类维护。
- bridge / SDK 必须拆成 `prepare_expire_snapshots` 和 `delete_expired_snapshot_files`
  两个入口，不能用一个内部“一次完成写 metadata 和删文件”的 SDK 接口伪装三段式。
- `prepare_expire_snapshots` 实现 snapshot 过期策略、dry-run 和候选新 metadata 写出；
  返回 `base_metadata_location`、`new_metadata_location`、expired/retained 摘要和删除候选报告。
- prepare 返回的删除候选只用于展示、审计和预估，不作为删除授权。
- Catalog 负责用 `base_metadata_location` CAS 发布 `new_metadata_location`。
- `delete_expired_snapshot_files` 只允许在 Catalog CAS 成功后调用；必须重新加载 base/new
  metadata，校验 table UUID，并重新计算 `old_refs - new_refs` 后 best-effort 删除。
- `expire_snapshots` 不直接删除 index registry Puffin 或 index segment artifact；索引清理只通过
  `vacuum_index` 或 Catalog 组合入口触发。
- 添加 snapshot 多版本数据测试。

### 9.3 cleanup_old_metadata

- Catalog 暴露 `cleanup_old_metadata(p_namespace, p_table, ...)`。
- bridge 实现 metadata 目录候选文件计算和 dry-run。
- 默认不由 `remove_orphan_files` 调用。
- `expire_snapshots` 成功发布新 metadata 后可按参数组合调用。
- 只处理 `<table_location>/metadata/` 下的 table metadata json。
- 必须保护当前 `metadata_location`、metadata log / previous metadata log、最近
  `retain_last` 个 metadata json 和 7 天安全窗口内的 metadata json。
- 优先通过 current metadata log / previous metadata log / retain_last 构建 known/protected set；
  unknown metadata json 需要解析内容并校验 table UUID 后才能进入 candidate。
- 不清理 manifest-list、manifest、statistics Puffin、index registry Puffin、index segment
  artifact、data files 或 delete files；这些文件分别交给 `delete_expired_snapshot_files`、
  `vacuum_index` 或 `remove_orphan_files`。
- table UUID 不匹配、路径越界或无法证明安全时 fail closed 或计入 `skipped`，不得删除。

### 9.4 index vacuum

- `iceberg-index` 实现 retained registry/artifact mark-and-sweep。
- bridge 暴露 `iceberg_index_rs_vacuum_index_by_metadata`。
- Catalog 暴露 `vacuum_index`。
- 对齐 LanceDB 行为：drop index 不物理删除，vacuum 才删除。
- 使用 `live_index_files`、`actual_index_files`、`candidate_index_files` 三个集合：
  先标记仍被 retained snapshots / branch / tag / reference / registry 引用的 live 文件，再扫描
  实际存在的 index 文件，最后用差集得到候选删除集合。
- `candidate_index_files` 还必须经过 7 天安全窗口、路径、Puffin 类型、table/index 归属和
  `index_name` 过滤校验，不能直接删除。
- `index_name` 指定时，只删除能确认归属该 index 的候选；无法归属的 orphan 文件计入
  `skipped`。
- actual 扫描范围当前至少包括 `{table_location}/indices/`，并以 Iceberg
  `statistics-files.statistics_path` 作为 registry Puffin 的权威来源。

### 9.5 Catalog 组合入口

`iceberg_catalog.gc_table(...)` 是 catalog 层的可选同步编排入口，用于简化运维调用；
它对应的是类似 LanceDB `optimize()` 的上层维护体验，而不是 Iceberg spec 的底层 action。
它不要求 bridge 或 `iceberg-index` 新增组合 C ABI / SDK 接口。bridge 和 SDK 仍只暴露独立维护动作，
catalog 组合入口按参数顺序调用这些独立 SQL/C ABI。

建议 SQL 形态：

```sql
SELECT iceberg_catalog.gc_table(
    p_namespace text,
    p_table text,
    expire_snapshots boolean DEFAULT true,
    remove_orphan_files boolean DEFAULT true,
    cleanup_old_metadata boolean DEFAULT false,
    vacuum_index boolean DEFAULT false,
    older_than interval DEFAULT interval '7 days',
    retain_last integer DEFAULT 1,
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

实现边界：

- 仅在 `iceberg_catalog` 层实现同步 orchestration，不新增维护任务表、维护队列或后台调度器。
- 子操作仍复用独立入口：`expire_snapshots`、`remove_orphan_files`、`cleanup_old_metadata`、
  `vacuum_index`。
- 索引清理只能通过 `vacuum_index` step 进入；`expire_snapshots` step 不直接删除 index
  registry Puffin 或 index segment artifact。
- 默认 `dry_run=true`，并把同一 dry-run 参数传递给所有启用的子操作。
- 执行顺序建议为：`expire_snapshots` -> `remove_orphan_files` -> `cleanup_old_metadata` ->
  `vacuum_index`。其中 `cleanup_old_metadata` 默认关闭，需要显式开启。
- `vacuum_index` 默认关闭，可通过 `vacuum_index => true` 显式启用。
- 返回 JSON 包含 `operation = "gc_table"` 和 `steps` 对象；每个 step 的值为对应独立维护接口
  返回的 JSON 摘要或错误信息。
- 组合入口不持久化 job 状态；调用方负责保存结果、审计、告警和重试。
- 任一子操作失败时，组合入口应停止后续依赖该状态的操作，并在返回 JSON 中标记失败 step；
  已成功完成的前置 step 不回滚。
- 实现清单约束：组合入口只在 Catalog 层编排独立维护动作；请求级错误可抛 SQL ERROR 并停止
  后续依赖 step，文件级删除失败保留在 step JSON 中，整体不回滚已完成的 CAS 或物理删除。

组合接口语义参考 LanceDB `optimize()` 的上层维护入口形态：一次调用可以组合多个维护动作，
并返回每个动作的统计结果。但由于本项目存在 openGauss Catalog、bridge 和 `iceberg-index`
多层边界，必须显式定义事务和错误语义：

- `dry_run=true`：按启用顺序调用各子操作的 dry-run，不写 metadata，不删物理文件，返回
  `steps` 汇总。
- `dry_run=false`：按启用顺序同步执行各子操作；前置 step 成功后，不因后续 step 失败回滚
  已完成的 metadata CAS 或物理删除。
- 请求级错误停止后续依赖 step。请求级错误包括参数非法、表不存在、storage 初始化失败、
  metadata 无法加载/解析、table UUID/location 安全校验失败、Catalog CAS 失败等。
- 文件级删除失败不视为请求级错误。子操作返回成功 JSON，并通过 `failed_file_count` 和
  `failed` 明细暴露；组合入口整体也返回成功 JSON，但保留对应 step 的失败统计。
- 如果某个 step 返回成功 JSON 但 `failed_file_count > 0`，后续与该失败文件无直接依赖的 step
  可以继续执行；有依赖关系的 step 应跳过并在 JSON 中标记 `skipped_due_to_failed_step`。
- SQL ERROR 只用于请求级错误；文件级失败、无法证明安全的 skipped 文件和 NotFound 幂等删除
  都应体现在返回 JSON 中。

组合入口返回格式固定为：

```json
{
  "operation": "gc_table",
  "dry_run": true,
  "table": "namespace.table",
  "table_uuid": "...",
  "base_metadata_location": "...",
  "final_metadata_location": "...",
  "status": "ok",
  "steps": {
    "expire_snapshots": {
      "enabled": true,
      "status": "ok",
      "result": {}
    },
    "remove_orphan_files": {
      "enabled": true,
      "status": "ok",
      "result": {}
    },
    "cleanup_old_metadata": {
      "enabled": false,
      "status": "skipped",
      "skip_reason": "disabled"
    },
    "vacuum_index": {
      "enabled": true,
      "status": "skipped_due_to_failed_step",
      "depends_on": "expire_snapshots",
      "skip_reason": "request_error"
    }
  }
}
```

字段约定：

- 顶层 `status` 取值：`ok`、`partial_failure`、`request_error`。
- step `status` 取值：`ok`、`partial_failure`、`request_error`、`skipped`、
  `skipped_due_to_failed_step`。
- step `result` 为对应独立维护接口返回的 JSON；如果 step 未执行，则不返回 `result`。
- step 发生请求级错误时，`status=request_error`，并返回 `error` 对象：
  `{"code": "...", "message": "...", "detail": "..."}`。
- step 只有文件级失败时，`status=partial_failure`，`result.failed_file_count > 0`，
  顶层 `status` 也应为 `partial_failure`。
- disabled step 使用 `status=skipped`、`skip_reason=disabled`。
- 因依赖 step 请求级错误而跳过时，使用 `status=skipped_due_to_failed_step`，并填充
  `depends_on` 和 `skip_reason`。
- 顶层 `final_metadata_location` 是本次组合入口结束时 Catalog 记录的 metadata pointer；
  如果未执行或未成功发布新 metadata，则等于 `base_metadata_location`。

## 10. 测试计划

### 10.1 remove_orphan_files

- 构造正常表和额外 orphan data file。
- `dry_run=true` 返回 orphan，但文件仍存在。
- `dry_run=false` 删除 orphan。
- 默认 7 天安全窗口生效：窗口内新文件不删除，超过窗口且确认安全的 orphan 才删除。
- 删除前 re-stat 生效：re-stat 后发现文件进入安全窗口、mtime 缺失或路径不安全时计入
  `skipped`。
- NotFound 幂等成功：候选文件删除前或删除时已不存在时不报请求级错误。
- 只处理 `{table_location}/data/`，不处理 `metadata/`、`indices/` 或其他目录。
- metadata table uuid 不一致时拒绝清理。
- table_location 完全相同或父子包含关系属于已知风险；后续在 `create_table`
  中增加唯一性和包含关系校验。

### 10.2 expire_snapshots

- 构造至少 3 个 snapshots。
- `retain_last=1` 保护当前 snapshot。
- `older_than` 只过期超过窗口的 snapshot。
- 被 branch/tag/reference 保护的 snapshot 不过期。
- dry-run 不改变 metadata_location。
- execute 走三段式：prepare 返回候选新 metadata，Catalog CAS 成功后调用
  delete-after-CAS。
- delete-after-CAS 重新加载 base/new metadata 并重新计算 `old_refs - new_refs`，不使用
  prepare 返回的候选作为删除授权。
- `delete_files=false` 时只发布新 metadata，不删除物理文件。
- `include_index=true` 时只在 Catalog 层组合调用 `vacuum_index`，`expire_snapshots` 本身不直接
  删除 index registry Puffin 或 segment artifact。
- 删除范围覆盖过期 snapshot 独占引用的 data/delete files、manifest、manifest-list 和
  Iceberg 原生 statistics Puffin。

### 10.3 cleanup_old_metadata

- `dry_run=true` 返回候选旧 metadata 文件但不删除。
- 当前 `metadata_location` 指向的 metadata.json 不删除。
- metadata log 仍引用的文件不删除。
- 最近 `retain_last` 个 metadata json 不删除。
- 默认 7 天安全窗口生效：窗口内 metadata json 不删除。
- known/protected metadata 文件通过 current metadata log / previous metadata log / retain_last
  识别，不要求逐个解析内容。
- unknown metadata json 需要解析为 Iceberg `TableMetadata` 并校验 table UUID；解析失败或 UUID
  不匹配时计入 `skipped`。
- 只处理 table metadata json，不删除 manifest-list、manifest、statistics Puffin、index registry
  Puffin、index segment artifact、data files 或 delete files。
- 无法证明安全的 metadata 文件计入 `skipped`。

### 10.4 vacuum_index

- 创建索引，drop index，确认 artifact 仍存在。
- `vacuum_index(dry_run=true)` 返回候选但不删除。
- `vacuum_index(dry_run=false)` 删除 dropped index artifact。
- 历史 snapshot 对应 index artifact 不删除。
- branch/tag/reference 可达 snapshot 对应 registry/artifact 不删除。
- live/actual/candidate 三集合差集可断言。
- 默认 7 天安全窗口生效：窗口内 index registry/artifact 不删除。
- Puffin sanity check 失败、路径越界、table UUID/registry 归属不明时计入 `skipped`。
- 指定 `index_name` 时，不删除无法归属该 index 的 orphan。
- `index_name IS NULL` 时可清理无法归属但超过窗口的 orphan index Puffin。
- NotFound 幂等成功。

### 10.5 gc_table 组合入口

- `dry_run=true` 汇总启用子操作的 dry-run 结果，不写 metadata，不删除物理文件。
- disabled step 返回 `status=skipped`、`skip_reason=disabled`。
- 请求级错误 step 返回 `status=request_error`，并停止后续依赖 step。
- 文件级删除失败 step 返回 `status=partial_failure`，顶层 `status=partial_failure`。
- 依赖失败导致跳过时返回 `status=skipped_due_to_failed_step`、`depends_on` 和 `skip_reason`。
- `base_metadata_location` / `final_metadata_location` 字段可断言。

### 10.6 存储后端覆盖

- 回归测试以本地 FileIO / 本地对象存储数据为主，保证可在开发环境一键运行。
- S3 需要有集成测试覆盖，验证 URI normalize、prefix 包含关系、stat/mtime、NotFound 幂等、
  dry-run 和 execute 删除流程能跑通。
- S3 集成测试需要通过环境变量显式开启，并要求配置 endpoint、bucket、access key、secret key、
  region/path-style 等必要参数；未配置时跳过，不影响本地回归。
- 本地回归和 S3 集成测试都必须覆盖默认 7 天安全窗口。

## 11. 开发注意事项

- 所有接口必须使用 `metadata_location`，不得绕过 openGauss Catalog 直接提交到 REST Catalog。
- FDW 不参与维护命令。
- 默认 `dry_run=true`。
- 默认安全窗口 7 天。
- SQL 维护入口统一使用 `p_namespace` / `p_table`，不要求调用方提供 `regclass`。
- 维护能力仅通过 `iceberg_catalog` SQL 函数暴露；不引入常驻外部服务。
- 不新增维护任务表，不引入维护队列或后台调度器。
- 文档、README 和测试必须明确当前物理删除范围。
- 删除逻辑必须 fail closed：不确定就跳过或报错，不做猜测删除。
- 索引清理必须支持历史 index 查询，不得只保护 current snapshot。
