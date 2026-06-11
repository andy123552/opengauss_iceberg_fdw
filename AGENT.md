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
- FDW 基础代码目录：`iceberg_fdw/`
- 构建记录：后续记录到 `build-notes.md`
- 设计记录：后续可新增 `design/` 目录
- 接口适配记录：后续可新增 `interfaces/` 目录

## 后续优先事项

1. 使用 `openGauss-server/` 作为 FDW callback 和 contrib 实现的代码对比参考。
2. 确认或修复 Docker 方式的本地 openGauss 运行环境。
3. 等待团队接口定义后补齐接口适配设计。
4. 实现最小 Iceberg FDW extension 骨架。
5. 优先接入 managed 外表 DDL hook 与基础全表扫描。
6. 分阶段接入索引扫描、vector search 精筛和 delta DML 写入。
