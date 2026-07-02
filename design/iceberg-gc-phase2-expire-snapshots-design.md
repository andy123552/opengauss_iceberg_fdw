# Iceberg GC 第二阶段：expire_snapshots 元数据重写与 CAS 发布设计

## 1. 背景

Iceberg 每次写入、删除、更新、compaction 或索引 registry 发布都会产生新的表版本或相关元数据。
如果历史 snapshot 永久保留，表会持续积累旧 data/delete files、manifest、manifest-list、statistics Puffin
和 metadata json，增加存储成本和规划开销。

第二阶段实现 `expire_snapshots`，它与第一阶段目录扫描型维护不同：

- 会生成新的 Iceberg table metadata json。
- 必须通过 openGauss Catalog CAS 发布新的 `metadata_location`。
- 物理删除必须发生在 CAS 成功之后。
- 删除前必须重新加载 base/new metadata 并重新计算引用差集，不能把 prepare 阶段返回的候选列表当成删除授权。

当前项目中，openGauss Catalog 是 `metadata_location` 的唯一事实来源；bridge / SDK 只能基于指定
`metadata_location` 读写候选 metadata，不能直接修改 `tables_internal`，也不能绕过 Catalog 提交到 REST Catalog。

## 2. 功能约束与设计边界

### 2.1 维护范围

第二阶段提供一个独立 SQL 入口：

```sql
iceberg_catalog.expire_snapshots(...)
```

能力范围：

- 按 `older_than`、`retain_last`、`snapshot_ids` 选择要过期的 snapshots。
- 保护当前 snapshot。
- 保护 branch/tag/reference 可达 snapshot。
- 生成候选新 metadata json。
- 由 Catalog 使用 CAS 发布 `new_metadata_location`。
- CAS 成功后，按 `delete_files` 决定是否执行物理文件删除。

不做：

- 不直接删除 index registry Puffin 或 index segment artifact。
- 不把 `vacuum_index` 合并进 bridge / SDK 的 `expire_snapshots` 实现。
- 不新增维护任务表、维护队列或后台 scheduler。
- 不在 bridge 中读写 `tables_internal`。

### 2.2 参数约束

- `older_than` 默认 7 天。
- `retain_last` 默认 1，用户可传参，必须为正整数。
- `snapshot_ids` 可显式指定候选 snapshot，但不能过期当前 snapshot 或 reference 保护的 snapshot。
- `dry_run=true` 不写新 metadata，不删除物理文件。
- `dry_run=false` 必须执行 prepare -> Catalog CAS -> delete-after-CAS 三段式。
- `delete_files=false` 只发布新 metadata，不删除物理文件。
- `include_index=true` 只表示 Catalog 层在 `expire_snapshots` 成功后可组合调用 `vacuum_index`；不改变
  `expire_snapshots` 底层语义。

### 2.3 与 Iceberg / LanceDB 的接口对比

| 能力 | Iceberg 原生行为 | LanceDB 行为 | 本项目第二阶段设计 |
| --- | --- | --- | --- |
| snapshot 过期 | `expire_snapshots` 删除旧 snapshot，并只删除不被未过期 snapshot 需要的文件；默认 `retain_last=1`，branch/tag 保护 snapshot | `optimize()` cleanup 清理超过 retention 的旧版本；tagged versions 受保护 | 独立 `expire_snapshots`，保护 current/reference snapshots，使用 Catalog CAS 发布新 metadata |
| 物理删除时机 | 引擎 action 通常在维护过程内完成 metadata 更新和文件删除 | `optimize()` 组合 compaction、cleanup、index update | 明确三段式：prepare 写候选 metadata，Catalog CAS，CAS 成功后重新计算并删除 |
| orphan 文件 | Iceberg 有独立 `remove_orphan_files` | cleanup 是 `optimize()` 的一部分 | 不在 `expire_snapshots` 中做目录扫描 orphan cleanup；交给第一阶段 `remove_orphan_files` |
| 旧 metadata json | Iceberg writer 可配置 delete-after-commit / previous-versions-max；Spark expire 可清理部分 metadata | cleanup 清理旧版本 | `expire_snapshots` 不隐式清理所有旧 metadata json；可由 Catalog 组合 `cleanup_old_metadata` |
| index 文件 | Iceberg 原生无本项目 index artifact | `optimize()` 会组合索引更新 | `expire_snapshots` 不直接删除 index registry / segment；Catalog 可组合 `vacuum_index` |

