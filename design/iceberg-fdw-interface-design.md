# Iceberg FDW 接口对接设计

## 目标与范围

本文档梳理 openGauss Iceberg FDW 的完整实现方案，重点定义 FDW 模块与外部 Iceberg catalog、索引模块、delta 写入模块之间的接口边界。

本项目只实现 openGauss FDW 侧的规划、执行、DML、事务、选项校验和类型转换流程，不重复实现 Iceberg 元数据管理、底层索引结构、底层扫描执行和 delta 写入能力。

## 总体架构

FDW 实现应拆成一个独立、可插拔的适配模块：

- `iceberg_fdw`：openGauss extension 入口，注册 FDW callbacks，解析 options，承接 planner/executor/DML/transaction 生命周期。
- `catalog_adapter`：封装团队提供的 Iceberg catalog/metadata 接口，向 FDW 暴露稳定的表、schema、snapshot、manifest、分区和权限信息。
- `index_adapter`：封装团队提供的索引元数据、索引选择、索引查找和索引扫描任务接口。
- `delta_adapter`：封装团队提供的 delta 表 insert/update/delete、提交、回滚、幂等和冲突检测接口。
- `type_adapter`：负责 openGauss 类型、Datum、NULL、collation、typmod 与 Iceberg 类型/外部值之间的转换。

FDW 主流程只依赖上述 adapter 抽象，不直接依赖某个 catalog、索引或 delta 的具体实现。具体实现通过编译期链接或运行期 option 选择接入。后续替换 catalog 类型、索引实现或 delta writer 时，不应修改 FDW planner/executor 主流程。

## openGauss FDW 侧主流程

### 查询规划

规划阶段需要完成：

- 解析 foreign table/server/user mapping options。
- 通过 catalog 获取表 schema、当前 snapshot、分区信息、数据文件统计、可用索引列表。
- 将 openGauss qual 拆分为：
  - catalog/partition 可裁剪条件。
  - index 可匹配条件。
  - reader 可下推条件。
  - 必须由 openGauss 本地 recheck 的条件。
- 根据普通扫描、分区裁剪、索引扫描候选路径分别估算 rows/cost/width。
- 在路径私有信息中记录后续执行所需的 snapshot id、列映射、条件映射、索引选择结果和 recheck 条件。

### 查询执行

执行阶段需要完成：

- `BeginForeignScan` 初始化 catalog/index 扫描上下文、snapshot、投影列、类型转换器和资源句柄。
- `IterateForeignScan` 从 index 或普通扫描任务中批量获取行数据，转换成 openGauss tuple/slot。
- `ReScanForeignScan` 重置扫描状态，保留可复用元数据和类型转换缓存。
- `EndForeignScan` 释放 catalog/index reader 句柄、外部 buffer、错误上下文和内存资源。
- `ExplainForeignScan` 输出 snapshot、索引选择、裁剪条件、recheck 条件、扫描文件数和预估行数等诊断信息。

### DML 执行

DML 阶段需要完成：

- `AddForeignUpdateTargets` 为 UPDATE/DELETE 增加能够定位原始行的隐藏列。
- `PlanForeignModify` 记录目标列、目标表、操作类型、行定位字段、返回列和约束相关信息。
- `BeginForeignModify` 初始化 delta 写事务、catalog snapshot、列映射和写入 buffer。
- `ExecForeignInsert` 将新行转换成 delta append/insert 记录。
- `ExecForeignUpdate` 将旧行定位信息和新行转换成 delta update/delete+insert 记录。
- `ExecForeignDelete` 将旧行定位信息转换成 delta delete 记录。
- `EndForeignModify` flush 写入 buffer，但最终提交必须受 openGauss 事务结果控制。
- 事务提交时调用 delta commit，事务中止时调用 delta abort/rollback。

## 索引扫描需要的接口

索引能力分为索引发现、索引选择、索引查找、行定位和结果读取五类。FDW 需要的接口不应泄漏底层索引文件格式。

### 1. 索引发现接口

需要接口：

- 根据 catalog table id、snapshot id 获取可用索引列表。
- 获取每个索引的列集合、表达式、索引类型、排序信息、唯一性、覆盖列、状态和统计信息。
- 判断索引是否对当前 snapshot 可见。
- 获取索引与数据文件、manifest、partition 的关联关系。

使用场景：

- 规划阶段判断是否可以生成索引扫描路径。
- 代价估算阶段读取 distinct、min/max、null fraction、数据文件覆盖率等统计信息。
- explain 输出可用索引和最终选择原因。

