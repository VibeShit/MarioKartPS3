/* Tiny synthetic "game" used to exercise the translator -> PS3 build pipeline
 * without any Nintendo data. Compiled for 32-bit PowerPC (750/Gekko subset). */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static volatile u32 g_counter;
static float g_values[16] = {1.0f, 2.0f, 3.0f, 4.0f};
static u32 g_table[64];

__attribute__((noinline)) static u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

__attribute__((noinline)) static float dot(const float* a, const float* b, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; ++i)
        s += a[i] * b[i];
    return s;
}

__attribute__((noinline)) static void fill(u32* dst, int n, u32 seed) {
    for (int i = 0; i < n; ++i) {
        seed = seed * 1103515245u + 12345u;
        dst[i] = (seed >> 16) ^ (seed << 3);
    }
}

typedef u32 (*op_fn)(u32, u32);
static u32 op_add(u32 a, u32 b) { return a + b; }
static u32 op_xor(u32 a, u32 b) { return a ^ b; }
static u32 op_rot(u32 a, u32 b) { return (a << (b & 31)) | (a >> ((32 - b) & 31)); }
static op_fn g_ops[3] = {op_add, op_xor, op_rot};

u32 game_main(void) {
    fill(g_table, 64, 7);
    u32 acc = fib(12);
    for (int i = 0; i < 64; ++i)
        acc = g_ops[i % 3](acc, g_table[i]);
    float d = dot(g_values, g_values, 16);
    g_counter = acc + (u32)d;
    return g_counter;
}

void _start(void) {
    __asm__ volatile("lis 1, 0x8170\n ori 1, 1, 0x0000\n");
    game_main();
    for (;;) {
    }
}
