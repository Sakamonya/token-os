// agent.c —— 裸机内核里的 agent（照 DSH 的骨架压缩而来）
//
//   DSH                     这里
//   ─────────────────────────────────────────────
//   session 事件日志    →   log_buf[]，append-only，永不删
//   surface 投影        →   surface_start 指针（日志留全部，模型只看窗口）
//   token-meter         →   surface_bytes() vs SURFACE_BUDGET
//   tools 注册表        →   tool_tab[] + ctools[]（模型自己定义的复合工具）
//   agent-loop step     →   状态机 IDLE → WAIT_MODEL → 执行 → IDLE
//   compaction 事务     →   摘要 → 遮蔽区间 → 摘要事件进入表面
//
// 规矩：中断只搬字节，这里所有事都在主循环里做。
#include <stdint.h>
#include <stddef.h>
#include "key.inc"          /* API_KEY，编译时编进镜像 */

/* ===== main.c 提供的底层能力 ===== */
void k_putc(char c);
void k_puts(const char *s);
void k_u64(uint64_t v);
void k_n(const char *s, uint32_t n);
uint64_t k_ticks(void);

#define T_REQUEST   1
#define T_FINAL     5

/* ===== 字符串小工具 ===== */
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static int sncmp(const char *a, const char *b, uint32_t n)
{ for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 1; return 0; }
static void scpy(char *d, const char *s, uint32_t cap)
{ uint32_t i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }

/* ===== 事件日志 + 表面 ===== */
#define LOG_MAX      1500
#define EV_TEXT      150
#define EV_TC_ARGS   200

enum { R_USER = 1, R_ASSIST = 2, R_TOOL = 3, R_SUMMARY = 4 };

typedef struct {
    uint16_t len;
    uint8_t  role;
    uint8_t  has_tc;
    char     text[EV_TEXT];
    char     tc_id[14];
    char     tc_name[24];
    char     tc_args[EV_TC_ARGS];
} ev_t;

static ev_t     log_buf[LOG_MAX];
static uint32_t log_len;
static uint32_t surface_start;

static int log_push(uint8_t role, const char *text)
{
    if (log_len >= LOG_MAX) return -1;
    ev_t *e = &log_buf[log_len++];
    e->role = role; e->has_tc = 0;
    scpy(e->text, text, EV_TEXT);
    e->len = (uint16_t)slen(e->text);
    e->tc_id[0] = 0; e->tc_name[0] = 0; e->tc_args[0] = 0;
    return 0;
}

static int log_push_tc(const char *text, const char *id, const char *name, const char *args)
{
    if (log_len >= LOG_MAX) return -1;
    ev_t *e = &log_buf[log_len++];
    e->role = R_ASSIST; e->has_tc = 1;
    scpy(e->text, text, EV_TEXT);
    scpy(e->tc_id, id, sizeof(e->tc_id));
    scpy(e->tc_name, name, sizeof(e->tc_name));
    scpy(e->tc_args, args, sizeof(e->tc_args));
    e->len = (uint16_t)slen(e->text);
    return 0;
}

static uint32_t surface_bytes(void)
{
    uint32_t n = 0;
    for (uint32_t i = surface_start; i < log_len; i++) {
        n += log_buf[i].len + 24;
        if (log_buf[i].has_tc) n += slen(log_buf[i].tc_args) + slen(log_buf[i].tc_name) + 40;
    }
    return n;
}

/* ===== 极简 JSON 读取（只认我们自己的 schema） ===== */
static void jws(const char **pp)
{ while (**pp == ' ' || **pp == '\t' || **pp == '\n' || **pp == '\r') (*pp)++; }

