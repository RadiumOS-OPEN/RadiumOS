#include "kbdtap.h"

#define Q_SIZE 128

static volatile uint8_t  g_q[Q_SIZE], g_aux[Q_SIZE];
static volatile uint32_t g_head, g_tail;
static volatile bool     g_ctrl, g_hit;
static volatile bool     g_wm, g_alt;
static volatile uint8_t  g_mq[64];
static volatile uint32_t g_mh, g_mt;
static volatile int      g_hot;

static inline uint8_t hw_in(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void kbdtap_reset(bool ctrl_down)
{
    g_ctrl = ctrl_down;
    g_hit = false;
}

void kbdtap_clear(void)
{
    g_head = g_tail = 0;
    g_hit = false;
}

void kbdtap_drain(void)
{
    for (int n = 0; n < 32; n++) {
        uint8_t st = hw_in(0x64);
        if (!(st & 1)) return;
        uint8_t b = hw_in(0x60);
        bool aux = (st & 0x20) != 0;
        if (g_wm && aux) {
            uint32_t mn = (g_mh + 1) % 64;
            if (mn != g_mt) { g_mq[g_mh] = b; g_mh = mn; }
            continue;
        }
        uint32_t next = (g_head + 1) % Q_SIZE;
        if (next != g_tail) { g_q[g_head] = b; g_aux[g_head] = aux; g_head = next; }
        if (aux) continue;
        if (g_wm) {
            if (b == 0x38) g_alt = true;
            else if (b == 0xB8) g_alt = false;
            else if (b == 0x57) g_hot = 2;
            else if (g_alt && b == 0x3E) g_hot = 1;
            else if (g_alt && b == 0x1C) g_hot = 2;
            else if (g_alt && b == 0x44) g_hot = 3;
        }
        if (b == 0x1D) g_ctrl = true;               // left or right Ctrl (E0 prefix is ignored)
        else if (b == 0x9D) g_ctrl = false;
        else if (b == 0x2E && g_ctrl) g_hit = true; // C pressed while Ctrl is down
    }
}

bool kbdtap_pop(uint8_t *byte)
{
    if (g_head == g_tail) return false;
    *byte = g_q[g_tail];
    g_tail = (g_tail + 1) % Q_SIZE;
    return true;
}

uint8_t kbdtap_fix_status(uint8_t status)
{
    if (g_head == g_tail) return status;
    status |= 1;
    if (g_aux[g_tail]) status |= 0x20; else status &= (uint8_t)~0x20;
    return status;
}

bool kbdtap_take_hit(void)
{
    bool h = g_hit;
    g_hit = false;
    return h;
}

void kbdtap_wm(bool on)
{
    g_wm = on;
    g_mh = g_mt = 0;
    g_hot = 0;
    g_alt = false;
}

bool kbdtap_wm_active(void) { return g_wm; }

bool kbdtap_mouse_pop(uint8_t *byte)
{
    if (g_mh == g_mt) return false;
    *byte = g_mq[g_mt];
    g_mt = (g_mt + 1) % 64;
    return true;
}

int kbdtap_take_hotkey(void)
{
    int h = g_hot;
    g_hot = 0;
    return h;
}
