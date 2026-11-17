#include "ctrlc.h"
#include "../io/kbdtap.h"
#include "../utility/utility.h"

kjmp_t g_ctrlc_jmp;

static volatile bool g_running, g_armed, g_pending;
static volatile int  g_hold;

__asm__(
    ".text\n"
    ".globl kjmp_set\n"
    "kjmp_set:\n"
    "    movl 4(%esp), %eax\n"
    "    movl %ebx, 0(%eax)\n"
    "    movl %esi, 4(%eax)\n"
    "    movl %edi, 8(%eax)\n"
    "    movl %ebp, 12(%eax)\n"
    "    leal 4(%esp), %ecx\n"
    "    movl %ecx, 16(%eax)\n"
    "    movl (%esp), %ecx\n"
    "    movl %ecx, 20(%eax)\n"
    "    pushfl\n"
    "    popl %ecx\n"
    "    movl %ecx, 24(%eax)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".globl kjmp_long\n"
    "kjmp_long:\n"
    "    movl 4(%esp), %edx\n"
    "    movl 8(%esp), %eax\n"
    "    movl 0(%edx), %ebx\n"
    "    movl 4(%edx), %esi\n"
    "    movl 8(%edx), %edi\n"
    "    movl 12(%edx), %ebp\n"
    "    movl 16(%edx), %esp\n"
    "    pushl 24(%edx)\n"
    "    popfl\n"
    "    jmp *20(%edx)\n"
);

// Full-screen programs and games read Ctrl+key themselves; prompts end on Ctrl+C by themselves.
static const char *const EXEMPT[] = {
    "doom", "snake", "life", "matrix", "vkdemo", "desktop", "rde", "gfxtest", "rie", "png", "mp4", "ytm", "discord", "files",
    "sysmon", "top", "gbrowser", "gb", "rchat", "help", "logout", "read", "text", "edit", "nano", "vi", "pong", "dshell",
};

bool ctrlc_exempt(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    for (int pass = 0; pass < 2; pass++) {                 // "sudo doom" is as exempt as "doom"
        char word[24];
        size_t n = 0;
        while (line[n] && line[n] != ' ' && line[n] != '\t' && n < sizeof(word) - 1) { word[n] = line[n]; n++; }
        word[n] = '\0';
        if (pass == 0 && strcmp(word, "sudo") == 0) {
            line += n;
            while (*line == ' ' || *line == '\t') line++;
            continue;
        }
        for (size_t i = 0; i < sizeof(EXEMPT) / sizeof(EXEMPT[0]); i++)
            if (strcmp(word, EXEMPT[i]) == 0) return true;
        break;
    }
    return false;
}

void ctrlc_begin(bool ctrl_down)
{
    g_hold = 0;
    g_pending = false;
    kbdtap_reset(ctrl_down);
    g_armed = true;
    g_running = true;
    __asm__ volatile("sti" ::: "memory");
}

void ctrlc_end(void)
{
    __asm__ volatile("cli" ::: "memory");
    g_running = g_armed = g_pending = false;
    g_hold = 0;
}

void ctrlc_disarm(void)
{
    if (!g_running) return;
    __asm__ volatile("cli" ::: "memory");
    g_armed = false;
}

bool ctrlc_running(void) { return g_running; }

static void abort_now(void)
{
    g_pending = false;
    g_hold = 0;
    g_armed = false;                // keys already queued, including the Ctrl+C itself, stay for the prompt
    kjmp_long(&g_ctrlc_jmp, 1);
}

void ctrlc_abort(void) { abort_now(); }

void ctrlc_tick(void)
{
    if (!g_running || !g_armed) return;
    kbdtap_drain();
    if (!kbdtap_take_hit()) return;
    if (g_hold > 0) { g_pending = true; return; }
    abort_now();
}

void ctrlc_hold(void) { g_hold++; }

void ctrlc_release(void)
{
    if (g_hold > 0) g_hold--;
    if (g_hold == 0 && g_pending && g_running && g_armed) abort_now();
}
