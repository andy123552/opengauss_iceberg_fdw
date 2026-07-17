# 基于引用证明与发布栅栏的湖表—索引协同 GC 创新方案

## 摘要

本方案面向 openGauss、Iceberg Catalog、Rust SDK、桥接层和自定义索引共同组成的湖表系统，提出一套
**Reference-Provenance-Aware GC Control Plane（引用证明感知的 GC 控制面）**。它不把垃圾回收理解为
“扫描目录并删除旧文件”，而是把每次删除转化为一个可验证的证明问题：哪个状态图证明该文件不再可达、哪个组件
拥有发布状态的权限、以及在哪个提交栅栏之后删除才安全。

方案将数据文件、delete 文件、manifest、manifest-list、Iceberg statistics、table metadata json、索引
registry Puffin 和索引 segment 分为不同的 artifact domain；每个 domain 使用独立的 live/protected set 和删除
证明规则。对于会改变 Iceberg 表状态的 snapshot expiration，系统采用 `prepare -> Catalog CAS ->
recompute-and-delete` 三段式协议：SDK 只生成候选 metadata，Catalog 是唯一发布 metadata pointer 的组件，物理
删除只能在 CAS 成功后依据新旧引用差集重新计算。

关键创新不是再增加一个统一 `gc` 命令，而是把“引用可达性、artifact 归属、metadata 发布、物理删除和索引
生命周期”组合为一个可审计、可编排、默认 fail-closed 的维护控制面。它既保留 Iceberg snapshot isolation、
time travel、branch/tag/reference 的语义，也让项目自定义索引的 registry 与 segment 生命周期不被通用
orphan-cleanup 误删。

## 现状

Iceberg 原生维护能力已经区分了两类重要问题：`remove_orphan_files` 清理未被有效 metadata 引用的物理残留，
`expire_snapshots` 则通过修改 table metadata 过期历史 snapshot，并删除只被过期 snapshot 独占引用的文件。
这两者的正确性依据不同：前者依赖目录扫描、引用全集和安全窗口，后者依赖新旧 snapshot 的引用差集。

在单一 Iceberg 引擎中，表 metadata、提交协议和文件删除通常位于一个控制域内。但在本项目中，状态跨越多个
边界：openGauss Catalog 保存当前 `metadata_location`，bridge 负责 C ABI，Rust SDK 负责解析和重写 Iceberg
metadata，自定义索引还会产生 registry Puffin 和 segment artifact。索引文件同时受 snapshot、branch、tag、
reference 和 index registry 的共同影响，不能套用“当前 metadata 未引用即删除”的普通规则。

如果把所有文件交给一个目录扫描型 GC，会出现三个问题：

- 无法区分从未提交的写入残留和曾被历史 snapshot 引用的数据文件；前者需要安全窗口扫描，后者需要 snapshot
  过期后的引用差集。
- 无法可靠判断旧 metadata json、普通 Iceberg statistics Puffin 与项目 index registry Puffin 的删除权限。
- 无法在并发维护或写入时保证“删除依据仍然对应当前已发布状态”；SDK 直接删除或 Catalog 先删后发都会形成
  数据损坏窗口。

因此，缺口并非缺少某个文件删除 API，而是缺少跨 Catalog、SDK 和索引系统的一致 GC 控制面：系统必须同时知道
“文件属于哪个生命周期域”“谁能发布状态”“哪个引用图是删除证明”，才能安全回收。

## 为什么要做协同 GC

第一层原因是正确性。湖表的物理路径不是逻辑可达性的唯一证据。当前 snapshot、历史 snapshot、branch、tag、
reference、metadata log 和 index registry 都可能使一个文件仍然合法可读。误删的后果不是一次任务失败，而可能是
time travel、rollback 或索引扫描在未来才暴露的不可恢复损坏。

第二层原因是并发安全。写入、优化、索引构建和维护任务可能同时进行。目录扫描得到的候选集在删除前可能已经被新
metadata 引用；反之，prepare 阶段看到的过期候选在 CAS 失败后没有删除资格。因此必须把状态发布与物理删除隔离，
并以已发布 metadata 为最终事实来源。

