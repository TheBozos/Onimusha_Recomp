"""oni_overrides_list.py <runtime dir> <out.txt>: every guest address the port overrides.

Scans runtime/*.cpp for registerFunction(0x...) and writes one "override 0xADDR" line per address, and for
`case 0x...u: // native site` lines (runtime/oni_port.cpp's inline instruction handler) one "native 0xADDR" line each:
the --overrides list bozo-externals reads (BozoEngine tools/externals). An address the port registers must be an override in the
generated code, or a direct call would skip the port's function. Overlay slots (0x4ba780-0x4dca80, and em090's
0x2bf200-0x2c3740) are refused: modules share those addresses.
"""
import pathlib
import re
import sys

OVERLAY_RANGES = [(0x4BA780, 0x4DCA80), (0x2BF200, 0x2C3740)]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    runtime, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    pattern = re.compile(r"registerFunction\(\s*(0x[0-9A-Fa-f]+)")
    native_pattern = re.compile(r"case\s+(0x[0-9A-Fa-f]+)u?\s*:\s*//\s*native site")
    addresses = set()
    native = set()
    for source in sorted(runtime.glob("*.cpp")):
        text = source.read_text(encoding="utf-8")
        addresses.update(int(a, 16) for a in pattern.findall(text))
        native.update(int(a, 16) for a in native_pattern.findall(text))
    bad = [a for a in addresses | native if any(lo <= a < hi for lo, hi in OVERLAY_RANGES)]
    if bad:
        print("overrides inside an overlay slot: " + ", ".join(f"0x{a:x}" for a in sorted(bad)), file=sys.stderr)
        return 1
    out.write_text("".join(f"override 0x{a:x}\n" for a in sorted(addresses)) +
                   "".join(f"native 0x{a:x}\n" for a in sorted(native)), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
