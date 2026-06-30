# Iceberg GC 与索引 Puffin 清理实现设计

## 1. 目标与边界

本文档定义 openGauss Iceberg 集成中的表维护能力，包括常规 Iceberg
垃圾回收和自定义索引 Puffin 文件清理。维护入口放在
`iceberg_catalog` 扩展中，FDW 扫描/执行路径不参与 GC。

当前实现统一使用 `metadata_location` 管理表状态：

```text
openGauss iceberg_catalog
  -> tables_internal.metadata_location
  -> bridge 通过 metadata_location 加载 Iceberg table
  -> bridge 写出新的 metadata.json / 删除确认安全的物理文件
  -> iceberg_catalog 原子更新 metadata_location
```

第一阶段不引入 bridge 直接连接 REST Catalog 并自行 `load_table()` /
`commit()` 的模式，避免 openGauss Catalog 和 bridge 同时成为
metadata pointer 的事实来源。

## 2. 当前代码现状

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
`remove_orphan_files` 第一阶段基础。该分支只覆盖 `<table_location>/data/`
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

## 3. 对外 SQL 接口

维护入口由 `iceberg_catalog` 提供。接口统一返回 `jsonb`，默认返回统计信息，
只有 `verbose => true` 时返回候选/删除文件明细。

### 3.1 expire_snapshots