### 2. 谓词匹配接口

需要接口：

- 将 openGauss qual 映射成索引模块可识别的 predicate 表达式。
- 判断操作符是否支持，例如 `=`, `<`, `<=`, `>`, `>=`, `IN`, range、前缀匹配、IS NULL。
- 判断类型、collation、大小写规则、时区、精度是否可安全下推。
- 返回可由索引处理的条件、需要 residual recheck 的条件，以及不可使用索引的原因。

使用场景：

- 规划阶段从多个索引中筛选候选索引。
- 避免语义不一致的谓词错误下推。
- 生成本地 recheck 条件，保证索引误判或精度差异不影响结果正确性。

### 3. 索引代价与选择接口

需要接口：

- 基于谓词、snapshot、分区裁剪结果估算命中行数、命中文件数、扫描任务数和随机/顺序读取成本。
- 支持多索引组合的能力声明，例如 bitmap/intersection/union 是否支持。
- 返回推荐的索引扫描计划，或返回不推荐索引的原因。

使用场景：

- FDW planner 中比较普通 Iceberg 文件扫描、分区裁剪扫描和索引扫描成本。
- 需要避免对低选择性条件强行使用索引。
- explain 展示索引路径选择依据。

### 4. 索引查找与扫描任务接口

需要接口：

- 创建索引扫描上下文，输入 table、snapshot、索引 id、predicate、projection、limit 等信息。
- 执行索引查找，返回扫描任务，而不是直接暴露底层索引页。
- 扫描任务至少应包含数据文件标识、delete/delta 应用信息、行组或行号范围、必要 residual predicate、读取列集合。
- 支持批量拉取任务、取消、错误返回和资源释放。

使用场景：

- `BeginForeignScan` 创建扫描上下文。
- `IterateForeignScan` 按批获取任务并读取数据。
- `ReScanForeignScan` 重放或重新创建扫描任务。

### 5. 行定位与可见性接口

需要接口：

- 返回稳定的行定位信息，例如 file path/file id、row position、partition id、snapshot id、sequence number。
- 判断该行在当前 snapshot 和 delta/delete 文件作用下是否可见。
- 在 UPDATE/DELETE 时将行定位信息传递给 delta writer。

使用场景：

- 索引扫描结果需要定位真实数据行。
- DML 的 UPDATE/DELETE 需要精确定位被修改行。
- 并发写入和 snapshot 切换时需要检测行是否仍然有效。

## DML 需要 catalog 提供的信息与接口

DML 不只依赖 delta writer，还需要 catalog 提供表级语义、schema 和提交校验信息。

### 1. 表与 schema 信息

需要信息/接口：

- 表唯一标识、catalog name、namespace/database、table name。
- 当前 schema、字段 id、字段名、类型、nullable、默认值、字段顺序。
- 分区 spec、排序 spec、主键/唯一键或业务键约束，如果 Iceberg 表侧存在。
- 隐藏列和系统列映射，例如 row locator、snapshot id、file id、row position。
- schema evolution 信息，包括字段 rename、add/drop、type promotion。

使用场景：

- INSERT/UPDATE 时按 Iceberg field id 而不是仅按列名写入。
- UPDATE/DELETE 时保存可稳定定位旧行的字段。
- 类型转换和默认值填充。
- 防止 openGauss foreign table 定义与 Iceberg schema 不一致。

### 2. 快照与并发控制信息

需要信息/接口：

- 获取 DML 开始时的 base snapshot id。
- 获取当前最新 snapshot id。
- 判断 base snapshot 到 commit 时是否发生冲突。
- 获取表 isolation/commit policy，例如 optimistic commit、serializable 检查范围。（是不是用数据库本身的就可以了？）
- 获取 delete/update 对数据文件或行级定位的冲突检测规则。

使用场景：

- BeginForeignModify 绑定写入基线。
- 事务提交时检测并发写冲突。
- UPDATE/DELETE 防止覆盖其他事务已经修改或删除的行。

### 3. 文件、分区与布局信息

需要信息/接口：

- 分区字段、分区 transform、partition value 编码规则。
- 目标数据文件布局策略，例如文件大小、目标目录、压缩格式。
- 表 location、metadata location、data location、delete/delta location。
- manifest/file statistics，用于判断 rewrite 或 delete 文件影响范围。

使用场景：

