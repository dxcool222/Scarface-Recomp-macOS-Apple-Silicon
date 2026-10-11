"""Cross-function JMP as CALL_FN;return versus Unicorn.

    .venv/bin/python -m pytest -q kit/tools/recomp/tests/test_translate_tailcall.py

A guest JMP does not push a return. The translator emits CALL_FN(dest);return
when dest is another translated body. That is a host call, not a guest CALL.
These tests check guest-visible state (EIP/ESP/EBP/GPRs/stack), not the C
spelling. Same-body goto is a different lowering and is not this family.
"""
import ctypes as C
import os
import platform
import random
import shutil
import struct
import subprocess
import sys
import tempfile

import pytest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INTR, UcError
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402
from test_translate_driver import synthetic_image, _EmitOpts  # noqa: E402

CODE_BASE, CODE_SIZE = 0x0D010000, 0x00020000
STACK_BASE, STACK_SIZE = 0x0EF00000, 0x00100000
ESP_INIT = 0x0EFFFF00
CAVE = 0x0DEAC000
MAGIC_RET = CAVE
EFLAGS_COMPARED = 0x0CC5
EFLAGS_MISC_INIT = 0x202
CS_SELECTOR = 0x1B
UC_REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
           UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
REG_NAMES = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]


class X86(C.Structure):
    _fields_ = [
        ("r", C.c_uint32 * 8), ("eip", C.c_uint32),
        ("eflags_cf", C.c_uint32), ("eflags_zf", C.c_uint32),
        ("eflags_sf", C.c_uint32), ("eflags_of", C.c_uint32),
        ("eflags_pf", C.c_uint32), ("eflags_af", C.c_uint32),
        ("eflags_df", C.c_uint32), ("eflags_misc", C.c_uint32),
        ("st", C.c_double * 8), ("st_bits", C.c_uint64 * 8),
        ("st_exact", C.c_uint8 * 8), ("fpu_top", C.c_uint32),
        ("fpu_cw", C.c_uint16), ("fpu_sw", C.c_uint16),
        ("fpu_tag", C.c_uint16),
        ("fs_base", C.c_uint32),
        ("xmm", (C.c_uint32 * 4) * 8),
        ("mm", C.c_uint64 * 8),
    ]


def _fn(img, start, addrs, listed=None):
    insns = img.recover(start, listed if listed is not None else (addrs - {start}))
    fn = T.Function(start, "fn", insns[-1].addr + 1 - start, insns)
    fn.measure(img)
    tr = T.Translator(img, addrs, _EmitOpts())
    tr.prepare(fn)
    return "\n".join(tr.translate(fn)), tr


def test_cross_function_jmp_emits_call_fn_not_guest_push():
    """JMP to an already-listed function is CALL_FN;return with no guest PUSH.

    Recover follows an unlisted dest into the same body (goto). The 18,991
    `_tailcall` sites are the other shape: dest is already a function.
    """
    jmp, dest = 0x00401000, 0x00401020
    img = synthetic_image({
        jmp: bytes.fromhex("e9") + struct.pack("<i", dest - (jmp + 5)),
        dest: bytes.fromhex("b8ddccbbaa c3"),
    })
    text, tr = _fn(img, jmp, {jmp, dest}, listed={dest})
    assert "CALL_FN(00401020); return;" in text
    assert "c->r[4] -= 4;" not in text
    assert tr.stats["_tailcall"] == 1


def test_jmp_to_listed_push_mem_ret_is_call_fn_then_recomp_jump():
    """The remaining 42 split trampolines: EB FF, overlap already listed as
    its own PUSH [abs]; RET body. JMP is CALL_FN; the trampoline is a jump."""
    import struct
    jmp, slot, dest = 0x00401000, 0x00401200, 0x00401030
    overlap = jmp + 1
    img = synthetic_image({
        jmp: bytes.fromhex("ebff35") + struct.pack("<I", slot) + b"\xc3",
        slot: struct.pack("<I", dest),
        dest: b"\xc3",
    }, size=0x3000)
    text_jmp, tr = _fn(img, jmp, {jmp, overlap, dest}, listed={overlap, dest})
    text_ov, _ = _fn(img, overlap, {jmp, overlap, dest}, listed={jmp, dest})
    assert "CALL_FN(00401001); return;" in text_jmp
    assert tr.stats["_tailcall"] == 1
    assert "recomp_jump" in text_ov
    assert "CALL_FN(00401030)" not in text_ov


