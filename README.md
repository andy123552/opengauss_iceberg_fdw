# openGauss Iceberg FDW Project

The implementation checkout lives in the `iceberg_fdw/` git submodule, which
tracks `https://github.com/DataInfraLab/iceberg_fdw`.

The scan runtime also depends on the separate bridge project:
`https://github.com/DataInfraLab/iceberg-rust-bridge`. That bridge is a C ABI
translation layer only; it does not vendor the Iceberg Rust SDK. Build the
bridge and the SDK in their own workspace, then point `iceberg_fdw` at the
resulting shared library with `ICEBERG_RUST_BRIDGE_SO` or
`ICEBERG_RUST_BRIDGE_HOME`.

The local openGauss Docker environment is aligned with the
`https://github.com/DataInfraLab/openGauss-server-datainfra` baseline for
source-reference and interface compatibility. The container image itself is
started from `opengauss/opengauss-server:latest`; the DataInfraLab openGauss
repository is used as the code/reference baseline for the FDW integration work,
not as a local source build target.

For bridge-side smoke validation, use the dedicated file-based round-trip test
in that repository. It creates real `file://` metadata and Parquet fixtures in
a writable local directory, then exercises `table_load` and `scan_open`
directly against the filesystem backend.

## Docker 安装 openGauss

本项目默认使用 Docker 镜像运行 openGauss，不再要求在宿主机源码编译安装。
源码编译容易在普通开发机上触发 OOM，除非明确需要调试 openGauss 内核或扩展
编译链路，否则只使用本仓库随附的 Docker Compose 和工具脚本启动运行环境。

### 前置要求

- 已安装 Docker Engine。
- 已安装 Docker Compose v2，即 `docker compose version` 可正常输出版本。
- 当前用户可以执行 Docker 命令，或在命令前自行加 `sudo`。

### 仓库内随附工具

- `docker-compose.yml`：openGauss 容器定义、端口映射、数据目录和内存限制。
- `tools/opengauss-docker.sh`：常用 Docker 操作封装，包括启动、停止、状态、
  日志、连接验证和进入 `gsql`。

### 一键启动

```bash
cd /home/andy/opengauss_iceberg_fdw
./tools/opengauss-docker.sh up
```

### 查看状态

```bash
cd /home/andy/opengauss_iceberg_fdw
./tools/opengauss-docker.sh status
```

### 容器内连接验证

```bash
cd /home/andy/opengauss_iceberg_fdw
./tools/opengauss-docker.sh check
```

### 进入 gsql

```bash
cd /home/andy/opengauss_iceberg_fdw
./tools/opengauss-docker.sh gsql
```

### 本机连接信息

- Host: `127.0.0.1`
- Port: `15432`
- Container port: `5432`
- Container name: `opengauss-iceberg-fdw`
- Image: `opengauss/opengauss-server:latest`
- Password configured in Compose: `openGauss@123`

Docker Compose 已设置 `mem_limit: 2g` 和 `memswap_limit: 2g`，防止容器无上限占用宿主内存。

### 常用维护命令

```bash
# 查看容器日志
./tools/opengauss-docker.sh logs

# 停止容器，但保留 docker-data/opengauss 数据目录
./tools/opengauss-docker.sh down

# 重启容器
./tools/opengauss-docker.sh restart
```

容器数据挂载到 `docker-data/opengauss`，该目录被 `.gitignore` 忽略，不应提交。
如需彻底重建本地数据库，先停止容器，再手动清理该目录。

## Managed Iceberg Foreign Table 使用步骤

当前实现是本地绑定的 managed Iceberg foreign table。`iceberg_catalog`
负责维护本地元数据表，`iceberg_fdw` 在 `CREATE FOREIGN TABLE` 时把
openGauss 外表和这些元数据记录绑定起来。

### 1. Catalog 接入

先在数据库里安装 catalog 扩展：

```sql
CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
```

`iceberg_catalog` 提供并维护以下本地元数据表：

- `iceberg_catalog.namespaces`
- `iceberg_catalog.tables_internal`
- `iceberg_catalog.table_schemas`
- `iceberg_catalog.partition_specs`

### 2. 启动 Docker openGauss

```bash
cd /home/andy/opengauss_iceberg_fdw
./tools/opengauss-docker.sh up
./tools/opengauss-docker.sh check
```

### 3. 准备 Iceberg Rust SDK 与 bridge

桥接层和 Iceberg Rust SDK 是独立仓库，测试时需要同时准备：