第三层原因是可运营性。GC 常被视为后台清理，但生产环境需要回答：为什么删除这个文件、由哪个规则决定、哪些文件
被跳过、哪些失败可重试、一次维护释放了多少数据与索引空间。将证明、计划、执行和回执结构化，才能支持 dry-run、
审计、限流、告警和逐步自动化。

第四层原因是扩展性。后续引入更多索引类型、Delta 写入、统计文件或新的表格式时，不应不断扩展一个脆弱的
“目录后缀白名单”；而应为新 artifact 声明其归属、live set 来源、删除前置条件和发布栅栏，接入同一控制面。

## GC 方案

### 1. 总体定位

方案定位为“引用证明感知的维护控制面”，位于 Catalog 状态、Iceberg metadata、对象存储和索引引擎之间：

```text
Catalog metadata_location -- CAS publish --> authoritative table state
        |                                      |
        v                                      v
GC planner <--- artifact provenance ---- Iceberg metadata / index registry
        |
        v
dry-run plan -> execution fence -> revalidated physical deletion -> audit receipt
```

它不替代 Iceberg 的 metadata 语义、不实现查询执行，也不把 Catalog 变成对象存储扫描器。Catalog 负责状态发布与
编排；SDK 负责 metadata 解析、引用集计算和候选 metadata 生成；bridge 负责 ABI 边界；索引引擎负责解析可证明
归属的 registry/segment；存储层只在最终删除阶段执行经过验证的物理操作。

### 2. 核心对象

`ArtifactDomain` 描述文件的生命周期域。初始域包括 `iceberg_orphan`、`table_metadata`、
`expired_snapshot_refs` 和 `index_artifact`。一个文件只能在可证明归属的 domain 内成为候选；归属未知时必须跳过。

`ReferenceProof` 描述删除所依赖的证据集合。它包含 base metadata location、table UUID、受保护 references、
live files、路径规范化结果、mtime 安全窗口和 artifact-specific 的归属校验结果。

`GCPlan` 是一次维护的不可变计划，包含操作类型、dry-run 标志、候选文件、跳过原因、预估回收空间和需要的
publication fence。它用于展示和审计，不直接授予删除权限。

`PublicationFence` 表示状态提交边界。目录扫描型操作不需要更新 metadata pointer，但必须在删除前 re-stat 和重验
证据；`expire_snapshots` 必须使用 Catalog CAS 作为 fence，CAS 成功后才能获得物理删除资格。

`GCReceipt` 是执行回执，记录最终 metadata pointer、成功删除、NotFound 幂等结果、跳过项、失败项、释放字节数和
每一项对应的 `reason`。它让失败可观测、可重试，而不撤销已经成功发布的 metadata。

### 3. 按证明域拆分维护动作

| 动作 | 证明来源 | 可删范围 | 不可删边界 |
| --- | --- | --- | --- |
| `remove_orphan_files` | 有效 metadata 引用全集 + 安全窗口 | 从未被有效 metadata 引用的 data/delete、manifest、manifest-list、statistics 残留 | metadata json、index artifact、窗口内文件、归属未知文件 |
| `cleanup_old_metadata` | current pointer + metadata log + previous log + `retain_last` | 已验证 table UUID 的旧 metadata json | current/log 保护文件、路径越界或解析失败文件 |
| `vacuum_index` | retained snapshot/reference 可达 registry + segment 归属 | 已证实不再 live 的 registry Puffin 与 segment | 普通 Iceberg statistics Puffin、index 归属不明文件 |
| `expire_snapshots` | 过期策略 + retained snapshot/reference + `old_refs - new_refs` | 过期 snapshot 独占的 data/delete、manifest、manifest-list、statistics | 新 metadata 或 retained reference 仍可达文件、所有 index artifact |

这种拆分的关键是：接口边界按“删除事实由什么证明”划分，而不是按调用方便程度或路径后缀划分。这样既能独立审计，
也能让上层按需组合，而不会在一个操作中混入互相冲突的保留策略。

### 4. CAS 发布栅栏

会改变 snapshot 可达性的维护采用以下协议：

```text
1. Catalog read base_metadata_location
2. bridge/SDK prepare new metadata and summarize expired/retained state
3. Catalog CAS: update pointer only when it still equals base_metadata_location
4. Reload base/new metadata and recompute old_refs - new_refs
5. Delete only the recomputed candidates; emit receipt
```