def test_same_body_jmp_is_goto_not_tailcall():
    start = 0x00401000
    img = synthetic_image({start: bytes.fromhex("40 eb00 c3")})  # INC EAX; JMP +0; RET
    text, tr = _fn(img, start, {start})
    assert "goto L_00401003;" in text or "goto L_00401002;" in text or "L_00401003" in text
    assert tr.stats["_tailcall"] == 0


@pytest.fixture(scope="module")
def built():
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang is not on PATH")
    if platform.system() == "Windows":
        pytest.skip("harness is POSIX")
    jmp, dest, chain = 0x0D012000, 0x0D012020, 0x0D012040
    slot = 0x0D012080
    img = synthetic_image({
        jmp: bytes.fromhex("e9") + struct.pack("<i", dest - (jmp + 5)),
        dest: bytes.fromhex("b8ddccbbaa c3"),
        chain: bytes.fromhex("e9") + struct.pack("<i", dest - (chain + 5)),
        slot: struct.pack("<I", MAGIC_RET),
    }, base=CODE_BASE, size=CODE_SIZE)
    texts = []
    addrs = {jmp, dest, chain}
    for start in (jmp, dest, chain):
        text, _tr = _fn(img, start, addrs, listed=addrs - {start})
        if start != dest:
            assert "CALL_FN(" in text
        texts.append("/* %08x */\n%s\n" % (start, text))
    work = tempfile.mkdtemp(prefix="tailcall-")
    with open(os.path.join(work, "synth.c"), "w") as fh:
        protos = "\n".join("void fn_%08x(X86 *c);" % a for a in sorted(addrs))
        fh.write('#include "x86.h"\n'
                 "#define FN_CAT_(a, b) a##b\n#define FN_CAT(a, b) FN_CAT_(a, b)\n"
                 "#define FN(a) FN_CAT(fn_, a)\n"
                 "#define CALL_FN(a) do { FN(a)(c); } while (0)\n"
                 + protos + "\n\n" + "\n".join(texts))
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write('#include "x86.h"\n')
        for addr in sorted(addrs):
            fh.write("void fn_%08x(X86 *c);\n" % addr)
        fh.write("int recomp_is_call_return(uint32_t target) { return target == 0x%08xu; }\n" % MAGIC_RET)
        fh.write("int32_t recomp_index_of(uint32_t target) { switch (target) {\n")
        for i, addr in enumerate(sorted(addrs)):
            fh.write("case 0x%08xu: return %d;\n" % (addr, i))
        fh.write("default: return -1; } }\n")
        fh.write("void recomp_call(X86 *c, uint32_t target) {\n    switch (target) {\n")
        for addr in sorted(addrs):
            fh.write("    case 0x%08xu: fn_%08x(c); return;\n" % (addr, addr))
        fh.write("    default: recomp_unknown_call(c, target); c->eip = rd32(c->r[R_ESP]); c->r[R_ESP] += 4;\n"
                 "    }\n}\n")
        fh.write("void recomp_jump(X86 *c, uint32_t target) {\n"
                 "    if (target == 0x%08xu) { c->eip = target; return; }\n"
                 "    recomp_call(c, target);\n}\n" % MAGIC_RET)
    lib = os.path.join(work, "libtail" + (".dylib" if platform.system() == "Darwin" else ".so"))
    subprocess.check_call([clang, "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Wno-unused",
                           "-fPIC", "-shared", "-I", os.path.join(ROOT, "runtime"),
                           os.path.join(HERE, "harness.c"), os.path.join(work, "synth.c"),
                           os.path.join(work, "table.c"), "-o", lib])
    libh = C.CDLL(lib)
    libh.harness_run.argtypes = [C.c_uint32, C.POINTER(X86)]
    libh.harness_eflags.restype = C.c_uint32
    libh.harness_eflags.argtypes = [C.POINTER(X86)]
    libh.harness_mem.restype = C.POINTER(C.c_uint8)
    return libh, jmp, dest, chain


