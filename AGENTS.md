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

- 本机根仓只保留项目主仓：`/home/andy/opengauss_iceberg_fdw`
- 本机只保留主仓自身内容，例如：`design/`、`README.md`、`AGENTS.md`、
  `iceberg_fdw/`、`tools/`、`skills/`、`docker-compose.yml`
- 本机不要保留依赖仓工作副本，不要在本机查看、修改、编译或提交依赖仓代码；
  若本机再次出现 `data_infra/`、`openGauss-server/`、`Catalog/`、
  `iceberg-rust-bridge/`、`iceberg-rust-datainfra/`、`iceberg-index/`
  等依赖仓目录，应视为需要清理的冗余副本
- 所有依赖仓代码检视、开发、编译、测试、提交、push、PR 更新统一在物理机
  `opengauss-ad` 上完成
- 物理机主开发根目录：`/data/ad/stack/data_infra`
- 物理机 bridge 仓：`/data/ad/stack/data_infra/deps/iceberg-rust-bridge`
- 物理机 rust sdk 仓：`/data/ad/stack/data_infra/deps/iceberg-rust-datainfra`
- 物理机 index 仓：`/data/ad/stack/data_infra/deps/iceberg-index`
- 物理机 catalog 仓：`/data/ad/stack/data_infra/plugins/openGauss-Catalog`
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

- 依赖仓只以物理机 `opengauss-ad` 上的工作副本为准。本机根仓不再保留依赖仓
  checkout，也不能再以本机副本作为代码事实来源。
- 所有代码检视、实现、编译、测试、提交、push、PR 更新统一在物理机进行；
  除非用户明确要求处理主仓文档或主仓脚本，本机只用于维护根仓自身内容。
- 物理机 `openGauss-server`、`Catalog`、`bridge`、`rust sdk`、`index`
  仓库是开发参考和集成依赖。`iceberg_fdw/` 应保持可独立构建，同时在
  headers、hooks、catalog tables 和 runtime behavior 上与这些仓库对齐。
- 开始代码修改前，刷新所有相关依赖仓库到 upstream head，并用具体 revision
  检查结果。不要只依赖之前的“已 pull”消息。对于任务触及的任何依赖仓库，
  在诊断集成失败前，确认 fetched remote tip 和本地 `HEAD` 都匹配目标最新
  提交。
- 当用户要求“提交”、“提交到仓库”、“上传到代码仓”或等价表述时，任务只有在
  相关仓库完成 fetch 最新 upstream 分支、rebase 或 fast-forward 到最新、
  只提交目标改动、并成功 push 到远端仓库后才算完成。除非用户明确要求只做
  本地 commit，否则 local-only commit 不足以结束任务。

## GitHub 提交与 PR 流程

- 对当前主仓 `opengauss_iceberg_fdw` 自身内容（例如 `AGENTS.md`、`design/`、
  `README*`、`tools/`、`skills/`）的修改，默认直接在当前本地 `main` 分支上完成，
  不要再创建临时 worktree、临时发布分支或 detached HEAD 发布链路。
- 当前主仓只有用户本人维护，默认目标状态是：本地 `main` 与 GitHub `origin/main`
  保持同一条提交线、同步前进。除非用户明确要求保留分叉历史，否则不要把主仓改动先提交到
  其他临时分支，再把远端 `main` 指到另一条历史线上。
- 当前主仓的标准提交流程：
  1. 在当前工作目录确认 `git branch --show-current` 为 `main`
  2. `git fetch origin`
  3. 如远端有新提交，先在当前 `main` 上执行 `git rebase origin/main`
  4. 直接在当前仓库修改文件
  5. 用 `git add` / `git commit` 在当前 `main` 上提交
  6. 直接执行 `git push origin main`
- 只有当用户明确要求覆盖远端历史，或已经确认远端 `main` 被错误提交切到错误历史上时，
  才允许对当前主仓使用 `git push --force-with-lease origin main`；执行前必须先确认
  要保留的本地 `main` 历史就是正确基线。
- 如果之前为当前主仓创建过临时 worktree、临时发布分支或只用于提交主仓文档/skill 的
  辅助分支，在改动已经合回当前 `main` 并成功 push 后，应立即清理，避免后续再次误用。
