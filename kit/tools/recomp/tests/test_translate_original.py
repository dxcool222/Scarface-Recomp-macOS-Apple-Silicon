"""Original Scarface recovered bodies vs Unicorn.

    .venv/bin/python -m pytest -q kit/tools/recomp/tests/test_translate_original.py

These are full recovered *functions* (or recovered CALL-free bodies), not
instruction-form fixtures. A pass is guest GPR/flag/stack agreement after the
body RETs to the planted address. Import, FS, and unmapped-memory bodies are
skipped, not counted as equivalence.
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
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
REPO = os.path.dirname(ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402
from test_translate_driver import _EmitOpts  # noqa: E402
from test_translate_tailcall import X86  # noqa: E402

STACK_BASE, STACK_SIZE = 0x0EF00000, 0x00100000
ESP_INIT = 0x0EFFFF00
SCRATCH, SCRATCH_SIZE = 0x0E100000, 0x00010000
CAVE, CAVE_SIZE = 0x0DEAC000, 0x1000
MAGIC_RET = CAVE
EFLAGS_COMPARED = 0x0CC5
EFLAGS_MISC_INIT = 0x202
UC_REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
           UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
REG_NAMES = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]

EXE = os.path.join(REPO, "original", "retail", "scarface.exe")
FUNCS_TSV = os.path.join(REPO, "analysis", "decompiled", "scarface.exe", "functions.tsv")

# Ghidra-listed this-call / stdcall leaves in .rdata game code (not ASProtect).
# Each is CALL-free in the Ghidra listing. Recovered bodies are the authority.
CURATED = [
    0x00401230,  # MOV dword [ECX],0; RET
    0x00401260,  # MOV AL,[ECX+4]; RET
    0x00401290,  # MOV byte [ECX+4],0; RET
    0x00401240,  # small leaf
    0x00401250,
    0x00401220,
    0x00401190,  # struct copy; RET 4
    0x004011C0,
    0x00401200,  # uses [0x0080ac04]
    0x00401270,
    0x004012A0,
    0x004012D0,
    0x00401330,
    0x00401010,
]

ALLOWED = frozenset({
    "MOV", "PUSH", "POP", "LEA", "ADD", "SUB", "XOR", "AND", "OR", "NOT", "NEG",
    "INC", "DEC", "TEST", "CMP", "JMP", "RET", "MOVZX", "MOVSX", "SHL", "SAL",
    "SHR", "SAR", "IMUL", "CDQ", "CWDE", "LEAVE", "CLC", "STC", "CLD", "NOP",
    "AND", "OR", "TEST", "CMP",
}) | {("J" + k) for k in (
    "Z", "NZ", "E", "NE", "A", "AE", "B", "BE", "G", "GE", "L", "LE",
    "S", "NS", "O", "NO", "P", "NP", "C", "NC")}


def _page_down(a):
    return a & ~0xFFF


def _page_up(a):
    return (a + 0xFFF) & ~0xFFF


def _recoverable(img, addr):
    insns = img.recover(addr, set())
    if not insns or insns[0].addr != addr:
        return None
    if insns[-1].mnem not in ("RET",):
        return None
    if len(insns) > 80:
        return None
    for i in insns:
        if i.mnem == "CALL" or (i.mnem not in ALLOWED and i.mnem not in T.JCC):
            return None
    return insns


@pytest.fixture(scope="module")
def original():
    if not os.path.isfile(EXE):
        pytest.skip("pinned scarface.exe missing")
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang is not on PATH")
    if platform.system() == "Windows":
        pytest.skip("harness is POSIX")
    img = T.Image(EXE)
    selected = []
    for addr in CURATED:
        insns = _recoverable(img, addr)
        if insns:
            selected.append((addr, insns))
    extra = []
    seen = {a for a, _ in selected}
    if os.path.isfile(FUNCS_TSV):
        attempts = 0
        with open(FUNCS_TSV) as fh:
            next(fh, None)
            for line in fh:
                parts = line.split("\t")
                if len(parts) < 3:
                    continue
                try:
                    addr, nbytes = int(parts[0], 16), int(parts[2])
                except ValueError:
                    continue
                if nbytes < 4 or nbytes > 48 or addr in seen:
                    continue
                if addr >= 0x00860000:
                    continue  # skip ASProtect .text for this corpus
                attempts += 1
                if attempts > 250:
                    break
                insns = _recoverable(img, addr)
                if insns:
                    extra.append((addr, insns))
                    seen.add(addr)
                if len(selected) + len(extra) >= 40:
                    break
    selected.extend(extra)
    if not selected:
        pytest.skip("no CALL-free recovered original bodies")
    texts, addrs = [], set()
    for addr, insns in selected:
        fn = T.Function(addr, "orig", insns[-1].addr + 1 - addr, insns)
        fn.measure(img)
        tr = T.Translator(img, {addr}, _EmitOpts())
        tr.prepare(fn)
        text = "\n".join(tr.translate(fn))
        if "recomp_unmodelled" in text or "CALL_FN" in text:
            continue
        texts.append("/* %08x */\n%s\n" % (addr, text))
        addrs.add(addr)
    selected = [(a, i) for a, i in selected if a in addrs]
    if not selected:
        pytest.skip("no original bodies emitted without traps")
    work = tempfile.mkdtemp(prefix="orig-recomp-")
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
    lib = os.path.join(work, "liborig" + (".dylib" if platform.system() == "Darwin" else ".so"))
    subprocess.check_call([clang, "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Wno-unused",
                           "-fPIC", "-shared", "-I", os.path.join(ROOT, "runtime"),
                           os.path.join(HERE, "harness.c"), os.path.join(work, "synth.c"),
                           os.path.join(work, "table.c"), "-o", lib])
    libh = C.CDLL(lib)
    libh.harness_run.argtypes = [C.c_uint32, C.POINTER(X86)]
    libh.harness_eflags.restype = C.c_uint32
    libh.harness_eflags.argtypes = [C.POINTER(X86)]
    libh.harness_mem.restype = C.POINTER(C.c_uint8)
    return libh, img, selected


def _map_image(emu, img):
    lo = _page_down(img.base)
    hi = _page_up(img.base + img.size)
    emu.mem_map(lo, hi - lo)
    emu.mem_write(img.base, img.data)


def test_original_call_free_functions_match_unicorn(original):
    """Function-level: recovered CALL-free Scarface bodies vs Unicorn.

    Counts are the selected corpus for this run, not the game. Each case is
    one recovered function from the pinned PE.
    """
    lib, img, selected = original
    mem = lib.harness_mem()
    native_base = C.addressof(mem.contents)
    C.memmove(native_base + img.base, img.data, len(img.data))
    passed, failed, unresolved = [], [], []
    rng = random.Random(0x534346)
    for addr, insns in selected:
        regs = {name: rng.getrandbits(32) for name in REG_NAMES if name != "ESP"}
        regs["ECX"] = SCRATCH
        regs["EDX"] = SCRATCH + 0x40
        args = [rng.getrandbits(32) for _ in range(4)]
        frame = struct.pack("<I", MAGIC_RET) + struct.pack("<4I", *args)
        scratch = bytes(rng.getrandbits(8) for _ in range(0x80))
        try:
            emu = Uc(UC_ARCH_X86, UC_MODE_32)
            _map_image(emu, img)
            emu.mem_map(STACK_BASE, STACK_SIZE)
            emu.mem_map(SCRATCH, SCRATCH_SIZE)
            emu.mem_map(CAVE, CAVE_SIZE)
            emu.mem_write(ESP_INIT, frame)
            emu.mem_write(SCRATCH, scratch)
            emu.reg_write(UC_X86_REG_EFLAGS, EFLAGS_MISC_INIT)
            for i, name in enumerate(REG_NAMES):
                emu.reg_write(UC_REGS[i], ESP_INIT if name == "ESP" else regs[name])
            emu.emu_start(addr, MAGIC_RET, timeout=2 * 1000 * 1000, count=100000)
        except (UcError, Exception) as e:
            unresolved.append("%08x unicorn %s" % (addr, e))
            continue
        C.memset(native_base + STACK_BASE, 0, STACK_SIZE)
        C.memset(native_base + SCRATCH, 0, SCRATCH_SIZE)
        C.memmove(native_base + ESP_INIT, frame, len(frame))
        C.memmove(native_base + SCRATCH, scratch, len(scratch))
        c = X86()
        for i, name in enumerate(REG_NAMES):
            c.r[i] = ESP_INIT if name == "ESP" else regs[name]
        c.eflags_misc = EFLAGS_MISC_INIT
        try:
            lib.harness_run(addr, C.byref(c))
        except Exception as e:
            failed.append("%08x native %s" % (addr, e))
            continue
        problems = []
        for i, name in enumerate(REG_NAMES):
            u = emu.reg_read(UC_REGS[i])
            if c.r[i] != u:
                problems.append("%s native %08x unicorn %08x" % (name, c.r[i], u))
        nf = lib.harness_eflags(C.byref(c)) & EFLAGS_COMPARED
        uf = emu.reg_read(UC_X86_REG_EFLAGS) & EFLAGS_COMPARED
        if nf != uf:
            problems.append("EFLAGS native %03x unicorn %03x" % (nf, uf))
        nst = C.string_at(native_base + SCRATCH, 0x80)
        ust = bytes(emu.mem_read(SCRATCH, 0x80))
        if nst != ust:
            problems.append("scratch differs")
        if problems:
            failed.append("%08x (%d insns): %s" % (addr, len(insns), "; ".join(problems)))
        else:
            passed.append(addr)
    assert passed, "no original function passed: failed=%s unresolved=%s" % (failed, unresolved)
    assert not failed, "%d passed, %d failed, %d unresolved\n%s" % (
        len(passed), len(failed), len(unresolved), "\n".join(failed[:20]))


def test_original_corpus_is_reported(original, tmp_path):
    """Machine-readable counts for the audit: functions tried, not insn forms."""
    _lib, _img, selected = original
    assert len(selected) >= 8
    out = os.path.join(os.path.dirname(REPO), "scarface-recomp",
                       "build", "audit", "stages", "original_fn_corpus.json")
    # Prefer the audit stages dir when the repo layout is present.
    stages = os.path.join(REPO, "build", "audit", "stages")
    path = os.path.join(stages, "original_fn_corpus.json") if os.path.isdir(stages) else str(
        tmp_path / "original_fn_corpus.json")
    payload = {
        "level": "full_recovered_function",
        "n_selected": len(selected),
        "addrs": ["%08x" % a for a, _ in selected],
        "insn_counts": {("%08x" % a): len(ins) for a, ins in selected},
        "note": "Unicorn agreement is test_original_call_free_functions_match_unicorn",
    }
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        import json
        json.dump(payload, fh, indent=1)
    assert payload["n_selected"] == len(payload["addrs"])
