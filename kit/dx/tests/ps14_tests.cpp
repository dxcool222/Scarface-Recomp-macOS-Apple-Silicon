// ps_1_4 assembler, ID3DXBuffer, and pixel-shader object tests.
//
// Expected DWORDs for the Wine vectors are copied from
// dlls/d3dcompiler_43/tests/asm.c. The focused DEF/DP3/MAD/MUL/negation
// vectors are built from those same operand encodings plus the D3DSIO opcode
// numbers, not by running this assembler and pasting its output.
#include "guest_abi.h"
#include "../d3d9_msl.h"
#include "../d3d9_pipeline.h"
#include "../d3d9_shader.h"
#include "../dx.h"
#include "../ps14_asm.h"
#include "../../runtime/memory.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static void dump_dwords(const std::vector<uint32_t> &d) {
    fprintf(stderr, "  got %zu:", d.size());
    for (uint32_t w : d)
        fprintf(stderr, " %08x", w);
    fprintf(stderr, "\n");
}

static bool assemble(const char *src, std::vector<uint32_t> *out) {
    std::string err;
    if (!ps14_assemble(src, std::strlen(src), out, &err)) {
        fprintf(stderr, "assemble failed: %s\n%s\n", err.c_str(), src);
        return false;
    }
    return true;
}

static void expect_words(const char *src, const uint32_t *exp, size_t n, bool decode) {
    std::vector<uint32_t> got;
    bool ok = assemble(src, &got);
    CHECK(ok);
    if (!ok)
        return;
    bool same = got.size() == n && std::memcmp(got.data(), exp, n * 4) == 0;
    CHECK(same);
    if (!same) {
        fprintf(stderr, "dword mismatch for:\n%s\n", src);
        dump_dwords(got);
    }
    if (!decode)
        return;
    std::vector<uint8_t> bytes(got.size() * 4);
    std::memcpy(bytes.data(), got.data(), bytes.size());
    const d9sh::Program &p = d9sh::program_for(bytes);
    CHECK(p.ok);
    CHECK(p.pixel && p.major == 1 && p.minor == 4);
    if (!p.ok)
        fprintf(stderr, "program_for: %s\n", p.why.c_str());
}

static const char *op_name(uint32_t op) {
    switch (op) {
    case d9sh::OP_MOV:
        return "mov";
    case d9sh::OP_ADD:
        return "add";
    case d9sh::OP_MAD:
        return "mad";
    case d9sh::OP_MUL:
        return "mul";
    case d9sh::OP_DP3:
        return "dp3";
    case d9sh::OP_TEX:
        return "texld";
    case d9sh::OP_TEXCOORD:
        return "texcrd";
    default:
        return "?";
    }
}

static char reg_letter(uint32_t type) {
    switch (type) {
    case d9sh::R_TEMP:
        return 'r';
    case d9sh::R_INPUT:
        return 'v';
    case d9sh::R_CONST:
        return 'c';
    case d9sh::R_ADDR:
        return 't';
    default:
        return '?';
    }
}

static std::string mask_text(uint32_t m) {
    if (m == 0xf)
        return "";
    std::string s = ".";
    if (m & 1)
        s += 'x';
    if (m & 2)
        s += 'y';
    if (m & 4)
        s += 'z';
    if (m & 8)
        s += 'w';
    return s;
}

static std::string swizzle_text(uint32_t sw) {
    if (sw == 0xe4)
        return "";
    int c[4];
    for (int i = 0; i < 4; ++i)
        c[i] = (sw >> (2 * i)) & 3;
    const char *n = "xyzw";
    if (c[0] == c[1] && c[1] == c[2] && c[2] == c[3])
        return std::string(".") + n[c[0]];
    std::string s = ".";
    for (int i = 0; i < 4; ++i)
        s += n[c[i]];
    return s;
}

