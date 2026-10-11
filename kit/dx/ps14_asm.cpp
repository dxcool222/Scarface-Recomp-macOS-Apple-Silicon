// ps14_asm.cpp - ps_1_4 text to Direct3D 9 tokens.
//
// Shader model 1 has no instruction-length field. The instruction token's low
// 16 bits are the D3DSIO opcode (d9sh::Op, which matches the D3D9 headers) and
// bit 30 is D3DSI_COISSUE. Destination and source parameter tokens carry the
// register, the write mask or swizzle, and the modifiers. `def` is the
// exception: its four immediates are raw floats, not source tokens.
#include "ps14_asm.h"

#include "d3d9_shader.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct OpInfo {
    const char *name;
    uint32_t op;
    int nsrc;
    bool dest;
};

const OpInfo *find_op(const std::string &name) {
    // bem is D3DSIO_BEM (89) and texdepth is D3DSIO_TEXDEPTH (87). Neither is
    // in d9sh::Op: the decoder does not execute them. They are assembled only
    // so Wine's ps_1_4 vectors for those opcodes stay byte-exact.
    static const OpInfo k[] = {
        {"mov", d9sh::OP_MOV, 1, true},       {"add", d9sh::OP_ADD, 2, true},
        {"mad", d9sh::OP_MAD, 3, true},       {"mul", d9sh::OP_MUL, 2, true},
        {"dp3", d9sh::OP_DP3, 2, true},       {"texld", d9sh::OP_TEX, 1, true},
        {"texcrd", d9sh::OP_TEXCOORD, 1, true}, {"bem", 0x59, 2, true},
        {"texdepth", 0x57, 0, true},          {"phase", d9sh::OP_PHASE, 0, false},
    };
    for (const OpInfo &o : k)
        if (name == o.name)
            return &o;
    return nullptr;
}

int component(char c) {
    switch (c) {
    case 'x':
    case 'r':
        return 0;
    case 'y':
    case 'g':
        return 1;
    case 'z':
    case 'b':
        return 2;
    case 'w':
    case 'a':
        return 3;
    default:
        return -1;
    }
}

uint32_t reg_type(char c) {
    switch (c) {
    case 'r':
        return d9sh::R_TEMP;
    case 'v':
        return d9sh::R_INPUT;
    case 'c':
        return d9sh::R_CONST;
    case 't':
        return d9sh::R_ADDR;
    default:
        return 0xffffffffu;
    }
}

struct Reg {
    bool neg = false;
    uint32_t type = 0;
    uint32_t index = 0;
    int comp[4] = {0, 1, 2, 3};
    int ncomp = 0;
    uint32_t modifier = 0; // source modifier before negation is folded in
};

bool parse_reg(const std::string &tok, Reg *r, std::string *error) {
    size_t i = 0;
    if (!tok.empty() && tok[0] == '-') {
        r->neg = true;
        i = 1;
    }
    if (i >= tok.size()) {
        *error = "empty register";
        return false;
    }
    uint32_t type = reg_type(tok[i]);
    if (type == 0xffffffffu) {
        *error = "bad register " + tok;
        return false;
    }
    ++i;
    if (i >= tok.size() || tok[i] < '0' || tok[i] > '9') {
        *error = "bad register " + tok;
        return false;
    }
    uint32_t index = 0;
    while (i < tok.size() && tok[i] >= '0' && tok[i] <= '9') {
        index = index * 10u + (uint32_t)(tok[i] - '0');
        ++i;
    }
    if (index > 0x7ffu) {
        *error = "register index out of range in " + tok;
        return false;
    }
    r->type = type;
    r->index = index;
    if (i < tok.size() && tok[i] == '.') {
        ++i;
        size_t b = i;
        while (i < tok.size() && tok[i] != '_')
            ++i;
        if (i == b) {
            *error = "empty swizzle in " + tok;
            return false;
        }
        r->ncomp = 0;
        for (size_t k = b; k < i; ++k) {
            int c = component(tok[k]);
            if (c < 0 || r->ncomp == 4) {
                *error = "bad swizzle in " + tok;
                return false;
            }
            r->comp[r->ncomp++] = c;
        }
    }
    if (i < tok.size() && tok[i] == '_') {
        std::string m = tok.substr(i + 1);
        if (m == "bx2")
            r->modifier = 4;
        else if (m == "bias")
            r->modifier = 2;
        else if (m == "x2")
            r->modifier = 7;
        else if (m == "dz")
            r->modifier = 9;
        else if (m == "dw")
            r->modifier = 10;
        else {
            *error = "unknown source modifier _" + m;
            return false;
        }
        i = tok.size();
    }
    if (i != tok.size()) {
        *error = "trailing junk in " + tok;
        return false;
    }
    return true;
}

