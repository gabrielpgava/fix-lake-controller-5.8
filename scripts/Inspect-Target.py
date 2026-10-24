"""Read-only, reproducible PE inventory; never loads or executes the target.

Imports/exports use pefile. Optional focused assembly uses Capstone, not REA.
Output can contain proprietary strings/code: keep it in ignored private/.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

import pefile

KNOWN_CONTOUR = "bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283"
TARGETS = ("Lake Controller.exe", "Contour.dll", "StdGraphics.dll", "DaylightGraphics.dll")
SNIPPETS = (
    ("command_line_nonfullscreen", 0x2E69E, 0x4A),
    ("command_line_windowed", 0x2E835, 0x18),
    ("main_class_registration", 0x2ED37, 0x70),
    ("popup_client_creation", 0x302A1, 0x78),
    ("native_parent_resize", 0x20120, 0x15B),
    ("legacy_focus_bounce", 0x20BC6, 0x11),
    ("activation_guard", 0x20C5D, 0x44),
    ("cooperative_level", 0x368D60, 0x80),
    ("exclusive_display_mode", 0x368E20, 0x93),
    ("framework_init", 0x3695B0, 0xA0),
    ("native_windowed_selection", 0x48B32, 0x7E),
    ("plugin_factory", 0x75290, 0x60),
    ("plugin_vtable_selection", 0x75372, 6),
    ("native_plugin_show", 0x75860, 0x2E),
)


def inspect(path, assembly):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    result = {
        "record_id": "local-pe-" + digest,
        "tool": {"name": "pefile", "version": pefile.__version__},
        "file": path.name,
        "sha256": digest,
        "bytes": len(data),
        "machine": hex(pe.FILE_HEADER.Machine),
        "image_base": hex(base),
        "imports": [],
        "delay_imports": [],
        "selected_exports": [],
        "selected_strings": [],
        "assembly": [],
        "message_dispatch": [],
        "limitations": [
            "Static PE facts are not observations of calls made at runtime.",
            "Normal/delay imports do not cover all GetProcAddress or COM calls.",
            "Printable strings and linear assembly do not recover full control flow.",
        ],
    }
    for attr, key in (("DIRECTORY_ENTRY_IMPORT", "imports"), ("DIRECTORY_ENTRY_DELAY_IMPORT", "delay_imports")):
        for descriptor in getattr(pe, attr, []):
            result[key].append({
                "module": descriptor.dll.decode("ascii", errors="replace"),
                "symbols": [{
                    "name": symbol.name.decode("ascii", errors="replace") if symbol.name else None,
                    "ordinal": symbol.ordinal,
                    "iat_va": hex(symbol.address),
                    "iat_rva": hex(symbol.address - base),
                } for symbol in descriptor.imports],
            })
    exports = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    if exports:
        for symbol in exports.symbols:
            name = (symbol.name or b"").decode("ascii", errors="replace")
            if any(part in name for part in ("Direct3D", "DirectDraw", "MkCreatePlugin")):
                result["selected_exports"].append({"name": name, "rva": hex(symbol.address)})
    for match in re.finditer(rb"[ -~]{4,}", data):
        if match.group() in (b"windowed", b"nonfullscreen", b"fullscreen", b"ContourParent",
                             b"Lake Controller", b"Lake Controller 5.8 Build 007"):
            rva = pe.get_rva_from_offset(match.start())
            result["selected_strings"].append({
                "text": match.group().decode("ascii"), "file_offset": hex(match.start()),
                "rva": hex(rva), "preferred_va": hex(base + rva),
            })
    if assembly and path.name == "Contour.dll":
        if digest != KNOWN_CONTOUR:
            result["limitations"].append("Unrecognized hash: build-specific disassembly ranges NOT applied.")
        else:
            import capstone

            engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
            result["assembly_tool"] = {"name": "Capstone", "version": capstone.__version__}
            native_show = struct.unpack("<I", pe.get_data(0x4696C8 + 0x24, 4))[0]
            native_start_index = pe.get_data(0x24F58 + 0x8032 - 0x8003, 1)[0]
            native_start_handler = struct.unpack("<I", pe.get_data(0x24E2C + 4 * native_start_index, 4))[0]
            result["native_show_contract"] = {
                "vtable_rva": "0x4696c8", "slot": "0x24", "show_method_rva": hex(native_show - base),
                "posted_message": "0x8032", "message_handler_rva": hex(native_start_handler - base),
                "coverage": "build-specific static tables; not an observed runtime call trace",
            }
            # Two-level switch observed at RVA 0x20a33/0x20a3a. This is a
            # bounded static table decode, not a whole-program CFG recovery.
            for message in range(1, 0x87):
                index = pe.get_data(0x24C8C + message - 1, 1)[0]
                destination = struct.unpack("<I", pe.get_data(0x24C60 + 4 * index, 4))[0]
                if destination != base + 0x24C36:
                    result["message_dispatch"].append({"message": hex(message), "target_rva": hex(destination - base)})
            for name, rva, size in SNIPPETS:
                instructions = list(engine.disasm(pe.get_data(rva, size), base + rva))
                result["assembly"].append({
                    "name": name, "rva": hex(rva), "file_offset": hex(pe.get_offset_from_rva(rva)),
                    "requested_bytes": size, "decoded_bytes": sum(i.size for i in instructions),
                    "instructions": [{"va": hex(i.address), "mnemonic": i.mnemonic, "operands": i.op_str}
                                     for i in instructions],
                })
    pe.close()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("private/inspection.json"))
    parser.add_argument("--disassemble", action="store_true")
    options = parser.parse_args()
    results = [inspect(options.app_dir / name, options.disassemble) for name in TARGETS]
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps({"scope": "offline-only", "artifacts": results}, indent=2), encoding="utf-8")
    for result in results:
        print(f"{result['file']}: machine={result['machine']} sha256={result['sha256']}")
        print("  imports:", ", ".join(d["module"] for d in result["imports"]))
    print("Private report:", options.output.resolve())


if __name__ == "__main__":
    main()
