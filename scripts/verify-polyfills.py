#!/usr/bin/env python3
"""Build-time syntax + smoke verification for data/js/polyfills.js.

Usage: verify-polyfills.py <qjsc> <qjs> <polyfills.js> <verify.js> <marker_out>

Runs qjsc in compile-only mode to catch syntax errors, then runs qjs
against the verify script (with polyfills included) to catch missing
or broken API definitions. Writes a marker file on success so meson
can skip the check when nothing has changed.
"""
import os
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 6:
        sys.stderr.write(__doc__)
        return 2
    qjsc, qjs, polyfills, verify, marker = sys.argv[1:]

    with tempfile.NamedTemporaryFile(suffix=".c", delete=False) as tmp:
        bytecode_c = tmp.name
    try:
        rc = subprocess.call([qjsc, "-C", "-P", "-n", "polyfills.js",
                              "-o", bytecode_c, polyfills])
    finally:
        try:
            os.unlink(bytecode_c)
        except OSError:
            pass
    if rc != 0:
        sys.stderr.write("polyfills.js: syntax check (qjsc) failed\n")
        return rc

    rc = subprocess.call([qjs, "-I", polyfills, verify])
    if rc != 0:
        sys.stderr.write("polyfills.js: smoke check (qjs) failed\n")
        return rc

    with open(marker, "w") as out:
        out.write("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
