#include "isa_rewrite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEADER_BYTES = 64, RECORD_BYTES = 64, MAX_SITES = 1u << 20, JMP_BYTES = 5 };
/* Red-zone section (scripts/red_zone.py): SECTION (the last u32, a source CRC, is the script's
 * cache key), STATS, code, then the groups. */
enum { RZ_SECTION_BYTES = 36, RZ_STATS_BYTES = 4 * ISA_RZ_STATS, RZ_GROUP_BYTES = 20, RZ_PATCH_BYTES = 10,
       RZ_VERSION = 1, RZ_MAX_PATCH = 64 };
#define RZ_MAX_SECTION (256ull << 20)

/* ---- SHA-256 (FIPS 180-4), for isa_table_matches ---- */

static uint32_t ror32(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void sha256_block(uint32_t h[8], const unsigned char *block) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 |
               block[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + (ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) +
                      k[i] + w[i];
        uint32_t t2 = (ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof *v);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += v[i];
}

static void sha256(const unsigned char *data, uint64_t size, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t done = 0;
    for (; size - done >= 64; done += 64) sha256_block(h, data + done);
    unsigned char tail[128] = {0};
    size_t rest = (size_t)(size - done), tail_size = rest < 56 ? 64 : 128;
    memcpy(tail, data + done, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++) tail[tail_size - 1 - i] = (unsigned char)(size * 8 >> 8 * i);
    for (size_t at = 0; at < tail_size; at += 64) sha256_block(h, tail + at);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- table ---- */

static uint64_t read_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int register_valid(uint8_t reg) { return reg <= 15 || reg == ISA_NO_REG; }

/* Field ranges of spec 4.4, plus what the patch writes: a 5-byte jmp needs a 5-byte site (mode 0)
 * or a relocated instruction (mode 1), an int3 site is 4 bytes, and every write stays inside the image.
 * The writer stores scale 0 without a memory operand, else iced's scale (1 when there is no index). */
static int record_valid(const IsaSite *s, uint64_t image_size) {
    if (s->length < 4 || s->length > 15 || s->relocated_length > 15) return 0;
    if (s->kind < ISA_MOVBE_LOAD || s->kind > ISA_MOVNTSD || s->mode > ISA_MODE_UNSUPPORTED) return 0;
    if (!register_valid(s->dst) || !register_valid(s->src1) || !register_valid(s->src2) || !register_valid(s->base) ||
        !register_valid(s->index) || s->mem_slot > ISA_MEM_SRC)
        return 0;
    if (s->kind <= ISA_BLSR && s->width != 16 && s->width != 32 && s->width != 64) return 0;
    /* EXTRQ/INSERTQ: register operands only; MOVNTSS/MOVNTSD are never rewritten. */
    if ((s->kind == ISA_MOVNTSS || s->kind == ISA_MOVNTSD) && s->mode != ISA_MODE_UNSUPPORTED) return 0;
    if (s->kind >= ISA_EXTRQ && s->kind <= ISA_INSERTQ_IMM &&
        (s->dst > 15 || s->mem_slot != ISA_MEM_NONE || ((s->kind != ISA_EXTRQ_IMM) && s->src1 > 15)))
        return 0;
    /* F16C: VEX.128 or VEX.256 (width 16 or 32 bytes), memory only as VCVTPH2PS source or VCVTPS2PH
     * destination; the sites are 5+ bytes, so never int3 or relocated. */
    if ((s->kind == ISA_VCVTPH2PS || s->kind == ISA_VCVTPS2PH) && s->mode != ISA_MODE_UNSUPPORTED) {
        const int mem_ok = s->kind == ISA_VCVTPH2PS ? s->mem_slot != ISA_MEM_DST : s->mem_slot != ISA_MEM_SRC;
        if ((s->width != 16 && s->width != 32) || !mem_ok || s->mode != ISA_MODE_JMP) return 0;
        if ((s->mem_slot == ISA_MEM_DST) != (s->dst == ISA_NO_REG) || (s->mem_slot == ISA_MEM_SRC) != (s->src1 == ISA_NO_REG))
            return 0;
    }
    if (s->scale != 1 && s->scale != 2 && s->scale != 4 && s->scale != 8 && !(s->scale == 0 && s->index == ISA_NO_REG))
        return 0;
    if (s->mode == ISA_MODE_JMP && s->length < JMP_BYTES) return 0;
    if (s->mode == ISA_MODE_INT3 && s->length != 4) return 0;
    if (s->mode == ISA_MODE_JMP_RELOCATE && (s->length != 4 || s->relocated_length == 0)) return 0;
    uint64_t end = s->mode == ISA_MODE_JMP_RELOCATE ? 4u + s->relocated_length : s->length;
    return s->offset <= image_size && end <= image_size - s->offset;
}

static IsaTable *load_fail(FILE *f, IsaTable *t, const char *reason, const char *path) {
    fprintf(stderr, "ISA: %s%s\n", reason, path ? path : "");
    if (f) fclose(f);
    if (t) {
        free(t->sites);
        free(t->hits);
        free(t->rz);
        free(t->rz_applied);
        free(t);
    }
    return NULL;
}

static uint16_t read_u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Checks the red-zone section's layout: every group, patch and stub inside its bounds. */
static int red_zone_valid(IsaTable *t) {
    const unsigned char *p = t->rz;
    if (t->rz_size < RZ_SECTION_BYTES + RZ_STATS_BYTES || memcmp(p, "G3RZONE1", 8) != 0) return 0;
    t->rz_code_offset = read_u64(p + 8);
    t->rz_code_bytes = read_u32(p + 16);
    t->rz_groups = read_u32(p + 20);
    t->rz_group_bytes = read_u32(p + 24);
    if (read_u32(p + 28) != RZ_VERSION) return 0;
    for (int i = 0; i < ISA_RZ_STATS; i++) t->rz_stats[i] = read_u32(p + RZ_SECTION_BYTES + 4 * i);
    if ((uint64_t)RZ_SECTION_BYTES + RZ_STATS_BYTES + t->rz_code_bytes + t->rz_group_bytes != t->rz_size) return 0;
    if (t->rz_code_offset < t->image_size || t->rz_code_offset > (1ull << 31)) return 0;
    const unsigned char *g = p + RZ_SECTION_BYTES + RZ_STATS_BYTES + t->rz_code_bytes, *end = g + t->rz_group_bytes;
    for (uint32_t i = 0; i < t->rz_groups; i++) {
        if (end - g < RZ_GROUP_BYTES) return 0;
        uint32_t stub = read_u32(g + 8), bytes = read_u32(g + 12), patches = read_u16(g + 18);
        if (!patches || stub > t->rz_code_bytes || bytes > t->rz_code_bytes - stub) return 0;
        g += RZ_GROUP_BYTES;
        for (uint32_t k = 0; k < patches; k++) {
            if (end - g < RZ_PATCH_BYTES) return 0;
            uint64_t offset = read_u64(g);
            uint16_t length = read_u16(g + 8);
            if (!length || length > RZ_MAX_PATCH || offset > t->image_size || length > t->image_size - offset ||
                end - g < RZ_PATCH_BYTES + 2 * length)
                return 0;
            g += RZ_PATCH_BYTES + 2 * length;
        }
    }
    return g == end;
}

IsaTable *isa_table_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return load_fail(NULL, NULL, "cannot open rewrite table ", path);
    unsigned char header[HEADER_BYTES];
    if (fread(header, 1, sizeof header, f) != sizeof header || memcmp(header, "G3ISA1\0\0", 8) != 0)
        return load_fail(f, NULL, "not a G3ISA1 table: ", path);
    uint32_t count = read_u32(header + 48);
    uint64_t rz_size = read_u64(header + 56);
    if (count > MAX_SITES) return load_fail(f, NULL, "table has too many sites", NULL);
    if (rz_size > RZ_MAX_SECTION) return load_fail(f, NULL, "red-zone section too large", NULL);
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) != (long)(HEADER_BYTES + (uint64_t)RECORD_BYTES * count + rz_size) ||
        fseek(f, HEADER_BYTES, SEEK_SET) != 0)
        return load_fail(f, NULL, "table size does not match its site count", NULL);
    IsaTable *t = calloc(1, sizeof *t);
    if (!t) return load_fail(f, NULL, "out of memory", NULL);
    t->sites = calloc(count ? count : 1, sizeof *t->sites);
    t->hits = calloc(count ? count : 1, sizeof *t->hits);
    if (!t->sites || !t->hits) return load_fail(f, t, "out of memory", NULL);
    if (fread(t->sites, sizeof *t->sites, count, f) != count) return load_fail(f, t, "cannot read table ", path);
    if (rz_size) {
        t->rz = malloc((size_t)rz_size);
        t->rz_size = rz_size;
        if (!t->rz) return load_fail(f, t, "out of memory", NULL);
        if (fread(t->rz, 1, (size_t)rz_size, f) != rz_size) return load_fail(f, t, "cannot read table ", path);
    }
    fclose(f);
    t->count = count;
    t->unsupported = read_u32(header + 52);
    t->image_size = read_u64(header + 8);
    if (t->rz && !red_zone_valid(t)) return load_fail(NULL, t, "invalid red-zone section", NULL);
    memcpy(t->sha256, header + 16, sizeof t->sha256);
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0 && t->sites[i].offset <= t->sites[i - 1].offset)
            return load_fail(NULL, t, "table records are not sorted", NULL);
        if (!record_valid(&t->sites[i], t->image_size)) {
            char reason[48];
            snprintf(reason, sizeof reason, "invalid table record %u", i);
            return load_fail(NULL, t, reason, NULL);
        }
    }
    return t;
}

