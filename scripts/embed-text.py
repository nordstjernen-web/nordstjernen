#!/usr/bin/env python3
import sys

def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: embed-text.py <src> <out> <symbol>\n")
        return 2
    src, out, sym = argv[1], argv[2], argv[3]
    with open(src, "rb") as f:
        data = f.read()
    bytes_per_line = 16
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i:i + bytes_per_line]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk))
    body = ",\n".join(lines)
    if body:
        body += ","
    with open(out, "w") as f:
        f.write("/* generated from %s — do not edit */\n" % src)
        f.write("static const char %s[] = {\n%s\n    0x00\n};\n" % (sym, body))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
