// token-os v4：内核里真的住着一个 agent
//
//   UART0 = 人的控制台     UART1 = token 链路（帧协议）
//   main.c 只负责：串口、GIC、定时器、帧收发
//   agent.c 负责：事件日志、surface、工具、循环、压缩
#include <stdint.h>
#include <stddef.h>

/* ============================ 串口 ============================ */
typedef struct {
    volatile uint32_t dr, rsrecr, _p0[4], fr, _p1, ilpr, ibrd, fbrd, lcrh,
                      cr, ifls, imsc, ris, mis, icr;
} pl011_t;

#define UART0  ((pl011_t *)0x09000000UL)
#define UART1  ((pl011_t *)0x09040000UL)

#define FR_TXFF   (1u << 5)
#define FR_RXFE   (1u << 4)
#define CR_UARTEN (1u << 0)
#define CR_TXE    (1u << 8)
#define CR_RXE    (1u << 9)
#define IMSC_RXIM (1u << 4)

/* ============================ GICv2 ============================ */
#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL
#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))
#define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x00c))
#define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x010))

#define IRQ_TIMER_EL1   30
#define IRQ_UART1       40      /* virt irqmap: VIRT_UART1 = 8 -> SPI 40 */

/* ============================ 小工具 ============================ */
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline uint64_t r_cntfrq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static inline void w_cntp_tval(uint64_t v) { __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v)); }
static inline void w_cntp_ctl(uint64_t v) { __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v)); }
static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void wfi(void) { __asm__ volatile("wfi"); }
extern char vector_table[];

/* ============================ 控制台 ============================ */
static void u0_init(void)
{
    UART0->cr = 0;
    UART0->ibrd = 13; UART0->fbrd = 1;
    UART0->lcrh = (3u << 5) | (1u << 4);
    UART0->cr = CR_UARTEN | CR_TXE | CR_RXE;
}
void k_putc(char c)
{
    while (UART0->fr & FR_TXFF) { }
    UART0->dr = (uint32_t)(unsigned char)c;
}
void k_puts(const char *s)
{
    while (*s) { if (*s == '\n') k_putc('\r'); k_putc(*s++); }
}
void k_n(const char *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) k_putc(s[i]); }
void k_u64(uint64_t v)
{
    char b[21]; int i = 20; b[i] = 0;
    if (!v) { k_putc('0'); return; }
    while (v > 0 && i > 0) { b[--i] = (char)('0' + (int)(v % 10)); v /= 10; }
    k_puts(&b[i]);
}

/* clang -O2 可能生成这些调用，裸机得自己提供 */
void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

/* ============================ 链路 ============================ */
static void u1_init(void)
{
    UART1->cr = 0;
    UART1->ibrd = 13; UART1->fbrd = 1;
    UART1->lcrh = (3u << 5) | (1u << 4);
    UART1->icr = 0x7FF;
    UART1->imsc = IMSC_RXIM;
    UART1->cr = CR_UARTEN | CR_TXE | CR_RXE;
}
static void u1_putc(char c) { while (UART1->fr & FR_TXFF) { } UART1->dr = (uint32_t)(unsigned char)c; }
static inline int u1_has(void) { return (UART1->fr & FR_RXFE) == 0; }
static inline uint8_t u1_get(void) { return (uint8_t)UART1->dr; }

#define MAGIC0   0xA5
#define MAGIC1   0x5A
#define T_REQUEST 1
#define T_ERROR   4
#define T_FINAL   5