int isa_table_matches(const IsaTable *t, const unsigned char *image, uint64_t size) {
    if (size != t->image_size) return 1;
    uint8_t digest[32];
    sha256(image, size, digest);
    return memcmp(digest, t->sha256, sizeof digest) != 0;
}

size_t isa_stub_area_size(const IsaTable *t) { return (size_t)t->count * ISA_STUB_BYTES; }

unsigned isa_site_set(const IsaSite *s) {
    switch (s->kind) {
    case ISA_MOVBE_LOAD: case ISA_MOVBE_STORE: return CPU_SET_MOVBE;
    case ISA_LZCNT: return CPU_SET_LZCNT;
    case ISA_TZCNT: case ISA_ANDN: case ISA_BEXTR: case ISA_BLSI: case ISA_BLSMSK: case ISA_BLSR: return CPU_SET_BMI1;
    case ISA_VCVTPH2PS: case ISA_VCVTPS2PH: return CPU_SET_F16C;
    case ISA_EXTRQ: case ISA_EXTRQ_IMM: case ISA_INSERTQ: case ISA_INSERTQ_IMM: case ISA_MOVNTSS: case ISA_MOVNTSD:
        return CPU_SET_SSE4A;
    default: return 0;
    }
}

/* ---- stub emission (spec 5.5) ---- */