差异原因：

- openGauss Catalog 管理 metadata pointer，必须通过 SQL 事务 CAS 控制并发。
- bridge / SDK 无权修改 Catalog 表，因此不能实现“一次性提交并删除”的单体 action。
- 自定义索引文件有独立历史查询保护规则，必须由 `vacuum_index` 处理。

## 3. 使用示例与测试目标

### 3.1 dry-run

```sql
SELECT iceberg_catalog.expire_snapshots(
    p_namespace => 'db',
    p_table => 'events',
    older_than => interval '7 days',
    retain_last => 1,
    snapshot_ids => NULL,
    dry_run => true,
    delete_files => true,
    include_index => false,
    verbose => true
);
```

目标：

- 返回将过期的 snapshot id。
- 返回被保护的 snapshot id。
- 返回预估候选文件统计。
- 不写新 metadata。
- 不更新 `tables_internal.metadata_location`。
- 不删除物理文件。

### 3.2 execute + delete_files

```sql
SELECT iceberg_catalog.expire_snapshots(
    p_namespace => 'db',
    p_table => 'events',
    older_than => interval '7 days',
    retain_last => 3,
    dry_run => false,
    delete_files => true,
    include_index => false,
    verbose => true
);
```

目标：

- bridge prepare 写出候选 `new_metadata_location`。
- Catalog 用 `base_metadata_location` CAS 发布新指针。
- CAS 成功后 bridge delete-after-CAS 重新加载 base/new metadata。
- 只删除 `old_refs - new_refs` 中可确认安全的文件。

### 3.3 execute + delete_files=false

```sql
SELECT iceberg_catalog.expire_snapshots(
    p_namespace => 'db',
    p_table => 'events',
    older_than => interval '7 days',
    retain_last => 3,
    dry_run => false,
    delete_files => false
);
```

目标：

- 只发布新 metadata。
- 不删除物理文件。
- 后续可由独立维护接口继续清理。

### 3.4 include_index

```sql
SELECT iceberg_catalog.expire_snapshots(
    p_namespace => 'db',
    p_table => 'events',
    dry_run => false,
    delete_files => true,
    include_index => true
);
```

目标：

- `expire_snapshots` 本身不删除 index 文件。
- Catalog 在 expire 成功后按第一阶段文档组合调用 `vacuum_index`。
- `vacuum_index` 的结果作为组合步骤返回。

## 4. 端到端链路

### 4.1 总体三段式

```text
1. prepare_expire_snapshots
   Catalog -> bridge -> SDK
   输入 base_metadata_location
   输出 new_metadata_location 和摘要

2. Catalog CAS
   UPDATE tables_internal
      SET metadata_location = new_metadata_location
    WHERE namespace = $ns
      AND table_name = $table
      AND metadata_location = base_metadata_location

3. delete_expired_snapshot_files
   Catalog -> bridge -> SDK
   仅在 CAS 成功且 delete_files=true 时调用
   SDK 重新加载 base/new metadata，重新计算 old_refs - new_refs，再删除
```

### 4.2 Catalog 步骤

1. SQL 参数校验。
2. 从 `tables_internal` 读取当前表记录，记为 `base_metadata_location`。
3. 构造 bridge storage handle。
4. `dry_run=true`：
   - 调用 bridge dry-run prepare。
   - 返回 JSON。
   - 不写 metadata，不 CAS，不删除文件。