- INSERT 时计算分区路径和写入目标。
- UPDATE 时判断是否跨分区变更。
- DELETE 时生成 position delete/equality delete 所需的文件定位。

### 4. 权限、约束和能力声明

需要信息/接口：

- 表是否允许 insert/update/delete。
- 是否支持 returning、batch write、upsert、merge、position delete、equality delete。
- NOT NULL、唯一性、check-like 约束或由 catalog 管理的表属性。
- 认证信息、租户/namespace 权限和写锁能力。

使用场景：

- options 校验和 DML planning 阶段提前拒绝不支持操作。
- 避免 FDW 接受 openGauss SQL 但在外部系统提交阶段才失败。
- 确定 UPDATE/DELETE 的实现策略。

## delta 模块需要提供的接口

delta 模块负责把 FDW 的行级 DML 变更转换成 Iceberg 可提交的增量变更。FDW 不应关心底层是 position delete、equality delete、copy-on-write、merge-on-read 还是自定义 delta 表。

### 1. 写事务生命周期接口

需要接口：

- `begin_write(table, base_snapshot, operation, options)`：创建写事务。
- `write_insert(handle, row_batch)`：写入新增行。
- `write_update(handle, old_locator_batch, new_row_batch)`：写入更新。
- `write_delete(handle, old_locator_batch)`：写入删除。
- `flush(handle)`：刷出内存 buffer，返回暂存文件或写入统计。
- `prepare_commit(handle)`：生成提交描述，执行提交前校验。
- `commit(handle)`：提交 Iceberg/delta 变更并返回新 snapshot 信息。
- `abort(handle)` 或 `rollback(handle)`：清理临时文件、撤销未提交变更。
- `close(handle)`：释放资源，要求可重复调用。

实现功能：

- 对接 openGauss foreign modify callbacks。
- 将 openGauss 事务结果映射到 delta commit/abort。
- 在错误路径、statement abort、transaction abort 中清理外部资源。

### 2. 行编码与批量写入接口

需要接口：

- 接收按 Iceberg field id 编码的行 batch。
- 支持 NULL bitmap、类型精度、decimal、timestamp/date、binary、array/map/struct 等类型。
- 支持默认值填充和列缺省。
- 返回逐行错误或批量错误，错误中包含列、字段 id 和原始操作。

实现功能：

- 提高 INSERT/UPDATE 写入效率。
- 让 FDW 可以把 openGauss Datum 批量转换后提交给 delta。
- 对复杂类型和精度不兼容给出明确错误。

### 3. UPDATE/DELETE 行定位接口

需要接口：

- 接收索引扫描或普通扫描返回的 row locator。
- 支持 position delete 所需的 file id/path + row position。
- 支持 equality delete 所需的 key field ids + key values。
- 校验 locator 是否属于 base snapshot，必要时返回冲突。

实现功能：

- UPDATE 可以实现为 delete old row + insert new row，或由 delta writer 使用原生 update。
- DELETE 可以生成位置删除或等值删除。
- 事务提交时发现目标行已经不可见时返回可识别的冲突错误。

### 4. 提交、冲突与幂等接口

需要接口：

- 提交时携带 base snapshot、operation id、transaction id、statement id。
- 对重复 commit/abort 调用具备幂等语义。
- 返回冲突类型，例如 snapshot advanced、file conflict、row conflict、schema conflict、permission conflict。
- 提供可重试/不可重试错误分类。
- 提供临时文件清理接口和孤儿文件回收建议。

实现功能：

- openGauss 事务中止或连接中断时能够安全清理。
- 避免网络重试导致重复提交。
- 在 FDW 层把外部冲突转换为合适的 SQL 错误。

### 5. 提交结果与 catalog 刷新接口

需要接口：

- 返回新 snapshot id、提交 summary、写入文件数、删除文件数、行数统计。
- 通知 catalog adapter 刷新表元数据缓存。
- 暴露可用于 explain/analyze 或日志的写入统计。

实现功能：

- DML 成功后让后续查询看到正确 snapshot。
- 为问题排查提供可观测性。
- 支持回归测试校验写入结果。

## 可插拔模块约束

为保证 FDW 与其他模块解耦，后续实现必须遵循以下约束：