static void emit_u8(unsigned char **p, uint8_t v) { *(*p)++ = v; }
static void emit_u32(unsigned char **p, uint32_t v) { for (int i = 0; i < 4; i++) emit_u8(p, (uint8_t)(v >> 8 * i)); }
static void emit_u64(unsigned char **p, uint64_t v) { for (int i = 0; i < 8; i++) emit_u8(p, (uint8_t)(v >> 8 * i)); }
static void emit_bytes(unsigned char **p, const uint8_t *bytes, size_t n) {
    memcpy(*p, bytes, n);
    *p += n;
}

/* TLG_ISA_STATS=0 leaves the hit counters out of the stubs (the report is then empty): the
 * increment is a memory read-modify-write on every emulation, on a line the neighbouring sites
 * share, and the sites that matter are hot. A value that is not 0 or 1 names the file where the
 * counters are dumped with the report. */
static int isa_hits_counted(void) {
    static int on = -1;
    if (on < 0) {
        const char *value = getenv("TLG_ISA_STATS");
        on = !(value && !strcmp(value, "0"));
    }
    return on;
}

static const char *isa_stats_path(void) {
    const char *value = getenv("TLG_ISA_STATS");
    if (!value || !*value || !strcmp(value, "0") || !strcmp(value, "1")) return NULL;
    return value;
}

/* mov rax, &hits[i] ; inc qword [rax] */
static void emit_hit_counter(unsigned char **p, uint64_t *hit) {
    if (!hit || !isa_hits_counted()) return;
    emit_bytes(p, (const uint8_t[]){0x48, 0xb8}, 2);
    emit_u64(p, (uint64_t)(uintptr_t)hit);
    emit_bytes(p, (const uint8_t[]){0x48, 0xff, 0x00}, 3);
}

/* mov [rsp+disp8], reg (opcode 89) or mov reg, [rsp+disp8] (opcode 8B): REX.W(.R) modrm(01 reg 100) SIB 24. */
static void emit_rsp_mov(unsigned char **p, uint8_t opcode, int reg, uint8_t disp) {
    emit_u8(p, reg >= 8 ? 0x4c : 0x48);
    emit_u8(p, opcode);
    emit_u8(p, (uint8_t)(0x44 | (reg & 7) << 3));
    emit_u8(p, 0x24);
    emit_u8(p, disp);
}
static void emit_mov_store(unsigned char **p, int reg, uint8_t disp) { emit_rsp_mov(p, 0x89, reg, disp); }
static void emit_mov_load(unsigned char **p, int reg, uint8_t disp) { emit_rsp_mov(p, 0x8b, reg, disp); }

/* rel32 from the byte after a jmp ending at `next` to `target`; 0 when it does not fit. */
static int rel32_to(const unsigned char *next, const unsigned char *target, int32_t *rel) {
    int64_t delta = (int64_t)((intptr_t)target - (intptr_t)next);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    *rel = (int32_t)delta;
    return 1;
}

/* movdqu [rsp+disp8], xmm (store) or movdqu xmm, [rsp+disp8] (load): F3 [REX.R] 0F 7F/6F modrm(01 xmm 100) SIB 24. */
static void emit_movdqu_rsp(unsigned char **p, int store, int xmm, uint8_t disp) {
    emit_u8(p, 0xf3);
    if (xmm >= 8) emit_u8(p, 0x44);
    emit_bytes(p, (const uint8_t[]){0x0f, store ? 0x7f : 0x6f, (uint8_t)(0x44 | (xmm & 7) << 3), 0x24, disp}, 5);
}

static const uint8_t lea_rsp_below_red_zone[] = {0x48, 0x8d, 0x64, 0x24, 0x80};                   /* lea rsp,[rsp-0x80] */
static const uint8_t lea_rsp_above_red_zone[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};   /* lea rsp,[rsp+0x80] */

/* Shared tail: the relocated instruction (JMP_RELOCATE) and the jmp back; 0 when out of rel32 range.
 * With red-zone protection a relocated instruction that can fault runs with rsp 128 bytes lower (a
 * Windows exception frame then lands below the guest's red zone), when the stub has room. */
static size_t emit_return(unsigned char *code, unsigned char *p, const unsigned char *at, const IsaSite *s,
                          unsigned char *image, int protect) {
    const unsigned char *back = image + s->offset + s->length;
    if (s->mode == ISA_MODE_JMP_RELOCATE) {
        size_t wrapped = (size_t)(p - code) + sizeof lea_rsp_below_red_zone + s->relocated_length +
                         sizeof lea_rsp_above_red_zone + JMP_BYTES;
        int wrap = protect && (s->mem_flags & ISA_MEM_RELOCATED_FAULTABLE) && wrapped <= ISA_STUB_BYTES;
        if (wrap) emit_bytes(&p, lea_rsp_below_red_zone, sizeof lea_rsp_below_red_zone);
        emit_bytes(&p, s->relocated, s->relocated_length);
        if (wrap) emit_bytes(&p, lea_rsp_above_red_zone, sizeof lea_rsp_above_red_zone);
        back = image + s->offset + 4 + s->relocated_length;
    }
    int32_t rel;
    if (!rel32_to(at + (p - code) + JMP_BYTES, back, &rel)) return 0;
    emit_u8(&p, 0xe9);
    emit_u32(&p, (uint32_t)rel);
    return (size_t)(p - code);
}

/* SSE4A stub: the operands are XMM registers, which isa_emulate's IsaRegs does not carry. It steps
 * over the red zone, saves rflags (then clears DF for the C call) and the registers the SysV ABI
 * lets isa_emulate_sse4a clobber, copies the destination and source XMM to a 32-byte buffer, calls
 * it and loads the destination back. The movdqu are legacy SSE like EXTRQ itself: bits 255:128 of
 * the YMM register stay as they were, and no other XMM register is touched. */
