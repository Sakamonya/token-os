#!/usr/bin/env python3
"""把 DSH 会话日志导成人能读的对话记录。

用法：python3 docs/.export_conversation.py <session.jsonl> docs/conversation.md

只取可见对话。工具调用压成单行，工具参数与输出、模型思考过程都不落盘。
"""
import json, re, sys, datetime

SRC, OUT = sys.argv[1], sys.argv[2]

# 运行时注入的噪音，不是人说的话
NOISE = ("<system-reminder>", "Current runtime context.",
         "Time sampled while preparing", "The approval policy changed",
         "This is an automatically generated checkpoint",
         "background job ")

TOOL_LABEL = {"bash": "终端", "read": "读文件", "write": "写文件", "edit": "改文件",
              "glob": "找文件", "grep": "搜内容", "job_output": "看后台输出",
              "job_kill": "停后台任务", "job_list": "列后台任务", "todo_write": "记进度",
              "ui": "看界面", "ask_user_question": "提问", "skill": "读技能",
              "present": "交付文件", "web_search": "上网搜", "web_fetch": "抓网页",
              "generate_image": "画图", "working_directory": "查看目录",
              "list_agents": "看子 agent", "memory_write": "记记忆", "memory_read": "读记忆"}


def ts(ms):
    return datetime.datetime.fromtimestamp(ms / 1000).strftime("%H:%M:%S")


def redact(s):
    # 已吊销的 key 也打码：GitHub 推送保护会拦这种形状，留着只是噪音
    s = re.sub(r"sk-[0-9a-fA-F]{16,}", "sk-****（已吊销）", s)
    # 连前缀也别留（对话里提到过 key 的前 7 位）
    return re.sub(r"sk-[0-9a-fA-F]{4,}", "sk-****", s)


def parse_pasted(text):
    """用户把一整段旧对话的 JSON 贴了进来 —— 能解析就还原成问答。"""
    s = text.strip()
    if not s.startswith("["):
        return None
    arr = None
    for strict in (True, False):
        try:
            # strict=False：贴进来的 JSON 里字符串带裸换行，严格模式直接判死
            arr = json.loads(s, strict=strict)
            break
        except Exception:
            continue
    if arr is None:
        return None
    if not isinstance(arr, list) or not arr:
        return None
    if not all(isinstance(x, dict) and "role" in x for x in arr):
        return None
    return arr