- FDW 主流程只依赖 `catalog_adapter`、`index_adapter`、`delta_adapter`、`type_adapter` 的稳定接口。
- adapter 接口应使用项目内定义的中立数据结构，避免把第三方 SDK 类型扩散到 FDW callback 实现中。
- 任何具体 catalog、索引、delta 实现只能出现在 adapter 实现文件中。
- adapter 必须声明能力集，例如是否支持 UPDATE、DELETE、position delete、equality delete、索引组合、批量写入、事务提交等。
- FDW planner/executor 根据能力集选择路径或拒绝 SQL，不通过硬编码实现类型判断能力。
- 所有外部资源句柄都必须绑定到 FDW scan/modify/transaction 上下文，并有明确释放路径。
- option 设计要区分 FDW 通用选项和具体 adapter 选项，具体 adapter 选项使用命名空间前缀。
- adapter 错误必须转换成 FDW 层统一错误码、错误消息和错误上下文。
- 单元测试应可以用 mock adapter 覆盖 planner、DML 和事务路径，不依赖真实 Iceberg 集群。

## 接口数据结构建议

建议定义以下中立结构作为 FDW 与 adapter 的边界：

- `IcebergFdwTableRef`：catalog、namespace、table、table id。
- `IcebergFdwSnapshotRef`：snapshot id、sequence number、timestamp、branch/tag。
- `IcebergFdwFieldMap`：openGauss attnum、Iceberg field id、类型、nullable、默认值。
- `IcebergFdwPredicate`：列、操作符、常量、范围、NULL 语义、collation、是否需要 recheck。
- `IcebergFdwIndexInfo`：index id、列集合、类型、统计信息、可见 snapshot 范围、能力集。
- `IcebergFdwScanTask`：数据文件、delete/delta 文件、行范围、row locator、residual predicate。
- `IcebergFdwRowLocator`：snapshot id、file id/path、row position、partition、sequence number、equality key。
- `IcebergFdwWriteHandle`：delta 写事务句柄。
- `IcebergFdwWriteResult`：新 snapshot、写入统计、冲突或错误分类。

## 事务与错误处理约束

- FDW 的 scan snapshot 和 DML base snapshot 必须明确记录，不能隐式使用 catalog 当前值。
- DML 的外部提交必须发生在 openGauss 事务提交路径中；statement 结束不等于外部提交。
- 外部写入已经 flush 但 openGauss 事务失败时，必须调用 abort/rollback。
- 任何 UPDATE/DELETE 如果缺少稳定 row locator，应在 planning 或 execution 早期报错。
- schema 在 DML 期间发生不兼容变更时，delta commit 必须失败并返回 schema conflict。
- 对索引扫描返回结果，即使索引声明精确，也应保留 residual recheck 能力作为安全边界。

## 分阶段实现建议

### Phase 1: adapter 抽象和 mock

- 定义 catalog/index/delta/type adapter 头文件和中立数据结构。
- 提供 mock adapter，用固定元数据和内存数据覆盖 planner、scan、DML 流程测试。
- 实现 options 解析、能力声明和错误转换框架。

### Phase 2: 查询与索引扫描

- 实现 catalog 元数据加载和列映射。
- 实现 qual 到 `IcebergFdwPredicate` 的转换。
- 接入 index discovery、predicate match、cost estimate、scan task 接口。
- 完成索引扫描执行、row materialization、rescan、end scan 和 explain。

### Phase 3: DML 与 delta 写入

- 实现 foreign modify callbacks。
- 接入 delta begin/write/flush/commit/abort。
- 补齐 row locator 隐藏列、UPDATE/DELETE 定位和 returning。
- 接入 openGauss 事务生命周期，验证 commit/rollback/异常中断。

### Phase 4: 联调与能力收敛

- 替换 mock adapter 为团队实际 catalog/index/delta 实现。
- 记录具体接口签名、错误码和能力限制。
- 完成兼容性、并发冲突、失败恢复和性能测试。

## 待确认问题

- 项目后续是否固定使用当前参考提交 `75f983eb0e7bbd7725eb317aba613f42337759e5`，还是需要对齐某个 release 分支。
- 团队索引模块是否支持多索引组合、覆盖索引、批量 row locator 返回。
- delta writer 的 UPDATE 是原生 update，还是 delete+insert。
- DELETE 使用 position delete 还是 equality delete，或者两者都支持。
- catalog 是否提供 schema evolution 的 field id 级别映射。
- 外部提交是否能做到幂等，以及 operation id 的生成规则。
- 是否需要支持 branch/tag、time travel、指定 snapshot 查询。
- 是否需要支持 COPY FROM 外表写入；如果需要，`ExecForeignInsert` 必须支持 lazy init 和批量 flush。
