#!/usr/bin/env bash
# nix_retry.sh COMMAND... - run a command that fetches from Nix's binary
# caches, and run it again when a download failed: every 30 s for up to
# 20 minutes, then once more with fallback = true, so Nix builds what it
# still can't fetch. Any other failure is the command's, at once.
#
# cache.nixos.org now and then drops a connection at the end of a NAR; Nix
# resumes it from the full length, the cache answers HTTP 416 and Nix takes
# that for final (NixOS/nix#16722, NixOS/infra#1106; download-attempts does
# not help). A fresh download of the same path works. ws3's nightly CI of
# 2026-11-09 and 2026-11-01 failed this way. Test: scripts/test_nix_retry.py
set -u
total=${NIX_RETRY_TOTAL:-1200}
every=${NIX_RETRY_INTERVAL:-30}
download='unable to download|HTTP error [0-9]+|there is no substituter that can build it|could not be realised|some substitutes for the outputs'
log=$(mktemp)
trap 'rm -f "$log"' EXIT
start=$SECONDS
attempt=1
while :; do
    # stdout stays the command's own (nix build --print-out-paths); stderr is kept to look at
    { "$@" 2>&1 1>&3 3>&- | tee "$log" >&2; } 3>&1
    rc=${PIPESTATUS[0]}
    [ "$rc" -eq 0 ] && exit 0
    grep -qE "$download" "$log" || exit "$rc"
    (( SECONDS - start + every < total )) || break
    echo "nix_retry: attempt $attempt failed downloading from a binary cache; again in ${every} s" >&2
    sleep "$every"
    attempt=$((attempt + 1))
done
echo "nix_retry: downloads still failing after $((SECONDS - start)) s; once more, building what can't be fetched (fallback = true)" >&2
NIX_CONFIG="${NIX_CONFIG:+$NIX_CONFIG
}fallback = true" exec "$@"