static std::string src_text(const d9sh::Src &s) {
    uint32_t mod = s.mod;
    bool neg = false;
    const char *suf = "";
    if (mod == 1)
        neg = true;
    else if (mod == 3) {
        neg = true;
        suf = "_bias";
    } else if (mod == 4)
        suf = "_bx2";
    else if (mod == 5) {
        neg = true;
        suf = "_bx2";
    } else if (mod == 7)
        suf = "_x2";
    else if (mod == 8) {
        neg = true;
        suf = "_x2";
    } else if (mod == 9)
        suf = "_dz";
    else if (mod == 10)
        suf = "_dw";
    std::string o = neg ? "-" : "";
    o += reg_letter(s.type);
    o += std::to_string(s.index);
    o += swizzle_text(s.swz);
    o += suf;
    return o;
}

static std::string inst_text(const d9sh::Inst &in) {
    std::string o = op_name(in.op);
    if (in.has_dst) {
        if (in.dst.shift == 1)
            o += "_x2";
        else if (in.dst.shift == 2)
            o += "_x4";
        else if (in.dst.shift == 3)
            o += "_x8";
        else if (in.dst.shift == -1)
            o += "_d2";
        else if (in.dst.shift == -2)
            o += "_d4";
        if (in.dst.saturate)
            o += "_sat";
    }
    std::string s = o;
    if (in.has_dst) {
        s += " ";
        s += reg_letter(in.dst.type);
        s += std::to_string(in.dst.index);
        s += mask_text(in.dst.mask);
    }
    for (uint32_t i = 0; i < in.nsrc; ++i) {
        s += ", ";
        s += src_text(in.src[i]);
    }
    return s;
}

static void test_wine() {
    static const uint32_t mov[] = {0xffff0104, 0x00000001, 0x800f0000, 0x80e40001, 0x0000ffff};
    static const uint32_t mov_c[] = {0xffff0104, 0x00000001, 0x800f0000, 0xa0e40007, 0x0000ffff};
    static const uint32_t phase[] = {0xffff0104, 0x0000fffd, 0x0000ffff};
    static const uint32_t texcrd[] = {0xffff0104, 0x00000040, 0x800f0000, 0xb0e40000, 0x0000ffff};
    static const uint32_t texcrd_sat[] = {0xffff0104, 0x00000040, 0x801f0004, 0xb0e40003, 0x0000ffff};
    static const uint32_t texld[] = {0xffff0104, 0x00000042, 0x800f0000, 0xb0e40000, 0x0000ffff};
    static const uint32_t texld_dz[] = {0xffff0104, 0x00000042, 0x800f0005, 0x89e40002, 0x0000ffff};
    static const uint32_t bem[] = {0xffff0104, 0x00000059, 0x80030001, 0xa0e40000, 0x80e40000,
                                   0x0000ffff};
    static const uint32_t texdepth[] = {0xffff0104, 0x00000057, 0x800f0005, 0x0000ffff};
    static const uint32_t bx2[] = {0xffff0104, 0x00000002, 0x800f0000, 0x80e40001, 0x84e40002,
                                   0x0000ffff};
    static const uint32_t x4[] = {0xffff0104, 0x00000002, 0x820f0000, 0x80e40001, 0x80e40002,
                                  0x0000ffff};
    static const uint32_t co[] = {0xffff0104, 0x00000002, 0x80070000, 0x80e40001, 0x80e40002,
                                  0x40000002, 0x80080000, 0x80e40001, 0x80e40002, 0x0000ffff};
    static const uint32_t depth_x2[] = {0xffff0104, 0x00000057, 0x810f0005, 0x0000ffff};
    static const uint32_t bem_d2[] = {0xffff0104, 0x00000059, 0x8f0f0001, 0xa0e40000, 0x80e40000,
                                      0x0000ffff};
    expect_words("ps_1_4\nmov r0, r1\n", mov, std::size(mov), true);
    expect_words("ps_1_4\nmov r0, c7\n", mov_c, std::size(mov_c), true);
    expect_words("ps_1_4\nphase\n", phase, std::size(phase), true);
    expect_words("ps_1_4\ntexcrd r0, t0\n", texcrd, std::size(texcrd), true);
    expect_words("ps_1_4\ntexcrd_sat r4, t3\n", texcrd_sat, std::size(texcrd_sat), true);
    expect_words("ps_1_4\ntexld r0, t0\n", texld, std::size(texld), true);
    expect_words("ps_1_4\ntexld r5, r2_dz\n", texld_dz, std::size(texld_dz), true);
    expect_words("ps_1_4\nbem r1.rg, c0, r0\n", bem, std::size(bem), false);
    expect_words("ps_1_4\ntexdepth r5\n", texdepth, std::size(texdepth), false);
    expect_words("ps_1_4\nadd r0, r1, r2_bx2\n", bx2, std::size(bx2), true);
    expect_words("ps_1_4\nadd_x4 r0, r1, r2\n", x4, std::size(x4), true);
    expect_words("ps_1_4\nadd r0.rgb, r1, r2\n+add r0.a, r1, r2\n", co, std::size(co), true);
    expect_words("ps_1_4\ntexdepth_x2 r5\n", depth_x2, std::size(depth_x2), false);
    expect_words("ps.1.4\nbem_d2 r1, c0, r0\n", bem_d2, std::size(bem_d2), false);
}