static size_t emit_sse4a_stub(unsigned char *code, const unsigned char *at, const IsaSite *s, uint64_t *hit,
                              unsigned char *image, int protect) {
    static const uint8_t pushes[] = {0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x55};
    static const uint8_t pops[] = {0x5d, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58};
    unsigned char *p = code;
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x64, 0x24, 0x80}, 5);   /* lea rsp,[rsp-0x80] */
    emit_u8(&p, 0x9c);                                                     /* pushfq */
    emit_u8(&p, 0xfc);                                                     /* cld */
    emit_bytes(&p, pushes, sizeof pushes);                                 /* rax rcx rdx rsi rdi r8-r11 rbp */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x64, 0x24, 0xe0}, 5);   /* lea rsp,[rsp-0x20] */
    emit_movdqu_rsp(&p, 1, s->dst, 0);
    if (s->src1 != ISA_NO_REG) emit_movdqu_rsp(&p, 1, s->src1, 16);
    emit_hit_counter(&p, hit);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xe5}, 3);                /* mov rbp, rsp */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x83, 0xe4, 0xf0}, 4);          /* and rsp, -16 */
    emit_bytes(&p, (const uint8_t[]){0x48, 0xbf}, 2);                      /* mov rdi, site */
    emit_u64(&p, (uint64_t)(uintptr_t)s);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xee}, 3);                /* mov rsi, rbp (the buffer) */
    emit_bytes(&p, (const uint8_t[]){0x48, 0xb8}, 2);                      /* mov rax, isa_emulate_sse4a */
    emit_u64(&p, (uint64_t)(uintptr_t)isa_emulate_sse4a);
    emit_bytes(&p, (const uint8_t[]){0xff, 0xd0}, 2);                      /* call rax */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xec}, 3);                /* mov rsp, rbp */
    emit_movdqu_rsp(&p, 0, s->dst, 0);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x64, 0x24, 0x20}, 5);   /* lea rsp,[rsp+0x20] */
    emit_bytes(&p, pops, sizeof pops);
    emit_u8(&p, 0x9d);                                                     /* popfq */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}, 8); /* lea rsp,[rsp+0x80] */
    return emit_return(code, p, at, s, image, protect);
}

/* vmovdqu [rsp+disp8], ymm (store, VEX.256.F3.0F 7F) or vmovdqu ymm, [rsp+disp8] (load, 6F): C5 {R̄ 1111 1 10}
 * modrm SIB 24 [disp8]. */
static void emit_vmovdqu_rsp(unsigned char **p, int store, int ymm, uint8_t disp) {
    emit_bytes(p, (const uint8_t[]){0xc5, ymm >= 8 ? 0x7e : 0xfe, store ? 0x7f : 0x6f}, 3);
    emit_u8(p, (uint8_t)((disp ? 0x44 : 0x04) | (ymm & 7) << 3));
    emit_u8(p, 0x24);
    if (disp) emit_u8(p, disp);
}

/* F16C stub (VCVTPH2PS, VCVTPS2PH; the host has AVX but not F16C). It steps over the red zone and
 * pushes an IsaRegs (gpr[4] then fixed to the guest rsp: memory operands may be rsp-based), saves
 * rflags and clears DF, copies the source YMM register to a 64-byte buffer and calls
 * isa_emulate_f16c (software conversion, no XMM) with the site, the registers and the buffer. The
 * destination register, when there is one, is loaded back whole: the VEX forms zero its bits above
 * the result. No other XMM/YMM register is touched, every GPR is popped back as it was. */
static size_t emit_f16c_stub(unsigned char *code, const unsigned char *at, const IsaSite *s, uint64_t *hit,
                             unsigned char *image, int protect) {
    static const uint8_t pushes[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x41, 0x53, 0x41, 0x52, 0x41, 0x51,
                                     0x41, 0x50, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51, 0x50};   /* r15..r8, rdi..rax */
    static const uint8_t pops_low[] = {0x58, 0x59, 0x5a, 0x5b};                                    /* rax rcx rdx rbx */
    static const uint8_t pops_high[] = {0x5d, 0x5e, 0x5f, 0x41, 0x58, 0x41, 0x59, 0x41, 0x5a, 0x41, 0x5b,
                                        0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f};             /* rbp..r15 */
    unsigned char *p = code;
    emit_bytes(&p, lea_rsp_below_red_zone, sizeof lea_rsp_below_red_zone);
    emit_bytes(&p, pushes, sizeof pushes);
    emit_u8(&p, 0x9c);                                                     /* pushfq: rsp = IsaRegs* */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x84, 0x24, 0x08, 0x01, 0x00, 0x00}, 8);   /* lea rax,[rsp+0x108] */
    emit_mov_store(&p, 0, 8 + 8 * 4);                                      /* gpr[4] = guest rsp */
    emit_u8(&p, 0xfc);                                                     /* cld */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x64, 0x24, 0xc0}, 5);   /* lea rsp,[rsp-0x40]: the buffer */
    if (s->src1 != ISA_NO_REG) emit_vmovdqu_rsp(&p, 1, s->src1, 0x20);
    emit_hit_counter(&p, hit);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xe5}, 3);                /* mov rbp, rsp */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x83, 0xe4, 0xf0}, 4);          /* and rsp, -16 */
    emit_bytes(&p, (const uint8_t[]){0x48, 0xbf}, 2);                      /* mov rdi, site */
    emit_u64(&p, (uint64_t)(uintptr_t)s);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x75, 0x40}, 4);          /* lea rsi, [rbp+0x40] (IsaRegs) */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xea}, 3);                /* mov rdx, rbp (the buffer) */
    emit_bytes(&p, (const uint8_t[]){0x48, 0xb8}, 2);                      /* mov rax, isa_emulate_f16c */
    emit_u64(&p, (uint64_t)(uintptr_t)isa_emulate_f16c);
    emit_bytes(&p, (const uint8_t[]){0xff, 0xd0}, 2);                      /* call rax */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xec}, 3);                /* mov rsp, rbp */
    if (s->dst != ISA_NO_REG) emit_vmovdqu_rsp(&p, 0, s->dst, 0);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x64, 0x24, 0x40}, 5);   /* lea rsp,[rsp+0x40] */
    emit_u8(&p, 0x9d);                                                     /* popfq */
    emit_bytes(&p, pops_low, sizeof pops_low);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x8d, 0x64, 0x24, 0x08}, 5);   /* lea rsp,[rsp+8]: gpr[4] */
    emit_bytes(&p, pops_high, sizeof pops_high);
    emit_bytes(&p, lea_rsp_above_red_zone, sizeof lea_rsp_above_red_zone);
    return emit_return(code, p, at, s, image, protect);
}

