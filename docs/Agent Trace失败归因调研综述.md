# Agent Trace 失败归因调研综述

> 覆盖范围：归因问题定义、现有 Trace 基础设施、Who&When、Who&When Pro，以及归因方法现状。

# 1. Agent Trace 故障归因：概念、因果标准与金标

## 1.1 核心结论

Agent Trace 归因要回答的是：**哪个 agent（Who）在什么步骤（When）做了决定性错误，从而使任务失败。**

其中“决定性”不是指动作看起来最可疑，也不是任务最后报错的位置；它要求一个反事实条件：**若把该动作改正，任务应从失败变为成功。**

## 1.2 决定性错误与 Who/When

### 1.2.1 归因对象与反事实定义

##### 给定一条最终失败的 agent 执行轨迹：

```text
Agent A 在 step 1 行动
Agent B 在 step 2 行动
…
任务最终失败
```

###### 若把 agent `i` 在 step `t` 的动作改正，并让任务继续执行后，任务由失败变为成功，则：

```text
Who  = agent i
When = step t
```

这对 `(agent i, step t)` 就是**决定性错误**（decisive error）。

这一反事实定义来自 [Who&When 的问题形式化](https://arxiv.org/html/2505.00212v3)。

### 1.2.2 决定性错误、症状与最终失败位置

| 位置 | 含义 | 能否直接作为 When |
|---|---|---|
| 最终失败位置 | 最终答案错误、任务超时或环境判定失败的位置 | 否，它只说明失败最终显现在哪里 |
| 症状步骤 | 已受前序错误影响、后续表现异常的动作 | 否，单独修正它不一定能恢复任务 |
| 决定性错误步骤 | 改正该动作后，任务可以恢复成功的动作 | 是，Who&When 中的 When |

**例子**：Planner 在 step 3 漏掉关键约束；Executor 在 step 8 按错误计划调用工具；Writer 在 step 10 输出错误结果。若只要改正 step 3，任务便会成功，那么：

```text
Who   = Planner
When  = step 3
step 8、step 10 = 后续症状或失败显现点，不是主因
```

### 1.2.3 单一主因约定的边界

一条真实轨迹可能有多个错误，甚至有多个动作各自改正后都能挽救任务。为了形成可评分的单一答案，Who&When 选择**最早的决定性错误**作为 `primary_cause`。

这是一项 benchmark 约定：它便于统一评测，但不表示现实中只有一个、绝对唯一的根因。

---

## 1.3 金标、反事实与任务成功 oracle

### 1.3.1 概念示例

假设任务是计算 `12 + 7`；独立评分脚本规定，最终提交 `19` 才算成功。

| 名称 | 在例子中的含义 |
|---|---|
| 任务成功 oracle | 评分脚本：答案等于 `19` 返回成功，否则返回失败。这里的 oracle 可以理解成“裁判/判定器”。 |
| 成功种子轨迹 | Calculator 在 step 2 得到 `19`，Writer 提交 `19`，oracle 判定成功。 |
| 单点注错 | 保持成功轨迹的前缀不变，只把 Calculator 的 step 2 改为产生 `20`，再让后续步骤继续执行。 |
| 反事实证据 | 原 step 2 对应成功；只替换 step 2 后失败。说明这处受控变化导致结果从成功变为失败。 |
| 金标（gold label） | 保存为评测参考答案的 `{ Who: Calculator, When: step_2, What: calculation_error }`。 |

三者的关系是：

```text
反事实证据：为什么可以相信这份标签
        ↓
金标：benchmark 保存的隐藏参考答案
        ↓
评测：模型预测 Who/When/What 是否与金标一致
```

**金标不是 oracle 的替代品。**oracle 判定任务是否成功；金标记录“本样本应定位到谁、哪一步”。反事实实验与 oracle 一起为金标提供可信依据。

### 1.3.2 离线评测流程

评测时只有四步：

```text
1. benchmark 将“失败轨迹”给归因模型
2. 模型输出预测：{ Who, When, What, Evidence }
3. benchmark 不向模型暴露金标
4. 评测器将模型预测与隐藏金标逐项比较并计分
```

例如金标是：

```text
{ Who: Calculator, When: step_2 }
```

模型预测相同，Who 与 When 均得分；若模型预测 Writer / step 3，则没有命中金标。这个离线步骤只是在“阅卷”，不会为了每一个模型预测再运行一次任务。

### 1.3.3 自然失败与单点注错的金标来源

| 对比项 | Who&When：自然失败 | Who&When Pro：单点注错 |
|---|---|---|
| 样本起点 | agent 在真实任务运行中自然失败的轨迹 | agent 已经成功完成任务的轨迹 |
| 金标来源 | 多位专家读完整轨迹，标出主责 agent、决定性步骤与原因；讨论达成共识并交叉检查 | 重放成功前缀，只注入一个合理错误，继续运行；保留 oracle 判为失败的轨迹，注错点即为 Who/When 的受控标签 |
| 反事实证据 | 主要来自专家根据轨迹作出的判断；通常不逐样本重跑“改正该步骤”的替代轨迹 | 直接来自成功种子与“只改一处后失败”的受控对照 |
| 优点 | 更接近真实失败，保留自然的复杂性与多错误现象 | 因果来源清楚、规模化方便、标签的一致性更强 |
| 局限 | 对精确主因的判断可能有歧义；因果验证强度通常较弱 | 单个受控错误不能完整代表真实世界的多因、偶发或环境相关失败 |

Who&When 使用三位专家的多轮共识与交叉检查来构造自然失败标签；Who&When Pro 则从成功种子 warm-start，注入一个错误后续跑，并只保留最终失败的轨迹。两者都把“最早决定性错误”作为主因，但金标的证据来源不同。[Who&When 标注流程](https://arxiv.org/html/2505.00212v3) [Who&When Pro 注错与重放流程](https://arxiv.org/html/2607.09996v1)

### 1.3.4 两类金标的互补性

不要简单说“自然失败金标不可信、Pro 金标可信”。更准确的结论是：

```text
自然失败金标：自然真实性更强；受控因果证据通常较弱
单点注错金标：受控因果证据更强；真实故障复杂性较弱
```

因此，后续 benchmark 应分开报告两类数据的结果，不能把它们混为一个总分后直接比较。

# 2. Who&When：自然失败轨迹上的归因基准

## 2.1 核心结论

Who&When 是自动 Agent Trace 失败归因的起点：它收集**自然发生的失败轨迹**，由专家标出 `Who`、`When` 和自然语言原因。它证明“从失败日志自动找根因”非常难——识别责任 agent 尚可，精确定位错误步骤远未达到实用水平；但它也提供了最接近真实失败形态的首批评测数据。

论文：[Which Agent Causes Task Failures and When?](https://proceedings.mlr.press/v267/zhang25cq.html)（ICML 2025）｜数据卡：[Hugging Face Who_and_When](https://huggingface.co/datasets/Kevin355/Who_and_When)

---

## 2.2 归因任务定义

**输入**：一个任务问题与一条完整的、最终失败的多 agent 轨迹。

**输出**：

```text
Who   = 首要责任 agent
When  = 最早决定性错误步骤
Why   = 该步骤导致失败的自然语言原因
```

这里的 `When` 仍遵循第 1 阶段的定义：改正该 agent 在该步骤的动作后，任务应能从失败转为成功；多个候选存在时，论文选择最早者作为主因。

## 2.3 数据来源与 Agent 系统构造方式

总计 184 条失败归因任务，来自 GAIA 与 AssistantBench 上运行的 127 个多 agent 系统。两种 split 的区别是**agent 系统的构造方式**：

| 构造方式 | Algorithm-Generated | Hand-Crafted |
|---|---|---|
| 系统如何得到 | CaptainAgent 根据当前任务自动组建适合的 agent 团队、角色提示词和工具组合 | 预先人工设计好一套通用的多 agent 系统，再用同一套系统处理不同任务 |
| 代表 | CaptainAgent：AG2 库中的 agent-system construction algorithm；AG2 是承载多 agent 协作的框架/库 | Magentic-One：预先设计的通用多 agent 系统，具有相对固定的角色和协作方式 |
| 样本数 | GAIA 98 条 + AssistantBench 28 条 = 126 条 | GAIA 30 条 + AssistantBench 28 条 = 58 条 |
| 轨迹长度 | 5–10 步 | 5–130 步 |
| 两者是否测同一任务 | 是。都给归因模型失败轨迹，要求预测 Who / When，并用相同的 Agent 与 Step 指标评分 | 是；但系统拓扑、agent 数和轨迹长度不同，因此结果必须分 split 解读 |

两类样本都经过人工归因标注。这里的 Hand-Crafted **不**是人工写日志或人工标注；它只描述 agent 系统本身是预先手工工程化的。CaptainAgent 与 Magentic-One 也不处于完全相同抽象层：前者是基于 AG2 的“按任务生成系统”的构造算法，后者是一个已经构造好的 agent 系统。

单条样本包含任务问题、完整失败 history、系统/agent 信息以及归因标签。

在当前 `trace-synthesis` 中，这些标签可见为 `failure.mistake_agent`、`failure.mistake_step`、`failure.mistake_reason`、`failure.ground_truth`，并能定位到 `failure.root_span`。

## 2.4 专家共识金标的构造

Who&When 的金标来自**专家审阅自然失败轨迹**，而非单点注错。

```text
失败日志
  → 专家阅读任务、agent 行为与工具使用过程
  → 标出首要错误 agent、错误步骤、自然语言理由
  → 标记确定 / 不确定
  → 对不确定样本讨论直至形成共识
  → 专家交叉复核并在必要时重标
  → gold label
```

论文采用三位 AI agent 专家、三轮流程：首轮按统一指南标注；对不确定项讨论至共识，而非简单多数投票；最后交叉验证其他专家的标注。三位专家的标注工时分别为 30.9、30.2、23.2 小时；初始不确定比例约为 15%–30%。这正说明自然失败的根因判断有实际难度和主观边界。[论文的标注过程](https://arxiv.org/html/2505.00212v3)

**怎样理解可信度**：这是一份专家共识金标。专家可基于轨迹进行反事实推理，但论文的自然失败标签通常没有逐样本执行“改正候选步骤并重跑”的受控验证。因此，它具有较高的自然真实性，但其因果证据强度弱于 Who&When Pro 的受控单点注错标签。

## 2.5 评测指标与信息条件

| 指标 | 判对条件 | 测量的能力 |
|---|---|---|
| Agent-Level Accuracy | 预测 agent 与金标 `mistake_agent` 完全一致 | Who 是否找对责任主体 |
| Step-Level Accuracy | 预测步骤与金标 `mistake_step` 完全一致 | When 是否精确定位 |
| Step-Level Accuracy with Tolerance（Step@±k） | 预测步骤落在金标步骤的 ±k 窗口内 | 已接近正确位置但未精确命中的程度 |

它没有受控、统一的错误 taxonomy，因此不像 Pro 那样以 Error Mode Macro-F1 作为核心指标；自然语言 `mistake_reason` 主要是解释与标注依据。

**Step@±k 的实际结果**：论文在 Hand-Crafted 长轨迹上报告了下表。`±1` 表示预测 step 只要与金标相差不超过 1 即判对；窗口更大时，分数只说明模型落在更宽的区域内，不能替代精确定位。

| 容忍窗口 | All-at-Once | Step-by-Step | Binary Search |
|---|---:|---:|---:|
| ±1 | 12.07% | **14.66%** | 13.79% |
| ±2 | **19.83%** | 16.38% | 18.97% |
| ±3 | **30.17%** | 18.10% | 22.41% |
| ±4 | **37.07%** | 31.90% | 31.89% |
| ±5 | **43.10%** | 33.62% | 36.21% |

**一个容易混淆的词：With Ground Truth**。这里给归因 LLM 的是**任务最终正确答案**，例如“正确答案应该是 19”，而不是 `mistake_agent` 或 `mistake_step` 金标。这个最终答案可被任务成功 oracle 用来判定成败，但它**不是 oracle 本身**：oracle 是执行“答案是否正确”判断的评分脚本、环境或人工规则。

它模拟开发团队调试已标注 benchmark 时的条件：开发者知道任务目标答案，可以让归因 LLM 对照该答案检查轨迹。真实线上失败通常没有现成答案，因而更接近 Without Ground Truth。两种条件提供给模型的信息量不同，分数不能混在一起解释。

## 2.6 基线方法与实验结果

| 方法 | 做法 | 主要优点 | 主要缺点 |
|---|---|---|---|
| All-at-Once | 一次读取完整失败日志，直接输出 Who 与 When | 全局上下文完整，Who 表现最好 | 在长日志中很难从大量步骤中精确找出 When |
| Step-by-Step | 从前往后逐步读日志，首个判错步骤即停止 | 精确 When 通常更好 | 容易过早锁定可疑步骤，成本高且缺少后续上下文 |
| Binary Search | 不断判断错误在日志前半还是后半，缩小区间 | 上下文与调用次数折中 | 二分时可能切掉关键上下文，整体表现居中 |

论文的“最佳”不是同一种方法同时取得两个分数，而是按子任务分别取优：

```text
Who（Agent Accuracy）最佳：All-at-Once，53.5%
When（精确 Step Accuracy）最佳：Step-by-Step，14.2%
```

因此不存在一个同时最擅长 Who 和 When 的单一基线。核心模式是：

```text
找 Who：需要理解全局协作关系 → All-at-Once 更有利
找 When：需要从很长轨迹定位一个步骤 → Step-by-Step 往往更有利
```

在 Hand-Crafted 数据中，论文把**已经完整记录的失败轨迹**按自身长度分成 5–17、19–29、31–49、51–91、93–130 步五档；对每条样本，归因模型仍读取该样本的完整日志，模型的最大上下文窗口并没有被人为调大或调小。随着“待诊断的完整轨迹本身”变长，三种方法的指标都会下降，精确步骤定位尤其敏感；最长一档的结果接近 0。

因此结论是：即使模型的上下文窗口**足以容纳、不发生截断地读取**整条更长的 Trace，也不代表模型能从更多正确动作、后续症状和干扰信息中可靠检索出一个早期决定性步骤。它不是说“给模型更长的上下文窗口反而更差”，也不是说长轨迹日志不完整；实验中比较的是不同长度的完整失败轨迹。Algorithm-Generated 的最长轨迹仅 10 步，无法形成有意义的长度分桶，因此论文没有对它进行同样的长度实验。[实验与指标](https://arxiv.org/html/2505.00212v3)

### 2.6.1 随机基线及其解释

随机基线是**完全不读任务和轨迹**的盲猜参照：对每条样本，随机选一个候选 agent 或一个候选步骤，再统计平均命中率。候选越少、轨迹越短，随机分数越高：

```text
Algorithm-Generated：短（5–10 步）→ 随机猜 Step 也有 19.06%
Hand-Crafted：长（最长 130 步）→ 随机猜 Step 只有 4.16%
```

“低于随机”只表示在**该方法 + 该 split + 该信息条件 + 该指标**的组合中，模型的定位还不如盲猜；不能据此说整个模型或整个 benchmark 都失效。论文表 1 中低于随机的组合为：

| split / 条件 | 低于随机的结果 | 对应随机基线 |
|---|---|---|
| Algorithm-Generated，With Ground Truth | All-at-Once 的 Step = 12.50% | Step 随机 = 19.06% |
| Algorithm-Generated，Without Ground Truth | All-at-Once / Step-by-Step / Binary Search 的 Step 分别为 13.53% / 15.31% / 16.59% | Step 随机 = 19.06% |
| Algorithm-Generated，Without Ground Truth | Step-by-Step 的 Agent = 26.02% | Agent 随机 = 29.10% |
| Hand-Crafted，Without Ground Truth | All-at-Once 的 Step = 3.51% | Step 随机 = 4.16% |

这些结果说明：某些信息条件下，协议会使 LLM 在精确步骤定位上产生系统性偏差，例如全量日志中的显眼后续症状掩盖早期根因，或逐步扫描过早停止。它们是需要改进的诊断信号，而非“这些样本本身会误解模型”的结论。

## 2.7 基准价值与适用边界

| 维度 | 带来的价值 | 对应的边界 / 代价 |
|---|---|---|
| 失败来源 | 自然失败保留真实的错误传播、协作失配和多错误现象 | 仅 184 条、两个任务来源与文本多 agent，覆盖范围有限 |
| 标签方式 | 专家理由与共识可支持错误机制分析 | 通常没有逐样本受控反事实重跑，因果证据强度有限 |
| 根因定义 | 明确将 Who 与精确 When 分为两个不同难度的子任务 | 用“最早主因”压缩了多因失败，不能表达完整因果图 |
| 研究用途 | 即使实例级分数有限，汇总后仍可观察高频责任 agent | 样本全是失败轨迹，不能单独评价模型面对成功轨迹时会不会误报错误 |

### 2.7.1 实例级定位与聚合级责任排序

| 层次 | 怎么计算 | 能回答什么 | 不能说明什么 |
|---|---|---|---|
| 实例级归因 | 对每一条 Trace，把预测 Who / When 与该条的金标比较；Agent Accuracy、Step Exact、Step@±k 都由这些逐条结果汇总 | “这一次失败，模型找对主责 agent 或决定性步骤了吗？” | 自然失败中，即使命中专家金标，也不自动等于已经通过受控重跑证明因果关系 |
| 聚合级责任排序 | 跨许多 Trace 统计各 agent 被标为或被预测为主因的次数，按次数由高到低排序，再与金标排序比较 | “在这一套系统中，哪个 agent 最常与失败相关，优先排查谁？” | 不能替代逐条 Who / When 的精确定位，也不能证明某一条具体失败的根因 |

论文观察到：虽然单条失败的 Who/When 预测并不稳定，但在 Hand-Crafted 系统上聚合后，模型预测出的最高频责任 agent 与金标的最高频 agent 一致。这是**统计级的排查优先级信号**，不是对某一条失败的因果证明。

# 3. Who&When Pro：受控注错、多模态数据与 What 评测

## 3.1 核心结论

Who&When Pro 从一条已验证成功的轨迹出发，只在一个受约束的位置制造错误，再让原系统继续执行。只保留最终任务失败的结果，因此每条轨迹都有可追溯的 Who、When、What 标签。

| 问题 | 标签 | 标签来源 |
|---|---|---|
| 谁造成决定性错误（Who） | agent | 被注错动作所属的 agent |
| 哪一步造成决定性错误（When） | step，或框架自己的坐标 | 被替换动作的位置 |
| 错误属于什么类型（What） | mode | 18 类 taxonomy 中的注错模式 |

它比自然失败的专家事后标注更容易追溯标签来源，但仍是“单个受控错误导致失败”的简化实验，不能完全代表真实系统的多因故障。

论文：[Who&When Pro: Can LLMs Really Attribute Failures in AI Agents?](https://arxiv.org/abs/2607.09996)  
本阶段数据源：[Leoxx/whowhen_pro](https://huggingface.co/datasets/Leoxx/whowhen_pro)

---

## 3.2 Hugging Face 数据：四个 split 与字段结构

以下结论来自对 Hugging Face 当前 revision 0bd196c8 的实际 JSONL 与视频资源目录抽样读取。

| split | 当前文件大小 | 媒体的实际存储方式 | 典型任务 |
|---|---:|---|---|
| text | 75.6 MB | 无图片/视频 payload | 问答、编程、数据分析等 |
| image | 6.79 GB | 图片以 Base64 字符串嵌在 JSON 中 | 图像、图表、视觉检索 |
| image_gui | 11.19 GB | GUI 截图以 Base64 字符串嵌在轨迹观察中 | 网页 / GUI 操作 |
| video | 21.35 MB | JSON 只保存视频元数据；采样帧是仓库内独立 JPEG 文件 | 长视频理解 |

因此，image 和 image_gui 的多模态输入是真正内嵌的数据 payload；video 的视频帧不内嵌在 trajectory 中。

### 3.2.1 原始记录的公共顶层字段

实际 JSONL 的顶层统一为下列七项；其中 task、trajectory、ground_truth、extras 在原始文件中是 JSON 字符串，读取后需要再解析一层。

| 顶层字段 | 解析后内容 | 用途 |
|---|---|---|
| id | 字符串 | 轨迹唯一标识 |
| framework | 字符串 | 产生轨迹的 agent 框架，例如 smolagents、agentoccam、dvd |
| benchmark | 字符串 | 原始任务 benchmark，例如 browsecomp_vl、webvoyager、lvbench |
| task | 对象 | 至少有 query 与 answer；image 样本还可有 images |
| trajectory | 事件数组 | 用户消息、agent 输出、工具调用、工具观察、规划、轮次等 |
| ground_truth | 对象 | 隐藏的 Who / When / What 参考答案 |
| extras | 对象 | 框架特有元数据，如 agents、执行状态、video_context |

| 内容 | 是否给归因模型 | 原因 |
|---|---|---|
| task.query、trajectory、必要媒体 | 给 | 这是模型诊断失败所依据的证据 |
| framework、benchmark、agent roster | 可给 | 是已知运行上下文，也可用于分组 |
| task.answer | 默认不给；单列 with-task-answer 实验 | 它是任务正确答案，不是归因金标；给它会改变诊断条件 |
| ground_truth | 不给 | 它就是评测器比对预测的隐藏参考答案 |

### 3.2.2 image 与 image_gui：内嵌 Base64 图片

实际 image 样本的 task.images、trajectory 中的 user.images 或 action.observation_images 都是类似对象：

    {
      "__type__": "PIL.Image",
      "data": "<Base64 编码的图片二进制>"
    }

抽样 image 记录中，一张图片的 data 长度为 2,306,764 个字符；Base64 解码后为 1,730,072 字节，文件头为 PNG。image_gui 的轨迹观察截图结构相同，例如一张截图 Base64 长度 1,060,040，解码后为 PNG。

这解释了两个 split 的 JSONL 为什么很大：图片不是一个外部 URL，同一任务图像还可能同时出现在 task、用户输入或后续观察中。

### 3.2.3 video：元数据与独立帧资源

实际 video 记录的 extras.video_context 包含：

    video_id
    video_duration_seconds
    time_reference
    question_type
    video_type

例如第一条 LVBench / DVD 样本的 video_id 为 1FsiZgGZU70。它的 trajectory 是普通 system / user / assistant / tool 事件，工具观察主要是文本摘要，不含 Base64 视频或帧。

对应的视觉帧存放在同一 Hugging Face 仓库的独立目录：

    video_assets/dvd/assets/frames/<video_id>/<asset-id>/frame_0000.jpg
    video_assets/eva/assets/frames/<video_id>/<asset-id>/frame_0000.jpg

这些是独立 JPEG 文件，不在 video.jsonl 内。严格说，video JSON 记录的是 video_id、时段等元数据，**不是直接的帧文件路径**；数据接入代码需要用 video_id 和仓库既定目录规则去解析 video_assets。只读取 video.jsonl 只能得到文字轨迹和 video_id，不能让归因模型看到视频视觉证据。

## 3.3 ground_truth：跨框架的 Who、When 与 What

所有框架都有 mode；它是 18 类错误模式中的一个代码，例如 R.1、PL.2、C.3。Who 和 When 的坐标则取决于 agent 框架如何组织执行过程。

| 标签含义 | 可能字段 | 怎样理解 |
|---|---|---|
| What | mode | 所有框架都有，例如 A.3 是输出格式错误 |
| Who | agent，或 agents[0] | 多 agent 中的责任 agent；单 agent 中可为 null，因为 Who 没有区分度 |
| When：线性动作 | step | 第几个 action |
| When：轮次 | round | 第几轮辩论 / 协作消息 |
| When：轮内位置 | position | 某轮中的第几个 agent / turn；要与 round 一起使用 |
| When：管线阶段 | stage | 顺序 pipeline 的第几个角色阶段 |

可以出现看上去像小数的坐标，例如 Magentic-One 的 step: "7.1"；但它不是时间上的 7.1 步，也不应按数值近似比较，而是该框架的复合地址。更多情况根本不是小数，而是 round + position 或 stage。

| 框架示例 | ground_truth 示意 | 正确解读 |
|---|---|---|
| smolagents（单 agent） | agent: null, step: 2, mode: A.3 | 第 2 个动作被注错；不评 Who |
| Multi-Agent Debate | agents: [agent_1], round: 0, mode: R.4 | agent_1 在第 0 轮的输出是决定性错误 |
| Magentic-One | agent: coder, step: "7.1", mode: R.1 | coder 的复合步骤地址为 7.1，原样比较 |
| MacNet | agent: node_0, round: 0, position: 0, mode: R.4 | node_0 在第 0 轮、第 0 个位置出错 |
| MetaGPT | agent: architect, stage: 0, mode: R.4 | architect 所在的第 0 个 pipeline 阶段出错 |

因此 evaluator 不能只存 step:int。它应保留原始 ground_truth，并按 framework 建立统一比较键：

    who_id = agent 或 agents[0]
    when_key = { coordinate_type, step / round / position / stage }
    mode = 18 类代码

## 3.4 Warm Start 与受控单点注错流程

Pro 的构造顺序是：

    自然失败轨迹
      → 人工归纳错误 taxonomy，并记录各 framework / benchmark 中合理的错误位置
    成功 seed 轨迹
      → 选择与目标 mode 相容的位置 t
      → 在 t 让原 agent 产生错误动作
      → 从 t+1 继续执行，任务失败才保留
      → gold = { Who, When, What }

如果每次都从第 1 步重新执行，LLM 随机生成、网页内容或浏览器状态变化，可能使轨迹在 t 之前已经偏离原成功 seed。此时最终失败不能确定归因于 t。

warm start 就用在 t 之前：先把成功 seed 的前缀重放到原框架中，恢复“马上执行第 t 步”时的 context 与环境，再只改第 t 步。

    成功 seed: a1 → a2 → ... → a(t-1) → at → ... → 成功
                       │
                       │ warm start：重放成功前缀
                       ▼
    注错轨迹: a1 → a2 → ... → a(t-1) → ãt → 后续原系统续跑 → 失败
                                              ↑
                                   只在这里加入注错提示

| 环节 | Pro 如何做 | 目的 |
|---|---|---|
| 成功 seed | 保存动作、工具请求、工具观察和环境交互 | 提供已知能完成任务的参照 |
| 回放 t 前前缀 | 用原 agent framework 的正常 pipeline 重放 a1…a(t-1)，包括工具解析、代码执行和环境交互 | 重建第 t 步前的 context 与环境状态 |
| 静态工具 | 搜索、网页抓取、图像搜索等工具以规范化参数的 SHA-256 为键查询 SQLite cache，返回 seed 时保存的原始响应 | 看到字节级相同的工具观察，不受网页更新影响 |
| 有状态环境 | 浏览器点击、表单输入、代码执行等重新执行；每一次重放动作都做 fidelity check（状态一致性检查），若观察状态不能与 seed 对齐，就放弃这次注错 | 不让注错前的环境漂移污染标签 |
| 构造错误动作 | frontier model 读取任务、t 前上下文和目标 mode，生成 injection prompt；将它插入 t 的原 agent 模型调用，使 base agent 生成错误动作 ãt | 错误符合当时上下文与 agent 风格 |
| 后续续跑与过滤 | t+1 后保持原设置继续；仅保留任务 evaluator 判失败的轨迹；去掉提示泄漏和 t 前答案已明显可见的样本 | 确保注错真的改变任务结果，降低投机猜标签 |

## 3.5 18 类错误 taxonomy

taxonomy 先来自自然失败轨迹。研究者为每个 framework 与 benchmark 归纳合理的错误类型和可出现位置，再用它约束自动注错；自动生成不是任意捏造错误。

| Family | 代码与名称 | 含义 |
|---|---|---|
| Perception 感知 | P.1 视觉误识别 | 看错图中的对象、实体或文字，例如读错图表数值、OCR 出错、认错相似实体 |
|  | P.2 grounding / 落点错误 | 指向图像或 UI 的错误位置，例如点错按钮、框错目标、裁错区域 |
| Reasoning 推理 | R.1 幻觉 | 声称了工具观察中没有依据的事实，用编造内容或过时记忆替代证据 |
|  | R.2 推理错误 | 已有正确观察却用错信息，例如实体混淆、时间颠倒、比较关系反向、遗漏边界条件 |
|  | R.3 计算错误 | 算术、单位换算、计数、测量、off-by-one 或四舍五入出错 |
|  | R.4 任务误解 | 对任务范围或抽象理解错，例如答了另一个问题、选错算法框架 |
| Planning 规划 | PL.1 无效规划 | 高层策略不可行，例如前提错误、子目标拆分差、路线失败后不重规划 |
|  | PL.2 目标漂移 | 多步执行中逐渐偏向无关子目标，丢失原始任务目标 |
| Action 执行 | A.1 工具参数错误 | 工具存在但参数、调用格式或前置操作错误 |
|  | A.2 幻觉工具 / 动作 | 调用了不存在的工具、API 或不可能的动作 |
|  | A.3 输出格式错误 | 结构化输出、语法或最终答案格式不符合要求 |
|  | A.4 过早终止 | 任务未完成即返回，例如只给部分结果或应调用工具却不调用 |
|  | A.5 循环行为 | 重复相同或等价动作而没有进展 |
| Verification 验证 | V.1 上下文丢失 | 忘记先前观察或约束，例如上下文溢出、无法取回历史信息 |
|  | V.2 验证不足 | 未验证或验证方式错误，例如忽略矛盾证据、盲目信任结果、过度修正 |
| Coordination 协作（仅多 agent） | C.1 委派 / 编排错误 | 子任务派错角色、拆分不当、角色混乱或 agent 动作冲突 |
|  | C.2 沟通失败 | 未传递关键发现、忽略其他 agent 输入或丢失共享上下文 |
|  | C.3 过度依赖其他 agent | 自己本来正确，却因采纳另一 agent 的较差答案而改错 |

每条 Pro 样本只标一个 mode。这利于构造与重复评分，但真实故障可同时包含“规划错 → 工具参数错 → 未验证”，taxonomy 不是完整的多因因果图。

## 3.6 Who、When、What 与联合归因评测

| 指标 | 判对条件 | 评价能力 |
|---|---|---|
| Agent | 预测的责任 agent 命中金标；仅多 agent trace 有意义 | Who |
| Step | 预测的决定性步骤 / 框架坐标与金标精确一致 | When |
| Error Macro-F1 | 对 18 个 mode 的等权 F1 平均 | What |
| Joint | Who、When、What 同时正确；任一维错则该条不计入 | 完整归因 |

### 3.6.1 Error Macro-F1 与类别不均衡

对每一个 mode m 分别统计：

| 计数 | 意义 |
|---|---|
| TP | 金标是 m，模型也预测 m |
| FP | 金标不是 m，模型却预测 m |
| FN | 金标是 m，模型预测成其他类 |

先用 TP、FP、FN 算该 mode 的 Precision、Recall、F1；再对 18 个 mode 的 F1 等权平均：

    Precision(m) = TP / (TP + FP)
    Recall(m)    = TP / (TP + FN)
    F1(m)        = 2 × Precision(m) × Recall(m) / (Precision(m) + Recall(m))
    Error Macro-F1 = (F1(P.1) + F1(P.2) + ... + F1(C.3)) / 18

例如 100 条样本中，90 条是 R.2、10 条是 A.4。若模型永远猜 R.2，普通 mode accuracy 是 90%，但它完全不会识别 A.4。此时 R.2 的 F1 约为 94.7%，A.4 的 F1 为 0，二类 Macro-F1 约为 47.4%。所以 Macro-F1 用来避免高频 mode 把低频 mode 的完全失效掩盖掉。

## 3.7 实验协议、模型与得分

### 3.7.1 评测设置与方法

默认主实验采用 All-at-Once：将一条完整 trajectory 一次性提供给归因 LLM，让它联合输出 Agent、Step、Error。论文默认不提供 task.answer，所有轨迹均未截断，能放进被测模型的上下文窗口。

| 方法 | 读轨迹方式 | 用途 |
|---|---|---|
| All-at-Once | 一次读完整条轨迹后联合预测 | 主实验默认方法 |
| Step-by-Step | 按顺序读；认为发现第一个决定性错误即停止 | 消融比较 |
| Binary Search | 递归比较轨迹片段定位错误步骤 | 消融比较 |

被测模型为 GPT-5.4、Claude Sonnet 4.6、Gemini 3 Flash、Grok 4.1、gpt-oss-120b、GLM-5、DeepSeek-V4-Pro、Gemma 4、Llama-4 Maverick、Qwen3.5-122B。文本模型不支持 image / video 的格子为空。

### 3.7.2 主实验：All-at-Once 且不给任务答案

本节三张表**只列主实验**，因此统一使用 All-at-Once、完整轨迹、不给 task.answer 这一种条件。这样每个模型、每种模态、每项指标都在同一协议下，主表可直接横向比较；同时 All-at-Once 也是论文默认且在消融中总体更强的策略。

Step-by-Step 与 Binary Search 的完整结果在论文的消融表中单独报告，且该表使用的是较小的 ablation set、数值为跨模态平均，不能与下列主表逐格混排。文档在第 6.3 节给出代表性消融数字，用来回答“为何选 All-at-Once”，而不是把它们误当作主实验结果。

同理，主表默认不给 task.answer，是为了固定在“只有失败轨迹”的诊断条件。task.answer 可以帮助模型核验最终结果，却也可能把注意力吸引到后续哪个答案不对，从而跳过更早的决定性错误；论文将它作为单独的条件分析，而不与默认条件混在同一张主表中。不给 task.answer 也更接近线上诊断时正确答案未知的场景。

#### Text

| 模型 | Agent | Step | Error | Joint |
|---|---:|---:|---:|---:|
| GPT-5.4 | 55.7 | 72.3 | 15.3 | 21.3 |
| Claude Sonnet 4.6 | 54.9 | 69.8 | 19.1 | 22.4 |
| Gemini 3 Flash | 52.1 | 71.2 | 16.2 | 17.3 |
| Grok 4.1 | 51.1 | 65.9 | 18.6 | 19.9 |
| gpt-oss-120b | 48.4 | 63.9 | 10.8 | 16.2 |
| GLM-5 | 54.9 | 71.1 | **22.2** | **25.3** |
| DeepSeek-V4-Pro | 52.0 | 68.7 | 16.6 | 17.5 |
| Gemma 4 | 52.8 | 68.4 | 16.1 | 17.1 |
| Llama-4 Maverick | 51.9 | 66.7 | 20.6 | 16.5 |
| Qwen3.5-122B | **57.5** | **73.9** | 17.0 | 21.6 |

#### Image

| 模型 | Agent | Step | Error | Joint |
|---|---:|---:|---:|---:|
| GPT-5.4 | 67.7 | **65.2** | 26.5 | **30.2** |
| Claude Sonnet 4.6 | 67.5 | 63.8 | **29.5** | 29.6 |
| Gemini 3 Flash | 62.5 | 63.9 | 26.0 | 29.6 |
| Grok 4.1 | 67.7 | 60.5 | 21.4 | 26.4 |
| Gemma 4 | 69.0 | 62.6 | 19.1 | 24.3 |
| Llama-4 Maverick | 61.0 | 54.9 | 20.6 | 19.0 |
| Qwen3.5-122B | **70.4** | 60.9 | 21.8 | 24.5 |

#### Video

| 模型 | Agent | Step | Error | Joint |
|---|---:|---:|---:|---:|
| GPT-5.4 | **85.1** | 60.5 | 33.8 | 21.6 |
| Claude Sonnet 4.6 | 79.8 | **64.4** | 37.4 | 26.4 |
| Gemini 3 Flash | 74.4 | 59.8 | **40.0** | 23.0 |
| Grok 4.1 | 70.1 | 53.4 | 36.7 | 23.8 |
| Gemma 4 | 71.5 | 58.7 | 35.7 | 27.7 |
| Llama-4 Maverick | 79.8 | 59.5 | 36.4 | **30.4** |
| Qwen3.5-122B | 74.9 | 60.3 | 35.2 | 16.1 |

### 3.7.3 结果解读

| 结论 | 直接数据证据 | 应怎样解释 |
|---|---|---|
| 没有全面最好模型 | Text 的最高 Agent/Step 是 Qwen3.5（57.5/73.9），最高 Error/Joint 是 GLM-5（22.2/25.3）；Image、Video 的各列最佳者也不同 | Who、When、What 是不同能力，不能把各模型的列最高分拼成一个假想系统 |
| 定位步骤不等于解释类型 | Text 最高 Step 73.9%，最高 Error 22.2%；Image 65.2% 对 29.5%；Video 64.4% 对 40.0% | 模型更易找到可疑动作，却难分清它是规划、验证、协作还是推理问题 |
| 完整归因仍不可靠 | 三种模态最高 Joint 仅为 25.3%、30.2%、30.4% | 单独 Who 或 Step 较高，三维同时正确仍然困难 |
| 视频的 Step 定位更难 | 最高 Step：Text 73.9%，Image 65.2%，Video 64.4%；论文按 token 长度分组也观察到轨迹越长，Step 越低 | 视频时间证据与更长、密集的轨迹增加了最早错误位置的歧义 |
| 显性症状更容易分类 | 论文混淆分析中视觉误识别、幻觉、过早终止较易识别；规划、验证、协作常被误归为 R.2 | 后三类真正根因早于后续症状，模型常按表面结果分类 |

方法消融支持将 All-at-Once 作为主协议：在跨模态消融集上，GPT-5.4 的 Step 为 All-at-Once 65.5%、Step-by-Step 55.8%、Binary Search 62.8%；GLM-5 为 67.4%、63.0%、63.6%。逐步停下容易把症状误当根因，二分也可能丢失跨片段因果链。

### 3.7.4 与原始 Who&When 的 Step 排名差异

这是两篇论文都直接讨论过的现象，但不能把两个数值当成同一实验的正反结果。

| 数据集 | Step 上表现更好者 | 作者给出的直接解释 |
|---|---|---|
| 原始 Who&When | Step-by-Step | All-at-Once 要在很长的自然失败日志中找出一个精确步骤，出现“space-in-the-needle”问题；逐步读时，模型只需聚焦当前步骤，较容易作出细粒度判定 |
| Who&When Pro | All-at-Once | Step-by-Step 容易在遇到第一个“看起来可疑”的步骤时过早承诺，之后看不到错误动作造成的后果，无法修正判断；完整读完能同时看到前因、注错动作和后续失败 |

这两种解释并不矛盾：Step-by-Step 的局部判断既可能帮助模型避免在长日志中“找针”，也可能使它把早期症状或普通可疑动作误认为决定性错误。究竟哪一种效应占上风，取决于数据、轨迹结构、提示词和被测模型。

尤其不能仅凭这两篇论文断言“受控注错使 All-at-Once 更好”。原始 Who&When 的主实验使用 GPT-4o、184 条文本自然失败多 agent 日志；Pro 的方法比较使用 1,444 条分层抽样的多模态受控数据和一组更新模型。两篇论文没有做一个固定模型、固定 prompt、固定轨迹分布，只替换“自然标签 / 注错标签”的交叉实验。因此，“标签来源导致排名反转”目前只能是假设，不是论文已经验证的因果结论。

## 3.8 基准边界

1. 每条数据只注入一个主因；真实失败常是多因、环境变化与恢复失败的组合。
2. taxonomy 固定为 18 类；新工具、新环境或新协作方式可能落在 taxonomy 之外。
3. 三位标注者对分层抽取的 100 条进行复核，认可率为 Step 94%、Agent 90%、Error family 90%；这是质量抽检，不是把每一条都重新做一次人工反事实实验。
4. video.jsonl 本身不含帧；若评测只给文本 trajectory 而未同时提供 video_assets，测到的是没有视觉证据的视频文本摘要归因，不能称为完整的视频归因。

# 4. Agent Trace 归因方法：证据强度、成本与适用边界

## 4.1 归因任务的能力分层

基于pro数据的有没有什么新的方法有更好的效果

Agent Trace 的**归因方法**并不是一条简单的“模型能力排行榜”；而 MAST 这样的 taxonomy 也不应和归因方法并列。前者回答“是谁、何时引入决定性错误”，后者回答“失败属于什么类型”。完整工作流中的不同环节如下：

| 环节 | 要回答的问题 | 典型方法 |
|---|---|---|
| 失败描述／标注支撑（非根因定位） | 这是什么类型的错误？ | MAST taxonomy |
| 候选发现 | 长轨迹中哪些位置值得怀疑？ | 全量一次输入的 LLM Judge（All-at-Once）、SAFARI、VerifyMAS |
| 传播追踪 | 最后的错误症状是从哪一个上游输入或决策传来的？ | FALAT |
| 可疑度排序 | 多次运行中，哪些动作最常与失败模式一起出现？ | FAMAS |
| 因果确认 | 改动候选步骤后，任务是否真的更可能成功？ | CAR；Who&When Pro 的受控注错逻辑 |
| 低成本复用 | 能否把已有标签蒸馏为专用归因器？ | AgenTracer |

因此，适合落地的方向不是让一个 LLM 一次读完整条轨迹并直接宣布根因，而是形成分层流程：

```text
检索/筛选候选步骤 → 追踪依赖与传播链 → 对少量候选做重放干预 → 用 oracle 判定并输出解释
```

前两步负责降低长轨迹的搜索成本；最后一步负责把“看起来可疑”提升为“已被反事实验证”。若运行环境无法重放，也应明确报告结果只是候选排序或依赖证据，不能表述成已证明的因果根因。

## 4.2 相关性、依赖与反事实证据

以下示例只用于区分方法输出，不重新定义第一阶段已经学习过的反事实概念。

设：第 3 步中 Agent A 查询年份时得到或采用了错误信息；第 4 步 Agent B 把该信息写入摘要；最终答案因此错误。

| 证据层次 | 在这个例子中会得到什么 | 能说明什么 | 不能说明什么 |
|---|---|---|---|
| 相关/可疑度 | 第 4 步文本最像错误答案，或第 3 步在失败轨迹中频繁异常 | 它们是高优先级检查对象 | 改第 3 步一定能修复任务 |
| 依赖 | 识别 A 第 3 步输出被 B 第 4 步读取，形成 A3 → B4 的传播边 | B4 更像症状，A3 是上游候选来源 | A3 是否是决定性原因；可能还有另一个隐藏原因 |
| 反事实干预 | 将 A3 替换为正确结果，从该点续跑，oracle 由失败变成功 | 在该运行条件下，A3 的修改对成功有因果作用 | 其他步骤是否也存在交互性原因；需要进一步实验 |

“依赖边”非常有价值：它能把最终症状向上游回溯，避免模型只报最后报错处。但依赖本身不是任务成功的证明；只有重放后交给独立 oracle 判定，才是这里所说的强因果证据。

## 4.3 方法版图

### 4.3.1 失败 taxonomy：MAST

[MAST: Why Do Multi-Agent LLM Systems Fail?](https://arxiv.org/abs/2503.13657) 从 7 个主流多 Agent 框架的 1,600 余条人工标注轨迹中归纳出 14 种失败模式，归入系统设计、Agent 间失配、任务验证三类。论文报告初始 taxonomy 标注的一致性为 Cohen's kappa 0.88，并进一步用 LLM-as-a-Judge 扩大标注。

其中，Cohen's kappa 是“多位标注者一致性、扣除偶然猜中后”的统计量：1 表示完全一致，0 表示与随机一致性相当，0.88 表明该 taxonomy 的人工标注一致性很高。它不是“标签有 88% 正确”，也不是模型准确率。LLM-as-a-Judge 则是将 LLM 当作按 rubric 打标签的评审器：MAST 先由专家建立和核验 taxonomy，再让 LLM 为更多轨迹分配失败类别，以降低人工标注成本。它的输出仍应以人工抽检校准。

MAST 的核心贡献是“错误语言与分类体系”：例如可用于统一记录规划、协作、验证等失败类型。它**不是**一个精确 Who/When 根因定位方法：仅知道某条轨迹属于某类失败，不能据此证明具体 agent、具体步骤就是决定性原因。它更适合作为后续归因方法的标签 vocabulary、数据分析维度或 What（错误模式）评估维度。

### 4.3.2 直接判断、主动检索与假设验证

| 方法 | 做法 | 主要解决的问题 | 主要边界 |
|---|---|---|---|
| 全量 LLM Judge | 一次提供全轨迹，让模型输出 Who/When | 最容易实现的基线 | 长轨迹中的注意力分散；通常没有干预验证 |
| [SAFARI](https://arxiv.org/abs/2606.24626) | 让诊断 agent 用工具检索轨迹片段，并维护短期记忆 | 轨迹超过上下文窗口时，仍可主动找证据 | 未取回的片段可能恰是根因；需报告检索预算和候选召回 |
| [VerifyMAS](https://arxiv.org/abs/2605.17467) | 先验证“是否存在错误”，再局部验证 agent/步骤假设 | 将大搜索空间分层，减少直接多分类难度 | 验证器判断仍需受数据、提示词和证据覆盖约束 |

全量 LLM Judge 在这里就是第三阶段的 All-at-Once：整条轨迹一次放入提示词后直接判断。SAFARI 不是 Step-by-Step：后者从第 1 步开始线性扫描、第一次判错后停止；SAFARI 则围绕假设调用 read(offset, limit) 和 search(pattern)，可直接跳到某个 span、回看片段，再由独立 LLM 只核查它引用的证据。它还保留短期记忆，使多轮检索的结论能够衔接。SAFARI 报告在 Who&When 与 TRAIL 上的长轨迹诊断改进；TRAIL（Trace Reasoning and Agentic Issue Localization）是面向超长 agent trace 的另一套 benchmark，含 GAIA 与 SWE-bench 子集，部分轨迹达到数百万 token；它标注多个故障点，而非每条仅一个决定性步骤。

这些工作说明：模型不一定要先读完全部日志。但它们产出的首先是更好的候选与判断，不应自动升级为已完成的因果证明。

### 4.3.3 依赖链追踪：FALAT

[FALAT](https://arxiv.org/abs/2606.00765) 先构造预期解，再识别可疑区域，沿 agent 决策、消息和工具输出的依赖关系向上游回溯，以区分“错误首次引入处”与“继承错误后的症状步骤”；再评估修正候选是否足以恢复预期结果。论文在 Who&When 上报告其 Step 指标为 Algorithm-Generated 46.0%、Hand-Crafted 29.1%。

它最重要的思想是将时间上靠后的失败症状与上游错误源分开。其风险在于：依赖关系、预期解以及“修正后是否恢复”的判断若主要由语言模型推断，图的遗漏或误连会直接影响根因结论。报告中应把它称为“依赖证据/修复假设验证”，除非实际环境完整重放并由任务 oracle 判定成功，否则不要与严格反事实证据混为一谈。

### 4.3.4 多次运行的可疑度排序：FAMAS

[FAMAS](https://arxiv.org/abs/2509.13782) 对多次 MAS 轨迹进行系统化 replay 与抽象，并用 spectrum analysis 融合 agent 行为组与 action 行为组的统计信息，为动作计算 failure suspiciousness 排名。论文在 Who&When 上与 12 个基线比较，并报告优于它们的定位结果。

这里的“多次 MAS 轨迹”不是多个不同系统，也不是一条轨迹中的多个步骤；而是对同一任务、同一 MAS 配置从头独立执行 k 次。由于 LLM 采样、工具结果和 agent 决策有随机性，得到的 k 条轨迹中有些成功、有些失败。FAMAS 并不把某一个候选动作强制改掉；它只是观察这些自然重跑中哪些行为与失败结果共同出现。

它的完整流程是：

```text
原始失败轨迹
    ↓ 对同一任务从头重跑 k 次
成功/失败的多条原始日志
    ↓ LLM 分块解析为「agent、action、resulting state」三元组，并聚类同义行为
跨运行的结构化行为表
    ↓ 用成功/失败结果与行为是否出现，计算每个动作的 suspiciousness
原始失败轨迹中按分数排序的 Who/When 候选
```

所谓 spectrum analysis（谱分析）沿用传统软件调试中的 Spectrum-Based Fault Localization，不是傅里叶变换。可以把它理解为一张“运行 × 行为”的覆盖表：每一行是一次重跑，每一列是一个归一化后的动作，格子记录该动作是否出现，行末记录任务成功或失败。某行为若常出现在失败运行、很少出现在成功运行，会获得较高可疑度。

例如，同一检索任务重跑 5 次：3 次失败都使用了“过于宽泛的搜索词”并得到错误网页，2 次成功都使用了更精确的搜索词。前一类搜索动作的分数会上升；而每次运行都存在的“制定计划”动作不会因出现次数高就被误报为根因。

论文所说的两个“行为组”是为了让这个分数适应多 Agent 系统，而非将两个向量做黑盒融合：

| 行为组 | 防止的偏差 | 论文中的作用 |
|---|---|---|
| Agent Behavior Group | 活跃 agent 做的动作本来就更多，不能仅因次数多而更可疑 | 衡量某动作在该 agent 的活动上下文中覆盖得多广、占该 agent 动作的比例多大，做归一化 |
| Action Behavior Group | 同一动作可能在一次失败中反复重试；某些元动作又在成功和失败中都普遍出现 | 强化失败轨迹中的异常局部重复，同时衰减跨运行到处重复的冗余/通用动作 |

两组统计量与“成功/失败运行中的出现情况”共同进入 suspiciousness 公式，最后输出的是排序分数，而不是一次干预的成功率。FAMAS 的优势是：当同一任务可多次运行时，它比单次 LLM 判断多出运行间的统计证据，也适合作为“哪些候选值得昂贵验证”的排序器。它的输出仍首先是可疑度排名：多次失败中经常出现的动作可能是根因，也可能只是下游症状或共同环境因素；若要确认单个动作的因果效应，仍需针对该动作做干预重放并由 oracle 判断。

### 4.3.5 直接干预重放：CAR -- 如何replay

[Causal Agent Replay (CAR)](https://arxiv.org/abs/2606.08275) 将 agent 轨迹表示为结构因果模型。它对某个步骤施加 do 干预，从该点在相同随机策略下向前重放，比较任务结果的分布变化；并用单步对比估计、point-of-commitment 和预算受限的 Monte-Carlo Shapley 方法处理随机性与多步骤交互，输出效应及置信区间。

CAR 是这组方法中因果证据最强的一类，因为候选步骤不是只被阅读或评分，而是真的被替换后续跑，再由结果比较估计影响。代价也最高：需要可控制的运行环境、状态恢复、重复运行和明确 oracle。该论文的核心验证包含带植入真值的合成结构因果模型，说明方法机制有效；将它直接外推成所有真实 agent 系统上的已验证收益则仍需真实环境 benchmark 补充。

### 4.3.6 训练式归因器：AgenTracer

[AgenTracer](https://arxiv.org/abs/2509.03312) 使用反事实 replay 和程序化故障注入自动产生失败轨迹标注（TracerTraj），再以多粒度强化学习训练轻量归因模型。论文报告相对于 Gemini 2.5 Pro、Claude 4 Sonnet 的 Who/When 改进，以及对系统自我修复的提升。

它的价值是把昂贵的推理或重放工作前置到数据构造阶段，在线时用专用模型低成本预测。风险也相应变化：需要检查训练、验证和测试轨迹是否泄漏；注错分布是否覆盖自然失败；以及比较时是否与免训练基线在同一测试集、同一可见字段下运行。训练模型的高分不自动等同于跨框架、跨任务的泛化能力。

## 4.4 方法比较

| 路线 | 代表工作 | 直接输出 | 对“决定性根因”的证据 | 主要成本 | benchmark 中必须补充报告的指标 |
|---|---|---|---|---|---|
| 失败 taxonomy／标注支撑（非归因） | MAST | 错误类别与分布 | 不产生 Who/When 根因结论 | 标注构造高；分类推理低 | taxonomy 一致性、类别覆盖、人工抽检 |
| 一次性 LLM 判断 | Who&When 基线 | Who、When、理由 | 弱：基于轨迹表面证据 | 低 | Who/When/Joint、token、轨迹长度分桶 |
| 主动检索/验证 | SAFARI、VerifyMAS | 候选片段、验证判断 | 弱到中：取决于证据是否被检到 | 中 | 定位指标、检索预算、候选召回、时延 |
| 依赖追踪 | FALAT | 上游依赖链、源/症状区分 | 中：说明传播路径，不天然证明修复成功 | 中 | 定位指标、依赖边质量、源与症状的混淆情况 |
| 统计可疑度 | FAMAS | action/agent 的可疑度排序 | 中：来自多次运行的统计证据 | 中到高 | Top-k 候选召回、重放次数、排序质量、成本 |
| 因果重放 | CAR | 干预效应、置信区间、归因分配 | 强：候选被改变后由结果判定 | 高 | Who/When、成功率变化、干预次数、CI、状态一致性 |
| 训练式归因 | AgenTracer | 专用模型的 Who/When/解释 | 取决于训练标签的证据等级 | 离线高、在线低 | 严格隔离测试、自然/注错分别报告、跨框架泛化 |

表中的“强/弱”是对根因因果主张的证据强度，不是模型整体优劣，也不是论文质量评分。低成本路线很适合做在线候选生成；高成本干预路线更适合关键问题复核、生成高可信标签或验证 benchmark。

## 4.5 参考论文

- [MAST](https://arxiv.org/abs/2503.13657)
- [Causal Agent Replay (CAR)](https://arxiv.org/abs/2606.08275)
- [FAMAS](https://arxiv.org/abs/2509.13782)
- [AgenTracer](https://arxiv.org/abs/2509.03312)
- [FALAT](https://arxiv.org/abs/2606.00765)
- [SAFARI](https://arxiv.org/abs/2606.24626)
- [VerifyMAS](https://arxiv.org/abs/2605.17467)

# 5. trace-synthesis 基础与归因 Benchmark 设计原则

本章汇总前述各基准与方法对后续归因 benchmark 的直接要求。自然失败、受控注错、不同模态和不同方法路线必须分开评测；定位结果、证据强度、成本与泛化能力也不能压缩为一个单一总分。

## 5.1 trace-synthesis：归因评测的现有基础与能力缺口

### 5.1.1 核心结论

`trace-synthesis` 已经是一个可复用的 **Agent Trace 数据转换、存储验证与查询验证基础设施**；它还不是一个“归因模型准确率 benchmark”。

因此后续工作不应重写现有转换与重放能力，而应在它输出的 Trace 之上增加一层独立的**归因评测协议**：隐藏金标、接收模型预测、按 Who / When / What / Evidence 评分。

### 5.1.2 Trace 转换、存储与查询验证能力

```text
原始 Agent 数据
  → SourceAdapter：读入、校验、规范化
  → LogicalTrace：统一逻辑树（trace → LLM request → Tool）
  → converter.build_spans：OTel 兼容 span JSONL
  → manifest / report / rejects
  → 可选 Replay、存储与查询验证
```

一次用户问题及其后续 agent/tool 因果链被定义为一条 Trace。仓库支持 `toucan`、`open-swe-traces`、`whowhen`、`whowhen-pro` 四类 source；输出默认是一行一个 span 的 JSONL，并保留生成配置、输入身份、质量报告和坏记录证据。

### 5.1.3 Who&When 与 Who&When Pro 标签映射

| 数据源 | 已保留的归因标签 | 已完成的 Trace 映射 |
|---|---|---|
| Who&When | 根 span：`failure.mistake_agent`、`failure.mistake_step`、`failure.mistake_reason`、`failure.ground_truth` | 对应的 LLM 或 Tool span 标记 `failure.root_span=true`，并携带 step / agent。 |
| Who&When Pro | 根 span：`failure.mistake_agent`、`failure.mistake_step`、`failure.mistake_mode`、`failure.ground_truth`；必要时保留步骤坐标 | 根据注错/ground-truth 的轨迹内标记定位决定性 span，并标记 `failure.root_span=true`。 |

这意味着：**Who 与 When 的已有金标没有丢失，并且 When 已能映射到 span。**这是构建归因 benchmark 的关键起点。

### 5.1.4 现有 benchmark 的评测范围

当前 benchmark 的主流程是：

```text
Replay Trace
→ 确认数据被接收且可见
→ 执行 Trace 查询场景
→ 验证查询结果正确性与延迟
→ 清理本轮 benchmark 数据
```

它回答的是三类基础设施问题：

1. Trace 能多快被写入；
2. 写入后多久可被读取；
3. 用户查询 Trace 时，是否返回正确结果及其延迟。

所以它的输入是已生成的 Trace；`failure.*` 是可查询 metadata。它**不会**让 LLM 读取失败轨迹并预测 Who/When，也没有把预测结果同金标进行打分。

### 5.1.5 可复用基础与归因评测缺口

| 可直接复用 | 归因 benchmark 仍需新增 |
|---|---|
| Who&When / Pro source adapter | 面向模型的、删除 `failure.*` 与 `failure.root_span` 的分析视图，防止标签泄漏 |
| 统一 Trace / Span ID、`failure.root_span` | 独立 gold-label sidecar：`trace_id`、gold Who、root span、step、mode、reason |
| JSONL 输出、manifest、质量报告、可复现配置 | 模型预测 schema：预测 Who / span 或 step / What / evidence / 模型与运行身份 |
| Replay、存储与查询验证 | 评分器：Who Accuracy、root-span / Step 指标、Mode Macro-F1、Joint、Evidence 指标 |
| 现有查询场景与参数 catalog | 数据切分、分桶报告、提示词与模型版本锁定、输出格式校验 |

### 5.1.6 参考的仓库材料

- `trace-synthesis/docs/architecture.md`
- `trace-synthesis/benchmark/README.md`、`benchmark/DESIGN.md` 的设计结论与主流程
- `src/trace_synthesis/sources/whowhen.py`
- `src/trace_synthesis/sources/whowhen_pro.py`
- `src/trace_synthesis/converter.py`

## 5.2 在 trace-synthesis 上增加并行归因评测层

第一版归因评测应当作为现有仓库的**并行评测层**：

```text
现有 adapter / converter
        ↓
同一份 Trace
   ├─ 现有 benchmark：验证摄入、可见性、查询
   └─ 新 attribution evaluator：隐藏标签 → 模型预测 → 离线评分
```

两者回答不同问题，不能用前者的查询性能结果代替后者的归因准确率，也不应为增加归因评分而破坏现有的 Trace 输出契约。

## 5.3 自然失败评测的协议要求

1. 将 Who&When 作为**自然失败评测集**单独报告，不与 Pro 的分数合并；
2. 第一版至少复现 Agent Accuracy、Step Exact 与 Step@±k；
3. 报告 Algorithm-Generated 与 Hand-Crafted split，并按轨迹长度分桶；
4. 明确是否把任务最终答案提供给归因模型，不能把两种设定混在一起；
5. 输入必须去除 `failure.*` 与 `failure.root_span`，否则模型会直接得到答案；
6. 分开报告实例级定位指标（Agent Accuracy、Step Exact、Step@±k）与聚合级责任排序；前者衡量单条归因是否命中金标，后者只用于发现高频排查对象。

## 5.4 Who&When Pro 的多模态与坐标接入要求

1. 先跑 text split，并与自然失败 Who&When 分开报告 Who / When / What。
2. 模型输入中删除 ground_truth、failure.*、root_span 与注错提示；gold 单独保存给评测器。
3. image / image_gui 输入要解码 Base64 PNG，并定义图片去重与重复观察的保留规则。
4. video 输入必须将 video_context 映射到 video_assets 的 JPEG 帧；仅加载 JSONL 不足以完成视频归因评测。
5. 为每个 framework 定义 When 坐标的精确序列化与匹配规则；不能把 7.1、round+position、stage 简化为整数。
6. 同时报 Agent、Step、Error Macro-F1、Joint；单 agent trace 的 Joint 计分规则必须写清。

## 5.5 证据、成本与泛化的评测要求

若我们在 trace-synthesis 基础上继续做归因 benchmark，至少应把“预测正确”与“证据充分”分开记录：

1. 基础定位集：保留 Who、When、错误模式、Joint 等精确指标；一次性 Judge、Step-by-Step、检索式方法都可公平比较。
2. 依赖集：为一小部分任务补充消息、工具输出和状态的依赖边，单独测“能否找到错误源而非最后症状”。没有边真值时不能凭模型解释声称依赖分析正确。
3. 干预验证集：选择可复现、带成功 oracle 的任务；从高分候选中重放替换，记录干预前后成功率、次数、状态一致性和置信区间。这是高可信金标或方法因果验证的来源。
4. 泛化与成本集：按自然失败/受控注错、框架、任务族、模态、轨迹长度分桶；对训练式方法增加训练来源隔离和跨框架测试；对检索式方法报告预算。

推荐的工程路径是“先宽后窄”：先用全量 Judge 或 SAFARI 取得少量候选，再由 FALAT 式依赖追踪解释传播关系，最后只对少数高价值候选进行 CAR 式或 Pro 式重放验证。这样既不会把每条轨迹都做昂贵 replay，也不会把纯语言模型的相关性判断误报成因果结论。
