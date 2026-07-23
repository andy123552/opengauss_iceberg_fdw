# DataInfra 构建仓架构与依赖说明

> 依据：`DataInfraLab/data_infra` 的 `main` 分支（核对提交：`8465543749d9ef0f5e9bb5e97880314d925c2132`）。
>
> 定位：`data_infra` 是一个 **meta repository（集成/构建母仓）**。它自身不承载 openGauss、Iceberg SDK 或各插件的主体实现，而是通过 Git submodule 固定一组可共同构建、测试的子仓提交，并提供统一构建、安装和回归测试入口。

## 1. 总体结构

```text
data_infra（母仓：版本组合、构建脚本、集成文档）
│
├─ opengauss/
│  └─ openGauss-server-datainfra       数据库内核与扩展构建宿主
│
├─ deps/                               Rust 与 C++ 依赖层
│  ├─ iceberg-rust-datainfra           Iceberg Rust SDK
│  ├─ iceberg-index                    索引实现及其 ABI
│  ├─ iceberg-rust-cache               缓存实现
│  ├─ iceberg-rust-bridge              C ABI bridge，连接插件与 Rust 能力
│  └─ iceberg-arrow-deps               Apache Arrow C++ 构建输入
│
├─ plugins/                            openGauss 扩展层
│  ├─ iceberg_fdw                      读路径：通过 FDW 访问 Iceberg 表
│  ├─ openGauss-Catalog                元数据/Catalog 扩展
│  └─ iceberg_delta                    写路径：Delta/MOR 扩展
│
├─ test/
│  └─ DataInfra-devtest                SQL 回归、端到端和覆盖率用例
│
├─ build/                              环境准备、编译、安装与产物校验
└─ docs/                               集成设计、模板和协作说明
```

## 2. 代码仓清单与职责

下表中的“跟踪分支”来自母仓 `.gitmodules`；真正可复现的版本以 `data_infra/main` 中记录的 **gitlink 提交** 为准。