events = []
for line in open(SRC, encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    o = json.loads(line)
    if o.get("type") in ("user/message", "assistant/message", "tool/call"):
        events.append(o)

out = []
W = out.append

W("""# 这段对话

`token-os` 是怎么被聊出来的 —— 原文照录，一轮不删。

| | |
|---|---|
| 时间 | 2026-11-17，Asia/Shanghai |
| 人 | 嵌入式专业本科生 |
| agent | 跑在平板上的 DSH，模型 DeepSeek |
| 交互方式 | 人只说中文大白话；agent 有权直接在这台平板上装包、编译、跑 QEMU、改文件、推仓库 |

## 为什么把它开源

提示词也是源代码。

这段记录里有三样东西是别处看不到的：

1. **一个人怎么把模糊念头逼成能跑的东西。** 从「win 的启动逻辑是什么」开始，
   到「让 token 直接活在电路上」，中间每一步都是被追问出来的。
2. **agent 在被完全授权时到底会走多远**，以及它在哪儿卡住。
3. **所有踩的坑都在这里先撞上、现场修掉**，不是事后补的总结。

## 读之前要知道的三件事

- **工具调用压成了单行**（`⚙ 终端（干了啥）`），参数和输出都删了 —— 那部分有几万行。
- **模型的思考过程删了**（约 34 万字符，几乎全英文）。想看原始的，见文末。
- **密钥全部打码**成 `sk-****（已吊销）`。第一版 key 是明文贴进来的，用完已在
  DeepSeek 网页上删除；即便吊销也照样打码，因为 GitHub 的推送保护会拦这种形状。

---
""")

turn = None
buf_tools = []
count = {"user": 0, "assist": 0, "tool": 0}
pasted_done = False


def flush_tools():
    global buf_tools
    if buf_tools:
        W("\n" + "".join(buf_tools).rstrip("　") + "\n")
        buf_tools = []


def start_turn(t, ms):
    global turn
    if turn != t:
        flush_tools()
        turn = t
        W(f"\n## 第 {t} 轮 · {ts(ms)}\n")


for o in events:
    typ, d = o["type"], o.get("data", {})
    ms = o.get("time", 0)

    if typ == "user/message":
        txt = "".join(c.get("text", "") for c in d.get("content", [])
                      if isinstance(c, dict) and c.get("type") == "text")
        if not txt.strip() or txt.startswith(NOISE):
            continue
        txt = redact(txt).strip()

        pasted = parse_pasted(txt) if not pasted_done else None
        if pasted:
            pasted_done = True
            n_user = sum(1 for x in pasted if x.get("role") == "user")
            W(f"""
---

# 一、前史：还没上 DSH 的时候

这一段本来是在网页版聊的。上来先把整段对话的 JSON 贴了进来 —— 下面是还原后的样子。

**我**

> 呃，我先把之前聊的贴给你。

<details><summary>里面是 {len(pasted)} 条消息、{n_user} 问 {len(pasted) - n_user} 答（点开看）</summary>

""")
            for item in pasted:
                role = item.get("role")
                body = item.get("content")
                if isinstance(body, list):
                    body = "".join(c.get("text", "") for c in body if isinstance(c, dict))
                if not isinstance(body, str):
                    body = json.dumps(body, ensure_ascii=False)
                body = redact(body).strip()
                who = "**我**" if role == "user" else "**agent**"
                W(f"{who}\n\n{body}\n\n")
                count["user" if role == "user" else "assist"] += 1
            W("</details>\n\n---\n\n# 二、进了 DSH，开始动真格的\n")
            continue

        count["user"] += 1
        start_turn(d.get("turn") or turn or 0, ms)
        flush_tools()
        quoted = "\n".join("> " + ln for ln in txt.splitlines())
        W(f"\n**我**\n\n{quoted}\n")

    elif typ == "assistant/message":
        msg = d.get("message", {})
        txt = "".join(c.get("text", "") for c in msg.get("content", [])
                      if c.get("type") == "text")
        if not txt.strip():
            continue
        count["assist"] += 1
        start_turn(d.get("turn") or turn or 0, ms)
        flush_tools()
        W(f"\n**agent**\n\n{redact(txt.strip())}\n")

    elif typ == "tool/call":
        count["tool"] += 1
        name = d.get("name", "?")
        label = TOOL_LABEL.get(name, name)
        desc = ""
        try:
            a = json.loads(d.get("arguments", "{}"))
            for k in ("description", "file_path", "pattern", "query", "text"):
                if a.get(k):
                    desc = str(a[k]).replace("\n", " ")[:64]
                    break
        except Exception:
            pass
        buf_tools.append(f"⚙ {label}" + (f"（{desc}）" if desc else "") + "　")

flush_tools()

hours = (events[-1]["time"] - events[0]["time"]) / 3600000
W(f"""
---

# 三、几个数字

| | |
|---|---|
| 人说的话 | {count['user']} 段 |
| agent 说的话 | {count['assist']} 段 |
| agent 动手 | {count['tool']} 次 |
| 跨度 | 约 {hours:.1f} 小时 |

# 四、原始文件在哪

DSH 把每次会话完整存在本机：

```
~/.dsh/sessions/<工作目录>/session-<uuid>/session.v4.jsonl.zstd
```

zstd 压缩的 JSONL，一个事件一行，含工具调用、工具输出、模型思考过程。
本文就是从这里导出来的，导出脚本在 `docs/.export_conversation.py`：

```bash
zstd -d session.v4.jsonl.zstd -o s.jsonl
python3 docs/.export_conversation.py s.jsonl docs/conversation.md
```
""")

open(OUT, "w", encoding="utf-8").write("\n".join(out))
print(f"写出 {OUT}：人 {count['user']} 段 / agent {count['assist']} 段 / 工具 {count['tool']} 次")