static const char *jskip_value(const char *p)
{
    jws(&p);
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') { p++; return p; }
            p++;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (*p) {
            if (*p == '"') { p = jskip_value(p); continue; }
            if (*p == '{' || *p == '[') { d++; p++; continue; }
            if (*p == '}' || *p == ']') { d--; p++; if (d == 0) return p; continue; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* 在对象里找顶层 key，返回值起点 */
static const char *jtop(const char *obj, const char *key)
{
    const char *p = obj;
    jws(&p);
    if (*p != '{') return 0;
    p++;
    for (;;) {
        jws(&p);
        if (*p != '"') return 0;
        const char *k = p + 1, *e = k;
        int esc = 0;
        while (*e) { char c = *e; if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') break; e++; }
        uint32_t kl = (uint32_t)(e - k), want = slen(key);
        int hit = (kl == want) && (sncmp(k, key, kl) == 0);
        p = e + 1;
        jws(&p);
        if (*p != ':') return 0;
        p++;
        jws(&p);
        if (hit) return p;
        p = jskip_value(p);
        jws(&p);
        if (*p == ',') { p++; continue; }
        return 0;
    }
}

static int jstr(const char *p, char *out, uint32_t cap)
{
    if (*p != '"') return -1;
    p++;
    uint32_t i = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case 'u': c = '?'; p += 4; break;
            default: c = *p; break;
            }
        }
        if (i + 1 < cap) out[i++] = c;
        p++;
    }
    out[i] = 0;
    return 0;
}

static int jint(const char *p, long *out)
{
    jws(&p);
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return -1;
    long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *out = neg ? -v : v;
    return 0;
}

static int jkey_str(const char *obj, const char *key, char *out, uint32_t cap)
{
    const char *v = jtop(obj, key);
    return v ? jstr(v, out, cap) : -1;
}
static int jkey_int(const char *obj, const char *key, long *out)
{
    const char *v = jtop(obj, key);
    return v ? jint(v, out) : -1;
}

/* ===== 硬件原语 ===== */
static void w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint32_t r32(uint64_t a) { return *(volatile uint32_t *)a; }

static void hx(char *out, uint32_t cap, uint64_t v)
{
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned d = (unsigned)((v >> (60 - i * 4)) & 0xF);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[18] = 0;
    scpy(out, b, cap);
}

/* ===== 复合工具（模型自己定义的） ===== */
#define CTOOL_MAX 12
typedef struct { char name[24]; char script[110]; } ctool_t;
static ctool_t ctools[CTOOL_MAX];
static uint32_t nctools;

static int run_script(const char *script, char *out, uint32_t cap)
{
    uint32_t o = 0;
    const char *p = script;
    while (*p) {
        while (*p == ';' || *p == ' ') p++;
        if (!*p) break;
        char op = *p++;
        if (*p == ':') p++;
        if (op == 'u') {
            char c = *p;
            if (c == '\\' && p[1]) { p++; c = (p[0] == 'n') ? '\n' : p[0]; }
            k_putc(c);
            if (o + 1 < cap) out[o++] = c;
        } else if (op == 'w') {
            uint64_t a = 0, v = 0;
            while (*p >= '0' && *p <= '9') { a = a * 10 + (uint64_t)(*p - '0'); p++; }
            while (*p == ',') p++;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (uint64_t)(*p - '0'); p++; }
            w32(a, (uint32_t)v);
        } else if (op == 'd') {
            uint64_t ms = 0;
            while (*p >= '0' && *p <= '9') { ms = ms * 10 + (uint64_t)(*p - '0'); p++; }
            for (volatile uint64_t i = 0; i < ms * 30000u; i++) { }
        } else {
            return -1;
        }
        while (*p && *p != ';') p++;
    }
    if (o == 0 && cap > 3) { out[0] = 'o'; out[1] = 'k'; o = 2; }
    out[o] = 0;
    return 0;
}