5. `dry_run=false`：
   - 调用 bridge `prepare_expire_snapshots`。
   - 校验返回的 `base_metadata_location` 等于 Catalog 当前读取值。
   - 在 SQL 事务中执行 CAS 更新。
   - CAS 失败：返回 request_error，不删除物理文件。
   - CAS 成功且 `delete_files=true`：调用 bridge `delete_expired_snapshot_files`。
   - CAS 成功且 `delete_files=false`：跳过物理删除。
6. 如 `include_index=true` 且 expire 成功，Catalog 可继续调用 `vacuum_index`。
7. 返回统一 JSON。

Catalog 独占职责：

- 当前 metadata pointer CAS。
- SQL 事务边界。
- 组合 `vacuum_index` / `cleanup_old_metadata` 等独立维护动作。

### 4.3 bridge 步骤

bridge 暴露两个底层 C ABI：

```c
IcebergBridgeStatus iceberg_bridge_table_prepare_expire_snapshots(...);
IcebergBridgeStatus iceberg_bridge_table_delete_expired_snapshot_files(...);
```

bridge 职责：

- 参数转换。
- storage handle 校验。
- 调用 `iceberg-index` SDK。
- 错误映射。
- 返回 JSON 字符串。

bridge 不做：

- 不 CAS。
- 不读取 `tables_internal`。
- 不把 prepare 和 delete 合成一个不可拆分接口。

### 4.4 SDK 步骤

`prepare_expire_snapshots`：

1. 加载 `base_metadata_location`。
2. 校验 table UUID。
3. 计算 retained snapshot set。
4. 计算 expired snapshot set。
5. 使用 Iceberg metadata builder 移除 expired snapshots。
6. 写出候选 new metadata json。
7. 返回 base/new metadata location、expired/retained 摘要、预估候选文件统计。

`delete_expired_snapshot_files`：

1. 重新加载 `base_metadata_location`。
2. 重新加载 `new_metadata_location`。
3. 校验两个 metadata 的 table UUID 一致。
4. 计算 old refs。
5. 计算 new refs。
6. `delete_candidates = old_refs - new_refs`。
7. 对候选执行路径、URI、mtime、文件类型保护规则。
8. best-effort 删除。
9. 返回删除统计和失败明细。

关键规则：

- prepare 返回的候选文件列表只用于展示、审计和预估，不能作为删除授权。
- delete-after-CAS 必须重新计算。
- CAS 成功后已发布的新 metadata 不回滚；文件级删除失败保留在 JSON。

## 5. 接口与实现细节

### 5.1 SQL 接口

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

参数语义：

- `older_than`：过期早于该窗口的 snapshot。
- `retain_last`：按当前 snapshot 祖先链至少保留最近 N 个 snapshot，默认 1，用户可传参，必须为正整数。
- `snapshot_ids`：显式候选；仍必须经过 current/reference 保护检查。
- `dry_run`：只计算，不写 metadata，不删除文件。
- `delete_files`：CAS 成功后是否删除物理文件。
- `include_index`：Catalog 层是否组合调用 `vacuum_index`。
- `verbose`：是否返回候选文件明细。

### 5.2 prepare_expire_snapshots

SDK request 建议：

```rust
pub struct MetadataPrepareExpireSnapshotsRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub base_metadata_location: String,
    pub older_than_ms: Option<i64>,
    pub retain_last: u32,
    pub snapshot_ids: Option<Vec<i64>>,
    pub dry_run: bool,
    pub file_io_config_json: String,
}
```

实现细节：

1. `retain_last == 0` 返回 invalid input。
2. `older_than` 和 `snapshot_ids` 都为空时，使用默认 7 天策略。
3. 当前 snapshot 永远进入 retained set。
4. current snapshot 祖先链最近 `retain_last` 个进入 retained set。
5. branch/tag/reference 可达 snapshot 进入 retained set。
6. 满足 `older_than` 或 `snapshot_ids` 且不在 retained set 的 snapshot 进入 expired set。
7. expired set 为空时可返回 no-op，`new_metadata_location` 可以为 `base_metadata_location` 或 `null`，但 Catalog 必须能识别 no-op。
8. 非 dry-run 写出 new metadata json。
9. dry-run 不写 new metadata，`new_metadata_location=null`。

