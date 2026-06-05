#!/usr/bin/env bash
# Sonic Loader wake-from-rest auto-redeployer (Linux / macOS).
#
# Run this on a PC / NAS / router that's always on. It polls the PS5
# elfldr port every few seconds and re-sends elf-arsenal.elf the
# moment the PS5 wakes from rest mode and the elfldr daemon is back.
#
# Usage:
#   PS5_IP=10.0.0.189 ./sonic-watchdog.sh /path/to/elf-arsenal.elf
#
# Or set defaults inline. Ctrl-C to stop.

set -euo pipefail

PS5_IP="${PS5_IP:-${1:-}}"
ELF="${ELF:-${2:-${1:-./elf-arsenal.elf}}}"
ELFLDR_PORT="${ELFLDR_PORT:-9021}"
LOADER_PORT="${LOADER_PORT:-6969}"
PROBE_INTERVAL="${PROBE_INTERVAL:-5}"
SETTLE_DELAY="${SETTLE_DELAY:-2}"

# Argument convention: if first arg looks like an IP and a second arg
# is given, use first as IP and second as ELF path. Otherwise the user
# can rely on env vars.
if [[ "${1:-}" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] && [[ -n "${2:-}" ]]; then
  PS5_IP="$1"
  ELF="$2"
fi

if [[ -z "$PS5_IP" || ! -f "$ELF" ]]; then
  cat <<EOF
Usage: PS5_IP=<ip> $0 <path/to/elf-arsenal.elf>
   or: $0 <ip> <path/to/elf-arsenal.elf>

Env knobs:
  ELFLDR_PORT      default 9021
  LOADER_PORT      default 6969  (used to confirm a successful
                                  redeploy — script only re-fires
                                  after this port closes again,
                                  which means rest mode was entered)
  PROBE_INTERVAL   default 5     (seconds between probes)
  SETTLE_DELAY     default 2     (seconds to wait between
                                  detecting elfldr and sending)
EOF
  exit 1
fi

echo "sonic-watchdog: PS5_IP=$PS5_IP  ELF=$ELF"
echo "sonic-watchdog: probing :$ELFLDR_PORT every ${PROBE_INTERVAL}s — Ctrl-C to stop"

probe() {
  # nc -z is the most portable; bash /dev/tcp is a backup.
  if command -v nc >/dev/null 2>&1; then
    nc -z -w 2 "$PS5_IP" "$1" 2>/dev/null
  else
    (exec 3<>"/dev/tcp/$PS5_IP/$1") 2>/dev/null && exec 3<&- 3>&-
  fi
}

deploy() {
  echo "sonic-watchdog: $(date '+%H:%M:%S') sending $(stat -c %s "$ELF" 2>/dev/null || stat -f %z "$ELF") bytes to $PS5_IP:$ELFLDR_PORT…"
  if cat "$ELF" | nc -q1 "$PS5_IP" "$ELFLDR_PORT" 2>/dev/null; then
    echo "sonic-watchdog: $(date '+%H:%M:%S') ✔ sent."
    return 0
  fi
  # BSD/macOS nc has -N instead of -q1.
  if cat "$ELF" | nc -N "$PS5_IP" "$ELFLDR_PORT" 2>/dev/null; then
    echo "sonic-watchdog: $(date '+%H:%M:%S') ✔ sent (nc -N)."
    return 0
  fi
  echo "sonic-watchdog: $(date '+%H:%M:%S') ✗ deploy failed (will retry)."
  return 1
}

# Track previous reachability of the loader's HTTP port so we only
# fire on a closed→open transition of elfldr (= "PS5 just woke and
# the JB chain is back"). If the loader is already up, we don't
# redeploy on every restart of the watchdog.
prev_elfldr_up=0
loader_was_up=0

while true; do
  if probe "$LOADER_PORT"; then
    loader_was_up=1
  fi

  if probe "$ELFLDR_PORT"; then
    if [[ $prev_elfldr_up -eq 0 ]]; then
      # closed → open transition. If the loader was up before this
      # transition (= console hasn't slept yet), skip — the user is
      # probably starting up, not waking. We re-fire only when we
      # observed the loader go down (rest mode).
      if [[ $loader_was_up -eq 1 ]] && probe "$LOADER_PORT"; then
        echo "sonic-watchdog: elfldr open + loader still up — first boot or no rest cycle. Skipping."
      else
        sleep "$SETTLE_DELAY"
        deploy || true
        loader_was_up=0
      fi
    fi
    prev_elfldr_up=1
  else
    if [[ $prev_elfldr_up -eq 1 ]]; then
      echo "sonic-watchdog: $(date '+%H:%M:%S') PS5 unreachable — likely rest mode."
    fi
    prev_elfldr_up=0
    loader_was_up=0
  fi

  sleep "$PROBE_INTERVAL"
done
