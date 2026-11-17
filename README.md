# token-os

一台没有操作系统的 ARM 机器，靠云端吐出的 token 自己活着。

- 裸机 aarch64 内核（无 Linux、无 libc、无标准库）
- 内核里住着一个 agent：事件日志、surface 投影、工具表、压缩事务
- 模型在云端，token 通过串口帧协议流进内核，由内核自己决定下一步做什么
- 已实测：**10 分钟自动跑 582 轮请求、456 次工具执行、171 个自定义工具、0 坏帧**

## 快速开始

依赖：`qemu-system-aarch64`、`clang`（含 lld）、`make`、`node`。

```bash
cp key.txt.example key.txt     # 把一行 API key 写进去
make                           # 编译出 kernel.elf
bash run-token.sh 600          # 跑 10 分钟：内核 + 中转一起起来
```

看结果：

```bash
tail -f log/console.log        # 内核控制台：它说的话、调的工具、画的东西
tail -f log/relay.log          # 中转：每次请求的状态（不含 key）
```

只想看心跳、不接云端：`make && make run`。

> **key 的位置**：`key.txt` 编译时被写进内核镜像（`key.inc` → `API_KEY`），
> 每次请求由内核自己带出来，中转只做转发、不落盘。
> `key.txt` / `key.inc` / `*.elf` 都在 `.gitignore` 里，**不要提交**。

## 骨架

```
                 UART1 (0x09040000)            unix socket          HTTPS
   ┌─────────┐   ┌──────────────────┐   ┌──────────────┐   ┌──────────────┐
   │ kernel  │──▶│  帧协议 A5 5A …  │──▶│  relay.js    │──▶│ api.deepseek │
   │ (裸机)  │◀──│  key + JSON      │◀──│  (中转)      │◀──│ .com         │
   └─────────┘   └──────────────────┘   └──────────────┘   └──────────────┘
        │
        └── UART0 (0x09000000) 人的控制台
```

## 现在能做什么

内核上电后，用**镜像里自带的 key**自己拼请求，通过 UART1 发出去；中转脚本把它原样翻成 HTTPS；云端开始吐 token；每个 token 在**串口中断**里落进本地的 1 MiB 上下文缓冲区，主循环每 10 秒报告一次进度；一轮结束后内核**自己**续下一轮。

全程：中断里只搬字节记账，打印和决策全在主循环。

## 怎么跑

```bash
make                                    # 编译（会从 key.txt 生成 key.inc 并编进镜像）
bash run-token.sh 600                   # 跑 600 秒：内核 + 中转一起起来
```

看结果：

```bash
tail -f log/console.log     # 内核控制台（心跳、进度、token 统计）
tail -f log/relay.log       # 中转日志（请求轮次、token 数，绝不含 key）
```

## 帧协议

```
[A5][5A][type][len_hi][len_lo][payload ...][crc8]
crc8 = CRC-8/ATM（多项式 0x07，初值 0），覆盖 type + len + payload
```

| type | 方向 | payload |
|---|---|---|
| 1 REQUEST | 内核 → 中转 | `key\n` + JSON 请求体 |
| 2 DELTA | 中转 → 内核 | 一个 token 的 UTF-8 字节 |
| 3 DONE | 中转 → 内核 | 空 |
| 4 ERROR | 中转 → 内核 | 错误文本 |

key 在**内核镜像里**，每次请求由内核自己带出来，中转用完即丢；中转日志只打印 `key前4位`。


## v4：内核里住着一个 agent

从 v3 的"收 token"升级成"会动手"：

```
上电 → 事件日志里写下目标 → agent 循环：
        组装 messages（system + surface 窗口）→ 带 tools 发请求
        ← FINAL 帧：finish_reason / content / tool_calls
        若是 tool_call：执行 → 结果写回事件日志 → 下一步
        若表面超预算：请求摘要 → 遮蔽旧区间（日志不删）→ 继续
```

| 概念 | 实现 | 位置 |
|---|---|---|
| 事件日志 | `log_buf[1500]`，append-only，永不删 | agent.c |
| 表面投影 | `surface_start` 指针 | agent.c |
| 工具表 | `tool_tab[]` 六个内建 + `ctools[]` 模型自定义 | agent.c |
| 循环 | 状态机 `IDLE → WAIT_MODEL → 执行 → IDLE` | agent.c |
| 压缩 | 表面超 2000 字节 → 摘要 → 遮蔽 → 摘要入表面 | agent.c |
| 硬重置 | 日志将满时只留最近 4 条 | agent.c |

内建工具：`uart_putc` / `read_reg` / `write_reg` / `ticks` / `define_tool` / `list_tools`。

`define_tool` 是自我扩展：`script` 是 `u:X`（打印字符）、`w:地址,值`（写寄存器）、`d:毫秒`（延时）用分号拼起来的小脚本。模型可以定义、覆盖、调用自己造的工具——内核里没有编译器，但有一套解释器。

## 关键数字（实测）

| 指标 | 值 |
|---|---|
| 定时器 | 62.5 MHz 计数器，1 秒一次中断 |
| 空转 CPU | 6 秒耗 0.083 s（约 1.4%，因为有 `wfi`） |
| 中断源 | 定时器 PPI 30、串口 SPI 40 |
| 上下文缓冲 | 1 MiB（`.bss`，由内核独占，约合 35 万中文字） |
| 事件日志 | 1500 条，满了硬重置 |
| 十分钟实测 | 582 请求 / 456 次工具执行 / 171 个自定义工具 / 184 次压缩 / 0 坏帧 |
| 75% 水位线 | 786 432 字节，到这条线打一行提示（留给压缩） |

## 踩过的坑（留着给下次）

1. **`-mstrict-align` 必须有。** MMU 关着的时候内存按 Device 属性访问，未对齐访问直接触发对齐异常（ESR `0x96000061`）。
2. **QEMU virt 的 UART1 中断号是 40，不是 34。** `hw/arm/virt.c` 里 `irqmap[VIRT_UART1] = 8`，加 SPI 基址 32。34 是 RTC。猜错了会看到"中断永远不来、但 FIFO 里有数据"。
3. **单核 GIC 的 ITARGETSR 是 RAZ/WI**（读回 0 是正常的），SPI 默认就路由到唯一的那个核，不用管。
4. **不能一次把整条 token 流灌进 16 字节的 UART FIFO。** 中转必须节流（`PACE_MS`，默认 1ms 一帧、每帧 6 字节），否则内核会丢字节、后续帧全部错位。
5. Termux 里没有 `qemu-system-aarch64` 这个包名，真名是 `qemu-system-aarch64-headless`。

## 文件

| 文件 | 作用 |
|---|---|
| `startup.s` | 设栈、清 BSS、从 EL2/EL3 落到 EL1 |
| `vectors.s` | 异常向量表、中断现场保存/恢复 |
| `main.c` | 串口、GIC、帧协议、上下文、主循环 |
| `relay.js` | 中转：帧 ↔ HTTPS |
| `run-token.sh` | 把内核和中转一起拉起来 |
| `linker.ld` | 加载到 `0x40000000` |
| `about.html` | 给外人看的项目介绍 |
