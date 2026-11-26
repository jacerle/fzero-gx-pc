#!/usr/bin/env python3
"""Embed compiled SPIR-V binaries into a C++ header as uint32_t arrays."""
import pathlib
import struct
import sys

def main() -> int:
    if len(sys.argv) < 3 or len(sys.argv[2:]) % 2 != 0:
        print("Usage: embed_spv.py <out.hpp> [<var_name> <file.spv>]...", file=sys.stderr)
        return 1

    out_path = pathlib.Path(sys.argv[1])
    pairs = list(zip(sys.argv[2::2], sys.argv[3::2]))

    lines = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <span>",
        "",
        "namespace fzgx::shaders {",
        "",
    ]

    for var_name, spv_file in pairs:
        data = pathlib.Path(spv_file).read_bytes()
        if len(data) % 4 != 0:
            raise ValueError(f"SPIR-V file {spv_file} size {len(data)} is not 4-byte aligned")
        words = struct.unpack(f"<{len(data) // 4}I", data)
        lines.append(f"inline constexpr uint32_t {var_name}_data[] = {{")
        for i in range(0, len(words), 8):
            chunk = ", ".join(f"0x{w:08x}u" for w in words[i : i + 8])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append(
            f"inline constexpr std::span<const uint32_t> {var_name}{{{var_name}_data}};"
        )
        lines.append("")

    lines.append("} // namespace fzgx::shaders")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