static void test_focused() {
    // def layout is Wine's ps_1_1 def; 0.5 is the 0x3f000000 immediate from
    // Wine's vs_1_1 "1.0f, 1.0f, 1.0f, 0.5f" vector. The version token is the
    // ps_1_4 token from the ps_1_4 mov vector.
    static const uint32_t def[] = {0xffff0104, 0x00000051, 0xa00f0001, 0x3f000000, 0x3f000000,
                                   0x3f000000, 0x3f000000, 0x0000ffff};
    // D3DSIO_DP3 is 8. Operand tokens match Wine's rN / cN encodings.
    static const uint32_t dp3[] = {0xffff0104, 0x00000008, 0x800f0003, 0x80e40000, 0xa0e40000,
                                   0x0000ffff};
    // D3DSIO_MAD is 4. .w mask is Wine's co-issued .a mask. _bx2 is Wine's
    // r2_bx2 encoding with the register number changed.
    static const uint32_t mad[] = {0xffff0104, 0x00000004, 0x80080001, 0x80ff0003, 0xa0e40005,
                                   0x84e40001, 0x0000ffff};
    // D3DSIO_MUL is 5. _x2 is the +1 destination shift from Wine's texdepth_x2.
    static const uint32_t mul[] = {0xffff0104, 0x00000005, 0x81070003, 0x80e40003, 0xa0e40007,
                                   0x0000ffff};
    // Negation is Wine's "-r1" source token, register number 3.
    static const uint32_t neg[] = {0xffff0104, 0x00000002, 0x80070003, 0x80e40000, 0x81e40003,
                                   0x0000ffff};
    static const uint32_t movx2[] = {0xffff0104, 0x00000001, 0x81040001, 0x80aa0001, 0x0000ffff};
    static const uint32_t movx4[] = {0xffff0104, 0x00000001, 0x82080001, 0x80ff0001, 0x0000ffff};
    expect_words("ps_1_4\ndef c1, 0.5, 0.5, 0.5, 0.5\n", def, std::size(def), true);
    expect_words("ps_1_4\ndp3 r3, r0, c0\n", dp3, std::size(dp3), true);
    expect_words("ps_1_4\nmad r1.w, r3.w, c5, r1_bx2\n", mad, std::size(mad), true);
    expect_words("ps_1_4\nmul_x2 r3.xyz, r3, c7\n", mul, std::size(mul), true);
    expect_words("ps_1_4\nadd r3.xyz, r0, -r3\n", neg, std::size(neg), true);
    expect_words("ps_1_4\nmov_x2 r1.b, r1.b\n", movx2, std::size(movx2), true);
    expect_words("ps_1_4\nmov_x4 r1.w, r1.w\n", movx4, std::size(movx4), true);

    std::vector<uint32_t> bad;
    std::string err;
    const char *bad_op = "ps_1_4\nnot_an_op r0, r1\n";
    const char *bad_ver = "vs_1_1\nmov r0, r1\n";
    CHECK(!ps14_assemble(bad_op, std::strlen(bad_op), &bad, &err));
    CHECK(bad.empty());
    CHECK(!ps14_assemble(bad_ver, std::strlen(bad_ver), &bad, &err));
    CHECK(bad.empty());
    CHECK(!ps14_assemble("", 0, &bad, &err));
}

