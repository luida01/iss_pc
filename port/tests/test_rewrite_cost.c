/* Cost of the rewrites the port applies to the guest's code: the Windows red-zone protection
 * (src/isa_rewrite.c isa_red_zone_apply) and the ISA emulation stubs (isa_rewrite.c emit_stub).
 *
 * Method: the same guest code is called in a loop, once with the rewrite applied and once
 * without; the difference is what the rewrite costs per execution (the call and the loop are in
 * both measurements, so they cancel). The red-zone part uses the fixture of
 * tests/test_red_zone.py (guest functions that keep a value in the red zone across a store): the
 * data page stays writable, so no fault happens and the measurement is the rewrite's cost alone
 * (the fault path costs the same with and without the rewrite).
 *
 * The ISA part measures the emulation stub against the native instruction on the same snippet
 * (BMI1 is native on every host the port runs on; SSE4A, the set this CPU lacks, is only
 * measured in absolute terms).
 *
 * Usage: rewrite-cost-test tests/data/red_zone_image.bin tests/data/red_zone_table.bin [calls]
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#endif
#include "isa_rewrite.h"
#include "cpu_check.h"

typedef uint64_t (ABI *Function)(uint32_t *page, uint64_t value);
typedef uint64_t (ABI *AnyFn)(uint64_t a, uint64_t b);
enum { PAGE = 4096, IMAGE = 2 * PAGE, DATA = PAGE, INDEX = 0xf00, AREA = 4 * PAGE };

static uint64_t now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ll) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t now_cycles(void) {
#if defined(__x86_64__)
    return __rdtsc();
#else
    return 0;
#endif
}

static volatile uint64_t sink;

/* The cheapest time of five runs of `calls` calls: the noise of the machine matters more than
 * the mean when the difference is a few nanoseconds. */
#define REPS 5
static void measure(AnyFn fn, uint64_t a, uint64_t b, uint64_t calls, uint64_t *ns, uint64_t *cycles) {
    uint64_t best_ns = UINT64_MAX, best_cycles = UINT64_MAX;
    for (int rep = 0; rep < REPS; rep++) {
        uint64_t start_ns = now_ns(), start_cycles = now_cycles(), total = 0;
        for (uint64_t i = 0; i < calls; i++) total += fn(a, b);
        uint64_t elapsed_ns = now_ns() - start_ns, elapsed_cycles = now_cycles() - start_cycles;
        sink += total;
        if (elapsed_ns < best_ns) {
            best_ns = elapsed_ns;
            best_cycles = elapsed_cycles;
        }
    }
    *ns = best_ns;
    *cycles = best_cycles;
}

/* ---- the red-zone fixture ---- */

static unsigned char *region, *pristine;
static IsaTable *table;

static uint32_t index_count(void) {
    uint32_t n;
    memcpy(&n, pristine + INDEX, 4);
    return n;
}

static void entry(uint32_t i, uint32_t *function) {
    memcpy(function, pristine + INDEX + 4 + 8 * i, 4);
}

static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fread(pristine, 1, IMAGE, f) == IMAGE && fgetc(f) == EOF);
    fclose(f);
    memcpy(region, pristine, IMAGE);
}

/* The fixture's function names (tests/test_red_zone.py fixture_functions). */
static const char *const FIXTURE_NAMES[] = {"jmp",  "forward", "backward", "rip",  "flags",
                                            "vex",  "push_sub", "dead",   "padding_relay", "host_relay"};

