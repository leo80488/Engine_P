#!/usr/bin/env python3
"""
gen_material_layout.py — emit the HLSL byte-offset table for MaterialGPUData.

WHY THIS EXISTS
---------------
`HybridReflIntersectHW.cs.hlsl` cannot declare `StructuredBuffer<MaterialGPUData>`:
its material table arrives through the shared BINDLESS array, which is declared
`ByteAddressBuffer g_Buffers[16384] : register(t0, space1)` — a RAW view. So it
indexes the table by hand, `slot * stride + fieldOffset`, with the stride and
twenty-two field offsets written as decimal literals.

Nothing connected those literals to the C++ struct. The failure mode is the worst
kind:

  * append a field  -> the STRIDE is wrong. Every material past slot 0 is read
    from the wrong place. No build error.
  * insert a field in the MIDDLE -> the stride is unchanged, so a size assert
    (and the size-only check this script replaced) passes, and every field offset
    after the insertion point is silently off by four bytes.

Both land as "the reflected NPR character's toon knobs are garbage", with no
compile error, no debug-layer message, and no runtime diagnostic — the reflection
just looks subtly wrong.

WHAT IT DOES
------------
Parses `Resource::MaterialGPUData` out of include/Resource/SystemHandles.h,
computes every member's byte offset, and writes shaders/material_layout.generated.hlsli
with `MATERIAL_GPU_DATA_STRIDE` plus one `MAT_OFF_<member>` per member.

The parse is safe to do textually because the struct is deliberately simple: it
sits inside `#pragma pack(push, 1)` and every member is a 4-byte scalar (float /
int32_t / uint32_t) or an array of them, so an offset is just "how many 4-byte
words came before me". The script REFUSES to emit anything it does not fully
understand rather than guessing.

SELF-CHECK — the reason you can trust the output
------------------------------------------------
The header already carries hand-written anchors:

    static_assert(sizeof(MaterialGPUData) == 912, ...);
    static_assert(offsetof(MaterialGPUData, nprSpecThreshold) == 768, ...);

This script recomputes both from its own parse and ABORTS on any disagreement.
So the generator cannot be quietly wrong: either it agrees with the compiler's
own assertions, or it produces nothing.

USAGE
  python tools/gen_material_layout.py            # write if the content changed
  python tools/gen_material_layout.py --check    # exit 1 on drift, write nothing
  python tools/gen_material_layout.py --stdout   # print, write nothing

The file is CHECKED IN, not generated at first use: shaders are compiled from
source at RUNTIME, so a missing header would be a boot failure rather than a
build failure. EngineCore.vcxproj regenerates it before compiling whenever
SystemHandles.h is newer, and ci_check.py fails the build on drift for anyone
who edits the struct without a full build (or without Python on PATH).
"""
from __future__ import annotations

import os
import re
import sys

REPO_DX12 = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CPP_HEADER = os.path.join(REPO_DX12, "include", "Resource", "SystemHandles.h")
OUT_HLSL = os.path.join(REPO_DX12, "shaders", "material_layout.generated.hlsli")

# Every member of MaterialGPUData is one of these, and all of them are 4 bytes
# under `#pragma pack(push, 1)`. A type not on this list is a hard error, not a
# default — see the module docstring.
SCALAR_BYTES = {"float": 4, "int32_t": 4, "uint32_t": 4, "int": 4, "uint": 4}


class LayoutError(RuntimeError):
    pass


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _constants(header: str) -> dict[str, int]:
    """`constexpr uint32_t kMaxTextureSlots = 8;` -> {'kMaxTextureSlots': 8}."""
    out: dict[str, int] = {}
    for m in re.finditer(r"constexpr\s+\w+\s+(k\w+)\s*=\s*(\d+)\s*;", header):
        out[m.group(1)] = int(m.group(2))
    return out


def parse_layout(header_text: str):
    """-> (members, total_bytes). members = [(name, offset, bytes), ...]."""
    consts = _constants(header_text)
    body = _strip_comments(header_text)

    m = re.search(r"struct\s+MaterialGPUData\s*\{", body)
    if not m:
        raise LayoutError("struct MaterialGPUData not found in SystemHandles.h")
    start = m.end()
    depth = 1
    i = start
    while i < len(body) and depth:
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
        i += 1
    if depth:
        raise LayoutError("unterminated MaterialGPUData body")
    decls = body[start:i - 1]

    members: list[tuple[str, int, int]] = []
    offset = 0
    for raw in decls.split(";"):
        decl = " ".join(raw.split())
        if not decl:
            continue
        # <type> <name>[<dim>][<dim>]
        d = re.fullmatch(r"([A-Za-z_]\w*)\s+([A-Za-z_]\w*)((?:\s*\[[^\]]+\])*)", decl)
        if not d:
            raise LayoutError(
                f"cannot parse MaterialGPUData member {decl!r} — this script "
                f"refuses to guess. Extend it, or keep the struct to plain "
                f"4-byte scalars and arrays of them.")
        ctype, name, dims = d.group(1), d.group(2), d.group(3)
        if ctype not in SCALAR_BYTES:
            raise LayoutError(
                f"member {name!r} has type {ctype!r}, which is not a known "
                f"4-byte scalar. #pragma pack(1) + a non-4-byte member would "
                f"also break the HLSL mirror's natural alignment.")
        count = 1
        for dim in re.findall(r"\[([^\]]+)\]", dims):
            dim = dim.strip()
            if dim.isdigit():
                count *= int(dim)
            elif dim in consts:
                count *= consts[dim]
            else:
                raise LayoutError(
                    f"member {name!r} has array bound {dim!r} that is not a "
                    f"literal or a `constexpr ... kName = N;` in this header.")
        size = SCALAR_BYTES[ctype] * count
        members.append((name, offset, size))
        offset += size
    if not members:
        raise LayoutError("MaterialGPUData parsed as empty")
    return members, offset