def _run_guest(lib, start, writes):
    """Run Unicorn and the native translation from `start` with the same GPRs.

    `writes` is {va: bytes} placed into Unicorn code memory.
    """
    regs = {name: random.Random(0x746169).getrandbits(32) for name in REG_NAMES if name != "ESP"}
    frame = struct.pack("<I", MAGIC_RET)
    emu = Uc(UC_ARCH_X86, UC_MODE_32)
    emu.mem_map(CODE_BASE, CODE_SIZE)
    emu.mem_map(STACK_BASE, STACK_SIZE)
    emu.mem_map(CAVE, 0x1000)
    for va, data in writes.items():
        emu.mem_write(va, data)
    emu.mem_write(ESP_INIT, frame)
    emu.reg_write(UC_X86_REG_EFLAGS, EFLAGS_MISC_INIT)
    for i, name in enumerate(REG_NAMES):
        emu.reg_write(UC_REGS[i], ESP_INIT if name == "ESP" else regs[name])
    emu.emu_start(start, MAGIC_RET, timeout=2 * 1000 * 1000, count=100000)

    mem = lib.harness_mem()
    C.memset(C.addressof(mem.contents) + STACK_BASE, 0, STACK_SIZE)
    C.memmove(C.addressof(mem.contents) + ESP_INIT, frame, len(frame))
    c = X86()
    for i, name in enumerate(REG_NAMES):
        c.r[i] = ESP_INIT if name == "ESP" else regs[name]
    c.eflags_misc = EFLAGS_MISC_INIT
    lib.harness_run(start, C.byref(c))
    problems = []
    for i, name in enumerate(REG_NAMES):
        u = emu.reg_read(UC_REGS[i])
        if c.r[i] != u:
            problems.append("%s native %08x unicorn %08x" % (name, c.r[i], u))
    nf = lib.harness_eflags(C.byref(c)) & EFLAGS_COMPARED
    uf = emu.reg_read(UC_X86_REG_EFLAGS) & EFLAGS_COMPARED
    if nf != uf:
        problems.append("EFLAGS native %03x unicorn %03x" % (nf, uf))
    assert not problems, "\n".join(problems)


def test_cross_function_jmp_then_ret_matches_unicorn(built):
    """Block-level: JMP other-fn; that fn MOV EAX,imm; RET. Guest ESP net is
    the original return pop; EAX is set; CALL_FN left no extra guest slot."""
    lib, jmp, dest, _chain = built
    _run_guest(lib, jmp, {
        jmp: bytes.fromhex("e9") + struct.pack("<i", dest - (jmp + 5)),
        dest: bytes.fromhex("b8ddccbbaa c3"),
    })


def test_tailcall_chain_matches_unicorn(built):
    """JMP dest from a second entry; dest MOV EAX,imm; RET."""
    lib, _jmp, dest, chain = built
    _run_guest(lib, chain, {
        chain: bytes.fromhex("e9") + struct.pack("<i", dest - (chain + 5)),
        dest: bytes.fromhex("b8ddccbbaa c3"),
    })


def test_recover_continues_past_les_fword():
    """Fail-before: unknown operand width 6 stopped recover at LES.
    Parsing fword keeps the RET. LES itself stays unmodelled until a
    Unicorn-backed segment-load test exists (protected-mode ES load)."""
    start = 0x00401000
    img = synthetic_image({start: bytes.fromhex("c401 c3")})
    body = img.recover(start, set())
    assert [ins.mnem for ins in body] == ["LES", "RET"]
    assert img.recover_errors == []
    opts = _EmitOpts()
    opts.allow_unmodelled = "fword LES fixture"
    fn = T.Function(start, "fn", body[-1].addr + 1 - start, body)
    fn.measure(img)
    tr = T.Translator(img, {start}, opts)
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "recomp_unmodelled" in text


def test_scarface_008e10c6_push_imm_ret_is_a_jump():
    """Original ole32 thunk from the pinned PE: PUSH EBP; PUSH 0x8e1370; RET."""
    exe = os.path.normpath(os.path.join(os.path.dirname(ROOT), "original", "retail", "scarface.exe"))
    if not os.path.isfile(exe):
        pytest.skip("pinned scarface.exe not at original/retail/scarface.exe")
    img = T.Image(exe)
    start, dest = 0x008E10C6, 0x008E1370
    insns = img.recover(start, set())
    assert [i.mnem for i in insns[:3]] == ["PUSH", "PUSH", "RET"]
    fn = T.Function(start, "thunk", insns[-1].addr + 1 - start, insns)
    fn.measure(img)
    tr = T.Translator(img, {start, dest}, _EmitOpts())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "recomp_jump(c, 0x8e1370" in text or "recomp_jump(c, 0x008e1370" in text
    assert "CALL_FN(008e1370)" not in text