/* Every fixture function with the rewrite and without it. */
static void bench_red_zone(uint64_t calls) {
    memcpy(region, pristine, IMAGE);
    uint32_t count = index_count();
    printf("Red zone: %u fixture functions, %llu calls each\n", count, (unsigned long long)calls);
    printf("  %-16s %12s %12s %10s %10s\n", "function", "plain ns", "patched ns", "delta ns", "delta cyc");
    uint64_t plain_total = 0, patched_total = 0, plain_cycles = 0, patched_cycles = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t function;
        entry(i, &function);
        Function fn = (Function)(void *)(region + function);
        uint64_t plain_ns, patched_ns, plain_cyc, patched_cyc;
        measure((AnyFn)(void *)fn, (uint64_t)(uintptr_t)(region + DATA + 0x40), 3, calls, &plain_ns, &plain_cyc);
        IsaRedZoneReport z = isa_red_zone_apply(table, region, region + table->rz_code_offset);
        assert(z.groups > 0);
        measure((AnyFn)(void *)fn, (uint64_t)(uintptr_t)(region + DATA + 0x40), 3, calls, &patched_ns, &patched_cyc);
        plain_total += plain_ns;
        patched_total += patched_ns;
        plain_cycles += plain_cyc;
        patched_cycles += patched_cyc;
        printf("  %-16s %12.2f %12.2f %10.2f %10.2f\n", i < 10 ? FIXTURE_NAMES[i] : "fixture",
               (double)plain_ns / calls, (double)patched_ns / calls,
               (double)((int64_t)patched_ns - (int64_t)plain_ns) / calls,
               (double)((int64_t)patched_cyc - (int64_t)plain_cyc) / calls);
        memcpy(region, pristine, IMAGE);   /* the next function starts from the same image */
    }
    printf("Red zone: rewrite cost %.2f ns per call (%.2f cycles), plain %.2f ns\n",
           (double)((int64_t)patched_total - (int64_t)plain_total) / (double)(calls * count),
           (double)((int64_t)patched_cycles - (int64_t)plain_cycles) / (double)(calls * count),
           (double)plain_total / (double)(calls * count));
}

/* The counters of TLG_REDZONE_STATS: what they cost per protected instruction. */
static uint64_t counter_total(const IsaTable *t) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < t->rz_hits_count; i++) total += t->rz_hits[i];
    return total;
}

static void bench_red_zone_counters(uint64_t calls) {
    memcpy(region, pristine, IMAGE);
    IsaRedZoneReport z = isa_red_zone_apply(table, region, region + table->rz_code_offset);
    assert(z.groups > 0);
    /* Every protected instruction of the fixture counts itself: the direct stubs and the ones
     * reached through a short-jump relay. */
    if (table->rz_hits && table->rz_hits_count) {
        for (uint32_t i = 0; i < index_count(); i++) {
            uint32_t function;
            entry(i, &function);
            uint64_t before = counter_total(table);
            ((Function)(void *)(region + function))((uint32_t *)(void *)(region + DATA + 0x40), 3);
            uint64_t after = counter_total(table);
            if (i != 7 && after != before + 1) {
                printf("  function %u (%s): %llu -> %llu\n", i, FIXTURE_NAMES[i],
                       (unsigned long long)before, (unsigned long long)after);
            }
            assert(i == 7 || after == before + 1);   /* "dead" has nothing protected */
        }
        printf("Red zone: every protected instruction of the fixture counts itself\n");
    }
    uint32_t function;
    entry(0, &function);
    Function fn = (Function)(void *)(region + function);
    uint64_t ns, cycles;
    measure((AnyFn)(void *)fn, (uint64_t)(uintptr_t)(region + DATA + 0x40), 3, calls, &ns, &cycles);
    printf("Red zone: with counters (TLG_REDZONE_STATS): %.2f ns per call\n", (double)ns / calls);
    if (table->rz_hits && table->rz_hits_count) {
        printf("Red zone: %llu executions counted over %u protected instructions\n",
               (unsigned long long)counter_total(table), table->rz_hits_count);
        assert(counter_total(table) == (uint64_t)REPS * calls + 9);   /* the fixture plus the check above */
    }
}

/* ---- the ISA stubs ---- */