static std::vector<uint8_t> load_fixture(const char *hash) {
    std::string path = std::string(PS14_FIXTURE_DIR) + "/" + hash + ".asm";
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in) {
        fprintf(stderr, "cannot read %s\n", path.c_str());
        b.clear();
    }
    return b;
}

static uint32_t count_token(const std::vector<uint32_t> &d, uint32_t w) {
    uint32_t n = 0;
    for (uint32_t x : d)
        if (x == w)
            ++n;
    return n;
}

static const char *k_prefix[] = {
    "texld r0, t0",
    "mov r1, c3",
    "dp3 r3, r0, c0",
    "add r3.xyz, r0, -r3",
    "mad r1.w, r3.w, c5, r1_bx2",
    "mul_x2 r3.xyz, r3, c7",
};
static const char *k_tail[] = {
    "mov r2, c6",
    "mad r3.xyz, r2_bx2, r3.w, r3",
    "mov r1.z, r1.w",
    "mov r3.w, c2.w",
    "add r0, r3, r1.z",
};
static const char *k_pair[] = {"mov_x2 r1.z, r1.z", "add r0.xyz, r0, r1.z"};

static void append(std::vector<std::string> *d, const char *const *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        d->push_back(s[i]);
}

static void test_fixtures() {
    struct Case {
        const char *hash;
        size_t len;
        int pairs;          // extra mov_x2/add pairs after the shared tail
        bool extra_add;     // the single add that shader 2 introduces
        const char *co_mov; // inserted after mul_x2, or null
        uint32_t co_dest;   // destination token of that co-issued mov
    };
    const Case cases[] = {
        {"43a44d82ed8511e8", 307, 0, false, nullptr, 0},
        {"2e877d62e631d4fe", 333, 0, true, nullptr, 0},
        {"be2c27bb738fab65", 380, 1, true, nullptr, 0},
        {"0829d11417f83838", 427, 2, true, nullptr, 0},
        {"ac73968818c97d03", 449, 2, true, "mov_x2 r1.w, r1.w", 0x81080001u},
        {"ee112e6ef1450672", 449, 2, true, "mov_x4 r1.w, r1.w", 0x82080001u},
    };
    for (const Case &cs : cases) {
        std::vector<uint8_t> raw = load_fixture(cs.hash);
        CHECK(raw.size() == cs.len);
        std::vector<uint32_t> dw;
        std::string err;
        bool ok = !raw.empty() && ps14_assemble((const char *)raw.data(), raw.size(), &dw, &err);
        CHECK(ok);
        if (!ok) {
            fprintf(stderr, "%s: %s\n", cs.hash, err.c_str());
            continue;
        }
        CHECK(!dw.empty() && dw.front() == 0xffff0104u && dw.back() == 0x0000ffffu);
        CHECK(count_token(dw, 0x0000fffdu) == 1);
        CHECK(count_token(dw, 0x40000004u) == 1); // the co-issued +mad
        if (cs.co_mov) {
            CHECK(count_token(dw, 0x40000001u) == 1);
            CHECK(count_token(dw, cs.co_dest) == 1);
        }
        std::vector<uint8_t> bytes(dw.size() * 4);
        std::memcpy(bytes.data(), dw.data(), bytes.size());
        const d9sh::Program &p = d9sh::program_for(bytes);
        CHECK(p.ok);
        if (!p.ok) {
            fprintf(stderr, "%s program_for: %s\n", cs.hash, p.why.c_str());
            continue;
        }
        auto d1 = p.defs.find(1);
        auto d2 = p.defs.find(2);
        CHECK(d1 != p.defs.end() && d2 != p.defs.end());
        if (d1 != p.defs.end())
            CHECK(d1->second[0] == 0.5f && d1->second[3] == 0.5f);
        if (d2 != p.defs.end())
            CHECK(d2->second[0] == 0.f && d2->second[3] == 0.f);

        std::vector<std::string> expect;
        append(&expect, k_prefix, std::size(k_prefix));
        if (cs.co_mov)
            expect.push_back(cs.co_mov);
        append(&expect, k_tail, std::size(k_tail));
        if (cs.extra_add)
            expect.push_back("add r0.xyz, r0, r1.z");
        for (int i = 0; i < cs.pairs; ++i)
            append(&expect, k_pair, std::size(k_pair));
        CHECK(p.code.size() == expect.size());
        for (size_t i = 0; i < p.code.size() && i < expect.size(); ++i) {
            std::string got = inst_text(p.code[i]);
            if (got != expect[i]) {
                fprintf(stderr, "%s inst %zu: got '%s' expected '%s'\n", cs.hash, i, got.c_str(),
                        expect[i].c_str());
                ++g_failures;
            }
            ++g_checks;
        }
    }
}