static inline uint8_t crc8b(uint8_t c, uint8_t b)
{
    c ^= b;
    for (int i = 0; i < 8; i++) c = (uint8_t)((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
    return c;
}

void link_send(uint8_t type, const uint8_t *p, uint16_t n)
{
    uint8_t c = crc8b(0, type);
    c = crc8b(c, (uint8_t)(n >> 8));
    c = crc8b(c, (uint8_t)(n & 0xFF));
    for (uint16_t i = 0; i < n; i++) c = crc8b(c, p[i]);
    u1_putc((char)MAGIC0); u1_putc((char)MAGIC1); u1_putc((char)type);
    u1_putc((char)(n >> 8)); u1_putc((char)(n & 0xFF));
    for (uint16_t i = 0; i < n; i++) u1_putc((char)p[i]);
    u1_putc((char)c);
}

/* ============================ 收帧 ============================ */
enum { RX_M0, RX_M1, RX_TYPE, RX_LH, RX_LL, RX_PAY, RX_CRC };
static volatile uint8_t  rx_state, rx_type, rx_crc;
static volatile uint16_t rx_need, rx_have;
static volatile uint8_t  rxp[4096];

static uint32_t g_frames, g_badcrc, g_bytes, g_err, g_big;

void agent_on_final(const char *json, uint32_t n);

/* 中断里只把整帧拷进作业槽，重活全在主循环里做。
   硬规矩：中断里一慢，串口 FIFO 就溢出，下一帧直接丢。 */
static char             job_buf[4096];
static volatile uint32_t job_len, job_ready, job_lost;

static void rx_frame(uint8_t type)
{
    if (type == T_FINAL) {
        g_frames++;
        if (job_ready) { job_lost++; return; }
        uint16_t n = rx_need;
        for (uint16_t i = 0; i < n; i++) job_buf[i] = (char)rxp[i];
        job_len = n;
        job_ready = 1;
    }
    else if (type == T_ERROR) {
        g_err++;
        k_puts("[链路] 中转报错: ");
        k_n((const char *)rxp, rx_need > 120 ? 120 : rx_need);
        k_puts("\n");
    }
}

static void rx_byte(uint8_t b)
{
    switch (rx_state) {
    case RX_M0: if (b == MAGIC0) rx_state = RX_M1; break;
    case RX_M1: rx_state = (b == MAGIC1) ? RX_TYPE : RX_M0; break;
    case RX_TYPE: rx_type = b; rx_crc = crc8b(0, b); rx_state = RX_LH; break;
    case RX_LH: rx_need = (uint16_t)(b << 8); rx_crc = crc8b(rx_crc, b); rx_state = RX_LL; break;
    case RX_LL:
        rx_need = (uint16_t)(rx_need | b);
        rx_crc = crc8b(rx_crc, b);
        rx_have = 0;
        if (rx_need > sizeof(rxp)) { g_big++; rx_state = RX_M0; }
        else rx_state = (rx_need == 0) ? RX_CRC : RX_PAY;
        break;
    case RX_PAY:
        rxp[rx_have++] = b;
        rx_crc = crc8b(rx_crc, b);
        if (rx_have >= rx_need) rx_state = RX_CRC;
        break;
    case RX_CRC:
        if (rx_crc == b) rx_frame(rx_type); else g_badcrc++;
        rx_state = RX_M0;
        break;
    default: rx_state = RX_M0; break;
    }
}

/* ============================ 中断 ============================ */
static volatile uint64_t g_ticks;

uint64_t k_ticks(void) { return g_ticks; }

void irq_handler(void)
{
    uint32_t iar = GICC_IAR;
    uint32_t irq = iar & 0x3FF;
    if (irq == IRQ_UART1) {
        while (u1_has()) { g_bytes++; rx_byte(u1_get()); }
        UART1->icr = 0x7FF;
    } else if (irq == IRQ_TIMER_EL1) {
        g_ticks++;
        w_cntp_tval(r_cntfrq());
        w_cntp_ctl(1);
    }
    GICC_EOIR = iar;
}

void fault_handler(uint64_t kind, uint64_t esr, uint64_t elr, uint64_t far)
{
    k_puts("\n!!! 异常 !!! kind=");
    k_u64(kind);
    k_puts(" esr=0x"); for (int s = 60; s >= 0; s -= 4) k_putc("0123456789abcdef"[(esr >> s) & 0xF]);
    k_puts(" elr=0x"); for (int s = 60; s >= 0; s -= 4) k_putc("0123456789abcdef"[(elr >> s) & 0xF]);
    k_puts(" far=0x"); for (int s = 60; s >= 0; s -= 4) k_putc("0123456789abcdef"[(far >> s) & 0xF]);
    k_puts("\n");
    for (;;) { __asm__ volatile("wfe"); }
}

/* ============================ GIC ============================ */
static void gic_enable_irq(uint32_t irq, uint8_t prio, uint8_t target)
{
    uint32_t shift = (irq & 3) * 8;
    volatile uint32_t *pr = (volatile uint32_t *)(GICD_BASE + 0x400 + (irq & ~3u));
    uint32_t pv = *pr;
    *pr = (pv & ~(0xFFu << shift)) | ((uint32_t)prio << shift);
    if (irq >= 32) {
        volatile uint32_t *tr = (volatile uint32_t *)(GICD_BASE + 0x800 + (irq & ~3u));
        uint32_t tv = *tr;
        *tr = (tv & ~(0xFFu << shift)) | ((uint32_t)target << shift);
    }
    GICD_ISENABLER[irq / 32] = (1u << (irq % 32));
}

static void gic_init(void)
{
    GICD_CTLR = 0;
    gic_enable_irq(IRQ_TIMER_EL1, 0xC0, 0x01);
    gic_enable_irq(IRQ_UART1,      0x10, 0x01);
    dsb();
    GICD_CTLR = 3;
    GICC_PMR = 0xF0;
    dsb();
    GICC_CTLR = 3;
    dsb();
}

/* ============================ main ============================ */
void agent_init(void);
void agent_tick(void);
int  agent_take_request(char *out, uint32_t cap);

static char reqbuf[8000];

int main(void)
{
    u0_init();
    k_puts("\n================================================\n");
    k_puts(" token-os v4 / agent 已住进内核\n");
    k_puts("================================================\n");

    __asm__ volatile("msr vbar_el1, %0" :: "r"(vector_table));
    isb();
    gic_init();
    u1_init();
    w_cntp_tval(r_cntfrq());
    w_cntp_ctl(1);

    k_puts("[init] 串口 / GIC / 定时器 就绪\n");
    k_puts("[init] 事件日志 / 工具表 / 压缩器 就绪\n");
    k_puts("[init] 开中断，交给 agent\n\n");

    agent_init();
    irq_enable();

    uint64_t last_report = (uint64_t)-1;

    for (;;) {
        /* 兜底：QEMU 的 chardev 偶尔会卡在"等前台开口"的状态，
           中断不再来，但 FIFO 里其实还有字节。主循环顺手抽一下就能解开。
           抽的时候关中断，避免和 ISR 抢同一个解析状态机。 */
        __asm__ volatile("msr daifset, #2" ::: "memory");
        while (u1_has()) { g_bytes++; rx_byte(u1_get()); }
        __asm__ volatile("msr daifclr, #2" ::: "memory");

        if (job_ready) {
            uint32_t len = job_len;
            job_ready = 0;
            agent_on_final(job_buf, len);
        }
        agent_tick();

        int n = agent_take_request(reqbuf, sizeof(reqbuf));
        if (n > 0) link_send(T_REQUEST, (const uint8_t *)reqbuf, (uint16_t)n);

        uint64_t slot = g_ticks / 10;
        if (slot != last_report) {
            last_report = slot;
            k_puts("[链路] t="); k_u64(g_ticks);
            k_puts(" 收到="); k_u64(g_frames);
            k_puts(" 字节="); k_u64(g_bytes);
            k_puts(" 坏帧="); k_u64(g_badcrc);
            k_puts(" 超长="); k_u64(g_big);
            k_puts(" 丢帧="); k_u64(job_lost);
            k_puts(" 上帧长="); k_u64(rx_need);
            k_puts("\n");
        }

        wfi();
    }
}