/* ===== 内建工具 ===== */
static int t_uart_putc(const char *a, char *out, uint32_t cap)
{
    char c[8];
    if (jkey_str(a, "c", c, sizeof(c)) == 0 && c[0]) {
        k_putc(c[0]);
        scpy(out, "printed", cap);
        return 0;
    }
    scpy(out, "missing c", cap);
    return -1;
}
static int t_read_reg(const char *a, char *out, uint32_t cap)
{
    long addr;
    if (jkey_int(a, "addr", &addr) != 0) { scpy(out, "missing addr", cap); return -1; }
    hx(out, cap, (uint64_t)r32((uint64_t)addr));
    return 0;
}
static int t_write_reg(const char *a, char *out, uint32_t cap)
{
    long addr, val;
    if (jkey_int(a, "addr", &addr) != 0 || jkey_int(a, "value", &val) != 0) {
        scpy(out, "missing addr/value", cap);
        return -1;
    }
    w32((uint64_t)addr, (uint32_t)val);
    scpy(out, "ok", cap);
    return 0;
}
static int t_ticks(const char *a, char *out, uint32_t cap)
{
    (void)a;
    uint64_t v = k_ticks();
    char b[21]; int i = 20; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v > 0 && i > 0) { b[--i] = (char)('0' + (int)(v % 10)); v /= 10; }
    scpy(out, &b[i], cap);
    return 0;
}
static int t_define_tool(const char *a, char *out, uint32_t cap)
{
    char name[24], script[110];
    if (jkey_str(a, "name", name, sizeof(name)) != 0 || !name[0]) { scpy(out, "missing name", cap); return -1; }
    if (jkey_str(a, "script", script, sizeof(script)) != 0) { scpy(out, "missing script", cap); return -1; }
    /* 同名就覆盖（改工具），不要占新槽 */
    for (uint32_t i = 0; i < nctools; i++) {
        if (scmp(ctools[i].name, name) == 0) {
            scpy(ctools[i].script, script, sizeof(ctools[i].script));
            k_puts("\n[改工具] ");
            k_puts(name);
            k_puts(" = ");
            k_puts(script);
            k_puts("\n");
            scpy(out, "redefined", cap);
            return 0;
        }
    }
    if (nctools >= CTOOL_MAX) { scpy(out, "too many tools", cap); return -1; }
    scpy(ctools[nctools].name, name, sizeof(ctools[nctools].name));
    scpy(ctools[nctools].script, script, sizeof(ctools[nctools].script));
    nctools++;
    k_puts("\n[新工具] ");
    k_puts(name);
    k_puts(" = ");
    k_puts(script);
    k_puts("\n");
    scpy(out, "defined", cap);
    return 0;
}
static int t_list_tools(const char *a, char *out, uint32_t cap)
{
    (void)a;
    uint32_t o = 0;
    for (uint32_t i = 0; i < nctools && o + 26 < cap; i++) {
        uint32_t n = slen(ctools[i].name);
        for (uint32_t j = 0; j < n && o + 2 < cap; j++) out[o++] = ctools[i].name[j];
        if (i + 1 < nctools) out[o++] = ',';
    }
    if (o == 0) scpy(out, "none", cap);
    else out[o] = 0;
    return 0;
}

typedef struct { const char *name; int (*fn)(const char *, char *, uint32_t); } tool_t;
static const tool_t tool_tab[] = {
    { "uart_putc",   t_uart_putc   },
    { "read_reg",    t_read_reg    },
    { "write_reg",   t_write_reg   },
    { "ticks",       t_ticks       },
    { "define_tool", t_define_tool },
    { "list_tools",  t_list_tools  },
};
#define TOOL_N (sizeof(tool_tab) / sizeof(tool_tab[0]))

static const char TOOLS_JSON[] =
"{\"type\":\"function\",\"function\":{\"name\":\"uart_putc\",\"description\":\"往控制台打印一个字符\","
"\"parameters\":{\"type\":\"object\",\"properties\":{\"c\":{\"type\":\"string\"}},\"required\":[\"c\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"read_reg\",\"description\":\"读一个32位寄存器\","
"\"parameters\":{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":\"integer\"}},\"required\":[\"addr\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"write_reg\",\"description\":\"写一个32位寄存器\","
"\"parameters\":{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"}},\"required\":[\"addr\",\"value\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"ticks\",\"description\":\"当前系统 tick(秒)\","
"\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"define_tool\",\"description\":\"定义一个新工具。script 用分号分隔：u:X 打印字符X，w:地址,值 写寄存器，d:毫秒 延时\","
"\"parameters\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"script\":{\"type\":\"string\"}},\"required\":[\"name\",\"script\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"list_tools\",\"description\":\"列出已定义的工具名\","
"\"parameters\":{\"type\":\"object\",\"properties\":{}}}}";

/* 系统提示与目标（UTF-8） */
static const char SYS_PROMPT[] =
"你是运行在一台 ARM 裸机内核里的 agent。你没有操作系统，工具直接操作硬件。"
"规则：每步只调一个工具，不要长篇解释；多用 define_tool 定义新工具再调用；同名工具会被覆盖（可以改），要真的动手。";

static const char GOAL[] =
"开始工作：先用 uart_putc 打出你的名字。然后用 define_tool 定义至少 3 个新工具"
"（例如画横线 line、画框 box、闪一下 blink），然后调用它们在这块屏幕上作画。";

/* ===== 请求拼装 ===== */
static char     req[8000];
static uint32_t reqlen;

