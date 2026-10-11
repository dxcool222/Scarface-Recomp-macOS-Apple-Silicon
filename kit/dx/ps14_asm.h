// ps14_asm.h - a ps_1_4 text assembler.
//
// Turns the assembly language Scarface passes to D3DXAssembleShader into the
// Direct3D 9 DWORD token stream the existing decoder (d3d9_shader.h) reads.
// ps_1_4 only. A source that uses any other profile, or a modifier this
// assembler does not implement, fails; the caller must not report success
// with an empty shader.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// On success, `dwords` is the complete token stream, version token through
// D3DSIO_END. On failure it is cleared and `error` explains the first problem.
bool ps14_assemble(const char *src, size_t len, std::vector<uint32_t> *dwords, std::string *error);