```sql
SELECT iceberg_catalog.expire_snapshots(
    rel regclass,
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
- `dry_run = false` 时执行 metadata 更新，并在 metadata 更新成功后 best-effort 删除确认安全的物理文件。
- `include_index = true` 时，在 snapshot 过期后组合调用 index vacuum；默认不自动清索引。

第一阶段物理删除范围必须保守，仅删除 100% 确认可安全删除的文件：

- 只被过期 snapshots 引用、且不被任何保留 snapshot 引用的 data files。
- 只被过期 snapshots 引用、且不被任何保留 snapshot 引用的 delete files。
- 只被过期 snapshots 引用、且不被任何保留 snapshot 引用的 manifest files。
- 已从新 table metadata 中移除的 statistics Puffin 文件。

第一阶段不删除无法完整证明引用关系的文件，统一计入 `skipped`。

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

### 3.2 remove_orphan_files

```sql
SELECT iceberg_catalog.remove_orphan_files(
    rel regclass,
    older_than interval DEFAULT interval '7 days',
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

语义：

- 清理 table location 下不被当前有效 Iceberg metadata 引用的物理文件。
- 这是目录扫描型维护动作，和 `expire_snapshots` 分开实现。
- 默认安全窗口为 7 天，避免误删并发写入但尚未提交的文件。
- 第一阶段只清理 `<table_location>/data/` 下的 orphan data/delete 文件。
- `metadata/` 目录、`indices/` 目录不由该接口清理。
- `indices/` 由 `vacuum_index` 清理。

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
- execute 删除前重新 stat，若文件已消失视为已清理。

### 3.3 vacuum_index

```sql
SELECT iceberg_catalog.vacuum_index(
    rel regclass,
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

第一阶段可删除范围：

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

## 4. Catalog 扩展实现

`iceberg_catalog` 负责 SQL 入口、权限检查、metadata pointer 管理和任务状态，
不在 C 侧解析 Iceberg manifest 或 Puffin registry。

### 4.1 Catalog 需要读取的信息

通过 `iceberg_catalog.tables_internal` 获取：

- `relid`
- `namespace`
- `table_name`
- `table_uuid`
- `metadata_location`
- `table_location`
- `current_snapshot_id`

Catalog 还需要构造 bridge storage handle 所需的 S3/FileIO 配置，保持和建表/扫描路径一致。

### 4.2 metadata_location 提交流程

所有会写新 Iceberg metadata 的维护动作必须走以下流程：

```text
1. Catalog 读取 tables_internal 当前 metadata_location 作为 base。
2. Catalog 调 bridge maintenance prepare/execute 接口。
3. bridge 基于 base metadata_location 生成新 metadata.json，返回：
   - base_metadata_location
   - new_metadata_location
   - table_uuid
   - current_snapshot_id
   - summary json
4. Catalog 在同一事务中 CAS 更新 tables_internal.metadata_location：
   WHERE relid = $relid AND metadata_location = $base_metadata_location
5. CAS 成功后，Catalog 再触发物理文件删除，或调用 bridge execute-delete 阶段。
6. CAS 失败时，不更新 catalog；bridge 已写出的未发布 metadata/临时文件依赖后续 orphan cleanup。
```

如果第一阶段 bridge 接口直接执行 metadata 写入和物理删除，Catalog 也必须要求 bridge
返回 `base_metadata_location` 和 `new_metadata_location`，并用 CAS 发布。不能让 bridge
绕过 openGauss Catalog 直接成为当前 metadata pointer。

### 4.3 任务状态表

建议新增维护任务表，方便重试和审计：

```sql
CREATE TABLE iceberg_catalog.maintenance_jobs (
    job_id uuid PRIMARY KEY,
    relid oid NOT NULL,
    operation text NOT NULL,
    base_metadata_location text,
    new_metadata_location text,
    dry_run boolean NOT NULL,
    status text NOT NULL,
    request jsonb NOT NULL,
    result jsonb,
    error text,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
```

第一阶段可以不引入后台调度器，但 SQL 函数应写入或至少具备扩展到该表的返回字段。

## 5. Bridge C ABI 设计

bridge 需要新增 maintenance ABI。所有接口接收 `metadata_location`，不接收 REST catalog URI。

### 5.1 通用返回

所有 maintenance ABI 返回 `IcebergBridgeString*`，内容为 JSON：

```json
{
  "operation": "...",
  "dry_run": true,
  "base_metadata_location": "...",
  "new_metadata_location": null,
  "table_uuid": "...",
  "current_snapshot_id": 1003,
  "candidate_file_count": 0,
  "candidate_bytes": 0,
  "deleted_file_count": 0,
  "deleted_bytes": 0,
  "skipped_file_count": 0,
  "failed": []
}
```

### 5.2 expire snapshots ABI

```c
typedef struct {
  const char* table_namespace_json;
  const char* table_name;
  const char* metadata_location;
  const char* snapshot_ids_json;      /* nullable JSON int64 array */
  int64_t older_than_ms;              /* <= 0 means unset */
  int32_t retain_last;                /* <= 0 means default 1 */
  bool dry_run;
  bool delete_files;
  bool verbose;
} IcebergBridgeExpireSnapshotsRequest;

IcebergBridgeStatus iceberg_bridge_table_expire_snapshots(
    IcebergBridgeStorage* storage,
    const IcebergBridgeExpireSnapshotsRequest* request,
    IcebergBridgeString** out,
    IcebergBridgeError** err);
```

实现要求：

- 加载 `metadata_location` 指定的 table。
- 按策略计算 expired / retained snapshot 集合。
- 生成 remove snapshot metadata update。
- dry-run 不写新 metadata，不删文件。
- execute 写新 metadata 并返回 `new_metadata_location`。
- 物理文件删除必须在 metadata 更新成功后执行。
- 删除失败写入 `failed`，不回滚 metadata。

### 5.3 remove orphan files ABI

可直接对齐 `origin/feat/orphan-file-cleanup` 已有接口，但建议命名抽象为
`remove_orphan_files`，并在 result 中统一字段：

```c
IcebergBridgeStatus iceberg_bridge_table_remove_orphan_files(
    IcebergBridgeStorage* storage,
    const char* metadata_location,
    const IcebergBridgeTableIdent* table_ident,
    bool dry_run,
    uint64_t grace_period_seconds,
    bool verbose,
    IcebergBridgeString** out,
    IcebergBridgeError** err);
```

第一阶段实际清理范围仍是 `<table_location>/data/`，文档和返回 JSON 中必须声明：

```json
{
  "scope": "data_files_only",
  "excluded_locations": ["metadata", "indices"]
}
```

### 5.4 index vacuum ABI

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

## 6. Rust SDK / iceberg-index 实现

### 6.1 常规 Iceberg maintenance

在 bridge 中新增 managed table maintenance 模块：

```text
src/services/managed_table/maintenance.rs
src/services/managed_table/expire_snapshots.rs
src/services/managed_table/orphan.rs
```

`expire_snapshots` 需要实现：

```text
load table from metadata_location
collect snapshots / refs / current snapshot
compute retained snapshot set
compute expired snapshot set
collect referenced files for retained snapshots
collect referenced files for expired snapshots
delete_candidates = expired_refs - retained_refs
write new metadata with RemoveSnapshots / RemoveStatistics
best-effort delete delete_candidates when dry_run=false && delete_files=true
return summary JSON
```

引用文件集合至少包括：

- data files
- delete files
- manifest files
- manifest list files
- statistics Puffin files

第一阶段可以先不删除旧 metadata json。旧 metadata json 的清理需要额外确认
metadata log、table property 和 CAS 发布策略，避免删掉仍可回滚或仍被外部引用的 metadata。

### 6.2 remove orphan files

复用 `origin/feat/orphan-file-cleanup` 的算法，但需要补齐：

- 统一 JSON 返回字段。
- `verbose` 控制是否返回文件列表。
- 默认 grace period 由 catalog 传入 7 天。
- 明确 scope 为 `data_files_only`。
- 目录扫描失败时 fail closed。

### 6.3 index vacuum

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

算法：

```text
1. load table from metadata_location
2. collect retained snapshot ids
   - current snapshot
   - all snapshots still present in current metadata
   - branch/tag/reference reachable snapshots
3. for each retained snapshot:
   - find registry Puffin from statistics-files
   - read SnapshotIndexRegistry
   - mark registry Puffin as live
   - mark all segment artifact_files as live
4. list {table_location}/indices recursively
5. candidate = actual_indices_files - live_files
6. filter candidate by:
   - older than grace period
   - optional index_name match when registry/artifact can be attributed
   - file extension / Puffin sanity checks
7. dry-run returns candidates
8. execute deletes candidates best-effort through ArtifactStore/FileIO
```

`index_name` 过滤策略：

- 能从 registry 中确认属于该 index 的 artifact 才按 index_name 精确清理。
- 对完全 orphan、无法归属某个 index 的文件，只有 `index_name IS NULL` 时才清理。
- 指定 `index_name` 时，不删除无法归属的 orphan 文件，计入 `skipped`。

## 7. 安全与并发语义

### 7.1 dry-run 与 execute

- `dry_run=true`: 不写 metadata，不删物理文件。
- `dry_run=false`: 执行 metadata 更新和物理删除。
- 所有危险接口默认 `dry_run=true`。

### 7.2 删除顺序

对于 snapshot expire：

```text
先写并发布新 metadata_location
再删除旧 metadata 不再引用的物理文件
```

对于 index vacuum / orphan cleanup：

```text
基于当前 metadata_location 计算 live set
只删除超过 7 天安全窗口的 orphan/candidate 文件
删除失败不回滚 metadata
返回 failed 列表供重试
```

### 7.3 失败语义

- metadata commit/CAS 失败：不删除物理文件。
- 物理删除部分失败：返回 `failed`，整体 SQL 可返回成功 JSON，`failed_file_count > 0`。
- 无法确认归属或引用关系：不删除，计入 `skipped`。
- 发现 table location 混用或 table uuid 不一致：fail closed，拒绝清理。

## 8. 分阶段落地

### Phase 1: Catalog SQL 壳与 bridge orphan 对接

- 在 `iceberg_catalog` 增加 `remove_orphan_files` SQL 函数。
- 对接 bridge `remove_orphan_files` ABI。
- 默认 7 天 grace period。
- 返回 jsonb。
- 只清理 `<table_location>/data/`。

### Phase 2: expire_snapshots

- bridge 实现 snapshot 过期策略和 dry-run。
- bridge 写新 metadata.json，返回 `new_metadata_location`。
- Catalog CAS 发布 `metadata_location`。
- execute 后 best-effort 删除确认安全文件。
- 添加 snapshot 多版本数据测试。

### Phase 3: index vacuum

- `iceberg-index` 实现 retained registry/artifact mark-and-sweep。
- bridge 暴露 `iceberg_index_rs_vacuum_index_by_metadata`。
- Catalog 暴露 `vacuum_index`。
- 对齐 LanceDB 行为：drop index 不物理删除，vacuum 才删除。

### Phase 4: 组合入口

- 增加 `iceberg_catalog.gc_table(...)`。
- 支持 `include_index => true` 组合调用。
- 加入维护任务表和可重试 job 状态。

## 9. 测试计划

### 9.1 remove_orphan_files

- 构造正常表和额外 orphan data file。
- `dry_run=true` 返回 orphan，但文件仍存在。
- `dry_run=false` 删除 orphan。
- 7 天窗口内的新文件不删除。
- metadata table uuid 不一致时拒绝清理。

### 9.2 expire_snapshots

- 构造至少 3 个 snapshots。
- `retain_last=1` 保护当前 snapshot。
- `older_than` 只过期超过窗口的 snapshot。
- 被 branch/tag/reference 保护的 snapshot 不过期。
- dry-run 不改变 metadata_location。
- execute 改变 metadata_location，并删除独占旧文件。

### 9.3 vacuum_index

- 创建索引，drop index，确认 artifact 仍存在。
- `vacuum_index(dry_run=true)` 返回候选但不删除。
- `vacuum_index(dry_run=false)` 删除 dropped index artifact。
- 历史 snapshot 对应 index artifact 不删除。
- 指定 `index_name` 时，不删除无法归属该 index 的 orphan。
- `index_name IS NULL` 时可清理无法归属但超过窗口的 orphan index Puffin。

## 10. 开发注意事项

- 所有接口必须使用 `metadata_location`，不得绕过 openGauss Catalog 直接提交到 REST Catalog。
- FDW 不参与维护命令。
- 默认 `dry_run=true`。
- 默认安全窗口 7 天。
- 文档、README 和测试必须明确第一阶段物理删除范围。
- 删除逻辑必须 fail closed：不确定就跳过或报错，不做猜测删除。
- 索引清理必须支持历史 index 查询，不得只保护 current snapshot。
