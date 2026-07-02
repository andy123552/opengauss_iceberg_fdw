# Iceberg GC 第一阶段：目录扫描型维护接口设计

## 1. 背景

openGauss Iceberg 集成当前通过 `iceberg_catalog.tables_internal.metadata_location`
管理 Iceberg 表的当前元数据指针。FDW 负责查询扫描，不参与表维护命令。表维护入口统一放在
`iceberg_catalog` 扩展中，由 SQL 函数同步触发 bridge 和 Rust SDK 能力。

第一阶段解决不需要改写 Iceberg `metadata.json` 的物理文件清理问题：

- 写入任务失败、取消、进程崩溃后，可能留下未被任何 Iceberg metadata 引用的数据文件。
- drop/rebuild index 只更新索引 registry，不应立即物理删除历史查询仍可能依赖的索引文件。
- 多次提交会积累旧 table metadata json；这些文件不属于数据文件，不能混入 orphan data cleanup。

第一阶段接口全部是目录扫描型维护动作，只清理已能证明安全的物理文件，不修改
`tables_internal.metadata_location`，不生成新的 Iceberg metadata，不执行 Catalog CAS。

参考实现边界：

- Iceberg 原生维护能力包含 snapshot 过期、orphan 文件清理和旧 metadata 清理。Iceberg 文档明确
  `remove_orphan_files` 需要保留足够长的 retention interval，否则可能删除正在写入但尚未提交的文件。
  Spark procedure 默认 orphan retention 是 3 天，`expire_snapshots` 默认 `retain_last=1`，并保护仍被
  branch/tag 引用的 snapshot。
- LanceDB OSS 将 compaction、旧版本清理和索引更新组合在 `optimize()` 中，默认 7 天 retention；
  tagged versions 不会被 cleanup 删除。
- 本项目采用 Iceberg 的独立维护动作语义，同时保留 LanceDB 风格的可选 Catalog 组合入口，但第一阶段
  不引入后台任务表、维护队列或常驻调度服务。

参考文档：

- Apache Iceberg maintenance: <https://iceberg.apache.org/docs/latest/maintenance/>
- Apache Iceberg Spark procedures: <https://iceberg.apache.org/docs/latest/spark-procedures/>
- LanceDB reindexing / optimize: <https://docs.lancedb.com/indexing/reindexing>
- LanceDB performance / cleanup: <https://docs.lancedb.com/performance>
- LanceDB version retention: <https://docs.lancedb.com/tables/versioning>

## 2. 功能约束与设计边界

### 2.1 维护范围

第一阶段提供三个独立 SQL 入口：

```sql
iceberg_catalog.remove_orphan_files(...)
iceberg_catalog.cleanup_old_metadata(...)
iceberg_catalog.vacuum_index(...)
```

三个入口都必须满足：

- 使用 `p_namespace text` 和 `p_table text` 定位表，不要求调用方传 `regclass`。
- 通过 `tables_internal` 获取 `table_uuid`、`table_location`、`metadata_location`、`current_snapshot_id`。
- 统一返回 `jsonb`。
- 默认 `dry_run=true`。
- 默认安全窗口为 7 天。
- 只删除 100% 确认安全的文件；不能确认时必须 fail closed，表现为请求级错误或文件级 `skipped`。
- 不新增维护任务表，不引入维护队列，不引入后台 scheduler。
- 不修改 `tables_internal.metadata_location`。
- 不绕过 openGauss Catalog 直接连接 REST Catalog 并提交 table metadata。

### 2.2 术语

- `metadata_location`：`tables_internal.metadata_location` 中记录的当前 Iceberg metadata json URI，是
  本项目唯一的表状态入口。
- `table_location`：Iceberg table root URI，来自当前 table metadata 或 Catalog 表记录。
- `安全窗口`：候选文件最后修改时间必须早于 `now - older_than`，默认 7 天。mtime 缺失或无法判断时不得删除。
- `fail closed`：只要安全判断无法完成，就拒绝本次清理或跳过该文件；不得猜测删除。
- `protected set`：必须保留的文件集合。
- `candidate set`：经过引用关系和路径范围筛选后，可能可以删除的文件集合。
- `execute`：`dry_run=false`，允许执行物理删除；仍必须逐文件复查保护规则。

