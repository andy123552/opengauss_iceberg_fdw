# Project Agent: openGauss Iceberg FDW

## 角色

本项目跟踪 openGauss 通过 FDW 访问 Iceberg 湖表的集成工作。
Agent 需要保留项目上下文、拆解实现任务、记录接口约束，并优先遵循
openGauss FDW 现有代码模式。

## 项目摘要

- 目标：通过 FDW 从 openGauss 访问 Iceberg 表。
- 必需能力包括索引扫描和 DML 写入支持。
- Iceberg 元数据管理、底层索引结构、底层索引扫描 API、delta 表写入
  API 预期由团队提供。
- 本项目应实现 openGauss FDW 侧的规划、执行、DML、事务、选项校验和
  类型转换流程。
- 当前主要实现方向是 managed Iceberg foreign table：Iceberg metadata
  通过 openGauss foreign-table DDL 创建和演进。
- 后续扫描演进应考虑：当团队索引 API 可用时，在执行阶段选择索引支撑
  的扫描。向量 top-k 路径可由上层 vector-search/refine 步骤包裹，并在
  openGauss 本地做精确 refinement。

## 本地布局

- 项目根目录：`/home/andy/opengauss_iceberg_fdw`
- openGauss 源码参考树：`openGauss-server/`
- 源码参考仓库：
  `https://github.com/DataInfraLab/openGauss-server-datainfra`
- 源码克隆说明：`design/source-reference.md`
- Catalog 源码参考树：`Catalog/`
- Catalog 源码参考仓库：`https://github.com/HardingHang/Catalog`
- Catalog 源码参考提交：
  `8ed555bc4db70e7f1fd2ca5b3722e5dc159d1b57`
- FDW 扩展骨架：`iceberg_fdw/`
- 运行模式：使用 `docker-compose.yml` 中的 Docker 镜像；默认不要编译
  openGauss 源码树。
- 项目总览：`README.md`
- Docker service：`opengauss`
- 容器名：`opengauss-iceberg-fdw`
- Host 端口：`15432`
- 默认数据库密码：`openGauss@123`

## 工作原则

- 不要重新实现团队已经提供的 Iceberg metadata、索引或 delta 写入底层能力。
- 优先调用已有团队接口，并清晰记录接口边界。
- 开始代码开发前，更新本地依赖参考仓库到最新 upstream 提交；至少包括
  `openGauss-server/` 和 `Catalog/`，当工作触及 bridge 行为或 ABI 时还包括
  `iceberg-rust-bridge/`。
- 实现前先检查 openGauss FDW callbacks 和 contrib 示例。
- 每当发现新约束时，更新 `README.md` 或新增聚焦的设计文档。
- 对 DML、事务处理、错误恢复、回滚和幂等性保持保守。
- 保留无关本地改动。源码树可能包含前序构建生成的文件。

## Catalog 模块上下文

团队 Catalog 仓库本地克隆在 `Catalog/`，它是外部源码参考，不是要 vendor
到本仓库的代码。正常提交中不要包含它。

已确认 Catalog 在提交 `8ed555bc4db70e7f1fd2ca5b3722e5dc159d1b57` 的能力：

- 它是名为 `iceberg_catalog` 的 openGauss 扩展。
- SQL 扩展创建 schema `iceberg_catalog`。
- 它定义 metadata tables：
  `namespaces`、`tables_internal`、`table_schemas`、`snapshots`、
  `partition_specs` 和 `tables_external`。
- 它定义兼容视图：
  `iceberg_catalog.iceberg_tables` 和
  `iceberg_catalog.iceberg_namespace_properties`。
- `tables_internal` 是 FDW 相关的表绑定面，把本地 `relid` 绑定到
  `namespace`、`table_name`、`table_uuid`、`metadata_location`、
  `table_location`、`current_schema_id`、`current_snapshot_id` 和
  `default_spec_id`。
- `table_schemas` 按 `table_uuid`、`schema_id`、`field_position` 和稳定的
  Iceberg `field_id` 存储展开后的顶层 Iceberg schema 字段。
- `snapshots` 存储 snapshot 摘要，包括 `snapshot_id`、可选 `schema_id`、
  `manifest_list` 和 `total_records`。
- `partition_specs` 存储 partition-spec 字段；未分区 spec 可以用
  `field_position = -1` 表示。
- `tables_external` 和兼容视图用于 JDBC Catalog 风格的外部记录，不包含
  本地 relation 绑定。

当前实现边界：

- `iceberg_catalog.create_table(...)` 是一个 C-language SQL function，但它的
  C 实现目前仍是 stub。它会校验必填参数，并返回占位 JSONB response。
- 这个 C stub 中包含 TODO：schema validation、namespace/table existence
  checks、Iceberg SDK `CreateTable`、storage creation 和 metadata-table
  registration。
