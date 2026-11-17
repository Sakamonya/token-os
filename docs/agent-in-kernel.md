# 让 agent 真的活在裸机里：调研与设计

> 调研时间：2026-11-17
> 目的：把 DSH（DeepSeek Harness）的 agent 架构，压缩成能在裸机内核里跑的最小内核
> 当前状态：token 链路已通（10 分钟 × 2 轮，零坏帧），但内核只会"收字节"，还不是 agent

---

## 0. 结论先行

**内核里不需要一个"小号 DSH"。需要的是 DSH 的四个骨架，每个压到几百字节的等价物：**

1. **事件日志 vs 表面（log / surface 分离）** —— 落盘的留全部，发给模型的只留窗口
2. **工具表（tool registry）** —— 名字 + 参数 schema + 权限位 + 执行函数
3. **轮次驱动（turn / step）** —— 模型说 `tool_calls` 就执行，说 `stop` 才结束
4. **压缩事务（compaction）** —— 摘要 + 遮蔽旧区间，而不是删除

四者齐了，内核才从"收 token 的管道"变成"会活动的 agent"。

---

## 1. DSH 的骨架（直接从源码抽出来的）

DSH 由 300+ 个包组成，但 agent 的核心只有这几块：

| DSH 包 | 机制 | 一句话 |
|---|---|---|
| `dsh-agent-loop` | `turn()` / `step()` | 每一步：组装 prompt → 请求 → 收流 → 落事件 → 若有 tool_call 就执行并回到 step |
| `dsh-session` | 事件日志 | 一切皆事件（`user/message`、`assistant/message`、`tool/call`、`tool/result`），append-only，带 `seq` |
| `dsh-session-projection` | 表面投影 | 事件带 `surfaceOp`：有的进模型视野，有的只落盘（log-only）。**留在盘上 ≠ 发给模型** |
| `dsh-token-meter` | 压力测量 | 每次请求后记录 token 用量，判断离压缩线还有多远 |
| `dsh-compaction` + `-basic` | 压缩事务 | `compaction/start`（上锁）→ 调模型写摘要 → `compaction/summary`（含 `shadowedRange`/`shadowedTokenCount`）→ `compaction/end` |
| `dsh-system-prompt` | 提示组装 | 分段渲染：身份、规则、环境、技能目录、工具说明 |
| `dsh-tools` + `dsh-tool-*` | 工具注册 | 每个工具一个包：schema、执行、超时、权限 |
| `dsh-goal` + `-round-driver` | 自持续跑 | 目标未完成就自动开下一轮，不靠用户催 |

### 最值得抄的一条：log 与 surface 分离

DSH 的压缩**不删事件**。它写三条事件标记一段区间被"遮蔽"（shadowed），然后在表面插入一条摘要 `user/message`。

```
日志（永久）：  [用户请求] [助手1] [工具调用] [工具结果] [助手2] ...
                      └──────────── 被遮蔽 ─────────────┘
表面（给模型）： [系统提示] [摘要 of 上面那段] [最近 8 轮原文]
```

这样做的三个好处，裸机同样成立：

- 审计永远完整（断电了日志还在）
- 压缩可回滚（遮蔽是标记，不是销毁）
- 摘要失败时系统仍然可用（旧的表面还在）

`selectCompactableRange` 里还有两条硬规则，直接照抄：

- **保留最近一段原文**（`retainTokens`），不是全压
- **绝不拆开 tool_call / tool_result 对**，拆开模型会精神错乱

---

## 2. 业界怎么做最小 agent 循环

标准做法（DeepSeek / Claude 都用同一套）：

```
循环：
  请求 {messages, tools}
  若 finish_reason == "tool_calls":
      对每个 tool_call：检查权限 → 执行 → 追加 {role:"tool", tool_call_id, content}
      继续循环
  若 finish_reason == "stop":
      结束本轮
```

关键细节（容易被忽略但会直接崩）：

- 助手那条带 `tool_calls` 的消息**必须原样回传**，否则模型不知道自己在调什么
- `tool_call_id` 必须配对
- 参数是 **JSON 字符串**（`arguments`），不是对象 —— 内核要解析它
- `finish_reason` 里还有 `length`、`content_filter`、`aborted`，不能只处理两种

DSH 在这一点上更狠：它把"模型看到的"和"日志记的"彻底分开，所以它能做 `tool-result-pruner` 这种插件 —— 只把巨大的工具结果从**表面**上裁掉，日志里还是完整的。

---

## 3. 裸机上的先例