enum { S_BLSR = 0x000, S_EXTRQ = 0x080 };
/* mov ecx, edi ; blsr ecx, ecx ; mov eax, ecx ; ret (the site at +2: 5 bytes, JMP). */
static const uint8_t BLSR_CODE[] = {0x89, 0xf9, 0xc4, 0xe2, 0x70, 0xf3, 0xc9, 0x89, 0xc8, 0xc3};
/* movdqu xmm0, [rdi] ; extrq xmm0, 8, 4 ; movdqu [rsi], xmm0 ; ret (the site at +4: 6 bytes). */
static const uint8_t EXTRQ_CODE[] = {0xf3, 0x0f, 0x6f, 0x07, 0x66, 0x0f, 0x78, 0xc0, 0x08, 0x04,
                                     0xf3, 0x0f, 0x7f, 0x06, 0xc3};
/* vcvtph2ps xmm0, [rdi] ; movdqu [rsi], xmm0 ; ret (the site at +0: 5 bytes, the F16C stub). */
static const uint8_t VCVTPH2PS_CODE[] = {0xc5, 0xf9, 0x6d, 0x47, 0x00, 0xf3, 0x0f, 0x7f, 0x06, 0xc3};

static IsaSite site_at(uint64_t offset, uint8_t length, uint8_t kind, uint8_t mode, uint8_t dst, uint8_t src1) {
    IsaSite s;
    memset(&s, 0, sizeof s);
    s.offset = offset;
    s.length = length;
    s.kind = kind;
    s.width = 32;
    s.mode = mode;
    s.dst = dst;
    s.src1 = src1;
    s.src2 = ISA_NO_REG;
    s.mem_slot = ISA_MEM_NONE;
    s.base = ISA_NO_REG;
    s.index = ISA_NO_REG;
    return s;
}

static IsaTable *table_of(const IsaSite *sites, uint32_t count) {
    IsaTable *t = calloc(1, sizeof *t);
    assert(t != NULL);
    t->sites = calloc(count, sizeof *sites);
    t->hits = calloc(count, sizeof *t->hits);
    assert(t->sites && t->hits);
    memcpy(t->sites, sites, count * sizeof *sites);
    t->count = count;
    t->image_size = PAGE;
    return t;
}

/* The same snippet with the emulation stub and without it (native: only when the host has the
 * instruction; SSE4A has no Intel CPU, so its snippet is only measured with the stub). */
static void bench_isa(const char *name, const uint8_t *code, size_t length, uint64_t at, const IsaSite *site,
                      int native, uint64_t calls) {
    unsigned char *image = region + IMAGE + AREA;
    unsigned char *stubs = image + PAGE;
    memcpy(image + at, code, length);
    memset(stubs, 0, PAGE);
    IsaSite sites[1];
    sites[0] = *site;
    sites[0].offset = at + site->offset;
    memcpy(sites[0].original, image + sites[0].offset, sites[0].length);
    IsaTable *t = table_of(sites, 1);
    AnyFn fn = (AnyFn)(void *)(image + at);
    uint8_t input[64], output[64];
    memset(input, 0x5a, sizeof input);
    memset(output, 0, sizeof output);
    uint64_t plain_ns = 0, patched_ns, plain_cyc = 0, patched_cyc;
    if (native) measure(fn, (uint64_t)(uintptr_t)input, (uint64_t)(uintptr_t)output, calls, &plain_ns, &plain_cyc);
    IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_BMI1 | CPU_SET_SSE4A | CPU_SET_F16C);
    assert(r.rewritten == 1);
    measure(fn, (uint64_t)(uintptr_t)input, (uint64_t)(uintptr_t)output, calls, &patched_ns, &patched_cyc);
    /* The stub computes the same thing as the instruction it replaces. */
    if (strcmp(name, "blsr") == 0) {
        assert(((AnyFn)(void *)(image + at))(0xf0, 0) == 0xe0);
    } else if (strcmp(name, "extrq") == 0) {
        uint64_t qword = 0;
        for (int i = 0; i < 8; i++) qword |= (uint64_t)input[i] << 8 * i;
        uint64_t want = (qword >> 4) & 0xff;   /* extrq xmm0, 8, 4 */
        memset(output, 0, sizeof output);
        ((AnyFn)(void *)(image + at))((uint64_t)(uintptr_t)input, (uint64_t)(uintptr_t)output);
        uint64_t got = 0;
        for (int i = 0; i < 8; i++) got |= (uint64_t)output[i] << 8 * i;
        assert(got == want);
    } else {
        float single = 0;
        memcpy(&single, output, 4);   /* vcvtph2ps xmm0, [rdi]: the first half of the input */
        assert(*(unsigned *)&single == isa_half_to_float(0x5a5a));
    }
    printf("ISA: %-6s native %6.2f ns, stub %6.2f ns, stub cost %6.2f ns (%5.2f cycles), %llu hits\n", name,
           native ? (double)plain_ns / calls : 0.0, (double)patched_ns / calls,
           (double)((int64_t)patched_ns - (int64_t)plain_ns) / calls,
           (double)((int64_t)patched_cyc - (int64_t)plain_cyc) / calls, (unsigned long long)t->hits[0]);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: rewrite-cost-test red_zone_image.bin red_zone_table.bin [calls]\n");
        return 2;
    }
    uint64_t calls = argc > 3 ? strtoull(argv[3], NULL, 10) : 2000000;
    setvbuf(stdout, NULL, _IONBF, 0);   /* the numbers are the point: do not lose them to a crash */