/* Writes the stub for `s` into `code` (laid out to run at `at`). Returns its size, or 0 when the
 * jmp back is out of rel32 range. */
static size_t emit_stub(unsigned char *code, const unsigned char *at, const IsaSite *s, uint64_t *hit,
                        unsigned char *image, int protect) {
    static const uint8_t lea_rsp_down[] = {0x48, 0x8d, 0xa4, 0x24, 0x00, 0xff, 0xff, 0xff};   /* lea rsp,[rsp-0x100] */
    static const uint8_t lea_rax_rsp_up[] = {0x48, 0x8d, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00}; /* lea rax,[rsp+0x100] */
    static const uint8_t lea_rsp_up[] = {0x48, 0x8d, 0xa4, 0x24, 0x00, 0x01, 0x00, 0x00};     /* lea rsp,[rsp+0x100] */
    if (isa_kind_is_sse4a(s->kind)) return emit_sse4a_stub(code, at, s, hit, image, protect);
    if (isa_kind_is_f16c(s->kind)) return emit_f16c_stub(code, at, s, hit, image, protect);
    unsigned char *p = code;
    emit_bytes(&p, lea_rsp_down, sizeof lea_rsp_down);
    for (int reg = 0; reg < 16; reg++)
        if (reg != 4) emit_mov_store(&p, reg, (uint8_t)(8 * reg));
    emit_bytes(&p, lea_rax_rsp_up, sizeof lea_rax_rsp_up);
    emit_mov_store(&p, 0, 8 * 4);                                   /* gpr[4] = guest rsp */
    emit_u8(&p, 0x9c);                                              /* pushfq: rsp = IsaRegs* */
    emit_hit_counter(&p, hit);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xe5}, 3);         /* mov rbp, rsp */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x83, 0xe4, 0xf0}, 4);   /* and rsp, -16 */
    emit_bytes(&p, (const uint8_t[]){0x48, 0xbf}, 2);               /* mov rdi, site */
    emit_u64(&p, (uint64_t)(uintptr_t)s);
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xee}, 3);         /* mov rsi, rbp */
    emit_bytes(&p, (const uint8_t[]){0x48, 0xb8}, 2);               /* mov rax, isa_emulate */
    emit_u64(&p, (uint64_t)(uintptr_t)isa_emulate);
    emit_bytes(&p, (const uint8_t[]){0xff, 0xd0}, 2);               /* call rax */
    emit_bytes(&p, (const uint8_t[]){0x48, 0x89, 0xec}, 3);         /* mov rsp, rbp */
    emit_u8(&p, 0x9d);                                              /* popfq: rsp = &gpr[0] */
    for (int reg = 0; reg < 16; reg++)
        if (reg != 4) emit_mov_load(&p, reg, (uint8_t)(8 * reg));
    emit_bytes(&p, lea_rsp_up, sizeof lea_rsp_up);
    return emit_return(code, p, at, s, image, protect);
}

/* Site bytes of spec 4.3. */
static void patch_site(unsigned char *site, const IsaSite *s, int32_t rel) {
    if (s->mode == ISA_MODE_INT3) {
        site[0] = 0xcc;
        return;
    }
    unsigned char *p = site;
    emit_u8(&p, 0xe9);
    emit_u32(&p, (uint32_t)rel);
    size_t covered = s->mode == ISA_MODE_JMP_RELOCATE ? 4u + s->relocated_length : s->length;
    memset(p, 0x90, covered - JMP_BYTES);
}