搜到一份同类设计的文档 [Agent-on-Metal](https://github.com/SuperInstance/fm-research-archive/blob/main/AGENT-ON-METAL-ARCHITECTURE.md)，作者给 Jetson 设计的三种形态：

| 形态 | 做法 | 对我们的参考价值 |
|---|---|---|
| A. 全裸机 | U-Boot 直接加载 agent 二进制，agent 就是 OS | 目标形态 |
| B. 硬件分区 | Jailhouse/TrustZone 切一半给 agent，一半留 Linux | **我们的下一步（AVF）** |
| C. 分布式 | 多板 + 云端协同 | 远期 |

它强调的三点，和我们的判断一致：

1. 裸机的价值是**确定性**（没有调度器抖动、没有 GC）
2. agent 直接当驱动，省掉 `驱动 → /dev → read() → 用户态` 的往返
3. 必须解决**"谁在失败的路径上"**——LLM 不能在中断里决策

---

## 4. 映射到内核：需要造什么

| DSH 里的东西 | 裸机等价物 | 规模 | 难点 |
|---|---|---|---|
| session 事件日志 | `.bss` 里的环形事件数组 + 单调 `seq` | ~200 行 | 无 |
| surface 投影 | 一个 `uint32_t surface_start;` 指针 | ~10 行 | 概念要转过来 |
| token-meter | 中转回传 `usage`，内核记水位 | ~30 行 | 要加控制帧 |
| tools 注册表 | 静态数组：名字、schema 字符串、权限位、函数指针 | ~80 行 | 无 |
| JSON 解析 | **只解析我们自己的 schema** 的最小递归下降 | ~150 行 | 别写通用解析器 |
| turn/step 循环 | 状态机：`IDLE → WAIT_MODEL → RUN_TOOL → WAIT_MODEL → DONE` | ~120 行 | 无 |
| compaction | 摘要请求 → 遮蔽区间 → 插入摘要事件 | ~150 行 | 摘要也是 LLM 调用，要处理"压缩时又满了" |
| goal 续跑 | 目标未完成就自动发下一轮 | ~40 行 | 我们已经有了（250/382 轮） |
| 权限 | 每个工具一个 capability 位 + 地址范围 | ~40 行 | 无 |

**总共约 800～1000 行 C。** 对比现在 main.c 的 ~500 行，是一个可以分三步走的量。

---

## 5. 三步路线（每步都有可验证的验收）

### M1 · 让内核执行一次工具调用（不含循环）

**做什么**：请求里带 `tools`（只给两个：`read_reg`、`write_reg`），解析返回的 `tool_calls`，执行，把结果打印出来，然后停下。

**验收**：
```
[step 1] 模型请求 write_reg(0x09000000, 'A')
[执行]   写串口寄存器，屏幕上出现 A
[step 1] 结束（不自动继续）
```
**风险**：JSON 解析错、tool_call 与结果配对错。这一步只验证协议，不验证智能。

### M2 · 加上循环，让它自己多走几步

**做什么**：`finish_reason == tool_calls` → 执行 → 回填 → 再请求；`stop` 才结束。加**步数上限**（比如 8 步）防止失控。

**验收**：
```
[step 1] write_reg(0x09000000, 'H')
[step 2] write_reg(0x09000000, 'i')
[step 3] stop
→ 屏幕上出现 "Hi"，日志里有完整因果链（谁调用了谁）
```
**风险**：模型可能陷在重复调用里（DSH 专门有个 `dsh-repeat-tool-reminder` 治这个）。所以要有步数上限 + 重复检测。

### M3 · 压缩 + 冷启动

**做什么**：Surface 指针 + 水位触发摘要请求 + 遮蔽区间写入事件日志；重启时从日志尾部恢复。

**验收**：
```
[t=170s] 水位 75% → 触发压缩
[压缩]   遮蔽 seq 1..4000，摘要 312 字节
[冷启动] 重启后读到摘要 + 最近原文，能说出"我之前在干什么"
```
**风险**：压缩时又满了（要留余量）、摘要请求失败（要能退回不压缩）、摘要比原文还长（要限制输出长度）。

---

## 6. 几个必须提前定的取舍

| 问题 | 建议 | 理由 |
|---|---|---|
| 内核里放 JSON 解析器？ | 只解析自己的 schema，字符串扫描，不做通用 | 通用解析器的 bug 面太大，裸机没有异常兜底 |
| 摘要由谁写？ | 云端模型（多一次请求，约 0.001 元） | 内核没有模型，只能这样 |
| 工具执行在哪里？ | 内核里（直接写寄存器），不走中转 | 这是整个项目的意义所在 |
| token 计数 | 中转把 `usage` 通过控制帧回传 | 内核自己数不准（没有分词器） |
| 中断里的规矩 | 只收字节、只记账 | 已经在做，不能破 |

---

## 7. 参考资料

- DSH 源码骨架：`@deepseek-ai/dsh-agent-loop`、`dsh-session`、`dsh-compaction-basic`、`dsh-token-meter`、`dsh-goal-round-driver`（本机 `files/engine/node_modules/` 内）
- [Agent-on-Metal: Bare-Metal Agent Architecture](https://github.com/SuperInstance/fm-research-archive/blob/main/AGENT-ON-METAL-ARCHITECTURE.md) —— 裸机 agent 的三种形态与动机
- [DeepSeek Chat Completions API](https://api-docs.deepseek.com/api/create-chat-completion) —— `tools` / `tool_calls` / `finish_reason` / `usage` 的确切字段
- [Claude: Build a tool-using agent](https://platform.claude.com/docs/en/agents-and-tools/tool-use/build-a-tool-using-agent) —— 同一个循环的另一种写法