#ifdef _WIN32
    region = VirtualAlloc(NULL, IMAGE + AREA + 3 * PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(region != NULL);
#else
    region = mmap(NULL, IMAGE + AREA + 3 * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region != MAP_FAILED);
#endif
    pristine = malloc(IMAGE);
    assert(pristine != NULL);
    load_image(argv[1]);
    table = isa_table_load(argv[2]);
    assert(table && table->rz && table->rz_code_offset == IMAGE);
    CpuFeatureWords words = cpu_feature_words();
    char missing[128];
    unsigned missing_sets = cpu_missing_rewritable(words, missing, sizeof missing);
    printf("CPU: missing %s\n", missing_sets ? missing : "nothing");
    printf("ISA counters: %s; red zone counters: %s\n", getenv("TLG_ISA_STATS") ? getenv("TLG_ISA_STATS") : "on",
           getenv("TLG_REDZONE_STATS") ? getenv("TLG_REDZONE_STATS") : "off");
    bench_red_zone(calls);
    bench_red_zone_counters(calls);
    IsaSite blsr = site_at(2, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    bench_isa("blsr", BLSR_CODE, sizeof BLSR_CODE, S_BLSR, &blsr, (missing_sets & CPU_SET_BMI1) == 0, calls);
    IsaSite extrq = site_at(4, 6, ISA_EXTRQ_IMM, ISA_MODE_JMP, 0, ISA_NO_REG);
    extrq.disp = 8 | 4 << 8;
    bench_isa("extrq", EXTRQ_CODE, sizeof EXTRQ_CODE, S_EXTRQ, &extrq, 0, calls);
    /* The F16C stub copies a whole YMM register: the most expensive one (this CPU has F16C, so
     * it is not rewritten here, but the code path exists for the hosts that lack it). */
    IsaSite f16c = site_at(0, 5, ISA_VCVTPH2PS, ISA_MODE_JMP, 0, ISA_NO_REG);
    f16c.width = 16;
    f16c.mem_slot = ISA_MEM_SRC;
    f16c.base = 7;
    f16c.scale = 1;
    bench_isa("f16c", VCVTPH2PS_CODE, sizeof VCVTPH2PS_CODE, S_EXTRQ + 0x40, &f16c, 0, calls);
    printf("rewrite-cost-test ok\n");
    return 0;
}