### 5.3 Catalog CAS

CAS SQL 必须等价于：

```sql
UPDATE iceberg_catalog.tables_internal
   SET metadata_location = $new_metadata_location,
       current_snapshot_id = $new_current_snapshot_id
 WHERE namespace = $namespace
   AND table_name = $table_name
   AND metadata_location = $base_metadata_location;
```

约束：

- 更新行数必须为 1。
- 更新行数为 0 表示并发修改，返回 request_error / commit conflict。
- CAS 失败不得调用 delete-after-CAS。
- CAS 失败留下的未发布 new metadata 由第一阶段 `cleanup_old_metadata` 在安全窗口后处理。

### 5.4 delete_expired_snapshot_files

SDK request 建议：

```rust
pub struct MetadataDeleteExpiredSnapshotFilesRequest {
    pub table_namespace: Vec<String>,
    pub table_name: String,
    pub base_metadata_location: String,
    pub new_metadata_location: String,
    pub grace_period_seconds: u64,
    pub file_io_config_json: String,
}
```

引用集合：

- `old_refs`：base metadata 中所有 snapshot、manifest-list、manifest、data/delete file、statistics file 引用。
- `new_refs`：new metadata 中仍然引用的同类文件。
- `delete_candidates`：`old_refs - new_refs`。

可删除范围：

- 只被过期 snapshots 引用且不被任何 retained snapshot 引用的 data files。
- 只被过期 snapshots 引用且不被任何 retained snapshot 引用的 delete files。
- 只被过期 snapshots 引用且不被新 metadata 引用的 manifest files。
- 只被过期 snapshots 引用且不被新 metadata 引用的 manifest-list files。
- 已从新 metadata 移除且确认为 Iceberg 原生 statistics Puffin 的文件。

不得删除：

- new metadata 仍引用的任何文件。
- 当前 snapshot 或 branch/tag/reference 可达 snapshot 需要的文件。
- index registry Puffin。
- index segment artifact。
- 无法确认文件类型或归属的文件。

删除保护：

- 删除前重新 stat。
- 安全窗口内文件跳过。
- mtime 缺失跳过。
- NotFound 幂等成功。
- URI 越界跳过或请求级 fail closed。

### 5.5 include_index 组合

`include_index=true` 的实现只允许在 Catalog 层：

```text
expire_snapshots step 成功
  -> if include_index then call vacuum_index(...)
```

不得：

- 在 SDK 的 prepare 阶段读取和删除 index artifact。
- 在 delete-after-CAS 中混入 index registry / segment 删除。
- 让 `expire_snapshots` 返回 index 文件作为自身删除结果。

## 6. URI 与保护规则

第二阶段删除物理文件时，必须复用第一阶段通用 URI 规则：

- 规范化 URI。
- scheme、authority/bucket、path 分别比较。
- table root 使用目录边界比较。
- 禁止 `..` 和无法规范化路径。
- 删除候选必须来自 `old_refs - new_refs`。
- 删除候选必须属于当前 table location 或 metadata 引用明确允许的位置。
- 文件类型必须可确认。
- table UUID 必须一致。

额外规则：

- base/new metadata UUID 不一致，整个 delete-after-CAS 请求失败。
- new metadata 无法加载，请求失败，不删除文件。
- base metadata 无法加载，请求失败，不删除文件。
- prepare 与 delete-after-CAS 之间不能共享删除授权，只能共享 base/new metadata location。

## 7. 返回 JSON 规范

`expire_snapshots` 返回：