- 当前 FDW scan planning 应把 catalog metadata tables/views 作为可靠的集成
  表面。在 TODO 实现前，不要假设 `create_table` function 会创建真实的
  Iceberg metadata。

## 当前开发原则

- 本地 `openGauss-server/` 和 `Catalog/` 仓库是开发参考和集成依赖。
  `iceberg_fdw/` 应保持可独立构建，同时在 headers、hooks、catalog tables
  和 runtime behavior 上与这些仓库对齐。
- 开始代码修改前，刷新所有相关依赖仓库到 upstream head，并用具体 revision
  检查结果。不要只依赖之前的“已 pull”消息。对于任务触及的任何依赖仓库，
  在诊断集成失败前，确认 fetched remote tip 和本地 `HEAD` 都匹配目标最新
  提交。
- 当用户要求“提交”、“提交到仓库”、“上传到代码仓”或等价表述时，任务只有在
  相关仓库完成 fetch 最新 upstream 分支、rebase 或 fast-forward 到最新、
  只提交目标改动、并成功 push 到远端仓库后才算完成。除非用户明确要求只做
  本地 commit，否则 local-only commit 不足以结束任务。

## GitHub 提交与 PR 流程

- 涉及 GitHub push、PR、CI 修复或更新 PR 时，默认先检查本机 GitHub CLI：
  `& 'C:\Program Files\GitHub CLI\gh.exe' auth status -h github.com`。如果 token
  失效，直接打开授权窗口：
  `Start-Process powershell -ArgumentList @('-NoExit','-Command','& ''C:\Program Files\GitHub CLI\gh.exe'' auth refresh -h github.com --scopes repo,read:org')`。
- DataInfraLab 上游仓库通常没有直接写权限。遇到
  `remote: Write access to repository not granted` 或 403 时，不要反复尝试
  push 上游；应使用 `andy123552/<repo>` fork 分支提交，并向
  `DataInfraLab/<repo>` 创建 PR。
- 物理机 `opengauss-ad` 到 GitHub 的下载和 push 可能不稳定，曾出现 GitHub CLI
  arm64 下载卡住、`Failed to connect to github.com port 443`、`Operation too slow`
  等问题。开发和验证仍在物理机完成，但 GitHub 发布优先使用本机 Windows 上的
  GitHub CLI；必要时把物理机生成的 patch 或单文件内容取回本机，再通过 `gh api`
  更新 fork 分支。
- 不要把 GitHub token 放进 remote URL 或命令输出中，例如不要使用
  `https://x-access-token:<token>@github.com/...` 作为长期 remote，也不要让该 URL
  出现在日志里。临时 push 需要鉴权时，优先使用 `http.https://github.com/.extraheader`
  的 Basic header；命令结束后立即删除临时 token 文件，并检查 `git config --list`
  中没有 `x-access-token` 或 `gho_`。
- 如果临时 URL 或 token 已经被输出到日志，必须把本机 `gh` token 视为已泄露：
  先清理物理机仓库的 remote/upstream 配置，再执行 `gh auth refresh` 重新授权。
- 当物理机网络或 git push 鉴权反复失败，但本机 `gh` 可用时，可以用 GitHub
  Contents API 更新 fork 分支上的文件：
  1. 用 `gh api repos/<user>/<repo>/contents/<path> --method GET -F ref=<branch> --jq .sha`
     获取目标文件 sha。
  2. 将本地文件 base64 后调用
     `gh api --method PUT repos/<user>/<repo>/contents/<path> --input <json>`，
     JSON 包含 `message`、`content`、`sha` 和 `branch`。
  3. 用 `gh pr view` 和 `gh pr checks` 确认 PR head 与 CI 状态。
- 更新 PR 的标准收尾：确认 PR head commit、检查 `gh pr checks`，如果 CI 失败先读
  Actions 日志定位是否由本次修改引入；修复后重新运行本地等价命令，再更新 PR。
- 开发实现、接口对齐和行为判断所参考的设计文档，只能以项目根仓库 `design/`
  目录第一层的相应设计文档为准。依赖仓库、子仓库或其他目录中的设计文档只能作为
  历史/背景参考，不能覆盖根仓 `design/` 下的主设计文档。
- 修改任何位置的设计文档时，无论该文档位于依赖仓库、子仓库还是其他目录，都必须
  同步检查并更新项目根仓库 `design/` 目录第一层的对应设计文档，确保根仓设计文档
  始终是实现依据和最新事实来源。
- runtime debugging 应使用本项目的 Docker openGauss 实例。把开发中的 shared
  library build/install 到该实例中测试；最终产品 packaging 和 preload 策略
  仍未定。
- 功能开发遵循 test-first 流程。实现行为前先添加目标 SQL/regression 或
  unit tests，再用这些测试守住每个增量步骤。