### 2.3 与 Iceberg / LanceDB 的接口对比

| 能力 | Iceberg 原生行为 | LanceDB 行为 | 本项目第一阶段设计 |
| --- | --- | --- | --- |
| orphan 文件清理 | `remove_orphan_files` / `deleteOrphanFiles` 清理未被 metadata 引用且超过 retention 的文件；短 retention 有误删风险 | `optimize()` 的 cleanup 会清理旧版本文件 | 独立 `remove_orphan_files`，只扫描 `{table_location}/data/`，默认 7 天，路径和 UUID 校验更保守 |
| 旧 metadata json 清理 | Iceberg writer 可通过 `write.metadata.delete-after-commit.enabled` 和 `write.metadata.previous-versions-max` 自动删除旧 metadata；也可由维护动作处理 | 旧版本由 cleanup/optimize 按 retention 清理 | 独立 `cleanup_old_metadata`，只处理 table metadata json，默认保留最近 100 个并支持用户传参 |
| index 文件清理 | Iceberg 原生没有本项目自定义 index registry / segment 语义 | `optimize()` 会组合索引更新；旧版本保留受 retention/tag 保护 | 独立 `vacuum_index`，mark-and-sweep，保护历史 snapshot / branch / tag / reference 可达索引 |
| 组合入口 | Spark procedures 通常是独立调用 | `optimize()` 是组合入口 | 可选 `gc_table(...)` 只在 Catalog 层编排独立接口，不新增 bridge / SDK 组合 ABI |
| 后台任务 | 取决于引擎或用户调度 | OSS 手动，Enterprise 可自动 | 不设计后台任务；调用方用 cron、脚本或数据库任务调 SQL |

差异原因：

- 本项目的 metadata pointer 由 openGauss Catalog 管理，bridge / SDK 不能直接成为当前 metadata 的事实来源。
- 自定义 index registry 和 segment artifact 不是 Iceberg 原生文件类型，必须单独定义保护规则。
- 为避免误删，第一阶段仅做目录扫描型清理，不做 snapshot expire 的 metadata rewrite。

## 3. 使用示例与测试目标

### 3.1 remove_orphan_files

```sql
SELECT iceberg_catalog.remove_orphan_files(
    p_namespace => 'db',
    p_table => 'events',
    older_than => interval '7 days',
    dry_run => true,
    verbose => true
);
```

目标场景：

- 表存在正常 snapshot，同时 `data/` 下存在一个未被任何 snapshot manifest 引用的旧文件。
- `dry_run=true` 返回候选，不删除。
- `dry_run=false` 删除超过安全窗口且仍未被引用的候选。
- mtime 在 7 天窗口内、mtime 缺失、路径越界、UUID 校验失败的文件都不得删除。

约束场景：

- 不扫描 `metadata/`。
- 不扫描 `indices/`。
- 不清理 manifest、manifest-list、statistics Puffin、index registry Puffin、index segment artifact。

### 3.2 cleanup_old_metadata

```sql
SELECT iceberg_catalog.cleanup_old_metadata(
    p_namespace => 'db',
    p_table => 'events',
    older_than => interval '7 days',
    retain_last => 100,
    dry_run => true,
    verbose => true
);
```

目标场景：

- 当前 metadata json 必须保留。
- metadata log / previous metadata log 可达的 metadata json 必须保留。
- 最近 `retain_last` 个 metadata json 必须保留，默认 100；调用方可显式传入。
- 超过安全窗口、解析成功、table UUID 匹配且不在保护集合中的旧 metadata json 才能删除。

约束场景：

- 不删除 manifest-list、manifest、statistics Puffin、index registry Puffin、index segment artifact、data/delete files。
- unknown metadata json 必须解析并校验 `table-uuid`；解析失败或 UUID 不匹配时跳过或请求级 fail closed。

