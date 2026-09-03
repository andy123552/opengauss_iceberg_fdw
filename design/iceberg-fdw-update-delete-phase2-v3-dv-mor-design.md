# Iceberg FDW 更新删除第二阶段：format v3 Deletion Vector MOR 设计

版本：v2.0（阶段二开发基线）  
更新时间：2026-09-03  
语义依据：[Apache Iceberg Table Spec](https://iceberg.apache.org/spec/)、
[Puffin Spec](https://iceberg.apache.org/puffin-spec/)

## 1. 目标、范围与非目标

### 1.1 目标

在阶段一 v2 Position Delete MOR 的 SQL、overlay、flush 和 Catalog CAS 模型上，增加 Iceberg format v3 的 Deletion Vector（DV）能力：

- v3 表的 DELETE 使用 Puffin deletion-vector-v1；
- v3 表的 UPDATE 在同一 RowDelta snapshot 中提交 DV 和 after-image data file；
- 新 DV 与已有 DV、升级遗留的 position delete 合并，每个 data file 在一个 snapshot 中最多一个有效 DV；
- 遵循 v3 row lineage（_row_id、_last_updated_sequence_number）；
- 失败、重试、并发冲突和 orphan 清理语义与阶段一一致；
- FDW、Delta、Bridge、Rust SDK 和 Catalog 的边界可独立测试。

### 1.2 非目标

- 不把 v3 降级成新写 position delete；
- 不在 FDW 中实现 Puffin 编码、manifest 构造或 metadata pointer 更新；
- 不在本阶段隐式把 v2 表升级成 v3，也不重写整个历史数据文件；
- 不以 row ID/业务主键代替物理 locator；row lineage 只负责行生命周期追踪；
- 不改变阶段一 v1/v2 的行为；
- 不把 index registry、index segment 或普通 orphan 文件删除混入 DV flush。

## 2. 版本门禁和兼容矩阵

| 表格式 | 新建/更新行为 | 读路径 |
| --- | --- | --- |
| v1 | 拒绝 row-level U/D flush | 保持阶段一 v1 读取 |
| v2 | 继续使用 Position Delete MOR | 读取 position/equality delete |
| v3 | 仅允许 DV MOR；禁止新增 position delete | 读取 DV，并兼容升级遗留 position delete |

v3 支持必须同时满足 SDK DV writer/reader、Bridge ABI、Delta flush、FDW gate、Catalog 发布和黑盒 reader 验收。任一能力缺失时，FDW/Delta 必须在 flush 前返回明确的 feature_not_supported，不得生成部分 Puffin、manifest 或 metadata。

v3 规范要点：

- DV 存储为 Puffin deletion-vector-v1 blob；
- DV 是单个 data file 的物理位置位图，位置为非负 64 位整数；
- delete manifest entry 必须包含 referenced_data_file、content_offset、content_size_in_bytes；
- Puffin blob properties 必须包含 referenced-data-file 和 cardinality，不写 compression-codec；
- snapshot-id 和 sequence-number 在写 Puffin 时未知，Puffin v1 blob metadata 使用 -1；
- 一个 data file 在一个 snapshot 中最多一个 DV；新写入必须合并旧 DV 及仍适用的旧 position delete；
- v3 不允许新增 position delete，但从 v2 升级后已有 position delete 仍合法，并在首次产生 DV 时合并；
- 删除 data file 时必须移除对应 DV manifest entry；包含该 DV 的 Puffin 文件可以留给后续 orphan 清理。

## 3. 术语和数据模型

- **BaseRowLocator**：阶段一返回的物理定位信息，至少包含 table UUID、基线 snapshot、metadata hash、data file path、physical position、data sequence 和 partition。
- **DV**：针对一个 data file 的 deleted physical positions 的 Roaring bitmap。
- **DV state**：某 data file 在基线 snapshot 中的既有 DV、遗留 position delete 和可见性信息。
- **DV merge**：existing_dv ∪ applicable_position_deletes ∪ new_positions，结果只能生成一个 DV。
- **after-image**：UPDATE 后由 openGauss executor 计算出的完整新行。
- **row lineage**：v3 对新增行分配和更新行继承 _row_id、_last_updated_sequence_number 的规则；不能替代 BaseRowLocator。
- **Puffin blob descriptor**：delete manifest 中描述 Puffin 文件位置和范围的 entry。
- **orphan candidate**：已经写入但没有被成功发布 metadata 引用的 Puffin、data、manifest 或 metadata 文件。

### 3.1 DV manifest 和 Puffin 关系

每个 DV manifest entry 必须能唯一映射到一个 data file 和一个 Puffin blob：

~~~text
delete manifest entry
  file_path            = Puffin 文件路径
  file_format          = puffin
  content              = DELETION_VECTOR
  referenced_data_file = data file location
  content_offset       = blob 起始偏移
  content_size         = blob 长度
  record_count         = DV cardinality

Puffin deletion-vector-v1 blob
  properties.referenced-data-file = 同一个 data file location
  properties.cardinality          = record_count
  properties.snapshot-id          = -1
  properties.sequence-number      = -1
~~~

manifest 的 content_offset / content_size_in_bytes 必须与 Puffin footer 中的 blob 范围逐字节一致。不能只验证 bitmap 内容而忽略 offset、length、referenced file 和 cardinality。

## 4. 组件职责和禁止越界

| 组件 | 阶段二职责 | 禁止事项 |
| --- | --- | --- |
| Rust SDK | 读取旧 DV/position delete、合并位置、写 Puffin、生成 DeleteFile、维护 row lineage/sequence、准备 metadata | 不更新 Catalog 表，不绕过 RowDelta 校验 |
| Rust Bridge | 暴露稳定 C ABI，序列化 request/result，映射错误和 ownership | 不在 C 层拼装 Puffin/manifest，不直接更新 metadata_location |
| iceberg_delta | 按 _delta_op 折叠并按 data file 分组，编排 flush、watermark、重试 | 不伪造 DV descriptor，不把 SQL 条件当 locator |
| iceberg_fdw | format gate、mutation scan、ForeignModify、residual qual、RETURNING、把行级 locator 传给 overlay | 不执行对象存储写入，不把 row ID/业务键当物理位置 |
| Catalog | lock_table 取基线，commit_table 做唯一 metadata CAS，记录 snapshot | 不直接解析 Puffin，不把 prepare 候选当删除授权 |
| Devtest | SDK/Bridge/SQL/E2E 回归、metadata/manifest/Puffin 黑盒检查 | 不通过修改 expected 隐藏功能失败 |

## 5. 端到端执行模型

~~~text
SQL UPDATE / DELETE
  -> FDW mutation scan 产生真实行级 locator
  -> _delta overlay 记录 I/U/D
  -> 显式 flush 获取 Catalog base metadata/snapshot
  -> 按 referenced data file 分组新 positions
  -> SDK 读取旧 DV 和适用的 position deletes
  -> 合并 bitmap，写 deletion-vector-v1 Puffin
  -> UPDATE 同时写 after-image data file
  -> SDK RowDelta validation / metadata prepare
  -> Catalog metadata_location CAS
  -> CAS 成功后写 watermark、清理 overlay
  -> 外部 v3 reader 验证新 snapshot
~~~

### 5.1 DELETE

DELETE 只向目标 data file 的 DV 增加位置。目标位置已经在旧 DV 中时，合并结果保持幂等，不重复增加 cardinality。没有新位置时返回 no-op，不写 Puffin、不推进 snapshot。

### 5.2 UPDATE

UPDATE 必须在同一 RowDelta snapshot 中：

1. 把 base row position 合并到该 data file 的 DV；
2. 写一条完整 after-image data row；
3. 按 v3 row lineage 继承或生成字段；
4. 让新 snapshot 的读路径只看到 after-image。

不能先提交 DV、再单独提交 after-image；两者不在同一 snapshot 会造成短暂丢行或重复行。

### 5.3 连续 mutation 和 insert-origin

- base 行 UPDATE -> UPDATE -> DELETE：最终只需把 base position 放入 DV，不提交中间 after-image；
- INSERT 后 UPDATE：只提交最终 data row，不为不存在于 base data file 的行写 DV；
- INSERT 后 DELETE：不生成湖端 data file 或 DV；
- 混合 INSERT/U/D：按 _delta_op 分流，在一个 RowDelta 中同时提交合法分支。

## 6. 并发、校验、幂等和失败恢复

### 6.1 基线锚定

flush request 至少包含：

~~~json
{
  "format_version": 3,
  "delete_mode": "deletion-vector",
  "table_uuid": "...",
  "base_snapshot_id": 123,
  "base_metadata_location": "...",
  "targets_by_data_file": [
    {
      "path": ".../a.parquet",
      "positions": [7, 10],
      "partition_json": "{}"
    }
  ],
  "validation": {
    "validate_from_snapshot": true,
    "validate_data_files_exist": true,
    "validate_deleted_files": true,
    "validate_no_conflicting_data_files": true,
    "validate_no_conflicting_delete_files": true
  },
  "request_id": "..."
}
~~~

validate_from_snapshot=true 但缺少 base_snapshot_id 必须直接返回 InvalidArgument。request 中只能出现本批次新位置；FDW/Delta 不得传 Puffin offset、manifest entry 或伪造旧 DV。

### 6.2 CAS 和副作用边界

- 所有 out 指针在读取 metadata、写 Puffin/data/manifest 前检查；
- CAS 失败不删除任何物理文件，不清理 overlay/watermark；
- CAS 成功后允许 delete-after-CAS 或 orphan 清理失败，但必须返回 partial_failure 和可重试清单；
- 重试使用 request_id、base metadata 和已发布 snapshot 判断幂等，不能重复提交同一逻辑 mutation；
- 未发布 Puffin、data、manifest、metadata 都登记为 orphan candidate；
- 删除 data file 时同步移除其 DV entry，但不要求立即重写包含其他 blob 的 Puffin 文件。

### 6.3 冲突规则

在 prepare/commit 阶段至少校验：

- 基线 metadata location 未被其他 writer 改变；
- 被引用 data file 仍存在且内容未被不兼容 rewrite；
- 相关旧 delete file 未产生冲突；
- 同一 data file 的 DV 合并输入来自同一基线；
- partition/spec、sequence 和 table UUID 一致。

冲突必须返回可识别错误，保留 overlay，要求重新 scan 后重试，不得按业务键静默补偿。

## 7. SDK 接口要求

阶段二 SDK 接口按 Java RowDelta 语义对齐，但保持 Rust 所有权和错误模型：

~~~rust
pub struct ExistingDeleteState {
    pub deletion_vector: Option<DeletionVector>,
    pub position_deletes: Vec<PositionDeleteFile>,
}

pub fn read_existing_deletes_for_file(
    table: &Table,
    snapshot_id: i64,
    data_file_path: &str,
) -> Result<ExistingDeleteState>;

pub fn merge_delete_positions(
    existing: ExistingDeleteState,
    new_positions: impl IntoIterator<Item = u64>,
) -> Result<DeletionVector>;

pub async fn write_deletion_vector_puffin(
    table: &Table,
    data_file: &DataFile,
    vector: &DeletionVector,
) -> Result<DeleteFile>;

pub struct RowDeltaV3Request {
    pub base_snapshot_id: i64,
    pub base_metadata_location: String,
    pub deletes: Vec<DeleteFile>,
    pub adds: Vec<DataFile>,
    pub validate_from_snapshot: bool,
    pub validate_data_files_exist: bool,
    pub validate_deleted_files: bool,
    pub validate_no_conflicting_data_files: bool,
    pub validate_no_conflicting_delete_files: bool,
    pub request_id: String,
}

pub async fn commit_row_delta_v3(
    table: &Table,
    request: RowDeltaV3Request,
) -> Result<RowDeltaCommitResult>;
~~~

SDK 必须：

- 严格校验 v3 table format、DV schema、Puffin properties、cardinality 和正数 64 位位置；
- 合并旧 DV 与 position delete，确保每个 data file 最多一个 DV；
- 生成合法 delete manifest、snapshot summary、sequence 和 row lineage；
- 对空操作、重复位置、缺失 data file、错误 partition、错误 schema 返回确定性错误；
- 提供 reader/inspection API，供测试核对 bitmap、footer、manifest 和外部 snapshot。

## 8. Bridge C ABI 要求

推荐将阶段二 request/result 设计成版本化 JSON 输入、顶层 owned string 输出，保持阶段一 ownership 规则：

~~~c
IcebergBridgeStatus iceberg_bridge_table_row_delta_commit_v3(
    IcebergBridgeStorage *storage,
    const char *request_json,
    IcebergBridgeString **out_result,
    IcebergBridgeError **out_error);

void iceberg_bridge_string_free(IcebergBridgeString *value);
void iceberg_bridge_error_free(IcebergBridgeError *error);
~~~

要求：

- ABI 先校验所有 NULL 输出指针和 request 字段，再进入 SDK；
- delete_mode=deletion-vector 与 format_version=3 必须成对校验；
- Bridge 只接收 data file path、positions、partition 和 validation，不接收 Puffin descriptor；
- nested string 只能由约定的 result/batch free 函数释放，C 头、Rust 注释和测试一致；
- bridge 使用串行化 runtime 入口，避免调用方自行嵌套 block_on；
- status/error 映射不能丢失 InvalidArgument、Conflict、NotFound、Io 和 Unsupported 分类；
- C ABI test 覆盖 count=0、count=1、多 data file、NULL out、错误 JSON、重复 request_id 和错误 schema。

## 9. FDW、Delta 和 Catalog 集成

### 9.1 FDW

- planner 显式序列化 format version、table UUID、base snapshot、metadata hash、row locator resno 和 relation id；
- executor 只消费行级 data_file_path + physical_position，保留真实物理属性号；
- residual qual 在 openGauss 本地复核，不能关闭 filter 来绕过 locator 错误；
- v3 capability 不完整时在执行前失败，不产生 overlay 湖端副作用；
- RETURNING、NULL、新增列和 partition 信息必须沿 fdw_private -> overlay -> bridge 全链路可追踪。

### 9.2 iceberg_delta

- _delta_op 分流 I/D/U；
- 以 data file 为单位合并 positions，分区/spec 从 locator 和 metadata 得到；
- watermark 只在 Catalog CAS 成功后推进；
- CAS 前失败保留 overlay；CAS 后失败按 orphan/recovery 规则处理；
- v2 仍走 position-delete，v3 只走 DV，不能共享一个无版本判断的 writer。

### 9.3 Catalog

Catalog 是唯一 metadata pointer 发布者：

1. iceberg_catalog_lock_table 读取 table UUID、format version、base snapshot 和 metadata location；
2. Bridge/SDK prepare DV metadata；
3. iceberg_catalog_commit_table 以 table_uuid + old_metadata_location 执行 CAS；
4. CAS 成功后更新 snapshots、metadata location 和 current snapshot；
5. 仅成功后清理 overlay/watermark。

Catalog 不直接读取/写入 Puffin，不直接删除对象存储，不把 prepare 候选文件列表当作删除授权。当前 create_table 仍为 stub 的环境中，v3 fixture 必须由 SDK/Bridge 构造并显式注册。

## 10. 可观测性和结果规范

每次 flush/重试至少记录：

- request_id、table UUID、format version、delete mode；
- base/new/final metadata location；
- base/current snapshot、sequence；
- 每个 data file 的 old DV cardinality、new positions、merged cardinality；
- Puffin path、offset、size、CRC、referenced data file；
- manifest entry、partition/spec、data/delete sequence；
- Catalog CAS 行数、watermark、overlay 清理结果；
- orphan candidates 和可重试原因。

结果状态统一使用：

- ok
- no_op
- unsupported
- conflict
- request_error
- partial_failure

## 11. 阶段二完成定义

必须同时满足：

1. v1/v2/v3 gate 自动化通过，v3 不新增 position delete；
2. 单 data file、多个 data file、多个 row group、已有 DV、遗留 position delete 合并通过；
3. DELETE、UPDATE、连续 mutation、insert-origin 和混合 I/U/D 通过；
4. Puffin footer、blob properties、manifest offset/size/cardinality、partition、sequence 全部一致；
5. row lineage 继承和新行分配通过；
6. rollback/savepoint、跨会话、flush fault matrix、CAS 冲突和 orphan 恢复通过；
7. SQL、Catalog metadata、外部 Iceberg reader 三方结果一致；
8. clean build、C ABI ownership/错误路径、全量 devtest 和覆盖率通过；
9. 未实现的并发 rewrite、既有 delete file 等能力明确记录为 P2/SKIP，不得隐式宣称完成。