static void apc(char c) { if (reqlen + 1 < sizeof(req)) req[reqlen++] = c; }
static void aps(const char *s) { while (*s) apc(*s++); }
static void apj(const char *s)
{
    while (*s) {
        char c = *s++;
        if (c == '"' || c == '\\') { apc('\\'); apc(c); }
        else if (c == '\n') { apc('\\'); apc('n'); }
        else if (c == '\r') { }
        else if ((unsigned char)c < 0x20) { }
        else apc(c);
    }
}

static void ap_messages(void)
{
    int first = 1;
    for (uint32_t i = surface_start; i < log_len; i++) {
        ev_t *e = &log_buf[i];
        if (!first) apc(',');
        first = 0;
        if (e->role == R_TOOL) {
            aps("{\"role\":\"tool\",\"content\":\"");
            apj(e->text);
            aps("\",\"tool_call_id\":\"");
            apj(e->tc_id);
            aps("\"}");
        } else if (e->role == R_ASSIST) {
            aps("{\"role\":\"assistant\",\"content\":\"");
            apj(e->text);
            aps("\"");
            if (e->has_tc) {
                aps(",\"tool_calls\":[{\"id\":\"");
                apj(e->tc_id);
                aps("\",\"type\":\"function\",\"function\":{\"name\":\"");
                apj(e->tc_name);
                aps("\",\"arguments\":\"");
                apj(e->tc_args);
                aps("\"}}]");
            }
            aps("}");
        } else {
            aps("{\"role\":\"user\",\"content\":\"");
            apj(e->text);
            aps("\"}");
        }
    }
}

static void build_step_request(void)
{
    reqlen = 0;
    aps(API_KEY);
    apc('\n');
    aps("{\"model\":\"deepseek-chat\",\"stream\":false,\"temperature\":0.3,\"max_tokens\":200,\"messages\":[");
    aps("{\"role\":\"system\",\"content\":\"");
    apj(SYS_PROMPT);
    aps("\"},");
    ap_messages();
    aps("],\"tools\":[");
    aps(TOOLS_JSON);
    aps("]}");
}

static void build_summary_request(void)
{
    reqlen = 0;
    aps(API_KEY);
    apc('\n');
    aps("{\"model\":\"deepseek-chat\",\"stream\":false,\"temperature\":0,\"max_tokens\":200,\"messages\":[");
    aps("{\"role\":\"system\",\"content\":\"你是压缩器。把对话压成三行以内要点，保留：目标、已定义的工具名、屏幕当前内容。只输出要点，不要解释。\"},");
    aps("{\"role\":\"user\",\"content\":\"");
    for (uint32_t i = surface_start; i < log_len && reqlen < sizeof(req) - 400; i++) {
        ev_t *e = &log_buf[i];
        aps("["); aps(e->role == R_TOOL ? "tool" : "asst"); aps("] ");
        apj(e->text);
        if (e->has_tc) { aps(" "); apj(e->tc_name); aps(" "); apj(e->tc_args); }
        aps("\\n");
    }
    aps("\"}]}");
}

/* ===== 状态机 ===== */
enum { A_IDLE = 0, A_WAIT_MODEL, A_WAIT_SUMMARY };

static int      state;
static uint32_t steps, rounds, total_calls;
static uint64_t goal_started;
static char     summary[360];
static uint32_t nsummaries;

#define STEP_MAX        60
#define SURFACE_BUDGET  2000
#define RUN_SECONDS     900

static char     pending[8000];
static uint32_t pending_len;

int agent_take_request(char *out, uint32_t cap)
{
    if (pending_len == 0) return 0;
    uint32_t n = pending_len < cap ? pending_len : cap;
    for (uint32_t i = 0; i < n; i++) out[i] = pending[i];
    pending_len = 0;
    return (int)n;
}

static void queue(int summary_mode)
{
    if (summary_mode) build_summary_request();
    else              build_step_request();
    uint32_t n = reqlen < sizeof(pending) ? reqlen : sizeof(pending) - 1;
    for (uint32_t i = 0; i < n; i++) pending[i] = req[i];
    pending_len = n;
    state = summary_mode ? A_WAIT_SUMMARY : A_WAIT_MODEL;
}