### 3.3 vacuum_index

```sql
SELECT iceberg_catalog.vacuum_index(
    p_namespace => 'db',
    p_table => 'events',
    index_name => NULL,
    older_than => interval '7 days',
    dry_run => true,
    verbose => true
);
```

目标场景：

- drop index 后 registry entry 已变为 Dropped，但历史 snapshot 仍可能引用旧 registry / segment。
- `vacuum_index` 必须保留所有 retained snapshot、branch、tag、reference 可达的 index registry 和 segment。
- 超过安全窗口、路径安全、Puffin 类型合理、归属本表且不在 live set 中的 index 文件才可删除。

约束场景：

- 指定 `index_name` 时，只删除能证明属于该逻辑索引的文件。
- 无法归属到指定 index 的 orphan 文件必须进入 `skipped`。
- `index_name IS NULL` 时可以清理所有能证明属于本表且不 live 的 index 文件。

## 4. 端到端链路

### 4.1 Catalog 通用步骤

1. SQL 函数接收 `p_namespace` / `p_table`、`older_than`、`dry_run`、`verbose` 等参数。
2. Catalog 从 `iceberg_catalog.tables_internal` 查询表记录。
3. Catalog 校验调用权限和表存在性。
4. Catalog 构造 bridge storage handle 所需的 FileIO 配置，必须与建表/扫描路径一致。
5. Catalog 调用对应 bridge C ABI。
6. Catalog 将 bridge 返回 JSON 转为 `jsonb` 返回。
7. 请求级错误抛 SQL ERROR；文件级删除失败保留在 JSON 的 `failed` 中。

Catalog 不做：

- 不解析 Iceberg manifest。
- 不解析 index registry Puffin。
- 不扫描对象存储目录。
- 不更新 `metadata_location`。

### 4.2 bridge 通用步骤

1. C ABI 校验 storage handle、`metadata_location`、table ident、参数范围。
2. bridge 将请求转换成 `iceberg-index-abi` 的 metadata-location request。
3. bridge 调用 SDK 同步入口。
4. bridge 不在本层实现清理算法；算法属于 `iceberg-index` SDK。
5. bridge 将 SDK JSON 字符串透传为 `IcebergBridgeString`。

### 4.3 SDK 通用步骤

1. 用 `metadata_location` 和 FileIO 加载固定版本 Iceberg table。
2. 从 table metadata 读取 `table_uuid`、`table_location`、snapshot、manifest、statistics-files 等信息。
3. 构建 protected/live set。
4. 扫描接口允许的物理目录。
5. 计算 candidate set。
6. 应用安全窗口、路径、URI、UUID、Puffin sanity 等保护规则。
7. `dry_run=true` 只返回候选和统计。
8. `dry_run=false` 对每个候选执行删除前 re-stat，复查安全窗口，再 best-effort 删除。
9. NotFound 按幂等成功处理，但 JSON 中记录 `reason=not_found`。

## 5. 接口与实现细节

### 5.1 remove_orphan_files

SQL 形态：

```sql
SELECT iceberg_catalog.remove_orphan_files(
    p_namespace text,
    p_table text,
    older_than interval DEFAULT interval '7 days',
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

bridge C ABI：

```c
IcebergBridgeStatus iceberg_bridge_table_remove_orphan_data_files(
    IcebergBridgeStorage *storage,
    const char *metadata_location,
    const IcebergBridgeTableIdent *table_ident,
    bool dry_run,
    uint64_t grace_period_seconds,
    IcebergBridgeString **out,
    IcebergBridgeError **err);