uint32_t source_modifier(const Reg &r, std::string *error) {
    uint32_t m = r.modifier;
    if (!r.neg)
        return m;
    if (m == 0)
        return 1;
    if (m == 2)
        return 3;
    if (m == 4)
        return 5;
    if (m == 7)
        return 8;
    *error = "negation does not combine with that source modifier";
    return 0xffffffffu;
}

uint32_t swizzle_of(const Reg &r, std::string *error) {
    if (r.ncomp == 0)
        return 0xe4;
    int c[4];
    if (r.ncomp == 1) {
        c[0] = c[1] = c[2] = c[3] = r.comp[0];
    } else if (r.ncomp == 4) {
        for (int i = 0; i < 4; ++i)
            c[i] = r.comp[i];
    } else {
        *error = "a source swizzle needs 1 or 4 components";
        return 0xffffffffu;
    }
    uint32_t s = 0;
    for (int i = 0; i < 4; ++i)
        s |= (uint32_t)(c[i] & 3) << (2 * i);
    return s;
}

uint32_t mask_of(const Reg &r) {
    if (r.ncomp == 0)
        return 0xf;
    uint32_t m = 0;
    for (int i = 0; i < r.ncomp; ++i)
        m |= 1u << r.comp[i];
    return m;
}

uint32_t param_token(uint32_t type, uint32_t index, uint32_t field, uint32_t mod_or_shift) {
    return 0x80000000u | ((type & 7u) << 28) | (index & 0x7ffu) | ((field & 0xffu) << 16) |
           ((mod_or_shift & 0xfu) << 24);
}

bool parse_float(const std::string &tok, float *out) {
    if (tok.empty())
        return false;
    std::string s = tok;
    if (s.back() == 'f' || s.back() == 'F')
        s.pop_back();
    if (s.empty())
        return false;
    char *end = nullptr;
    float v = strtof(s.c_str(), &end);
    if (!end || end != s.c_str() + s.size())
        return false;
    *out = v;
    return true;
}

bool parse_shift(const std::vector<std::string> &suffix, int *shift, bool *sat, std::string *error) {
    *shift = 0;
    *sat = false;
    bool saw_shift = false;
    for (const std::string &s : suffix) {
        int next = 0;
        bool is_shift = true;
        if (s == "x2")
            next = 1;
        else if (s == "x4")
            next = 2;
        else if (s == "x8")
            next = 3;
        else if (s == "d2")
            next = -1;
        else if (s == "d4")
            next = -2;
        else if (s == "d8")
            next = -3;
        else if (s == "sat")
            is_shift = false;
        else {
            *error = "unknown opcode modifier _" + s;
            return false;
        }
        if (!is_shift) {
            if (*sat) {
                *error = "repeated _sat";
                return false;
            }
            *sat = true;
            continue;
        }
        if (saw_shift) {
            *error = "two destination shifts on one opcode";
            return false;
        }
        saw_shift = true;
        *shift = next;
    }
    return true;
}

void tokenize(const std::string &line, std::vector<std::string> *tok, bool *coissue) {
    *coissue = false;
    size_t i = 0;
    auto skip = [&] {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == ','))
            ++i;
    };
    skip();
    if (i < line.size() && line[i] == '+') {
        *coissue = true;
        ++i;
    }
    for (;;) {
        skip();
        if (i >= line.size())
            break;
        size_t b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != ',')
            ++i;
        tok->push_back(line.substr(b, i - b));
    }
}

std::string strip_comment(std::string line) {
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    size_t cut = line.size();
    size_t a = line.find("//");
    size_t b = line.find(';');
    if (a != std::string::npos)
        cut = a;
    if (b != std::string::npos && b < cut)
        cut = b;
    return line.substr(0, cut);
}

} // namespace

