# openGauss-Iceberg 类型与操作符对齐知识整理

## 1. 目标

本文整理实现 openGauss Iceberg FDW 类型转换、索引谓词下推和向量近邻查询时需要掌握的背景知识。当前优先覆盖：

- `int`/`long` 整数族
- `char`/字符串类
- `vector`

范围包含 openGauss 类型系统、Iceberg 类型系统、索引系统需要的类型与操作符语义，以及可参考的 LanceDB 设计逻辑。

本文是知识地图，不直接规定实现接口；接口方案见 `design/type-operator-conversion-design.md`。

## 2. openGauss 类型系统要点

### 2.1 Datum 与系统目录

openGauss 扩展侧所有值以 `Datum` 承载，`Datum` 是机器字大小的 `uintptr_t`。固定宽度且 pass-by-value 的类型直接编码在 `Datum` 中，变长类型和较大类型通过指针传递。

实现类型转换时必须从系统目录或 `TupleDesc` 读取这些元数据：

| 元数据 | 来源 | 用途 |
| --- | --- | --- |
| `atttypid` | `pg_attribute` | openGauss 列类型 OID |
| `atttypmod` | `pg_attribute` | 类型修饰符，例如 `varchar(n)`、`char(n)`、`vector(n)` |
| `attcollation` | `pg_attribute` | 字符串比较、排序和谓词下推安全性 |
| `typlen` | `pg_type` | 定长/变长布局判断 |
| `typbyval` | `pg_type` | Datum 复制和释放策略 |
| `typalign` | `pg_type` | 构造 tuple 或 array 时的对齐要求 |
| `typstorage` | `pg_type` | 是否可能 toast/detoast |

本地参考文件：

- `openGauss-server/src/include/postgres.h`
- `openGauss-server/src/include/catalog/pg_type.h`
- `openGauss-server/src/include/catalog/pg_attribute.h`
- `openGauss-server/src/include/utils/lsyscache.h`
- `openGauss-server/contrib/postgres_fdw/postgres_fdw.cpp`
- `openGauss-server/contrib/file_fdw/file_fdw.cpp`

### 2.2 首期目标类型 OID

| openGauss 类型 | OID 宏 | OID | 说明 |
| --- | --- | ---: | --- |
| `"char"` | `CHAROID` | 18 | 单字节内部类型，不等同 SQL `char(n)` |
| `int8` / `bigint` | `INT8OID` | 20 | 64 位有符号整数 |
| `int2` / `smallint` | `INT2OID` | 21 | 16 位有符号整数 |
| `int4` / `integer` | `INT4OID` | 23 | 32 位有符号整数 |
| `float4` | `FLOAT4OID` | 700 | vector 元素使用 float32 语义 |
| `float8` | `FLOAT8OID` | 701 | 距离函数返回常见为 float8 |
| `bpchar` / `char(n)` | `BPCHAROID` | 1042 | 定长 blank-padded 字符串 |
| `varchar` | `VARCHAROID` | 1043 | 变长字符串 |
| `vector` | `VECTOROID` | 8305 | openGauss datavec 向量类型 |
| `_vector` | `VECTORARRAYOID` | 8308 | vector 数组类型 |

注意：

- 用户说的 `char` 如果来自 SQL DDL，通常是 `char(n)`，在系统目录里是 `BPCHAROID`，不是 `CHAROID`。
- `CHAROID=18` 多用于系统内部单字节类型，首期不建议作为 Iceberg 字符串主路径。
- `vector` 是 openGauss 当前源码中已有类型，内部是 varlena，字段为 `dim` 加 float 数组。

### 2.3 int / long 整数族

openGauss 的常用整数类型有 `int2`、`int4`、`int8`。Iceberg 对应的整数 primitive 有 `int` 和 `long`：

| openGauss | Iceberg | 策略 |
| --- | --- | --- |
| `int2` | `int` | 安全提升，可直接支持 |
| `int4` | `int` | 精确匹配 |
| `int8` | `long` | 精确匹配 |
| `int2` | `long` | 安全提升，可支持 |
| `int4` | `long` | 安全提升，可支持 |
| `int8` | `int` | 范围收缩，默认拒绝；只有显式 cast 且常量/值在 int32 范围内才可转换 |
| Iceberg `long` -> openGauss `int4`/`int2` | 范围收缩，默认拒绝 |

Datum 转换入口：

- `int2`：使用 `DatumGetInt16` / `Int16GetDatum`。
- `int4`：使用 `DatumGetInt32` / `Int32GetDatum`。
- `int8`：使用 `DatumGetInt64` / `Int64GetDatum` 或同等 openGauss 宏/函数。

谓词和索引下推原则：

- 精确匹配和安全提升可以强下推，前提是索引系统声明列的实际 Iceberg 类型。
- 范围收缩默认不下推，避免 openGauss 侧可表达的值在 Iceberg `int` 中溢出。
- 常量谓词如 `bigint_col = 1::int4` 可以在解析后规范化为 Iceberg `long` 常量下推。
- `smallint_col = 1` 映射到 Iceberg `int` 时，下推值按 int32 编码，但读回 Datum 仍按 openGauss `int2` 构造并校验范围。