- DataInfraLab 上游仓库通常没有直接写权限。遇到
  `remote: Write access to repository not granted` 或 403 时，不要反复尝试
  push 上游；应使用 `andy123552/<repo>` fork 分支提交，并向
  `DataInfraLab/<repo>` 创建 PR。
- GitHub 发布默认也在物理机 `opengauss-ad` 完成。物理机已验证可直接安装
  `gh`、登录 GitHub、配置 `gh auth setup-git`、推送 fork 分支并更新 PR。
- 涉及 GitHub push、PR、CI 修复或更新 PR 时，默认先在物理机检查：
  `export PATH="$HOME/.local/bin:$PATH"; gh auth status -h github.com`
- 如果在某个环境中 `gh` 不可用，不要直接判断为未安装；必须先检查当前
  `PATH`、常见绝对路径（Windows `C:\Program Files\GitHub CLI\gh.exe`，
  Linux/WSL/物理机 `$HOME/.local/bin/gh`、`/usr/local/bin/gh`）以及其它可用
  环境中的 `gh --version` / `gh auth status`。只有这些检查都失败后，才认为
  需要安装或重新配置 GitHub CLI。
- 物理机标准发布链路：
  1. 在目标仓库确认当前分支、上游分支、工作区状态和 `HEAD`
  2. 如有需要，先 `git fetch origin` / `git fetch fork`
  3. 提交代码后，用 `git push fork HEAD:refs/heads/<branch>` 更新 fork 分支
  4. 用 `gh pr view`、`gh pr edit`、`gh pr checks` 查看或更新 PR
- 如果目标分支已经存在同名本地遗留分支，先保留或改名旧分支，再新建一个跟踪
  `fork/<branch>` 的本地分支，避免混入历史上重复生成但 SHA 不同的提交链。
- 不要把 GitHub token 放进 remote URL 或命令输出中，例如不要使用
  `https://x-access-token:<token>@github.com/...` 作为长期 remote，也不要让该 URL
  出现在日志里。临时 push 需要鉴权时，优先使用 `http.https://github.com/.extraheader`
  的 Basic header；命令结束后立即删除临时 token 文件，并检查 `git config --list`
  中没有 `x-access-token` 或 `gho_`。
- 如果临时 URL 或 token 已经被输出到日志，必须把对应环境中的 `gh` token 视为已泄露：
  先清理仓库的 remote/upstream 配置，再执行 `gh auth refresh` 重新授权。
- 更新 PR 的标准收尾：确认 PR head commit、检查 `gh pr checks`，如果 CI 失败先读
  Actions 日志定位是否由本次修改引入；修复后重新运行本地等价命令，再更新 PR。
- 只要本次会话里执行了 push、更新了 PR 描述、或以其他方式刷新了 PR head，就必须把
  `gh pr checks` 作为收尾步骤的一部分；若发现新增或仍存在的红灯检查属于本次修改影响，
  需要先修复并重新推送，再把任务视为完成，不能把失败的检查留到“下次再看”。
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

## 常用自动化流程

### 飞书文档 / 模板读取

- 当用户要求“按飞书模板改 PR 描述”“读取飞书文档模板”“查看飞书文档内容”这类操作时，优先走飞书 OpenAPI，不要先依赖浏览器 DOM 抓取。
- 当前已验证可用的链路是：
  1. 用本机临时回调监听 `http://127.0.0.1:8765/feishu/callback` 接授权回调。
  2. 打开飞书 OAuth 授权页，至少申请这些 scope：
     - 搜索和下载文件：`drive:drive`
     - 读取 docx 正文：`docx:document:readonly`
     - 如需读取用户信息或辅助定位，可保留：`auth:user.id:read`
  3. 用授权回调里的 `code` 调 `https://open.feishu.cn/open-apis/authen/v2/oauth/token` 换 `user_access_token`。
  4. 如果目标是飞书普通文件模板，例如 `PR_TEMPLATE.md`，先调用 `POST https://open.feishu.cn/open-apis/suite/docs-api/search/object` 用标题搜索真实 `docs_token` 和 `docs_type`。
  5. 若搜索结果类型是 `file`，使用 `GET https://open.feishu.cn/open-apis/drive/v1/files/{file_token}/download` 下载原始文件内容。
  6. 若搜索结果类型是 `docx`，使用 `GET https://open.feishu.cn/open-apis/docx/v1/documents/{document_id}` 和 `GET https://open.feishu.cn/open-apis/docx/v1/documents/{document_id}/raw_content` 读取标题与正文。