IsaReport isa_rewrite_apply(IsaTable *t, unsigned char *image, unsigned char *stubs, unsigned sets) {
    IsaReport report = {0};
    t->image = image;
    t->stubs = stubs;
    isa_emulate_set_image(image);
    for (uint32_t i = 0; i < t->count; i++)
        if (t->sites[i].mode == ISA_MODE_UNSUPPORTED && (isa_site_set(&t->sites[i]) & sets)) report.unsupported_needed++;
    if (report.unsupported_needed) return report;
    for (uint32_t i = 0; i < t->count; i++) {
        const IsaSite *s = &t->sites[i];
        if (s->mode == ISA_MODE_UNSUPPORTED || !(isa_site_set(s) & sets)) continue;
        unsigned char *site = image + s->offset;
        if (memcmp(site, s->original, s->length) != 0) {
            fprintf(stderr, "ISA: site 0x%llx %s skipped: bytes changed\n", (unsigned long long)s->offset,
                    isa_kind_name(s->kind));
            report.skipped++;
            continue;
        }
        if (s->mode == ISA_MODE_JMP_RELOCATE && memcmp(site + 4, s->relocated, s->relocated_length) != 0) {
            /* A patch or hook changed the instruction the jmp would cover: the stub would run the old copy. */
            fprintf(stderr, "ISA: site 0x%llx %s: next instruction changed, int3 fallback\n",
                    (unsigned long long)s->offset, isa_kind_name(s->kind));
            t->sites[i].mode = ISA_MODE_INT3;
        }
        if (s->mode == ISA_MODE_INT3) {
            patch_site(site, s, 0);
            report.int3++;
            report.rewritten++;
            continue;
        }
        unsigned char *stub = stubs + (size_t)i * ISA_STUB_BYTES;
        unsigned char code[2 * ISA_STUB_BYTES];
        int32_t rel;
        size_t size;
        if (!rel32_to(site + JMP_BYTES, stub, &rel) || (size = emit_stub(code, stub, s, &t->hits[i], image, t->protect_red_zone)) == 0) {
            fprintf(stderr, "ISA: site 0x%llx out of rel32 range\n", (unsigned long long)s->offset);
            report.skipped++;
            continue;
        }
        if (size > ISA_STUB_BYTES) {
            fprintf(stderr, "ISA: stub for site 0x%llx exceeds 256 bytes, site skipped\n",
                    (unsigned long long)s->offset);
            report.skipped++;
            continue;
        }
        memcpy(stub, code, size);
        patch_site(site, s, rel);
        if (s->mode == ISA_MODE_JMP) report.jmp++;
        else report.relocated++;
        report.rewritten++;
    }
    return report;
}

/* ---- red-zone protection ---- */

/* TLG_REDZONE_STATS counts how many times each protected instruction runs: 1 counts, a path counts
 * and dumps the counters to that file with the report, 0 (the default) leaves the stubs alone (no
 * trampoline, no counter, no cost). The counters live in the red-zone area, after the stub code,
 * and the trampoline that counts reaches them with a rip-relative increment: no register is
 * touched, the guest's registers must come out of the stub exactly as they went in. */
enum { RZ_TRAMP_BYTES = 12 };   /* inc qword [rip+disp32] ; jmp rel32 */

static int rz_stats_kind(void) {   /* 0 off, 1 count, 2 count and dump */
    static int kind = -1;
    if (kind < 0) {
        const char *value = getenv("TLG_REDZONE_STATS");
        kind = (!value || !*value || !strcmp(value, "0")) ? 0 : !strcmp(value, "1") ? 1 : 2;
    }
    return kind;
}

static const char *rz_stats_path(void) {
    const char *value = getenv("TLG_REDZONE_STATS");
    return (value && *value && strcmp(value, "0") && strcmp(value, "1")) ? value : NULL;
}

int isa_red_zone_counters(void) { return rz_stats_kind(); }

size_t isa_red_zone_code_size(const IsaTable *t) {
    if (!t || !t->rz) return 0;
    size_t size = t->rz_code_bytes;
    if (rz_stats_kind()) size += (size_t)t->rz_groups * (8 + RZ_TRAMP_BYTES);
    return size;
}

/* The jmp of a group's patches that reaches the stub running the protected instruction: its offset
 * in the image and its target (an image offset). The patched span starts with a jmp to the stub
 * (the span may begin before the site, when the site is shorter than five bytes and the previous
 * instructions go with it), or with a 2-byte jmp to a relay whose jmp near is inside another
 * patch of the same group (a "short relay"). */
static int rz_stub_jump(const unsigned char *first, const unsigned char *q, uint64_t site, uint64_t *at,
                        int64_t *target) {
    const unsigned char *patch = NULL;
    uint32_t length = 0;
    uint64_t offset = 0;
    for (const unsigned char *r = first; r < q;) {
        uint16_t len = read_u16(r + 8);
        uint64_t off = read_u64(r);
        if (site >= off && site < off + len) {
            patch = r;
            length = len;
            offset = off;
        }
        r += RZ_PATCH_BYTES + 2 * len;
    }
    if (!patch || length < 2) return 0;
    const unsigned char *new_bytes = patch + RZ_PATCH_BYTES + length;
    if (new_bytes[0] == 0xe9) {   /* jmp rel32 to the stub */
        *at = offset;
        *target = (int64_t)offset + 5 + (int32_t)read_u32(new_bytes + 1);
        return 1;
    }
    if (new_bytes[0] != 0xeb) return 0;   /* jmp short to a relay */
    int64_t relay = (int64_t)site + 2 + (int8_t)new_bytes[1];
    for (const unsigned char *r = first; r < q;) {
        uint16_t len = read_u16(r + 8);
        uint64_t off = read_u64(r);
        if (relay >= (int64_t)off && relay < (int64_t)off + len) {
            const unsigned char *other = r + RZ_PATCH_BYTES + len;
            size_t index = (size_t)(relay - (int64_t)off);
            if (index + 5 <= len && other[index] == 0xe9) {
                *at = (uint64_t)relay;
                *target = relay + 5 + (int32_t)read_u32(other + index + 1);
                return 1;
            }
        }
        r += RZ_PATCH_BYTES + 2 * len;
    }
    return 0;
}