### 2.4 char / 字符串

openGauss 至少有三类容易混淆的字符类型：

| 类型 | OID | 语义 |
| --- | ---: | --- |
| `"char"` | 18 | 单字节，不代表文本字符串 |
| `bpchar` / `char(n)` | 1042 | 定长字符，比较时有尾部空格语义 |
| `varchar` | 1043 | 变长字符串，typmod 可限制长度 |

FDW 转换和谓词下推需要关注：

- `typmod`：`char(n)`/`varchar(n)` 的长度限制是 openGauss 约束，Iceberg `string` 不携带该长度。
- `collation`：Iceberg 字符串比较通常不是 openGauss locale collation。只在语义可证明一致时下推排序/范围谓词。
- `bpchar` 尾部空格：`char(n)` 的等值比较语义与原始字节比较不同。下推给索引系统前必须确定索引端是否做同样规范化，否则只能下推为候选并在 openGauss 本地 recheck。
- 编码：应统一按 UTF-8 字节串处理；若数据库编码不是 UTF-8，转换层必须显式转码或拒绝下推。

首期建议：

- 业务字符串主路径支持 `BPCHAROID`、`VARCHAROID`，后续补 `TEXTOID`。
- `CHAROID` 仅按单字节类型处理，不映射到 Iceberg `string`，除非用户显式声明兼容模式。
- 字符串索引仅安全下推 `=`、`IS NULL`；`<`、`<=`、`>`、`>=` 必须要求 binary/C collation 且索引系统声明同序。

### 2.5 vector

openGauss `vector` 在 DataInfraLab openGauss 源码中是 datavec 类型：

```c
typedef struct Vector {
    int32 vl_len_;
    int16 dim;
    uint16 isoValue;
    float x[FLEXIBLE_ARRAY_MEMBER];
} Vector;
```

关键约束：

- 维度范围：`1..16000`。
- 元素类型：float32。
- 输入不允许 `NaN` 和 `Inf`。
- `vector(n)` 的 `typmod` 表示期望维度。
- 两个 vector 运算前必须维度一致。

openGauss 当前向量操作符和距离函数：

| SQL 操作符 | 函数 | 语义 |
| --- | --- | --- |
| `<->` | `l2_distance(vector, vector)` | L2/Euclidean distance，返回 `sqrt(sum(diff^2))` |
| `<#>` | `vector_negative_inner_product(vector, vector)` | 负内积；用于 `ORDER BY ASC` 表达最大内积相似 |
| `<=>` | `cosine_distance(vector, vector)` | `1 - cosine_similarity` |
| `<+>` | `l1_distance(vector, vector)` | L1/Manhattan distance |

索引 opclass 侧：

- `vector_l2_ops` 使用 `<->`，support function 里常用 squared L2 以省掉 `sqrt`。
- `vector_ip_ops` 使用 `<#>`。
- `vector_cosine_ops` 使用 `<=>`，support function 可能使用负内积和 norm。
- `vector_l1_ops` 使用 `<+>`。
- openGauss 目前存在 `hnsw`、`ivfflat`、`diskann`、`gv_graph` 等向量索引路径。

## 3. Iceberg 类型系统要点

Iceberg 表 schema 以 field id 作为列身份，列名和列顺序可以演进。读数据和投影必须按 field id 对齐，而不是只按列名。

Iceberg 规范中与首期相关的类型：

| Iceberg 类型 | 说明 | 首期映射 |
| --- | --- | --- |
| `int` | 32 位整数 | openGauss `int2`/`int4`，`int2` 为安全子集 |
| `long` | 64 位整数 | openGauss `int8`，也可承载 `int2`/`int4` 安全提升 |
| `string` | UTF-8 字符串 | openGauss `varchar`/`bpchar`，有 recheck 约束 |
| `float` | 32 位浮点 | vector 元素 |
| `list` | 带 element field id 的列表 | vector 的推荐物理表示 |

Iceberg 没有标准 primitive `vector`。可选表示：

| 表示 | 优点 | 风险 |
| --- | --- | --- |
| `list<float>` | 符合 Iceberg 标准，便于跨引擎读取 | 不天然表达固定维度 |
| `fixed[N]` / `binary` | 存储紧凑，可约定 float32 little-endian | 跨引擎语义弱，容易出现端序/长度错误 |
| `list<double>` | 兼容部分生成端 | openGauss vector 是 float32，写回会有精度变化 |

首期建议用 `list<float>` 作为 Iceberg 逻辑类型，并在表/列 property 或索引 metadata 中保存固定维度：

```text
iceberg type: list<float>
column property: vector.dimension = N
column property: vector.element_type = float32
```

## 4. 索引系统与 LanceDB 参考逻辑

LanceDB 的向量检索思路可抽象为：

- 向量列使用固定维度的数组/list 表示。
- 距离度量是索引定义的一部分，常见为 `l2`、`cosine`、`dot`。
- 查询时 `query vector + metric + k/limit + filter` 共同决定索引路径。
- 标量过滤可以在 vector search 前 prefilter，也可以在候选结果后 postfilter。
- ANN 索引可能近似，最终结果可做 refine/recheck。

