# openGauss Iceberg FDW：SDK 接口与下推参数

本文配合《5. Iceberg_FDW 详细设计》§4.3，集中列出 FDW 经 C 桥接层调用取数 SDK 的接口参数，以及 SDK 内部行列下推等自动能力所依赖的参数。结论：**行列下推所需参数与 `open` 接口一致，由 `open` 一次传入、SDK 内部自动施加，无需额外接口或单独传参**。

## 1. SDK 接口

桥接层对 FDW 暴露 `open` / `next` / `close` 三接口（签名见 fdw-iceberg-sdk-arrow_design.md §3.2.1）。`open`、`next` 均以入参发起，结果经返回值与出参带回。表身份不下传——FDW 规划期已由 Catalog 把 `(namespace, table_name)` 解析为完整 `metadata_location`，`open` 传入的是该路径。

| 接口 | 方向 | 参数 / 返回（FDW 经桥接层传下 · SDK 返回） |
| --- | --- | --- |
| `iceberg_scan_open` | 入 | `cxt` 内存上下文 · `metadata_path` 完整 `metadata_location`（规划期 Catalog 解析，非表身份）· `storage_config` 凭证 · `columns`/`n_columns` 投影列名（列裁剪）· `filter` 下推谓词串，可空（谓词裁剪，见 §2） |
| `iceberg_scan_open` | 回 | `IcebergScan*` reader 指针 + 出参 `out_schema`（`ArrowSchema*`：列名/类型，建 converters） |
| `iceberg_scan_next` | 入 | `scan` reader 指针 |
| `iceberg_scan_next` | 回 | `int nrows`（0 = 结束）+ 出参 `out_array`（`ArrowArray*` 本批列数据）+ `out_schema`（`ArrowSchema*`） |
| `iceberg_scan_close` | 入 | `scan` reader 指针 |
| `iceberg_scan_close` | 回 | 无返回值；释放 reader 与扫描资源 |

## 2. SDK 内部能力函数与参数

列裁剪与谓词裁剪是 SDK 在 `open`/`next` 内部自动完成的能力，不另设接口。其参数全部来自 `open` 的入参（`columns`、`filter`），FDW 一次传入即可，运行期无需再传。

| 能力 | SDK 内部调用 | 参数 | 参数来源 |
| --- | --- | --- | --- |
| 列裁剪 | `TableScan.select(Collection<String>)`（Scan.java:104） | 投影列名集合 | = `open` 的 `columns` |
| 谓词绑定 | `TableScan.filter(Expression)`（Scan.java:123） | Iceberg `Expression` | = `open` 的 `filter`，桥接/SDK 侧反解析为 `Expression` |
| 分区剪枝 | `scan.planFiles()` 内 `ManifestEvaluator` / `ResidualEvaluator` | — | 自动（`filter` 已绑定） |
| 列统计剪枝 | `scan.planFiles()` 内 `InclusiveMetricsEvaluator` | — | 自动（`filter` 已绑定） |
| 行组剪枝 | `VectorizedParquetReader` 经 `ReadConf.shouldSkip[]` | — | 自动（`filter` 已绑定） |
| 向量化批读 | `VectorizedTableScanIterable(TableScan, batchSize)`（VectorizedTableScanIterable.java:54） | 已配 select/filter 的 scan + 批大小 | scan 来自上述 select/filter；`batchSize` 为 SDK 内部默认，决定 `next` 单批行数 |

调用链：`open` 内 `table.newScan().select(columns).filter(expr)` 得到配好的 `TableScan`，交给 `VectorizedTableScanIterable`；`next` 每次推进迭代器取一批 Arrow。分区 / 列统计剪枝在 `planFiles()` 内、行组剪枝在向量化读取器内，均随 `filter` 自动生效。

**边界提示**：向量化快路径的 `filter` 只做 I/O 裁剪（manifest / 文件 / row-group 跳过），不做行级精确过滤；故 FDW 仍须把同一谓词保留为 `ForeignScan` 节点 qual 复核（详见 5.md §3.3、§3.5）。delete file（MOR）本期不支持。