```json
{
  "operation": "expire_snapshots",
  "dry_run": false,
  "table": "db.events",
  "table_uuid": "...",
  "base_metadata_location": "...",
  "new_metadata_location": "...",
  "final_metadata_location": "...",
  "expired_snapshot_ids": [1001, 1002],
  "retained_snapshot_ids": [1003, 1004],
  "protected_snapshot_ids": [1004],
  "candidate_file_count": 12,
  "candidate_bytes": 1048576,
  "deleted_data_files_count": 1,
  "deleted_position_delete_files_count": 0,
  "deleted_equality_delete_files_count": 0,
  "deleted_manifest_files_count": 2,
  "deleted_manifest_lists_count": 1,
  "deleted_statistics_files_count": 0,
  "deleted_bytes": 524288,
  "skipped_file_count": 0,
  "failed_file_count": 0,
  "failed": [],
  "steps": {
    "prepare": {"status": "ok"},
    "catalog_cas": {"status": "ok"},
    "delete_files": {"status": "ok"}
  }
}
```

字段规则：

- dry-run 中 `new_metadata_location=null`。
- no-op 中 `expired_snapshot_ids=[]`，`steps.catalog_cas.status="skipped"`。
- CAS 失败返回 request_error，不出现物理删除结果。
- `failed` 始终返回。
- verbose 控制候选/删除/跳过明细。

## 8. Catalog 组合入口

第二阶段实现后，第一阶段文档中的 `gc_table(...)` 可以启用 `expire_snapshots` step：

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

组合语义：

- `dry_run=true`：所有启用 step 都只做 dry-run。
- `dry_run=false`：按顺序同步执行。
- `expire_snapshots` CAS 成功后不因后续 step 失败而回滚。
- 请求级错误停止后续依赖 step。
- 文件级失败保留在 step JSON，并让 step 状态为 `partial_failure`。

推荐顺序：

```text
expire_snapshots -> remove_orphan_files -> cleanup_old_metadata -> vacuum_index
```

## 9. 测试计划

### 9.1 snapshot 策略

- 构造至少 3 个 snapshots。
- `retain_last=1` 保护当前 snapshot。
- `retain_last=3` 保护最近 3 个祖先 snapshots。
- `older_than` 只过期超过窗口的 snapshot。
- `snapshot_ids` 显式指定时仍保护 current/reference snapshot。
- branch/tag/reference 可达 snapshot 不过期。

### 9.2 dry-run

- 不写新 metadata。
- 不更新 `tables_internal.metadata_location`。
- 不删除物理文件。
- 返回 expired/retained 摘要和候选统计。

### 9.3 execute

- prepare 写出候选 new metadata。
- Catalog CAS 成功后 metadata pointer 更新。
- CAS 失败不删除物理文件。
- delete-after-CAS 重新加载 base/new metadata。
- delete-after-CAS 重新计算 `old_refs - new_refs`。
- `delete_files=false` 不删除物理文件。

### 9.4 删除保护

- new metadata 仍引用的文件不删除。
- 当前 snapshot 文件不删除。
- branch/tag/reference 可达文件不删除。
- index registry Puffin 和 index segment artifact 不删除。
- URI 越界不删除。
- mtime 在 7 天窗口内不删除。
- NotFound 幂等成功。

### 9.5 S3 集成

S3 测试通过环境变量显式启用，未配置时跳过。覆盖：

- prepare 写 metadata。
- CAS 后 delete-after-CAS 删除。
- URI normalize。
- bucket / authority 比较。
- stat / mtime。
- NotFound 幂等。

## 10. 开发清单

### 10.1 SDK

- 实现 `prepare_expire_snapshots`。
- 实现 `delete_expired_snapshot_files`。
- 实现 snapshot 策略计算。
- 实现 base/new metadata 引用差集。
- 实现路径、UUID、文件类型和安全窗口保护。

### 10.2 bridge

- 暴露 prepare C ABI。
- 暴露 delete-after-CAS C ABI。
- 映射 SDK 错误到 bridge status。
- 不实现 Catalog CAS。

### 10.3 Catalog

- 暴露 `expire_snapshots` SQL。
- 查询 `tables_internal`。
- 调 prepare。
- 执行 CAS。
- CAS 成功后按 `delete_files` 调 delete-after-CAS。
- 按 `include_index` 组合调用 `vacuum_index`。
