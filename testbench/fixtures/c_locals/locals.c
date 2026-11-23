/* cl65 -t cx16 -Or -g -Wl --dbgfile,locals.dbg -o locals.prg locals.c */

#include <6502.h>
#include <stdarg.h>

#define NOTE(x) (g_addr[g_n++] = (unsigned)&(x))

unsigned g_addr[32];
unsigned char g_n;
unsigned char g_case;
unsigned char g_spin;
volatile unsigned char g_irq_seen;
unsigned g_irq_addr;
unsigned g_main_addr[3];
unsigned char g_irq_stack[128];

void probe(void)
{
    ++g_case;
}

void irq_probe(void)
{
    ++g_irq_seen;
}

int fast1(int a)
{
    int l = 0x1234;
    unsigned char arr[3];
    arr[0] = 0xA1; arr[1] = 0xA2; arr[2] = 0xA3;
    g_n = 0;
    NOTE(a); NOTE(l); NOTE(arr);
    probe();
    return a + l + arr[1];
}

long fast3(char a, int b, long c)
{
    char x = 0x5A;
    g_n = 0;
    NOTE(a); NOTE(b); NOTE(c); NOTE(x);
    probe();
    return a + b + c + x;
}

int __cdecl__ cd(int a, char b)
{
    int l = 0x7788;
    g_n = 0;
    NOTE(a); NOTE(b); NOTE(l);
    probe();
    return a + b + l;
}

int recurse(unsigned char depth)
{
    int tag = 0x4000 + depth;
    int r;
    g_addr[depth] = (unsigned)&tag;
    if (depth == 0) {
        g_n = 4;
        probe();
        return tag;
    }
    r = recurse(depth - 1);
    return r + tag;
}

int viaptr(int v)
{
    int w = 0x2468;
    g_n = 0;
    NOTE(v); NOTE(w);
    probe();
    return v + w;
}

int va(int fixed, ...)
{
    va_list ap;
    int l = 0x3344;
    va_start(ap, fixed);
    g_n = 0;
    NOTE(l);
    probe();
    l += va_arg(ap, int);
    va_end(ap);
    return l + fixed;
}

int withreg(int a)
{
    register int r = a + 1;
    int l = 0x6655;
    g_n = 0;
    NOTE(a); NOTE(l);
    probe();
    return r + l;
}

unsigned char on_irq(void)
{
    int mine = 0x1EE7;
    g_irq_addr = (unsigned)&mine;
    irq_probe();
    return mine == 0x1EE7 ? IRQ_NOT_HANDLED : IRQ_HANDLED;
}

void spin(int v)
{
    int w = 0x5150;
    g_n = 0;
    NOTE(v); NOTE(w);
    probe();
    for (;;)
        ++g_spin;
}

int (*g_fp)(int) = viaptr;
long g_sink;

int main(int argc, char *argv[])
{
    int mine = 0x4D41;
    g_main_addr[0] = (unsigned)&argc;
    g_main_addr[1] = (unsigned)&argv;
    g_main_addr[2] = (unsigned)&mine;
    g_sink  = fast1(0x1111);
    g_sink += fast3(0x22, 0x3333, 0x44556677L);
    g_sink += cd(0x0102, 0x03);
    g_sink += recurse(3);
    g_sink += g_fp(0x0BAD);
    g_sink += va(0x0F0F, 0x0505);
    g_sink += withreg(0x0A0B);

    set_irq(on_irq, g_irq_stack, sizeof g_irq_stack);
    while (!g_irq_seen)
        ;
    reset_irq();

    spin(0x7E57);
    return mine + argc + (argv != 0);
}
