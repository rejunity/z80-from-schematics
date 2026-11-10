#!/usr/bin/env python3
"""test_nix_retry.py - scripts/nix_retry.sh, with a stand-in for nix.

ws3's nightly CI of 2026-11-09 (ws3 run 42555038041) failed before it built
anything: Nix lost the connection to cache.nixos.org at the end of a NAR,
resumed it from its full length and got

  error: unable to download 'https://cache.nixos.org/nar/1kz8...nar.zst': HTTP error 416
  error: path '/nix/store/qxaq...-gcc-15.2.0' is required, but there is no substituter that can build it

(NixOS/nix#16722, NixOS/infra#1106: Nix takes the 416 for final; a fresh
download of the same path works). Checked here:
  transient  - two such failures, then the download works: run again, and
               nix_retry.sh's own stdout is the command's alone
  fallback   - the cache keeps failing: after the retries' time is up, one
               more run with fallback = true (Nix builds what it can't fetch)
  other      - a failure that is not a download is the command's, at once
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "nix_retry.sh")

FAKE = r'''#!/usr/bin/env python3
import os, sys
mode, count = sys.argv[1], sys.argv[2]
n = int(open(count).read()) + 1 if os.path.exists(count) else 1
open(count, "w").write(str(n))
fallback = "fallback = true" in os.environ.get("NIX_CONFIG", "")
open(count + ".log", "a").write(f"{n} {int(fallback)}\n")
bad = ("error: unable to download 'https://cache.nixos.org/nar/1kz8gjqcx81ym0pyrs6lar29xs04w0wy393nid49l9yqg9zbll0w.nar.zst': HTTP error 416\n"
       "error: path '/nix/store/qxaq7jz61a6zkr2mq49i0zvqip2m2jj8-gcc-15.2.0' is required, but there is no substituter that can build it\n")
if mode == "transient" and n <= 2 or mode == "fallback" and not fallback:
    sys.stderr.write(bad); sys.exit(1)
if mode == "other":
    sys.stderr.write("error: builder for '/nix/store/x-test.drv' failed with exit code 3\n"); sys.exit(3)
print("/nix/store/0000-devshell")
'''


def run(mode, tmp):
    fake, count = os.path.join(tmp, "nix"), os.path.join(tmp, mode)
    open(fake, "w").write(FAKE)
    os.chmod(fake, 0o755)
    env = dict(os.environ, NIX_RETRY_TOTAL="3", NIX_RETRY_INTERVAL="1")
    env.pop("NIX_CONFIG", None)
    p = subprocess.run(["bash", SCRIPT, fake, mode, count], capture_output=True, text=True, env=env)
    log = [l.split() for l in open(count + ".log").read().splitlines()] if os.path.exists(count + ".log") else []
    return p, log


def main():
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        p, log = run("transient", tmp)
        if p.returncode != 0 or len(log) != 3 or any(f == "1" for _, f in log):
            bad.append(f"transient: rc {p.returncode}, attempts {log} (want 0, three, none with fallback)")
        if p.stdout != "/nix/store/0000-devshell\n":
            bad.append(f"transient: stdout {p.stdout!r} (want the command's own)")
        p, log = run("fallback", tmp)
        if p.returncode != 0 or len(log) < 2 or log[-1][1] != "1" or any(f == "1" for _, f in log[:-1]):
            bad.append(f"fallback: rc {p.returncode}, attempts {log} (want 0, retries, the last alone with fallback)")
        p, log = run("other", tmp)
        if p.returncode != 3 or len(log) != 1:
            bad.append(f"other: rc {p.returncode}, attempts {log} (want 3, one)")
    for b in bad:
        print("FAIL", b)
    print("nix_retry: " + ("FAIL" if bad else "PASS (transient, fallback, other)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