| 层级 | 母仓路径 | 仓库 | 跟踪分支 | 当前 gitlink | 作用 |
|---|---|---|---|---|---|
| 母仓 | `/` | [DataInfraLab/data_infra](https://github.com/DataInfraLab/data_infra) | `main` | `8465543` | 固定子仓组合；提供构建、安装、验证脚本和集成文档。 |
| 数据库内核 | `opengauss/openGauss-server-datainfra` | [DataInfraLab/openGauss-server-datainfra](https://github.com/DataInfraLab/openGauss-server-datainfra) | `datainfra_dev` | `b871af5` | DataInfra 定制的 openGauss 源码；构建出 `gaussdb`、`gsql`、`pg_config` 和扩展安装目录。 |
| Iceberg SDK | `deps/iceberg-rust-datainfra` | [DataInfraLab/iceberg-rust-datainfra](https://github.com/DataInfraLab/iceberg-rust-datainfra) | `main` | `c5adf7b` | Iceberg Rust SDK 本体及 REST catalog、Opendal storage 等能力。 |
| 索引 | `deps/iceberg-index` | [DataInfraLab/iceberg-index](https://github.com/DataInfraLab/iceberg-index) | `main` | `ede582a` | Iceberg 索引实现；向 bridge 暴露共享 index ABI。 |
| 缓存 | `deps/iceberg-rust-cache` | [DataInfraLab/iceberg-rust-cache](https://github.com/DataInfraLab/iceberg-rust-cache) | `main` | `3824426` | Rust 缓存引擎及 Iceberg storage/file-read 集成适配层。 |
| Rust bridge | `deps/iceberg-rust-bridge` | [DataInfraLab/iceberg-rust-bridge](https://github.com/DataInfraLab/iceberg-rust-bridge) | `main` | `d92091d` | 生成 `libiceberg_rust_bridge.so` 与 C 头文件；把 C/C++ 扩展调用转接到 SDK、索引、缓存和 Arrow RecordBatch 能力。 |
| Arrow C++ | `deps/iceberg-arrow-deps` | [DataInfraLab/iceberg-arrow-deps](https://github.com/DataInfraLab/iceberg-arrow-deps) | `master` | `4c6aca6` | 提供 Apache Arrow C++ 的构建脚本/依赖输入，产出供 Delta 等 C++ 组件链接的 Arrow 安装树。 |
| 读扩展 | `plugins/iceberg_fdw` | [DataInfraLab/iceberg_fdw](https://github.com/DataInfraLab/iceberg_fdw) | `main` | `6d3d24c` | openGauss FDW 扩展；使用 bridge 访问 Iceberg，负责 SQL/FDW 规划和执行侧接入。 |
| Catalog 扩展 | `plugins/openGauss-Catalog` | [DataInfraLab/openGauss-Catalog](https://github.com/DataInfraLab/openGauss-Catalog) | `main` | `782c90d` | `iceberg_catalog` 扩展；维护 Iceberg 表、schema、snapshot、partition 等 Catalog 元数据接口。 |
| 写扩展 | `plugins/iceberg_delta` | [DataInfraLab/iceberg_delta](https://github.com/DataInfraLab/iceberg_delta) | `master` | `de1d27a` | `iceberg_delta` 扩展；提供 Iceberg Delta/MOR 写入相关能力。 |
| 测试 | `test/DataInfra-devtest` | [DataInfraLab/DataInfra-devtest](https://github.com/DataInfraLab/DataInfra-devtest) | `main` | `d134a52` | 统一 SQL 回归、端到端验证与覆盖率用例；通过已安装的 openGauss 和扩展验证集成基线。 |

## 3. 关键依赖关系

```text
iceberg-rust-datainfra ─┐
iceberg-index ─────────┼─> iceberg-rust-bridge ─┬─> iceberg_fdw
iceberg-rust-cache ────┘                         ├─> openGauss-Catalog
                                                   └─> iceberg_delta

iceberg-arrow-deps ─> Arrow C++ install ─────────────> iceberg_delta

openGauss-server-datainfra ─> pg_config / headers / extension ABI
                             └───────────────────────> 三个插件构建与安装

已安装的 openGauss + bridge + 三个扩展 ──────────────> DataInfra-devtest
```

具体边界如下：

- `iceberg-rust-bridge` 的 Cargo 配置直接以相对路径依赖 SDK、索引 ABI 和缓存的 Iceberg 集成 crate；因此这四个 Rust 子仓必须作为相邻的 `deps/` checkout 协同存在。
- `iceberg_fdw` 依赖 openGauss 的 `pg_config`、服务端头文件和 bridge 共享库。构建脚本将 bridge 安装至 `$GAUSSHOME/lib/postgresql/`，再编译/安装 FDW。
- `openGauss-Catalog` 同样复用 bridge 共享库和头文件，产出 `iceberg_catalog.so` 及其 SQL/control 文件。
- `iceberg_delta` 通过 CMake 同时接入 openGauss 源码、bridge、Catalog 头文件和 Arrow C++ 安装树，产出 `iceberg_delta.so`。
- `DataInfra-devtest` 不参与编译产物链接；它启动/连接已构建的实例，以 SQL 用例验证读、写、Catalog 与错误路径的组合行为。

## 4. 构建流程与产物

母仓的标准入口是 `build/build-all.sh`。执行顺序为：

1. `build/build-arrow.sh`：若目标目录不存在 `libarrow.so`，调用 `iceberg-arrow-deps/build_arrow.sh` 构建 Arrow C++。
2. `build/01-build-opengauss.sh`：编译并安装 openGauss；默认使用 debug/assert 风格，可配置 release。
3. `build/02-build-rust-components.sh`：对索引 workspace 执行 `cargo check --workspace`，并构建 release bridge。
4. `build/03-install-bridge-and-fdw.sh`：安装 `libiceberg_rust_bridge.so`，再编译并安装 `iceberg_fdw`。
5. `build/05-build-catalog.sh`：复制 bridge ABI 依赖，编译并安装 Catalog 扩展。
6. `build/06-build-delta.sh`：以 CMake 构建并安装 Delta 扩展。
7. `build/04-verify-build.sh`：检查 openGauss 可执行文件、三类扩展的 `.so`/control/SQL 文件、动态链接依赖和关键导出符号。

在全量构建之前，`build/00-prepare-env.sh` 会检查所有子模块、准备 binarylibs、检查 Rust/Cargo、生成或核对 `opengauss.env`，并处理平台相关环境。默认安装根目录为 `mppdb_temp_install/`，日志在 `build/logs/`。

构建完成后，可进入 `test/DataInfra-devtest` 执行 `test/run_all.sh`，对指定端口和 schedule 跑 SQL 回归。

## 5. 子模块版本管理机制

`data_infra` 的核心价值是将“可工作的组合”变成可复现基线：

- `git submodule update --init --recursive`：按母仓当前提交记录的 gitlink checkout 所有子仓，复现已验证基线。
- `git submodule update --remote --recursive`：按 `.gitmodules` 指定分支临时追踪子仓最新提交，供集成验证使用；会使母仓工作区出现 gitlink 变化。
- 业务代码、单元测试和 PR 应提交到各自子仓；母仓只提交构建/测试/文档改动，或在构建与回归都通过后提交新的 gitlink 组合。

因此，母仓不是各子仓的代码镜像，也不应把子仓源码复制进来；它是 **依赖版本锁定 + 一键集成构建 + 集成回归验证** 的控制平面。

## 6. 常用入口

```bash
# 获取当前已验证组合
git clone --recurse-submodules https://github.com/DataInfraLab/data_infra.git
cd data_infra
git checkout main
git submodule update --init --recursive

# 准备环境并执行完整构建
./build/00-prepare-env.sh
./build/build-all.sh

# 运行串行 SQL 回归（示例端口）
cd test/DataInfra-devtest
./test/run_all.sh --port 59200 --schedule test/schedules/serial_schedule
```

> 本文只描述构建/集成关系，不替代各子仓自身的接口、实现和设计文档。涉及具体功能设计时，应以当前项目根仓 `design/` 目录中的设计文档为准。