// The CPU interpreter applies def immediates. The Metal generator reads c[n]
// from the constant bank and does not substitute them. These six Scarface
// shaders only read a def of zero, so the gap does not change their result.
// This test fails if a future change bakes def into the generator without
// updating the expected gap, or if the decoder stops recording def.
static void test_def_gap() {
    const char *src = "ps_1_4\ndef c2, 0.25, 0.25, 0.25, 0.25\nmov r0, c2\n";
    std::vector<uint32_t> dw;
    CHECK(assemble(src, &dw));
    std::vector<uint8_t> bytes(dw.size() * 4);
    std::memcpy(bytes.data(), dw.data(), bytes.size());
    const d9sh::Program &p = d9sh::program_for(bytes);
    CHECK(p.ok);
    auto it = p.defs.find(2);
    CHECK(it != p.defs.end() && it->second[0] == 0.25f);
    std::string msl, why;
    CHECK(d9msl::pixel_source(p, {}, &msl, &why));
    if (msl.empty())
        fprintf(stderr, "pixel_source: %s\n", why.c_str());
    CHECK(msl.find("c[2]") != std::string::npos);
    CHECK(msl.find("0.25") == std::string::npos);
}

static const uint32_t kDev_Create = 16;
static const uint32_t kDev_SetFVF = 89;
static const uint32_t kDev_GetFVF = 90;
static const uint32_t kDev_CreatePS = 106; // vtable +0x1A8
static const uint32_t kDev_SetPS = 107;
static const uint32_t kDev_GetPS = 108;
static const uint32_t kBuf_Release = 2;         // vtable +0x08
static const uint32_t kBuf_GetPointer = 3;      // vtable +0x0C
static const uint32_t kBuf_GetSize = 4;
static const uint32_t kPS_GetFunction = 4;