- 已确认案例：分享链接 `https://my.feishu.cn/file/EC2qbQy3yoefO0x5U8VcEGLfnvd` 对应的是 `file` 类型文档 `PR_TEMPLATE.md`，不能当作 `docx document_id` 直接读取，必须先按 `file` 路径搜索或下载。
- 浏览器里出现“已屏蔽”并不代表授权失败。只要本机回调文件里收到了新的 `code` 和 `state`，就视为授权成功。

### GitHub 提交 / 更新 PR

- 当用户要求“提交到代码仓”“更新 PR”“提 PR”“同步到 GitHub”时，默认标准是：本地改动整理完成、推送到远端分支成功、PR 已创建或已更新后任务才算结束。
- 涉及 DataInfraLab 多仓协作时，代码开发、代码检视、编译验证、Git 提交、GitHub push 和 PR 更新都优先在物理机完成，不再使用本机依赖仓副本参与发布流程。
- 推荐流程：
  1. 在物理机仓库确认目标分支、最新 upstream、工作区状态和最终 commit。
  2. 确保本地工作分支跟踪 `fork/<branch>`，不要在 detached HEAD 或历史遗留分支上直接开发。
  3. 已有 PR 分支更新时，先 `git fetch fork <branch>`，再用 `git push fork HEAD:refs/heads/<branch>` 或 `--force-with-lease` 更新远端。
  4. 新 PR 使用物理机 `gh pr create`；已有 PR 使用物理机 `gh pr edit` 更新标题和描述。
  5. 推送后必须用 `gh pr view` 和 `gh pr checks` 核对 PR head、描述和检查状态。
- 如果需要批量更新多个仓库 PR，优先把 PR 模板正文先落到物理机本地文件，再统一用 `gh pr edit --body-file <file>` 批量更新，避免手工拼接多段命令时出错。
- 飞书模板改 PR 的组合链路已经验证可行：飞书 OAuth -> 搜索/下载 `PR_TEMPLATE.md` -> 在物理机生成 `body-file` -> `gh pr edit`。
- 只要输出内容包含中文（PR 描述、issue、README、设计文档、评论等），都必须在提交或发布前后做一次编码/显示核对，确认没有乱码、`????`、BOM 干扰或控制台编码误判。
- 对 GitHub 上的中文正文，不能只看本地终端输出；应优先用 UTF-8 方式读取本地文件，并在发布后通过 `gh pr view --json body`、`gh issue view --json body` 或 GitHub API 回读确认正文内容正常，再视需要刷新网页复核。
- 以上两类常用流程的仓内共享版 skill 放在项目根目录 `skills/`。更新流程时，以物理机工作流为准同步维护说明。

## GitHub 环境补充约束

- 物理机 `opengauss-ad` 上的 GitHub CLI 已确认可用，但不要假设 `gh` 一定在默认 `PATH` 里。
- 当前已验证可用的 `gh` 路径是：`/home/ad/.local/bin/gh`。
- 通过 `ssh opengauss-ad '...'` 执行非交互命令时，常见情况是：
  - `command -v gh` 为空；
  - 但 `/home/ad/.local/bin/gh` 实际存在且可执行；
  - 因此不能仅凭一条 `gh: command not found` 就判断“物理机没有安装 gh”或“GitHub 授权失效”。
- 只要任务涉及 GitHub 提交、push、PR 创建、PR 更新、PR 检查，默认先执行下面这组探测，再决定后续操作：
  1. `ssh opengauss-ad 'echo PATH=$PATH; command -v gh || true; ls /home/ad/.local/bin/gh 2>/dev/null || true'`
  2. 若 `command -v gh` 为空但绝对路径存在，则后续统一直接调用 `/home/ad/.local/bin/gh ...`
  3. 再执行 `/home/ad/.local/bin/gh auth status`
- 物理机上第一次做 git commit 前，还必须检查 git 身份是否已配置：
  - `git config --global --get user.name`
  - `git config --global --get user.email`
- 如果上述值为空，默认配置为：
  - `git config --global user.name 'andy123552'`
  - `git config --global user.email 'andy123552@users.noreply.github.com'`
- 后续若再次出现“gh 找不到”，优先归因于 `PATH` 差异，而不是直接归因于：
  - 没安装 `gh`
  - GitHub 未登录
  - 之前记录的发布流程失效