bool ps14_assemble(const char *src, size_t len, std::vector<uint32_t> *dwords, std::string *error) {
    dwords->clear();
    if (!src || !len) {
        *error = "empty shader";
        return false;
    }
    std::string text(src, src + len);
    std::vector<uint32_t> out;
    out.push_back(0xffff0104u);
    bool saw_profile = false;
    int line_no = 0;
    size_t at = 0;
    auto fail = [&](const std::string &why) {
        *error = "line " + std::to_string(line_no) + ": " + why;
        dwords->clear();
        return false;
    };
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = strip_comment(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        at = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++line_no;
        std::vector<std::string> tok;
        bool coissue = false;
        tokenize(line, &tok, &coissue);
        if (tok.empty()) {
            if (coissue)
                return fail("'+' with no opcode");
            if (nl == std::string::npos)
                break;
            continue;
        }
        if (!saw_profile) {
            if (coissue || tok.size() != 1 || (tok[0] != "ps_1_4" && tok[0] != "ps.1.4"))
                return fail("expected ps_1_4");
            saw_profile = true;
            if (nl == std::string::npos)
                break;
            continue;
        }
        if (tok[0] == "def") {
            if (coissue)
                return fail("def cannot be co-issued");
            if (tok.size() != 6)
                return fail("def needs a constant and four values");
            Reg dst;
            std::string why;
            if (!parse_reg(tok[1], &dst, &why))
                return fail(why);
            if (dst.neg || dst.ncomp || dst.modifier || dst.type != d9sh::R_CONST)
                return fail("def destination must be a plain constant register");
            out.push_back(d9sh::OP_DEF);
            out.push_back(param_token(dst.type, dst.index, mask_of(dst), 0));
            for (int k = 0; k < 4; ++k) {
                float v = 0;
                if (!parse_float(tok[2 + k], &v))
                    return fail("bad def immediate " + tok[2 + k]);
                uint32_t bits = 0;
                memcpy(&bits, &v, 4);
                out.push_back(bits);
            }
        } else {
            std::string name = tok[0];
            std::vector<std::string> suffix;
            for (;;) {
                size_t u = name.rfind('_');
                if (u == std::string::npos)
                    break;
                suffix.insert(suffix.begin(), name.substr(u + 1));
                name.resize(u);
            }
            const OpInfo *op = find_op(name);
            if (!op)
                return fail("unknown opcode " + tok[0]);
            int shift = 0;
            bool sat = false;
            std::string why;
            if (!parse_shift(suffix, &shift, &sat, &why))
                return fail(why);
            if (!op->dest && (shift || sat || coissue))
                return fail("phase takes no modifier");
            size_t need = (op->dest ? 1u : 0u) + (size_t)op->nsrc;
            if (tok.size() != 1 + need)
                return fail("wrong operand count for " + tok[0]);
            uint32_t inst = op->op;
            if (coissue)
                inst |= 0x40000000u;
            out.push_back(inst);
            size_t arg = 1;
            if (op->dest) {
                Reg dst;
                if (!parse_reg(tok[arg++], &dst, &why))
                    return fail(why);
                if (dst.neg || dst.modifier)
                    return fail("destination has a source modifier");
                int enc = shift < 0 ? shift + 16 : shift;
                uint32_t dt = param_token(dst.type, dst.index, mask_of(dst), (uint32_t)(enc & 0xf));
                if (sat)
                    dt |= 1u << 20;
                out.push_back(dt);
            }
            for (int s = 0; s < op->nsrc; ++s) {
                Reg src;
                if (!parse_reg(tok[arg++], &src, &why))
                    return fail(why);
                uint32_t mod = source_modifier(src, &why);
                uint32_t sw = swizzle_of(src, &why);
                if (mod == 0xffffffffu || sw == 0xffffffffu)
                    return fail(why);
                out.push_back(param_token(src.type, src.index, sw, mod));
            }
        }
        if (out.size() > 4096)
            return fail("shader is too long");
        if (nl == std::string::npos)
            break;
    }
    if (!saw_profile)
        return fail("expected ps_1_4");
    out.push_back(d9sh::OP_END);
    *dwords = std::move(out);
    return true;
}