步骤 2 的候选只能用于 dry-run、容量预估和审计；步骤 4 的重算结果才是最终删除授权。CAS 失败时，新 metadata
保留给后续受控清理，且不得删除任何数据文件。CAS 成功后，个别文件删除失败应记入 `GCReceipt.failed`，但不得反向
回滚 metadata pointer，因为已经发布的逻辑状态才是新的权威状态。

### 5. 索引生命周期协同

自定义索引不属于 Iceberg 原生 snapshot 文件集合。索引 registry Puffin 和 segment artifact 可能服务于 retained
snapshot、branch/tag/reference 或历史查询，因此不能随 `expire_snapshots` 的文件差集被隐式删除。

Catalog 可在 snapshot 过期发布成功或 no-op 成功后，再编排 `vacuum_index`：索引引擎先计算 `live_index_files`，
再以 table UUID、Puffin blob type、`index_name` 和路径边界验证候选。这个顺序让湖表状态演进与索引空间回收保持
协同，同时保留各自独立的正确性证据和失败处理。

### 6. 默认安全策略

- 所有接口默认 `dry_run=true`，默认安全窗口为 7 天。
- 删除前重新 `stat`，并复查 mtime、URI scheme/authority/normalized path、table UUID 和 artifact 归属。
- `NotFound` 视为幂等成功并记录原因；权限、网络或对象存储错误进入 `failed`，可重试。
- 无法解析 metadata、Puffin 或 URI 时 fail-closed：跳过并返回证据不足原因，绝不猜测删除。
- `GCPlan` 和 `GCReceipt` 以 JSON 返回，支持调用方比较 dry-run 与实际执行、记录审计日志和统计回收收益。

### 7. 分阶段落地

第一阶段建立目录扫描型维护闭环：实现 orphan、旧 metadata 与索引 artifact 三个独立动作，统一输出候选、跳过和
失败回执。目标是验证 domain 拆分、安全窗口、路径边界和归属校验。

第二阶段接入 metadata 重写与 CAS：实现 `expire_snapshots` 的 prepare、Catalog 条件发布和 delete-after-CAS，验证
并发 CAS 失败不删除、成功后差集删除以及 branch/tag/reference 保护。

第三阶段建立组合编排：提供 `gc_table` 等组合入口，按 `expire_snapshots -> remove_orphan_files ->
cleanup_old_metadata -> vacuum_index` 的依赖顺序执行，并在每个 step 保留独立回执。目标是让运维获得一个入口，
而不牺牲底层动作的可解释性。

第四阶段走向自动治理：基于 `GCReceipt` 聚合 orphan 增长、回收字节、失败率、dry-run 与实际差异、索引空间占比
等指标；再引入计划任务、租户配额、速率限制、异常告警和按表策略。自动化只应在 dry-run 证据、权限和安全窗口满足
时逐步开启。

### 8. 可观测指标

- 每个 `ArtifactDomain` 的 candidate、deleted、skipped、failed 数量与字节数。
- CAS 冲突率、prepare-to-publish 时延、delete-after-CAS 时延。
- 由于安全窗口、路径越界、UUID 不匹配、Puffin 不可证明归属而跳过的比例。
- orphan 增长速率、metadata 文件保留量、index registry/segment 空间占比。
- dry-run 候选与实际删除差异；差异过大通常提示并发写入、路径异常或策略配置问题。
- 维护操作对查询、写入、索引构建和对象存储 API 的延迟与错误率影响。

### 9. 风险与边界

本方案不能把“未在当前 metadata 中出现”当作删除证据。历史 snapshot、reference、metadata log 和自定义索引
registry 都可能使物理文件仍然有效；所有 domain 必须使用自己的完整证明集。

本方案也不应把 GC 与事务回滚混为一谈。CAS 以前的 metadata 仍是权威状态；CAS 以后的单文件删除失败是可恢复的
物理清理问题，不应回滚已发布的逻辑表状态。

最后，自动 GC 不应默认全量扫描或立即删除。对象存储列表成本、海量表扫描、未提交写入和跨引擎并发都会放大风险。
推荐从 dry-run、显式运维调用和稳定热表开始，积累 `GCReceipt` 与失败模式后再逐步自动化。