IsaRedZoneReport isa_red_zone_apply(IsaTable *t, unsigned char *image, unsigned char *area) {
    IsaRedZoneReport report = {0};
    if (!t || !t->rz) return report;
    if (area != image + t->rz_code_offset) {
        fprintf(stderr, "Red zone: stub area not where the table expects it, protection off\n");
        report.skipped = t->rz_groups;
        return report;
    }
    free(t->rz_applied);
    t->rz_applied = calloc(t->rz_groups ? t->rz_groups : 1, sizeof *t->rz_applied);
    t->rz_applied_count = 0;
    t->rz_hits = NULL;
    t->rz_hits_count = 0;
    if (!t->rz_applied) {
        report.skipped = t->rz_groups;
        return report;
    }
    const unsigned char *code = t->rz + RZ_SECTION_BYTES + RZ_STATS_BYTES, *g = code + t->rz_code_bytes;
    memcpy(area, code, t->rz_code_bytes);
    t->rz_area = area;
    /* The counters and their trampolines follow the stub code in the same area. */
    const int stats = rz_stats_kind();
    unsigned char *counters = stats ? area + t->rz_code_bytes : NULL;
    unsigned char *tramps = stats ? counters + (size_t)t->rz_groups * 8 : NULL;
    if (stats) memset(counters, 0, (size_t)t->rz_groups * 8);
    for (uint32_t i = 0; i < t->rz_groups; i++) {
        uint64_t site = read_u64(g);
        uint32_t stub = read_u32(g + 8), bytes = read_u32(g + 12), protected_count = read_u16(g + 16),
                 patches = read_u16(g + 18);
        const unsigned char *first = g + RZ_GROUP_BYTES, *q = first;
        int matches = 1;
        for (uint32_t k = 0; k < patches; k++) {
            uint16_t length = read_u16(q + 8);
            if (memcmp(image + read_u64(q), q + RZ_PATCH_BYTES, length) != 0) matches = 0;
            q += RZ_PATCH_BYTES + 2 * length;
        }
        if (!matches) {
            fprintf(stderr, "Red zone: site 0x%llx skipped: bytes changed\n", (unsigned long long)site);
            report.skipped++;
        } else {
            const uint32_t index = t->rz_applied_count;
            uint64_t jump_at = 0;
            int64_t jump_target = 0;
            unsigned char *tramp = NULL;
            if (stats && rz_stub_jump(first, q, site, &jump_at, &jump_target)) {
                /* The trampoline counts and jumps to the stub: inc qword [rip+disp32] ; jmp rel32. */
                tramp = tramps + (size_t)index * RZ_TRAMP_BYTES;
                uint64_t counter = (uint64_t)(uintptr_t)(counters + (size_t)index * 8);
                uint64_t here = (uint64_t)(uintptr_t)tramp;
                int32_t disp = (int32_t)(int64_t)(counter - (here + 7));
                uint64_t target = (uint64_t)(uintptr_t)area + (uint64_t)(jump_target - (int64_t)t->rz_code_offset);
                int32_t rel = (int32_t)(int64_t)(target - (here + RZ_TRAMP_BYTES));
                tramp[0] = 0x48;
                tramp[1] = 0xff;
                tramp[2] = 0x05;
                memcpy(tramp + 3, &disp, 4);
                tramp[7] = 0xe9;
                memcpy(tramp + 8, &rel, 4);
            }
            for (const unsigned char *r = first; r < q;) {
                uint16_t length = read_u16(r + 8);
                unsigned char buffer[RZ_MAX_PATCH];
                const unsigned char *new_bytes = r + RZ_PATCH_BYTES + length;
                memcpy(buffer, new_bytes, length);
                if (tramp) {   /* the jmp to the stub goes through the trampoline */
                    uint64_t offset = read_u64(r);
                    size_t at = (size_t)(jump_at - offset);
                    if (jump_at >= offset && at + 5 <= length && buffer[at] == 0xe9) {
                        int32_t rel = (int32_t)(int64_t)((uint64_t)(uintptr_t)tramp -
                                                           ((uint64_t)(uintptr_t)image + offset + at + 5));
                        memcpy(buffer + at + 1, &rel, 4);
                    }
                }
                memcpy(image + read_u64(r), buffer, length);
                r += RZ_PATCH_BYTES + 2 * length;
            }
            t->rz_applied[t->rz_applied_count++] = (IsaRedZoneStub){site, stub, bytes};
            report.groups++;
            report.protected_instructions += protected_count;
            report.short_relays += patches > 1;
        }
        g = q;
    }
    if (stats) {
        t->rz_hits = (uint64_t *)(void *)counters;
        t->rz_hits_count = t->rz_applied_count;
    }
    return report;
}

int isa_red_zone_site_of_stub(const IsaTable *t, uintptr_t rip, uint64_t *site) {
    if (!t || !t->rz_area || rip < (uintptr_t)t->rz_area || rip - (uintptr_t)t->rz_area >= t->rz_code_bytes) return 0;
    uint32_t at = (uint32_t)(rip - (uintptr_t)t->rz_area), lo = 0, hi = t->rz_applied_count;
    while (lo < hi) {   /* stubs are laid out in group order: ascending offsets */
        uint32_t mid = lo + (hi - lo) / 2;
        if (t->rz_applied[mid].stub + t->rz_applied[mid].stub_bytes <= at) lo = mid + 1;
        else hi = mid;
    }
    if (lo == t->rz_applied_count || at < t->rz_applied[lo].stub) return 0;
    *site = t->rz_applied[lo].site;
    return 1;
}

/* ---- int3 trap, stub lookup, report ---- */

