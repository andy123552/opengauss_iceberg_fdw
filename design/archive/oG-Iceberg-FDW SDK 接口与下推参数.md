# openGauss Iceberg FDW：SDK 接口与下推参数（Iceberg-rust 版）

本文配合《5. Iceberg_FDW 详细设计》§4.3，集中列出 FDW 经 C 桥接层调用取数 SDK（**iceberg-rust**）的接口参数，以及 SDK 内部行列下推等自动能力所依赖的参数。结论：**行列下推所需参数与 `open` 接口一致，由 `open` 一次传入、SDK 内部自动施加，无需额外接口或单独传参**。与 Java 版的关键不同在于：rust 取 Arrow 仅 `to_arrow()` 一条**列式**路径，行级精确过滤与 MOR 在该路径中**默认实现**（详见 §3、§4）。

> 源码版本：`catalog-service-research/src_ref/iceberg-rust`，commit `427670d5`（2026-06-12），锚点相对 `crates/iceberg/src/`。能力核实详见《Iceberg-SDK-能力核实（临时）.md》。

## 1. SDK 接口

桥接层对 FDW 暴露 `open` / `next` / `close` 三接口（C ABI 签名见《5. Iceberg_FDW 详细设计》§4.3 / fdw-iceberg-sdk-arrow_design.md §3.2.1，接口形态不随 SDK 由 Java 改 rust 而变）。`open`、`next` 均以入参发起，结果经返回值与出参带回。表身份不下传——FDW 规划期已由 Catalog 把 `(namespace, table_name)` 解析为完整 `metadata_location`，`open` 传入的是该路径。

| 接口 | 方向 | 参数 / 返回（FDW 经桥接层传下 · SDK 返回） |
| --- | --- | --- |
| `iceberg_scan_open` | 入 | `cxt` 内存上下文 · `metadata_path` 完整 `metadata_location`（规划期 Catalog 解析，非表身份）· `storage_config` 凭证 · `columns`/`n_columns` 投影**列名**（列裁剪，**仅列名字符串、非 field_id**，见 §2）· `filter` 下推谓词串，可空（谓词裁剪，见 §2） |
| `iceberg_scan_open` | 回 | `IcebergScan*` reader 指针 + 出参 `out_schema`（`ArrowSchema*`：列名/类型，建 converters） |
| `iceberg_scan_next` | 入 | `scan` reader 指针 |
| `iceberg_scan_next` | 回 | `int nrows`（0 = 结束）+ 出参 `out_array`（`ArrowArray*` 本批列数据）+ `out_schema`（`ArrowSchema*`） |
| `iceberg_scan_close` | 入 | `scan` reader 指针 |
| `iceberg_scan_close` | 回 | 无返回值；释放 reader 与扫描资源 |

## 2. SDK 内部能力函数与参数

列裁剪与谓词裁剪是 SDK 在 `open`/`next` 内部自动完成的能力，不另设接口。其参数全部来自 `open` 的入参（`columns`、`filter`），FDW 一次传入即可，运行期无需再传。rust 比 Java 多出两项**默认生效**的能力——行级精确过滤、MOR——同样无须额外参数（下表末两行）。

