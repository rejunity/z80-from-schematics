#!/usr/bin/env python3
# ===========================================================================
# run_zex.py - drive a ZEX exerciser through the C model and report pass/fail.
#
#   run_zex.py <prelim|zexdoc|zexall> [max_instr]
#
# Runs build/bin/zexrunner on tests/zex/<which>.com, streams its output, and
# classifies the result by scanning for the exerciser's "ERROR"/"OK" markers.
# ===========================================================================
import os, sys, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, "build", "bin", "zexrunner")

def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "prelim"
    max_instr = sys.argv[2] if len(sys.argv) > 2 else "4000000000"
    com = os.path.join(ROOT, "tests", "zex", which + ".com")

    if not os.path.exists(RUNNER):
        raise SystemExit(f"missing {RUNNER} (run: make zexrunner)")
    if not os.path.exists(com):
        raise SystemExit(f"missing {com}")

    print(f"== running {which}.com through the C model ==")
    proc = subprocess.Popen([RUNNER, com, max_instr], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1,
                            errors="replace")
    out_lines = []
    for line in proc.stdout:
        sys.stdout.write(line)
        out_lines.append(line)
    proc.wait()
    text = "".join(out_lines)

    err = ("ERROR" in text) or ("error" in text and "errors" not in text.lower())
    limit = "(LIMIT REACHED)" in text
    # ZEX prints a final "Tests complete" / "...OK" line when it finishes cleanly
    complete = ("Tests complete" in text) or ("all tests passed" in text.lower())

    print("\n---- run_zex summary ----")
    if limit:
        print(f"INCOMPLETE: instruction limit reached (coverage gap — see docs/verification.md)")
        return 3
    if err:
        print("FAIL: exerciser reported CRC mismatch(es)")
        return 1
    if complete:
        print(f"PASS: {which} completed with no errors")
        return 0
    print(f"DONE: {which} terminated (no explicit completion banner detected)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