int isa_rewrite_trap(IsaTable *t, uintptr_t address, IsaRegs *regs, void *xmm, uintptr_t *next_rip) {
    if (!t || !t->image || address < (uintptr_t)t->image) return 0;
    uint64_t offset = address - (uintptr_t)t->image;
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (t->sites[mid].offset < offset) lo = mid + 1;
        else hi = mid;
    }
    if (lo == t->count || t->sites[lo].offset != offset) return 0;
    if (t->sites[lo].mode != ISA_MODE_INT3 || *(const unsigned char *)address != 0xcc) return 0;
    const IsaSite *s = &t->sites[lo];
    if (isa_kind_is_f16c(s->kind)) return 0;   /* never int3 (VEX sites are 5+ bytes); YMM is not in the frame */
    if (isa_kind_is_sse4a(s->kind)) {
        if (!xmm) return 0;
        uint64_t operands[4] = {0, 0, 0, 0};
        unsigned char *dst = (unsigned char *)xmm + 16 * s->dst;
        memcpy(operands, dst, 16);
        if (s->src1 != ISA_NO_REG) memcpy(operands + 2, (unsigned char *)xmm + 16 * s->src1, 16);
        isa_emulate_sse4a(s, operands);
        memcpy(dst, operands, 16);
    } else {
        isa_emulate(s, regs);
    }
    t->hits[lo]++;
    *next_rip = address + t->sites[lo].length;
    return 1;
}

int isa_rewrite_site_of_stub(const IsaTable *t, uintptr_t rip) {
    if (!t || !t->stubs || rip < (uintptr_t)t->stubs) return -1;
    uintptr_t at = rip - (uintptr_t)t->stubs;
    return at < isa_stub_area_size(t) ? (int)(at / ISA_STUB_BYTES) : -1;
}

const char *isa_kind_name(uint8_t kind) {
    static const char *const names[] = {"?",       "movbe",   "movbe",   "lzcnt",   "tzcnt",     "andn",
                                        "bextr",   "blsi",    "blsmsk",  "blsr",    "vcvtph2ps", "vcvtps2ph",
                                        "extrq",   "extrq",   "insertq", "insertq", "movntss",   "movntsd"};
    return kind < sizeof names / sizeof names[0] ? names[kind] : "?";
}

/* Report order: more hits first, then lower offset. */
static int ranks_before(const IsaTable *t, uint32_t a, uint32_t b) {
    return t->hits[a] > t->hits[b] || (t->hits[a] == t->hits[b] && t->sites[a].offset < t->sites[b].offset);
}

/* The counters of the protected instructions, one line per group: "0x<site> <hits>". */
int isa_red_zone_dump_hits(const IsaTable *t, const char *path) {
    if (!t || !t->rz_hits || !path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (uint32_t i = 0; i < t->rz_hits_count; i++)
        fprintf(f, "0x%llx %llu\n", (unsigned long long)t->rz_applied[i].site,
                (unsigned long long)t->rz_hits[i]);
    return fclose(f) == 0 ? 0 : -1;
}

/* The protected instructions with the most executions: the same order as the ISA sites. */
static void red_zone_report(const IsaTable *t, FILE *out) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < t->rz_hits_count; i++) total += t->rz_hits[i];
    fprintf(out, "Red zone: %u groups with a protected instruction, %llu executions counted; most executed:\n",
            t->rz_hits_count, (unsigned long long)total);
    int64_t prev = -1;
    for (int line = 0; line < 10; line++) {
        int64_t best = -1;
        for (uint32_t i = 0; i < t->rz_hits_count; i++) {
            if (t->rz_hits[i] == 0) continue;
            if (prev >= 0 && !(t->rz_hits[i] > t->rz_hits[prev] ||
                               (t->rz_hits[i] == t->rz_hits[prev] && t->rz_applied[i].site < t->rz_applied[prev].site)))
                continue;
            if (best < 0 || t->rz_hits[i] > t->rz_hits[best] ||
                (t->rz_hits[i] == t->rz_hits[best] && t->rz_applied[i].site < t->rz_applied[best].site))
                best = i;
        }
        if (best < 0) return;
        fprintf(out, "  0x%llx %llu\n", (unsigned long long)t->rz_applied[best].site,
                (unsigned long long)t->rz_hits[best]);
        prev = best;
    }
}

void isa_rewrite_report(const IsaTable *t, FILE *out) {
    fprintf(out, "ISA: most executed sites:\n");
    int64_t prev = -1;   /* last printed site; each pass takes the best one ranked after it */
    for (int line = 0; line < 10; line++) {
        int64_t best = -1;
        for (uint32_t i = 0; i < t->count; i++) {
            if (t->hits[i] == 0 || (prev >= 0 && !ranks_before(t, (uint32_t)prev, i))) continue;
            if (best < 0 || ranks_before(t, i, (uint32_t)best)) best = i;
        }
        if (best < 0) break;
        fprintf(out, "  0x%llx %s %llu\n", (unsigned long long)t->sites[best].offset,
                isa_kind_name(t->sites[best].kind), (unsigned long long)t->hits[best]);
        prev = best;
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < t->count; i++) total += t->hits[i];
    fprintf(out, "ISA: %llu rewrites executed over %u sites\n", (unsigned long long)total, t->count);
    const char *isa_path = isa_stats_path();
    if (isa_path && t->sites && t->hits) {
        FILE *f = fopen(isa_path, "w");
        if (!f) fprintf(stderr, "ISA: cannot write the counters to %s\n", isa_path);
        else {
            for (uint32_t i = 0; i < t->count; i++)
                fprintf(f, "0x%llx %llu\n", (unsigned long long)t->sites[i].offset,
                        (unsigned long long)t->hits[i]);
            fclose(f);
        }
    }
    if (t->rz_hits && t->rz_hits_count) {
        red_zone_report(t, out);
        const char *path = rz_stats_path();
        if (path && isa_red_zone_dump_hits(t, path) != 0)
            fprintf(stderr, "Red zone: cannot write the counters to %s\n", path);
    }
}
