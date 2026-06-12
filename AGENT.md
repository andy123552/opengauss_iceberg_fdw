# Project Agent: openGauss Iceberg FDW

## 角色定位

这个 agent 专门跟进 openGauss 通过 FDW 连接 Iceberg 湖表的项目。它的职责是维护项目上下文、拆解实现步骤、记录接口约束，并在后续实现时优先遵循 openGauss FDW 的既有代码结构。

## 当前任务摘要

- 目标是在 openGauss 中通过 FDW 访问 Iceberg 湖表。
- 需要实现索引扫描和 DML 写入能力。
- 元数据管理、索引底层结构、索引扫描底层接口、delta 表写入接口均由团队提供。
- 本项目主要实现 openGauss FDW 侧规划、执行、DML、事务和类型转换流程。
- 当前主实现方向是 managed Iceberg foreign table：Iceberg metadata 通过 openGauss 外表 DDL 创建和演进。
- 后续扫描演进需要考虑在执行期按团队索引接口选择索引扫描；向量 top-k 路径可由上层 vector search 包装，并在 openGauss 本地做精筛。

## 工作原则

- 不重复实现团队已提供的 Iceberg 元数据、索引和 delta 写入底层能力。
- 优先调用已有接口，并把接口边界记录清楚。
- 实现前先阅读 openGauss 现有 FDW callback 和 contrib 示例。
- 每次新增约束都更新 `README.md` 或新增设计文档。
- 对 DML、事务、错误恢复相关逻辑保持保守设计。

## 后续上下文入口

- 项目总览：`README.md`
- openGauss 源码参考树：`openGauss-server/`
- openGauss 目标参考仓：`https://github.com/DataInfraLab/openGauss-server-datainfra`
- 源码仓克隆说明：`design/source-reference.md`
- Catalog 源码参考树：`Catalog/`
- Catalog 目标参考仓：`https://github.com/HardingHang/Catalog`
- Catalog 当前参考提交：`8ed555bc4db70e7f1fd2ca5b3722e5dc159d1b57`
- FDW 基础代码目录：`iceberg_fdw/`
- 构建记录：后续记录到 `build-notes.md`
- 设计记录：后续可新增 `design/` 目录
- 接口适配记录：后续可新增 `interfaces/` 目录

## Catalog 模块上下文

- `Catalog/` 是团队提供的 openGauss 扩展参考仓，本项目只把它作为外部源码参考，不把完整仓库纳入当前项目提交。
- 扩展名为 `iceberg_catalog`，安装 SQL 创建 `iceberg_catalog` schema。
- 当前已提供的元信息表包括：`namespaces`、`tables_internal`、`table_schemas`、`snapshots`、`partition_specs`、`tables_external`。
- 当前已提供的兼容视图包括：`iceberg_catalog.iceberg_tables`、`iceberg_catalog.iceberg_namespace_properties`。
- FDW scan 侧最相关的是 `tables_internal`、`table_schemas`、`snapshots`、`partition_specs`：分别提供本地 `relid` 到 Iceberg 表身份/metadata 指针的绑定、字段级 schema、snapshot 摘要、分区 spec。
- `iceberg_catalog.create_table(...)` 目前只是 C 函数骨架：已做必填参数校验并返回占位 JSONB，真实 schema 校验、namespace/table 检查、Iceberg SDK CreateTable、storage 创建、metadata 注册仍是 TODO。
- 在 TODO 实现前，FDW 侧不要假设 `create_table` 已能创建真实 Iceberg metadata；scan 规划优先直接消费 catalog 元信息表/视图。

## 当前开发原则

- 项目目录中的 `openGauss-server/` 和 `Catalog/` 是开发参考与集成依赖；`iceberg_fdw/` 代码要保持可独立编译，同时接口、hook、catalog 表和行为要能对接这两个仓库。
- 调试 openGauss 使用本项目 Docker 实例，把开发中的动态库安装/连接到容器内验证；最终接入、预加载和发布方式暂未定。
- 功能开发采用先写目标测试用例再实现并验证的方式；测试和代码组织参考 `pg_lake/`，但不把 pg_lake 源码纳入本项目。
- 开发过程中凡是新增 adapter 边界、catalog 写入、事务回调、类型映射规则，都要补充聚焦的 UT 或回归用例看护。

## 后续优先事项

1. 使用 `openGauss-server/` 作为 FDW callback 和 contrib 实现的代码对比参考。
2. 确认或修复 Docker 方式的本地 openGauss 运行环境。
3. 等待团队接口定义后补齐接口适配设计。
4. 实现最小 Iceberg FDW extension 骨架。
5. 优先接入 managed 外表 DDL hook 与基础全表扫描。
6. 分阶段接入索引扫描、vector search 精筛和 delta DML 写入。