对应到本 FDW：

| LanceDB 概念 | FDW / 索引系统概念 |
| --- | --- |
| vector column | Iceberg `list<float>` + fixed dimension metadata |
| metric `l2` | openGauss `<->` / `vector_l2_ops` |
| metric `cosine` | openGauss `<=>` / `vector_cosine_ops` |
| metric `dot` | openGauss `<#>` / `vector_ip_ops`，注意 openGauss 用负内积做 ASC 排序 |
| scalar index/filter | Iceberg 底层索引的 int/long/string 谓词 |
| prefilter | 先用标量谓词缩小向量搜索空间 |
| postfilter/refine | 向量索引返回候选后由 FDW/openGauss recheck |

与 LanceDB 对齐时最重要的是分清“距离”和“相似度”：

- openGauss planner 的 `ORDER BY embedding <-> query LIMIT k` 是距离升序。
- 最大内积相似要用负内积 `<#>` 升序，对外传给索引系统时 metric 应标记为 `dot` 或 `inner_product`，并带上 `score_direction=max_similarity` 或 `openGauss_operator_returns_negative=true`。
- cosine 必须处理零向量。openGauss `cosine_distance` 对零向量会产生不可用值；LanceDB 文档也提示 cosine 对零向量无定义。索引下推应拒绝或过滤零向量，不能把零向量结果当成正常距离。

## 5. 操作符系统要点

openGauss 操作符由 `pg_operator` 定义，至少包含：

- 操作符名。
- 左右参数类型。
- 底层函数 `oprcode`。

索引可用性由 opfamily/opclass 进一步定义：

- `pg_opclass` 声明某访问方法和类型的默认/非默认 opclass。
- `pg_opfamily` 声明一组相关操作符和 support function。
- `pg_amop` 把操作符映射到 opfamily 的 strategy number。
- btree 风格 strategy number 中，`<` 是 1，`=` 是 3，`>` 是 5。

FDW 不能只看操作符名字来下推，必须至少匹配：

```text
operator oid + left type oid + right type oid + collation + expression shape
```

首期可下推形态：

| SQL 形态 | 下推条件 |
| --- | --- |
| `col = const` | int 直接下推；字符串需 collation/recheck 策略 |
| `col < const` 等范围 | int 直接下推；字符串默认不下推或只候选下推 |
| `col IS NULL` | 下推到 null bitmap/statistics/index |
| `ORDER BY vector_col <-> const LIMIT k` | 下推为 vector top-k，metric=l2 |
| `ORDER BY vector_col <#> const LIMIT k` | 下推为 vector top-k，metric=dot/max inner product |
| `ORDER BY vector_col <=> const LIMIT k` | 下推为 vector top-k，metric=cosine |

## 6. 准确性原则

### 6.1 类型准确性

- 所有列映射以 Iceberg field id 为主键。
- openGauss `typmod` 必须参与校验，尤其是 `bpchar/varchar/vector`。
- vector 必须校验维度、有限浮点、元素精度。
- 字符串必须记录编码、collation 和尾部空格语义。

### 6.2 谓词准确性

- 精确语义可证明一致时才做“强下推”。
- 语义可能不一致但能缩小候选时，只做“候选下推”，并保留 openGauss local recheck。
- ANN/vector 索引默认视为候选路径；如果索引系统返回近似 top-k，FDW 需要明确 explain 输出，并按能力决定是否做 refine。

### 6.3 NULL 与错误

- NULL 不进入普通比较和 vector distance。
- `IS NULL`/`IS NOT NULL` 单独建 predicate。
- vector 中元素不允许 NULL；Iceberg `list<float>` 若存在 null element，读入 openGauss vector 应报错或按列策略跳过，不能静默置 0。
- 对类型不兼容、维度不一致、NaN/Inf、字符串编码失败，应在 FDW 转换边界报明确错误。

## 7. 参考资料

本地源码：

- `openGauss-server/src/include/postgres.h`
- `openGauss-server/src/include/catalog/pg_type.h`
- `openGauss-server/src/include/catalog/pg_operator.h`
- `openGauss-server/src/include/catalog/pg_opclass.h`
- `openGauss-server/src/include/catalog/pg_opfamily.h`
- `openGauss-server/src/include/catalog/pg_amop.h`
- `openGauss-server/src/include/access/datavec/vector.h`
- `openGauss-server/src/common/backend/utils/adt/vector.cpp`
- `openGauss-server/src/include/catalog/upgrade_sql/upgrade_catalog_maindb/upgrade-post_catalog_maindb_93_019.sql`

外部资料：

- Apache Iceberg Specification: https://iceberg.apache.org/spec/
- LanceDB Vector Search: https://docs.lancedb.com/search/vector-search
- LanceDB Indexing: https://docs.lancedb.com/indexing
- LanceDB Vector Indexes: https://docs.lancedb.com/indexing/vector-index