```

SDK request：

```rust
pub struct MetadataRemoveOrphanFilesRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub metadata_location: String,
    pub dry_run: bool,
    pub grace_period_seconds: u64,
    pub file_io_config_json: String,
}
```

SDK 实现步骤：

1. 加载 `metadata_location` 指向的 table metadata。
2. 读取 `table_location` 和 `table_uuid`。
3. 扫描 `<table_location>/metadata/` 下所有 `*.metadata.json`，校验 `table-uuid` 均等于当前表 UUID。
4. 遍历当前 table metadata 中所有 snapshots。
5. 对每个 snapshot 读取 manifest-list 和 manifest。
6. 收集所有被任意有效 snapshot 引用的 data/delete file URI，规范化为 table-relative path。
7. 只扫描 `<table_location>/data/`。
8. `actual_data_files - referenced_data_files` 得到初始 orphan set。
9. 对每个候选 stat，mtime 必须早于 `now - grace_period_seconds`。
10. `dry_run=true` 返回候选。
11. `dry_run=false` 删除前再次 stat；mtime 进入安全窗口、mtime 缺失或 stat 无法判断时加入 `skipped`。
12. 删除成功加入 `removed`；NotFound 加入 `removed` 且 `reason=not_found`；其他删除失败加入 `failed`。

保护规则：

- 只允许删除 `{table_location}/data/` 下文件。
- metadata UUID 校验失败时请求级 fail closed。
- URI 必须规范化后比较 scheme、authority/bucket、path，不能用字符串前缀代替。
- 不认识的 scheme、包含 `..`、越界路径、mtime 缺失、路径归属不明，必须跳过。

### 5.2 cleanup_old_metadata

SQL 形态：

```sql
SELECT iceberg_catalog.cleanup_old_metadata(
    p_namespace text,
    p_table text,
    older_than interval DEFAULT interval '7 days',
    retain_last integer DEFAULT 100,
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

`retain_last` 约束：

- 用户可传参，默认 100。
- 必须为正整数。
- 表示按 metadata 版本顺序保留最近 N 个 table metadata json。
- `retain_last` 只保护 table metadata json，不保护 manifest、manifest-list 或 Puffin。

bridge C ABI 建议：

```c
IcebergBridgeStatus iceberg_bridge_table_cleanup_old_metadata(
    IcebergBridgeStorage *storage,
    const char *metadata_location,
    const IcebergBridgeTableIdent *table_ident,
    uint64_t retain_last,
    bool dry_run,
    uint64_t grace_period_seconds,
    IcebergBridgeString **out,
    IcebergBridgeError **err);
```

SDK 实现步骤：

1. 加载当前 `metadata_location`。
2. 获取 `table_uuid`、`table_location`。
3. 构建 `protected_metadata_files`：
   - 当前 `metadata_location`。
   - 当前 metadata log / previous metadata log 引用的 metadata json。
   - `<table_location>/metadata/` 下按版本号或 last_modified 排序后的最近 `retain_last` 个 metadata json。
   - 安全窗口内的 metadata json。
4. 扫描 `<table_location>/metadata/` 下 table metadata json 文件。
5. 文件在 protected set 中则跳过。
6. unknown metadata json 必须读取内容并解析 Iceberg `TableMetadata`。
7. 校验 `table-uuid` 等于当前表 UUID。
8. 解析失败、UUID 不匹配、路径越界、mtime 缺失或安全窗口内，加入 `skipped`。
9. `dry_run=true` 返回候选。
10. `dry_run=false` 删除前 re-stat 并重复安全窗口检查，再 best-effort 删除。

保护规则：

- 不清理 manifest-list、manifest、statistics Puffin、index registry Puffin、index segment artifact。
- 不清理任何当前 metadata log 可达的 metadata json。
- 不清理最近 `retain_last` 个 metadata json。
- 不能解析为当前表 metadata 的文件不得删除。

### 5.3 vacuum_index

SQL 形态：

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

bridge C ABI 建议：

```c
IcebergBridgeStatus iceberg_index_rs_vacuum_index_by_metadata(
    IcebergBridgeStorage *storage,
    const IcebergIndexVacuumByMetadataRequest *request,
    IcebergBridgeString **out,
    IcebergBridgeError **err);
```

SDK request 建议：

```rust
pub struct MetadataVacuumIndexRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub metadata_location: String,
    pub index_name: Option<String>,
    pub dry_run: bool,
    pub grace_period_seconds: u64,
    pub file_io_config_json: String,
}
```

SDK 实现采用 mark-and-sweep：

1. 加载 `metadata_location` 指向的 table metadata。
2. 计算 retained snapshot 集合：
   - 当前 snapshot。
   - 未被过期策略排除的历史 snapshot。
   - branch/tag/reference 可达 snapshot。
3. 对 retained snapshot 读取 `statistics-files.statistics_path`。
4. 只把包含本项目 index registry blob type 且 table UUID 匹配的 Puffin 作为 registry Puffin。
5. 读取 registry，收集 registry 自身路径和所有 segment artifact 路径，得到 `live_index_files`。
6. 扫描 `{table_location}/indices/`，并结合 statistics-files 中可确认的 registry Puffin，得到 `actual_index_files`。
7. 计算 `candidate_index_files = actual_index_files - live_index_files`。
8. 对 candidate 应用安全窗口、URI root、Puffin magic/footer、registry table UUID、index_name 过滤。
9. `dry_run=true` 返回候选。
10. `dry_run=false` 删除前 re-stat，复查保护规则，再 best-effort 删除。

三个集合定义：

- `live_index_files`：仍被 retained snapshot / branch / tag / reference / registry 引用，必须保留。
- `actual_index_files`：对象存储中实际存在且可能属于该表索引的文件。
- `candidate_index_files`：`actual_index_files - live_index_files` 后仍需继续通过保护规则的候选。

保护规则：

- 当前 snapshot 的 registry Puffin 和 segment artifact 不得删除。
- 任意 retained snapshot 可达的 registry Puffin 和 segment artifact 不得删除。
- `index_name` 指定时，无法证明属于该 index 的文件不得删除。
- Puffin sanity check 失败时不得删除。
- table UUID / registry ownership 不匹配时请求级 fail closed 或文件级 skipped。

Puffin sanity check 至少包括：

- 文件扩展名或路径布局符合本项目索引文件约定。
- Puffin magic/footer 可读。
- registry Puffin 必须包含 index registry blob type。
- registry table UUID 必须等于当前 table UUID。
- segment artifact 必须能从 live/dropped registry entry 或 artifact metadata 中证明归属。

## 6. URI 与删除保护规则

所有会删除物理文件的接口都必须执行以下通用规则：

- 将输入路径和候选路径解析为规范 URI。
- 分别比较 scheme、authority/bucket、normalized path。
- table root 比较必须使用带目录分隔符的 normalized prefix。
- 禁止 `..`、空 path segment、模糊重复 slash、无法规范化的 URI。
- cleanup root 必须符合接口范围：
  - `remove_orphan_files`: `{table_location}/data/`
  - `cleanup_old_metadata`: `{table_location}/metadata/` 中 table metadata json
  - `vacuum_index`: `{table_location}/indices/` 和 confirmed index registry Puffin
- 删除前必须 re-stat。
- mtime 缺失或无法判断时不得删除。
- NotFound 按幂等成功处理。
- table UUID 混用、table_location 重叠、候选归属其他表时 fail closed。

当前不强制实现 `table_location` 全局唯一性和父子包含关系检查；后续应在 `create_table` 中补充：

- 不同表的 `table_location` 不能完全相同。
- 不同表的 `table_location` 不能存在父子包含关系。

## 7. 返回 JSON 规范

独立接口统一返回：

```json
{
  "operation": "remove_orphan_files",
  "dry_run": true,
  "scope": "data_files_only",
  "table": "db.events",
  "table_uuid": "...",
  "base_metadata_location": "...",
  "new_metadata_location": null,
  "current_snapshot_id": 123,
  "grace_period_seconds": 604800,
  "candidate_file_count": 1,
  "candidate_bytes": 1024,
  "deleted_file_count": 0,
  "deleted_bytes": 0,
  "skipped_file_count": 0,
  "failed_file_count": 0,
  "candidates": [],
  "removed": [],
  "skipped": [],
  "failed": []
}
```

字段规则：

- `new_metadata_location` 第一阶段固定为 `null`。
- `failed` 始终返回。
- `candidates`、`removed`、`skipped` 仅在 `verbose=true` 或测试模式下返回完整明细；默认可只返回统计。
- 请求级错误使用 SQL ERROR / C ABI error。
- 文件级删除失败放入 `failed`，不回滚已完成删除。

## 8. Catalog 组合入口

第一阶段可以设计但不要求立即实现：

```sql
SELECT iceberg_catalog.gc_table(
    p_namespace text,
    p_table text,
    expire_snapshots boolean DEFAULT false,
    remove_orphan_files boolean DEFAULT true,
    cleanup_old_metadata boolean DEFAULT false,
    vacuum_index boolean DEFAULT false,
    older_than interval DEFAULT interval '7 days',
    retain_last integer DEFAULT 100,
    dry_run boolean DEFAULT true,
    verbose boolean DEFAULT false
);
```

组合入口只在 Catalog 层同步编排独立 SQL/C ABI，不要求 bridge 或 SDK 增加组合接口。

执行顺序建议：

```text
expire_snapshots -> remove_orphan_files -> cleanup_old_metadata -> vacuum_index
```

第一阶段中 `expire_snapshots=false` 是默认值。若第二阶段实现后启用 `expire_snapshots`，组合入口必须遵守第二阶段文档的
CAS 和 delete-after-CAS 规则。

返回 JSON 使用固定 step 状态：

- `ok`
- `partial_failure`
- `request_error`
- `skipped`
- `skipped_due_to_failed_step`

## 9. 测试计划

### 9.1 本地回归

- 使用本地 FileIO / 对象存储构造真实 Iceberg metadata。
- 覆盖 `dry_run=true` 和 `dry_run=false`。
- 覆盖默认 7 天安全窗口。
- 通过缩短 `older_than` 或修改本地文件 mtime 构造超过安全窗口的文件。
- 覆盖 NotFound 幂等。
- 覆盖路径越界、mtime 缺失、UUID 不匹配、Puffin sanity check 失败。

### 9.2 S3 集成

S3 测试通过环境变量显式开启，未配置时跳过，不影响本地回归。至少需要：

- endpoint
- bucket
- access key
- secret key
- region
- path-style 配置

S3 测试必须覆盖：

- URI normalize。
- scheme / authority / bucket 比较。
- prefix 包含关系。
- stat / mtime。
- NotFound 幂等。
- dry-run 和 execute。

### 9.3 接口级测试

- `remove_orphan_files` 只删除 `data/` 下 orphan data/delete files。
- `cleanup_old_metadata` 保护当前 metadata、metadata log、最近 `retain_last` 个 metadata json。
- `vacuum_index` 保护 retained snapshot / branch / tag / reference 可达的 registry 和 segment。
- 组合入口 disabled step 返回 `skipped`。
- 文件级失败返回 `partial_failure`，请求级失败返回 `request_error`。

## 10. 开发清单

### 10.1 SDK

- 在 `iceberg-index` 实现 `remove_orphan_files`。
- 在 `iceberg-index` 实现 `cleanup_old_metadata`。
- 在 `iceberg-index` 实现 `vacuum_index`。
- 所有实现都必须接收 `metadata_location` 和 FileIO config。
- 所有实现都必须返回统一 JSON。

### 10.2 bridge

- 暴露 `iceberg_bridge_table_remove_orphan_data_files`。
- 暴露 `iceberg_bridge_table_cleanup_old_metadata`。
- 暴露 `iceberg_index_rs_vacuum_index_by_metadata`。
- bridge 只做 C ABI 参数转换和错误映射，不实现核心清理算法。

### 10.3 Catalog

- 暴露三个 SQL 函数。
- 从 `tables_internal` 查表。
- 构造 bridge storage handle。
- 返回 jsonb。
- 不新增维护任务表或队列。