def self_check(header_text: str, members, total: int) -> None:
    """Cross-check the parse against the header's own static_asserts."""
    by_name = {n: o for n, o, _ in members}

    m = re.search(r"sizeof\(MaterialGPUData\)\s*==\s*(\d+)", header_text)
    if not m:
        raise LayoutError("SystemHandles.h has no `sizeof(MaterialGPUData) == N` "
                          "assert to check the parse against")
    asserted = int(m.group(1))
    if asserted != total:
        raise LayoutError(
            f"parse disagrees with the compiler: this script computes "
            f"sizeof(MaterialGPUData) == {total}, the header asserts {asserted}. "
            f"Refusing to emit an offset table that the build does not agree with.")

    anchors = re.findall(r"offsetof\(MaterialGPUData,\s*(\w+)\)\s*==\s*(\d+)", header_text)
    if not anchors:
        raise LayoutError("SystemHandles.h has no offsetof anchors — the parse "
                          "would be unverifiable against a REORDER")
    for name, want in anchors:
        want = int(want)
        if name not in by_name:
            raise LayoutError(f"anchor names member {name!r}, which the parse did not find")
        if by_name[name] != want:
            raise LayoutError(
                f"parse disagrees with the compiler: offsetof(MaterialGPUData, "
                f"{name}) computes as {by_name[name]}, the header asserts {want}.")


def render(members, total: int) -> str:
    width = max(len(n) for n, _, _ in members)
    lines = [
        "// material_layout.generated.hlsli — GENERATED FILE, DO NOT EDIT BY HAND.",
        "//",
        "// Byte offsets of every Resource::MaterialGPUData member, emitted from",
        "// include/Resource/SystemHandles.h by tools/gen_material_layout.py.",
        "// Regenerate with:  python tools/gen_material_layout.py",
        "//",
        "// WHO NEEDS THIS. Only a shader that reads the material table as a RAW",
        "// ByteAddressBuffer, because it cannot declare",
        "// StructuredBuffer<MaterialGPUData> — today that is exactly one shader,",
        "// HybridReflIntersectHW.cs.hlsl, whose table arrives through the shared",
        "// bindless g_Buffers[] array. Every other shader gets the struct by name",
        "// from material.hlsli and must keep doing so.",
        "//",
        "// The generator cross-checks its own parse against the static_asserts in",
        "// SystemHandles.h and refuses to emit on any disagreement, and",
        "// tools/ci_check.py fails the build if this file has drifted from the",
        "// struct. So editing MaterialGPUData is a two-file edit (the C++ struct",
        "// and the HLSL mirror in material.hlsli) and this table follows.",
        "",
        "#ifndef MATERIAL_LAYOUT_GENERATED_HLSLI",
        "#define MATERIAL_LAYOUT_GENERATED_HLSLI",
        "",
        "// Byte stride of ONE MaterialGPUData element == sizeof() of the C++ mirror.",
        f"#define MATERIAL_GPU_DATA_STRIDE {total}u",
        "",
    ]
    for name, off, size in members:
        pad = " " * (width - len(name))
        note = "" if size == 4 else f"   // {size} bytes"
        lines.append(f"#define MAT_OFF_{name}{pad} {off}u{note}")
    lines += ["", "#endif // MATERIAL_LAYOUT_GENERATED_HLSLI", ""]
    return "\n".join(lines)


def build() -> str:
    with open(CPP_HEADER, "r", encoding="utf-8", errors="replace") as fh:
        header = fh.read()
    members, total = parse_layout(header)
    self_check(header, members, total)
    return render(members, total)


def main(argv) -> int:
    # A redirected or piped stdout is cp932 here, which has no em dash, and the
    # FAIL lines below contain one (see scenario_run._utf8_console).
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass
    check_only = "--check" in argv
    to_stdout = "--stdout" in argv
    try:
        text = build()
    except LayoutError as e:
        print(f"[material-layout] FAIL — {e}")
        return 1

    if to_stdout:
        sys.stdout.write(text)
        return 0

    current = None
    if os.path.exists(OUT_HLSL):
        with open(OUT_HLSL, "r", encoding="utf-8", errors="replace") as fh:
            current = fh.read()

    if current == text:
        if not check_only:
            print("[material-layout] up to date")
        return 0

    if check_only:
        print("[material-layout] FAIL — shaders/material_layout.generated.hlsli is "
              "stale. Regenerate:  python tools/gen_material_layout.py")
        return 1

    # Write only on a real change: this file lives in shaders/, which the engine
    # hot-reload watches, so rewriting it every build would trigger a pointless
    # shader recompile on every run.
    with open(OUT_HLSL, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(f"[material-layout] wrote {os.path.relpath(OUT_HLSL, REPO_DX12)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