static void test_guest() {
    cpu_reset();
    std::vector<uint8_t> raw = load_fixture("43a44d82ed8511e8");
    CHECK(raw.size() == 307);
    if (raw.size() != 307)
        return;
    std::memcpy(gm_ptr(sc(0)), raw.data(), raw.size());
    wr32(sc(0x800), 0x11111111u);
    wr32(sc(0x804), 0x22222222u);
    uint32_t hr = call_shim(tramp("d3dx9_29.dll", "D3DXAssembleShader"),
                            {sc(0), (uint32_t)raw.size(), 0, 0, 0, sc(0x800), sc(0x804)});
    CHECK(hr == 0);
    uint32_t buf = rd32(sc(0x800));
    CHECK(buf != 0);
    CHECK(rd32(sc(0x804)) == 0);
    if (!buf)
        return;
    uint32_t bytes = call_method(buf, kBuf_GetPointer);
    uint32_t size = call_method(buf, kBuf_GetSize);
    CHECK(bytes != 0);
    CHECK(size >= 8 && (size % 4) == 0);
    if (bytes && gm_fits(bytes, size) && size >= 8) {
        CHECK(rd32(bytes) == 0xffff0104u);
        CHECK(rd32(bytes + size - 4) == 0x0000ffffu);
    }

    std::memset(gm_ptr(sc(0xC00)), 0, 64);
    wr32(sc(0xD00), 0);
    uint32_t d3d = call_shim(tramp("d3d9.dll", "Direct3DCreate9"), {32});
    CHECK(d3d != 0);
    hr = call_method(d3d, kDev_Create, {0, 1, 0, 0, sc(0xC00), sc(0xD00)});
    CHECK(hr == 0);
    uint32_t dev = rd32(sc(0xD00));
    CHECK(dev != 0);
    if (!dev || !bytes)
        return;

    wr32(sc(0x808), 0);
    hr = call_method(dev, kDev_CreatePS, {bytes, sc(0x808)});
    CHECK(hr == 0);
    uint32_t ps = rd32(sc(0x808));
    CHECK(ps != 0);
    ComObj *pso = com_this(ps);
    CHECK(pso && pso->kind == K_D3D9PS);
    CHECK(pso && pso->blob.size() == size);

    uint32_t rel = call_method(buf, kBuf_Release);
    CHECK(rel == 0);

    wr32(sc(0x80C), 0);
    hr = call_method(ps, kPS_GetFunction, {0, sc(0x80C)});
    CHECK(hr == 0);
    CHECK(rd32(sc(0x80C)) == size);
    wr32(sc(0x80C), size);
    hr = call_method(ps, kPS_GetFunction, {sc(0x1000), sc(0x80C)});
    CHECK(hr == 0);
    if (pso)
        CHECK(std::memcmp(gm_ptr(sc(0x1000)), pso->blob.data(), size) == 0);

    hr = call_method(dev, kDev_SetPS, {ps});
    CHECK(hr == 0);
    ComObj *devo = com_this(dev);
    CHECK(devo && devo->kind == K_D3D9DEVICE);
    if (devo && pso) {
        D9Pipeline &pl = d9_pipeline(devo->id);
        CHECK(!pl.ps.empty());
        CHECK(pl.pixel_shader == pso->id);
        CHECK(pl.vs.empty());
    }
    wr32(sc(0x810), 0x11111111u);
    hr = call_method(dev, kDev_GetPS, {sc(0x810)});
    CHECK(hr == 0);
    CHECK(rd32(sc(0x810)) == ps);

    hr = call_method(dev, kDev_SetPS, {0});
    CHECK(hr == 0);
    if (devo) {
        CHECK(d9_pipeline(devo->id).ps.empty());
        CHECK(d9_pipeline(devo->id).pixel_shader == 0);
    }
    hr = call_method(dev, kDev_GetPS, {sc(0x810)});
    CHECK(hr == 0);
    CHECK(rd32(sc(0x810)) == 0);

    hr = call_method(dev, kDev_SetFVF, {0x44});
    CHECK(hr == 0);
    hr = call_method(dev, kDev_GetFVF, {sc(0x814)});
    CHECK(hr == 0);
    CHECK(rd32(sc(0x814)) == 0x44u);
    if (devo)
        CHECK(d9_pipeline(devo->id).fvf == 0x44u);

    const char *bad = "ps_1_4\nnot_an_op r0\n";
    std::memcpy(gm_ptr(sc(0)), bad, std::strlen(bad));
    wr32(sc(0x800), 0x11111111u);
    hr = call_shim(tramp("d3dx9_29.dll", "D3DXAssembleShader"),
                   {sc(0), (uint32_t)std::strlen(bad), 0, 0, 0, sc(0x800), sc(0x804)});
    CHECK(hr != 0);
    CHECK(rd32(sc(0x800)) == 0);
    uint32_t ebuf = rd32(sc(0x804));
    CHECK(ebuf != 0);
    if (ebuf) {
        uint32_t ep = call_method(ebuf, kBuf_GetPointer);
        CHECK(ep != 0);
        if (ep)
            CHECK(std::strstr((const char *)gm_ptr(ep), "not_an_op") != nullptr);
        CHECK(call_method(ebuf, kBuf_Release) == 0);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    if (!g_scratch) {
        fprintf(stderr, "scratch alloc failed\n");
        return 1;
    }
    test_wine();
    test_focused();
    test_fixtures();
    test_def_gap();
    test_guest();
    printf("ps14_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