- 使用 `pg_lake/` 作为 DDL handling、extension test layout 和 error-path
  coverage 的外部参考，但不要 vendor pg_lake 代码到本项目。
- 每当开发步骤引入新的 adapter boundary、catalog write、transaction callback
  或 type-mapping rule 时，都要补充聚焦的 unit/regression 覆盖。
- 本地功能测试和回归检查优先使用 debug/assert openGauss build，让 executor
  protocol violations（例如 virtual tuple slot misuse）立即失败。release build
  只用于 benchmark/performance 场景，在这些场景中优化和吞吐数字才重要。
- 把 `ForeignScan.fdw_private` 视为可执行契约，而不是 EXPLAIN-only metadata。
  每个序列化进 `fdw_private` 的值都必须被 `BeginForeignScan` 或更低层执行
  adapter 消费；否则必须在代码和测试中明确命名为 diagnostic-only。
- 当 planner callback 序列化 scan options、filters、projections、snapshot IDs、
  index requests 或其他 SDK/bridge inputs 时，必须把该值一路追踪到具体
  bridge ABI call，之后才能认为改动完成。
- pushdown 测试必须验证 adapter/bridge 边界行为，而不只是 planner
  classification 或 EXPLAIN 文本。测试中使用的 fake bridge 应断言它实际收到的
  `scan_options`、`filter_json`、projection、snapshot 或 index request。
- 当 bridge ABI 变化导致 request structs 或 adapter code 需要重建时，审计
  request type 中所有被删除或新增的字段，并确认替代路径保持行为不变。不要
  留下只用于 presence checks 或 EXPLAIN counters 的 serialized plan state。
- 对 filter pushdown，必须保持三类断言一致：planner 将 qual 分类为可 pushdown；
  executor 将序列化 filter 传入 `IcebergBridgeScanOptions.filter_json` 或当前
  bridge 等价字段；local quals/recheck behavior 仍然正确。

## 构建安全

- 物理机编译默认走 AD 主机快捷命令，不要重新推导或手工拼装构建流程。只要用户要求
  “拉新代码编译”“物理机编译”“debug 编译”或等价表述，无论是否在同一会话中，都应：
  1. 登录物理机 `opengauss-ad`。
  2. 先按任务需要更新相关仓库到最新，并确认目标分支和 `HEAD`。
  3. 直接调用 `/data/ad/tools/bin/ad-build` 编译。
- 物理机全量 debug 构建命令：
  `ssh opengauss-ad 'cd /data/ad && /data/ad/tools/bin/ad-build all'`
- 物理机仅编译/验证 Iceberg 相关组件时使用：
  `ssh opengauss-ad 'cd /data/ad && /data/ad/tools/bin/ad-build rust fdw catalog delta verify'`
- 物理机仅验证当前安装产物时使用：
  `ssh opengauss-ad 'cd /data/ad && /data/ad/tools/bin/ad-build verify'`
- `/data/ad/tools/bin/ad-build` 已包含 AD 主机本地环境适配：Rust PATH 修复、Cargo native crate
  编译所需 gcc support libs、bridge 对本地 `iceberg-index` crate 布局的兼容 patch、FDW/Catalog
  clean 入口和 `CMAKE_BIN` wrapper。后续编译应优先维护这个快捷脚本，而不是在会话中散落临时命令。
- 之前的本地源码构建路径导致过 OOM。除非用户明确要求，不要重试源码编译。
- 优先使用 Docker image installation 和 runtime checks。
- 在较长的 openGauss 或 C/C++ 构建前，运行 `free -h`。
- 检查遗留 compiler/build 进程：
  `ps -eo pid,ppid,cmd | rg 'make|gcc|g\\+\\+|cc1|cc1plus'`
- 避免无界并行。使用 `MAKEFLAGS=-j2`、`make -j2`，或最接近的项目支持 job flag。
- 除非用户明确要求，不要使用 `make -j` 或 `-j$(nproc)`。
- 长构建输出要写入日志文件，并用 `tail` 或 `rg` 检查。
- 如果内存压力或 swap 使用量快速升高，停止并报告资源瓶颈。

## 可能的后续步骤

1. 使用 `openGauss-server/` 作为 FDW callback 和 contrib implementation
   对比参考。
2. 确认或修复本地 Docker-based openGauss runtime environment。
3. 记录团队提供的 Iceberg metadata、index scan 和 delta write APIs。
4. 实现最小 Iceberg FDW extension skeleton。
5. 先添加 managed foreign-table DDL hooks 和 full-scan execution。
6. 添加 DML callbacks 和 delta writes 的 transaction integration。
7. 在基础 managed full-scan path 稳定后，添加 index-backed scan 和
   vector-search/refine integration。