static void do_tool(const char *id, const char *name, const char *args, char *out, uint32_t cap)
{
    total_calls++;
    k_puts("[工具] ");
    k_puts(name);
    k_puts("\n");

    for (uint32_t i = 0; i < TOOL_N; i++) {
        if (scmp(tool_tab[i].name, name) == 0) {
            out[0] = 0;
            tool_tab[i].fn(args, out, cap);
            if (!out[0]) scpy(out, "ok", cap);
            log_push(R_TOOL, out);
            if (log_len) scpy(log_buf[log_len - 1].tc_id, id, sizeof(log_buf[log_len - 1].tc_id));
            return;
        }
    }
    for (uint32_t i = 0; i < nctools; i++) {
        if (scmp(ctools[i].name, name) == 0) {
            run_script(ctools[i].script, out, cap);
            log_push(R_TOOL, out);
            if (log_len) scpy(log_buf[log_len - 1].tc_id, id, sizeof(log_buf[log_len - 1].tc_id));
            return;
        }
    }
    scpy(out, "unknown tool", cap);
    log_push(R_TOOL, out);
    if (log_len) scpy(log_buf[log_len - 1].tc_id, id, sizeof(log_buf[log_len - 1].tc_id));
}

/* main.c 收到 FINAL 帧时调用 */
void agent_on_final(const char *json, uint32_t n)
{
    (void)n;

    if (state == A_WAIT_SUMMARY) {
        char c[360];
        c[0] = 0;
        if (jkey_str(json, "content", c, sizeof(c)) == 0 && c[0]) {
            scpy(summary, c, sizeof(summary));
            nsummaries++;
            surface_start = log_len;              /* 遮蔽前面的原文（日志还在） */
            log_push(R_SUMMARY, summary);          /* 摘要作为新表面的第一条 */
            k_puts("[压缩] 摘要 ");
            k_u64(slen(summary));
            k_puts(" 字节，遮蔽到 seq ");
            k_u64(surface_start);
            k_puts("\n");
        } else {
            k_puts("[压缩] 摘要失败，保持原样\n");
        }
        state = A_IDLE;
        return;
    }

    char fr[20], content[150];
    fr[0] = 0; content[0] = 0;
    jkey_str(json, "finish_reason", fr, sizeof(fr));
    jkey_str(json, "content", content, sizeof(content));
    if (content[0]) {
        k_puts("[说] ");
        k_puts(content);
        k_puts("\n");
    }

    const char *tc = jtop(json, "tool_calls");
    if (tc && *tc == '[') {
        const char *p = tc + 1;
        jws(&p);
        if (*p == '{') {
            char id[14] = "call", name[24] = "", args[EV_TC_ARGS] = "";
            jkey_str(p, "id", id, sizeof(id));
            jkey_str(p, "name", name, sizeof(name));
            jkey_str(p, "arguments", args, sizeof(args));
            if (name[0]) {
                log_push_tc(content, id, name, args);
                char out[140];
                out[0] = 0;
                do_tool(id, name, args, out, sizeof(out));
                steps++;
                state = A_IDLE;
                return;
            }
        }
    }

    if (content[0]) log_push(R_ASSIST, content);
    k_puts("[轮次] 结束：");
    k_u64(k_ticks() - goal_started);
    k_puts("s / 工具调用 ");
    k_u64(total_calls);
    k_puts(" 次 / 压缩 ");
    k_u64(nsummaries);
    k_puts(" 次\n");
    steps = 0;
    state = A_IDLE;
}

/* 每个主循环调用一次 */
void agent_tick(void)
{
    if (state != A_IDLE) return;
    if (k_ticks() - goal_started > RUN_SECONDS) return;

    /* 日志快满了：硬压缩，只留最近 4 个事件（日志是环形的，不是无限的） */
    if (log_len >= LOG_MAX - 16) {
        uint32_t from = (log_len > 4) ? log_len - 4 : 0;
        uint32_t k = 0;
        for (uint32_t i = from; i < log_len; i++) log_buf[k++] = log_buf[i];
        log_len = k;
        surface_start = 0;
        k_puts("[压缩] 日志满，硬重置，只留 "); k_u64(k); k_puts(" 条\n");
    }

    if (surface_bytes() > SURFACE_BUDGET && (log_len - surface_start) > 4) {
        k_puts("[压缩] 表面 ");
        k_u64(surface_bytes());
        k_puts(" 字节超预算\n");
        queue(1);
        return;
    }

    if (steps >= STEP_MAX) {
        steps = 0;
        rounds++;
        log_push(R_USER, "继续");
    }
    queue(0);
}

void agent_init(void)
{
    log_len = 0; surface_start = 0;
    nsummaries = 0; summary[0] = 0;
    steps = 0; rounds = 1; total_calls = 0;
    state = A_IDLE;
    goal_started = k_ticks();
    log_push(R_USER, GOAL);
}