- Iceberg Rust SDK: [apache/iceberg-rust](https://github.com/apache/iceberg-rust)
- bridge: [DataInfraLab/iceberg-rust-bridge](https://github.com/DataInfraLab/iceberg-rust-bridge)

推荐把两个仓库放在同一个父目录下，供 bridge 的 Cargo path 依赖使用：

```bash
mkdir -p /tmp/iceberg-stack
cd /tmp/iceberg-stack
git clone https://github.com/apache/iceberg-rust.git
git clone https://github.com/DataInfraLab/iceberg-rust-bridge.git

cd iceberg-rust
cargo build

cd ../iceberg-rust-bridge
cargo build
cargo test --test file_scan_roundtrip file_scan_roundtrip -- --nocapture
```

`iceberg-rust-bridge` 的文件扫描 smoke 会生成真实的 `file://`
warehouse、metadata 和 Parquet 文件，随后直接调用 bridge 的
`table_load` / `scan_open` 读取这些文件。若需要让 `iceberg_fdw`
在容器里加载这份 bridge，可以把桥库安装到容器内并设置：

```bash
export ICEBERG_RUST_BRIDGE_SO=/usr/local/opengauss/lib/postgresql/libiceberg_rust_bridge.so
# 或
export ICEBERG_RUST_BRIDGE_HOME=/tmp/iceberg-rust-bridge
```

### 4. 安装并重启

`iceberg_fdw` 的 managed DDL 依赖本地 `Catalog/` 扩展源码和它提供的
`iceberg_catalog` schema。`Catalog/` 是外部依赖源码，不 vendor 到 FDW
扩展中，来源仓库为 [HardingHang/Catalog](https://github.com/HardingHang/Catalog)。

```bash
cd /home/andy/opengauss_iceberg_fdw

rm -rf /tmp/iceberg_catalog_build
mkdir -p /tmp/iceberg_catalog_build
cp -a Catalog/. /tmp/iceberg_catalog_build/
docker cp /tmp/iceberg_catalog_build/. opengauss-iceberg-fdw:/tmp/iceberg_catalog_build/

docker exec opengauss-iceberg-fdw sh -lc '
  mkdir -p /tmp/ogsrc/src
  ln -sfn /tmp/openGauss-src-include /tmp/ogsrc/src/include
  cd /tmp/iceberg_catalog_build
  printf "\nexclude_option = -fPIE\noverride CPPFLAGS := \$(filter-out \$(exclude_option),\$(CPPFLAGS))\noverride CFLAGS := \$(filter-out \$(exclude_option),\$(CFLAGS))\noverride CXXFLAGS := \$(filter-out \$(exclude_option),\$(CXXFLAGS))\n" >> Makefile
  export GAUSSHOME=/usr/local/opengauss
  export LD_LIBRARY_PATH=/usr/local/opengauss/lib:$LD_LIBRARY_PATH
  export GAUSS_SRC=/tmp/ogsrc
  MAKEFLAGS=-j2 make PG_CONFIG=/usr/local/opengauss/bin/pg_config
  make install PG_CONFIG=/usr/local/opengauss/bin/pg_config
  cp /usr/local/opengauss/lib/postgresql/iceberg_catalog.so \
     /usr/local/opengauss/lib/postgresql/proc_srclib/iceberg_catalog
  chown omm:omm /usr/local/opengauss/lib/postgresql/proc_srclib/iceberg_catalog
'
```

### 5. 构建并安装 iceberg_fdw

```bash
cd /home/andy/opengauss_iceberg_fdw

rm -rf /tmp/iceberg_fdw_build
mkdir -p /tmp/iceberg_fdw_build
cp -a iceberg_fdw/. /tmp/iceberg_fdw_build/
docker cp /tmp/iceberg_fdw_build/. opengauss-iceberg-fdw:/tmp/iceberg_fdw_build/

docker exec opengauss-iceberg-fdw sh -lc '
  cd /tmp/iceberg_fdw_build
  export GAUSSHOME=/usr/local/opengauss
  export LD_LIBRARY_PATH=/usr/local/opengauss/lib:$LD_LIBRARY_PATH
  MAKEFLAGS=-j2 make PG_CONFIG=/usr/local/opengauss/bin/pg_config
  make install PG_CONFIG=/usr/local/opengauss/bin/pg_config
'

./tools/opengauss-docker.sh restart
```

重启是为了让 openGauss 后端重新加载新安装的扩展动态库。

### 6. SQL 配置与建表示例

进入 `gsql`：

```bash
cd /home/andy/opengauss_iceberg_fdw
./tools/opengauss-docker.sh gsql
```

执行以下 SQL：

```sql
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

CREATE SERVER iceberg_managed_srv
FOREIGN DATA WRAPPER iceberg_fdw
OPTIONS (
    warehouse 'file:///var/lib/opengauss/data/tmp/iceberg_fdw_demo'
);

CREATE FOREIGN TABLE managed_orders (
    order_id bigint NOT NULL,
    user_id integer,
    small_status smallint,
    status varchar(32),
    note text,
    code char(4)
)
SERVER iceberg_managed_srv
OPTIONS (
    namespace 'demo_ns',
    table_name 'orders'
);

SELECT namespace, table_name, table_uuid, metadata_location, table_location,
       current_schema_id, current_snapshot_id
FROM iceberg_catalog.tables_internal
WHERE relid = 'managed_orders'::regclass;

SELECT field_position, field_id, field_name, field_required, field_type
FROM iceberg_catalog.table_schemas
WHERE table_uuid = (
    SELECT table_uuid
    FROM iceberg_catalog.tables_internal
    WHERE relid = 'managed_orders'::regclass
)
ORDER BY field_position;

SELECT spec_id, field_position, field_id, source_id, field_name, transform
FROM iceberg_catalog.partition_specs
WHERE table_uuid = (
    SELECT table_uuid
    FROM iceberg_catalog.tables_internal
    WHERE relid = 'managed_orders'::regclass
)
ORDER BY spec_id, field_position;
```

`CREATE SERVER` 目前只需要 `warehouse`。本地 catalog 已通过
`iceberg_catalog` 扩展和 openGauss 绑定，不再额外暴露 `catalog_type`、
`catalog_uri` 这类外部 catalog 配置。

`CREATE FOREIGN TABLE` 仍然需要 `namespace` 和 `table_name`。当前实现支持
`int2`、`int4`、`int8`、`text`、`varchar`、`bpchar` 和固定维度 `vector(n)`。
不支持 `metadata_location` 或 `path`，因为这条路径不是外部只读绑定。
`DROP FOREIGN TABLE` 会同步清理对应的本地 `iceberg_catalog` 记录。

### 7. 构造测试数据并验证扫描

当前 FDW 的 scan 验证建议按下面顺序执行，保证其他开发者/agent 能复现同一套数据：

1. 准备本地 warehouse 目录

```bash
docker exec opengauss-iceberg-fdw bash -lc 'mkdir -p /var/lib/opengauss/data/tmp/iceberg_fdw_regress'
```

2. 使用 bridge 仓库里的 file fixture smoke 生成并验证 Iceberg 数据

```bash
cd /tmp/iceberg-stack/iceberg-rust-bridge
cargo test --test file_scan_roundtrip file_scan_roundtrip -- --nocapture
```

3. 在 openGauss 中创建 managed 表并指向同一目录

```sql
CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

CREATE SERVER iceberg_scan_srv
FOREIGN DATA WRAPPER iceberg_fdw
OPTIONS (
    warehouse 'file:///var/lib/opengauss/data/tmp/iceberg_fdw_regress'
);

CREATE FOREIGN TABLE managed_scan_table (
    c_smallint smallint,
    c_integer integer,
    c_bigint bigint,
    c_text text,
    c_varchar varchar(32),
    c_bpchar char(4),
    c_vector vector(3)
)
SERVER iceberg_scan_srv
OPTIONS (
    namespace 'scan_ns',
    table_name 'scan_table'
);
```

4. 验证全表扫描和谓词下推

```sql
SELECT * FROM managed_scan_table;

SELECT count(*) FROM managed_scan_table WHERE c_integer = 42;

SELECT count(*) FROM managed_scan_table WHERE c_integer = 42 AND c_text = 'iceberg text';

SELECT count(*) FROM managed_scan_table WHERE c_bpchar = 'ABCD';
```

5. 若要复现 bridge 侧单独验证，直接执行：

```bash
cd /tmp/iceberg-stack/iceberg-rust-bridge
cargo test --test file_scan_roundtrip file_scan_roundtrip -- --nocapture
```

### 8. 回归 SQL

当前镜像缺少 PGXS `pg_regress`，因此 `make installcheck` 不能直接运行。
可以用 `gsql` 手动执行回归 SQL：

```bash
docker exec opengauss-iceberg-fdw sh -lc '
  su - omm -c "gsql -d postgres -p 5432 -c \"DROP DATABASE IF EXISTS iceberg_fdw_regress;\""
  su - omm -c "gsql -d postgres -p 5432 -c \"CREATE DATABASE iceberg_fdw_regress;\""
  su - omm -c "gsql -d iceberg_fdw_regress -p 5432 -v ON_ERROR_STOP=1 -f /tmp/iceberg_fdw_build/sql/managed_ddl.sql"
  su - omm -c "gsql -d iceberg_fdw_regress -p 5432 -v ON_ERROR_STOP=1 -f /tmp/iceberg_fdw_build/sql/managed_scan.sql"
'
```

## 项目目标

通过 openGauss 的 FDW 能力连接 Iceberg 湖表，并补齐当前团队接口之外的 FDW 流程实现，使 openGauss 可以面向 Iceberg 表完成：

- 索引扫描能力
- 数据写入 DML 能力

## 本地源码参考

- openGauss 源码参考树：`openGauss-server/`
- 当前目标源码仓：`https://github.com/DataInfraLab/openGauss-server-datainfra`
- 源码仅用于后续 FDW 代码对比与接口核对，不作为默认编译目标。
- 源码仓克隆说明与当前访问状态见 `design/source-reference.md`。
- 关键参考文件：
  - `openGauss-server/src/include/foreign/fdwapi.h`
  - `openGauss-server/contrib/postgres_fdw/postgres_fdw.cpp`
  - `openGauss-server/contrib/postgres_fdw/connection.cpp`
  - `openGauss-server/contrib/postgres_fdw/option.cpp`
  - `openGauss-server/contrib/file_fdw/file_fdw.cpp`
  - `openGauss-server/src/gausskernel/optimizer/plan/createplan.cpp`
  - `openGauss-server/src/gausskernel/runtime/executor/nodeModifyTable.cpp`
  - `openGauss-server/src/gausskernel/optimizer/commands/copy.cpp`

## 已有团队能力

以下能力已由团队完成并提供接口，本项目实现时应调用这些接口，不重复实现底层机制：

- 元数据管理
  - Iceberg 表元数据访问
  - 表 schema、分区、快照、文件清单等元数据解析
  - openGauss FDW 侧需要的表级/列级映射信息

- 索引底层结构
  - 索引元数据
  - 索引文件/索引段结构
  - 索引查找接口

- 索引扫描执行
  - 根据查询条件调用 Iceberg 与索引相关接口
  - 获取满足条件的数据文件、行定位信息或扫描任务
  - 将底层扫描结果转换为 openGauss FDW 可返回的 tuple/slot

- 写入底层能力
  - DML 写入应落到 delta 表
  - delta 表写入、提交、回滚等能力已有对应接口实现

## 本项目待实现范围

本项目主要实现 openGauss FDW 侧流程 glue code 和执行链路接入。

### 当前代码目录

- `iceberg_fdw/`：FDW extension 基础代码仓目录，包含 `Makefile`、control、
  升级 SQL、handler/validator 入口和 planner/executor/DML callback 骨架。
- 当前骨架只做 FDW 装载、选项声明和生命周期占位；查询执行、索引扫描和 DML
  提交在团队 metadata/index/delta adapter 接口明确后接入。

### 1. FDW 对象与选项设计

- 设计 foreign server、foreign table、user mapping 需要的 options。
- 明确 Iceberg catalog、database、table、snapshot、分区、索引开关等配置入口。
- 校验 options 的合法性，并给出清晰错误信息。
- 记录 openGauss 类型与 Iceberg 类型之间的映射边界。

### 2. 元数据接入流程

- 在 FDW 初始化或规划阶段调用团队提供的元数据接口。
- 建立 foreign table 列信息与 Iceberg schema 的映射。
- 处理列裁剪、隐藏列、分区列、系统列等边界。
- 处理元数据缓存、失效、刷新策略。

### 3. 查询规划与代价估算

- 实现 FDW planner hooks。
- 根据查询条件判断是否可使用索引扫描。
- 将可下推条件拆分为：
  - Iceberg 元数据/分区裁剪条件
  - 索引可用条件
  - openGauss 本地 recheck 条件
- 估算索引扫描与普通扫描的 cost、rows、width。
- 在 explain 中暴露必要的 Iceberg/索引扫描信息。
- 普通扫描路径、catalog-only cost、计划生成和外表 DDL 约束详见
  `design/fdw-optimizer-ddl-detail-design.md`。

### 4. 索引扫描执行流程

- 在 BeginForeignScan 中初始化 Iceberg 表、索引、快照和扫描上下文。
- 在 IterateForeignScan 中调用 Iceberg 与索引扫描接口获取数据。
- 将扫描结果转换为 openGauss executor 所需 tuple/slot。
- 支持列裁剪、投影、NULL、类型转换、批量读取等细节。
- 在 ReScanForeignScan 中正确重置扫描状态。
- 在 EndForeignScan 中释放外部资源。

### 5. DML 写入流程

- 实现或补齐 FDW DML callbacks：
  - AddForeignUpdateTargets
  - PlanForeignModify
  - BeginForeignModify
  - ExecForeignInsert
  - ExecForeignUpdate
  - ExecForeignDelete
  - EndForeignModify
- 写入时调用团队提供的 delta 表写入接口。
- 明确 insert、update、delete 在 Iceberg/delta 表语义下的映射。
- 处理事务提交、回滚、错误恢复和幂等边界。
- 处理 returning、batch insert、约束检查、类型转换等能力边界。

### 6. 事务与一致性

- 对齐 openGauss 事务生命周期与 Iceberg/delta 提交生命周期。
- 明确快照隔离、并发写入冲突、失败回滚策略。
- 记录 DDL、DML、查询之间的可见性语义。

### 7. 测试计划

- FDW options 校验测试。
- Iceberg foreign table 建表与查询测试。
- 索引扫描路径选择测试。
- 索引扫描结果正确性测试。
- Insert/update/delete 写入 delta 表测试。
- 事务提交、回滚、异常中断测试。
- Explain 输出与 planner 行为测试。
- 与团队接口的 mock/integration 测试。

## 阶段性处理步骤

### Phase 0: 环境准备

- 安装 Docker Engine 和 Docker Compose v2。
- 使用本仓库 `docker-compose.yml` 拉取并启动 openGauss 镜像。
- 使用 `./tools/opengauss-docker.sh check` 验证容器内 `gsql` 连接。
- 记录镜像版本、端口、密码、数据目录和内存限制。
- 不默认编译 `openGauss-server/` 源码树；该目录只作为 FDW 接口和 contrib 示例参考。

### Phase 1: openGauss FDW 代码路径调研

- 梳理 openGauss FDW callback 定义。
- 找到已有 FDW 示例或 contrib 扩展作为参考。
- 明确 foreign scan、foreign modify、explain 的接入点。
- 记录需要修改或新增的文件清单。

### Phase 2: Iceberg FDW 骨架

- 新增 FDW extension 骨架。
- 支持 create extension、create server、create foreign table。
- 完成 options 解析与元数据接口占位接入。

### Phase 3: 索引扫描接入

- 实现 planner 条件识别。
- 接入团队索引扫描接口。
- 完成 tuple materialization。
- 增加 explain 与测试。

### Phase 4: DML 写入接入

- 实现 FDW DML callback 链路。
- 接入 delta 表写入接口。
- 补齐事务生命周期处理。
- 增加 DML 正确性与异常测试。

### Phase 5: 联调与收敛

- 与团队元数据、索引、delta 写入接口联调。
- 补齐性能测试与回归测试。
- 梳理限制项、已知问题和上线前 checklist。

## 待补充约束

- 团队提供接口的头文件、库文件、调用约定。
- Iceberg catalog 类型和认证方式。
- 索引类型、支持的谓词和返回数据结构。
- delta 表写入语义和事务接口。
- FDW 侧实现必须保持独立可插拔：通过 catalog/index/delta/type adapter
  对接外部模块，FDW planner/executor/DML 主流程不得直接依赖具体 Iceberg
  catalog、索引或 delta writer 实现。
- 具体 adapter 需要声明能力集，例如索引谓词支持范围、UPDATE/DELETE
  支持方式、position/equality delete、批量写入、提交幂等等，FDW 根据能力
  选择路径或提前拒绝不支持的 SQL。
- 详细接口边界见 `design/iceberg-fdw-interface-design.md`。
- 是否要求支持 update/delete，还是首期只支持 insert。
- 是否要求谓词下推、聚合下推、limit 下推。
- openGauss 目标版本、分支、编译环境。
- 测试环境中的 Iceberg catalog 和样例表。