| 能力 | SDK 内部调用 | 参数 | 参数来源 |
| --- | --- | --- | --- |
| 列裁剪 | `TableScanBuilder::select(impl IntoIterator<Item = impl ToString>)`（scan/mod.rs:119）；`build()` 内按名解析为 field_id（`field_id_by_name`，scan/mod.rs:256），下游 `ProjectionMask` 按 field_id 投影（arrow/reader/projection.rs:87） | 投影**列名**集合（`Vec<String>`） | = `open` 的 `columns` |
| 谓词绑定 | `TableScanBuilder::with_filter(Predicate)`（scan/mod.rs:99） | iceberg-rust `Predicate`（表达式对象，非 SQL 串） | = `open` 的 `filter`，桥接侧构造为 `Predicate` |
| 分区剪枝 | `plan_files()` 内 `ManifestEvaluator`（scan/context.rs:225）/ 文件级 `ExpressionEvaluator`（scan/mod.rs:524） | — | 自动（`filter` 已绑定） |
| 列统计剪枝 | `plan_files()` 内 `InclusiveMetricsEvaluator::eval`（scan/mod.rs:529） | — | 自动（`filter` 已绑定） |
| 行组剪枝 | `to_arrow()` 内 `RowGroupMetricsEvaluator`（`get_selected_row_group_indices`，arrow/reader/pipeline.rs:326）；**默认开启**（scan/mod.rs:80） | — | 自动（`filter` 已绑定） |
| 行级精确过滤 | `to_arrow()` 内 Arrow `RowFilter`（`get_row_filter`，arrow/reader/row_filter.rs:41；挂载 pipeline.rs:323）；**默认生效** | — | 自动（`filter` 残差，逐行求值） |
| MOR（delete 应用） | `to_arrow()` 内：位置删除 → `RowSelection`（pipeline.rs:359）；等值删除 → 转 `Predicate` 与 filter 取 AND（pipeline.rs:265）；**默认生效** | — | 自动（`FileScanTask.deletes`，规划期归并） |
| 列式批读 | `ArrowReaderBuilder` → `ParquetRecordBatchStreamBuilder`（arrow/reader/pipeline.rs:211），`to_arrow()` 返回 `ArrowRecordBatchStream`（scan/mod.rs:464） | 已配 select/filter 的 scan + 批大小 | scan 来自上述 select/filter；`batch_size` 为 SDK 默认或经 `with_batch_size` 设定，决定 `next` 单批行数 |

调用链：`open` 内 `StaticTable::from_metadata_file(metadata_path, …)`（table.rs:364）→ `scan().select(columns).with_filter(predicate).build()` 得到配好的 `TableScan` → `to_arrow()` 得 `ArrowRecordBatchStream`；`next` 每次推进该异步流取一批 `RecordBatch`（即一批 Arrow）。分区 / 列统计剪枝在 `plan_files()` 内、行组剪枝 / 行级过滤 / MOR 在 `to_arrow()` 的列式读取器内，均随 `filter`（及 `deletes`）自动生效。

## 3. 投影按列名而非 field_id（FDW 须注意）

`select` 仅接收**列名字符串**，SDK 在 `build()` 内即时按名解析为 field_id（`field_id_by_name`，scan/mod.rs:256），其后所有投影（`ProjectionMask`）均按 field_id 进行。即 field_id 仅为 SDK **内部**表示，**公开接口不接收 field_id**。由此：

- FDW 即便以 field_id 维系列身份（规避列改名问题），调用前仍须先经 schema 把 field_id 映射为**当前快照的列名**再传入 `columns`，无法直接下传 field_id。
- `select` 仅支持**顶层标量列**；引用嵌套字段（struct 子列）在 `build()` 报错（scan/mod.rs:266）。投影列名须为表 schema 顶层字段名，且存在性在 `build()` 校验（不存在即报错，scan/mod.rs:230）。

## 4. 边界提示（与 Java 版的实质差异）

rust 取 Arrow 仅 `to_arrow()` 一条列式路径（对应 Java 的"向量化快路径"，均为列式 Parquet→Arrow），但该路径较 Java 快路径多做两事：

1. **下推谓词在 rust 是行级精确过滤，非仅 I/O 裁剪。** `filter` 经 `RowFilter` 逐行生效（pipeline.rs:323），故 Java 版"向量化路径只做 manifest/文件/row-group 跳过、须把同一谓词保留为 `ForeignScan` 节点 qual 由 `ExecQual` 兜底"的**必选**要求，对 rust 已成功转为 `Predicate` 并下推的顶层标量谓词，就正确性而言不再必需。是否保留节点 qual 复核作为防御层属设计取舍。
2. **MOR 默认支持。** 含 delete 的表无须 Java 的"delete 探测分路 / 行式回退"，位置与等值删除在 `to_arrow()` 中默认应用（pipeline.rs:359/265）。即 rust 不存在 Java"要 Arrow 就拿不到 MOR"的耦合；本期范围如限定无 delete 表，可不依赖此能力，但不必为此分路。
3. **桥接层无法翻译为 `Predicate` 的谓词**（如非顶层标量、超出转换器支持的形态）本就留作 `local_exprs`，由执行器逐行兜底；此与 Java 一致。
