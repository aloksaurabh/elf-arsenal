#!/usr/bin/env bash
#
# Build a comprehensive elf-arsenal offline pack zip.
#
# Run on a machine with internet access (NOT the PS5). Output zip lands at
# dist/elf-arsenal-offline-pack-<date>.zip and is structured so its
# contents extract directly to /data/ on the console — every file lands
# in its picker's install path with no manual moving required.
#
# FTP the resulting zip to /data/elf-arsenal-offline-pack.zip on the
# console and tap Settings → 🔌 Offline pack → Extract now.

set -eu
set -o pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATE="$(date +%Y-%m-%d)"
DIST="$ROOT/dist"
STAGE="$DIST/stage-$DATE-$$"
ZIP_OUT="$DIST/elf-arsenal-offline-pack-$DATE.zip"

# Sweep stale stage dirs (any prior aborted run) and stale tmpfs leftovers
# before claiming fresh space.
for d in "$DIST"/stage-* /tmp/ea-offpack.*; do
  [ -d "$d" ] && rm -rf "$d"
done
rm -f "$ZIP_OUT"
mkdir -p "$DIST"

mkdir -p "$DIST"
mkdir -p "$STAGE/kstuff.elf.versions"
mkdir -p "$STAGE/shadowmount"
mkdir -p "$STAGE/elf-arsenal/dl"
mkdir -p "$STAGE/elf-arsenal/pkgs"
mkdir -p "$STAGE/elf-arsenal/cheats/repos"
mkdir -p "$STAGE/elf-arsenal/offline-pack/kstuff"
mkdir -p "$STAGE/elf-arsenal/offline-pack/smp"
mkdir -p "$STAGE/elf-arsenal/offline-pack/linux"
mkdir -p "$STAGE/ps5_autoloader"

UA="elf-arsenal-pack-builder/1.0"
CURL=(curl -fsSL --connect-timeout 10 --max-time 600 -A "$UA")

say() { printf '\033[1;36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*"; }

############################################################
# Helper: fetch latest release JSON from GitHub
############################################################
gh_json() {
  local repo="$1"
  "${CURL[@]}" "https://api.github.com/repos/$repo/releases/latest"
}

gh_asset_url() {
  local json="$1" pat="$2"
  printf '%s' "$json" | python3 -c "
import sys, json, re
j = json.load(sys.stdin)
pat = re.compile(sys.argv[1])
for a in j.get('assets', []):
    if pat.search(a.get('name','')):
        print(a['browser_download_url'])
        sys.exit(0)
" "$pat"
}

gh_tag() {
  printf '%s' "$1" | python3 -c "import sys,json; print(json.load(sys.stdin).get('tag_name',''))"
}

############################################################
# Pack manifest
############################################################
MANIFEST_TOOLS='{}'

add_version() {
  # tool, tag, asset, publishedAt
  MANIFEST_TOOLS=$(python3 -c "
import json, sys
m = json.loads('''$MANIFEST_TOOLS''')
tool, tag, asset, pub = sys.argv[1:5]
m.setdefault(tool, {'versions': []})
m[tool]['versions'].append({'tag': tag, 'asset': asset, 'publishedAt': pub})
print(json.dumps(m))
" "$1" "$2" "$3" "$4")
}

############################################################
# kstuff — three sources
############################################################
say "Fetching kstuff EchoStretch latest"
JSON="$(gh_json EchoStretch/kstuff-lite)"
TAG="$(gh_tag "$JSON")"
URL="$(gh_asset_url "$JSON" '^kstuff\.elf$' || true)"
[ -z "$URL" ] && URL="$(gh_asset_url "$JSON" '\.elf$')"
if [ -n "$URL" ]; then
  DEST="$STAGE/elf-arsenal/offline-pack/kstuff/echostretch-$TAG/kstuff.elf"
  mkdir -p "$(dirname "$DEST")"
  "${CURL[@]}" -o "$DEST" "$URL"
  cp "$DEST" "$STAGE/kstuff.elf"
  add_version kstuff "echostretch-$TAG" kstuff.elf "$DATE"
else
  warn "no .elf asset on EchoStretch latest"
fi

say "Fetching kstuff drakmor latest"
JSON="$(gh_json drakmor/kstuff-lite)" || warn "drakmor fetch failed"
if [ -n "$JSON" ]; then
  TAG="$(gh_tag "$JSON")"
  URL="$(gh_asset_url "$JSON" '^kstuff\.elf$' || true)"
  [ -z "$URL" ] && URL="$(gh_asset_url "$JSON" '\.elf$')"
  if [ -n "$URL" ]; then
    DEST="$STAGE/elf-arsenal/offline-pack/kstuff/drakmor-$TAG/kstuff.elf"
    mkdir -p "$(dirname "$DEST")"
    "${CURL[@]}" -o "$DEST" "$URL"
    add_version kstuff "drakmor-$TAG" kstuff.elf "$DATE"
  fi
fi

say "Fetching kstuff-lowfw v1.0.3 (direct)"
DEST="$STAGE/elf-arsenal/offline-pack/kstuff/lowfw-v1.0.3/kstuff.elf"
mkdir -p "$(dirname "$DEST")"
"${CURL[@]}" -o "$DEST" \
  "https://git.etawen.dev/soniciso/elf-arsenal/raw/branch/main/payloads/kstuff-lowfw.elf" \
  && add_version kstuff "lowfw-v1.0.3" kstuff.elf "$DATE" \
  || warn "kstuff-lowfw fetch failed"

############################################################
# ShadowMountPlus — release may ship as .elf directly OR as a .zip
# containing shadowmountplus.elf. Try .elf first, fall back to .zip.
############################################################
say "Fetching ShadowMountPlus latest"
JSON="$(gh_json drakmor/ShadowMountPlus)"
TAG="$(gh_tag "$JSON")"
DEST="$STAGE/elf-arsenal/offline-pack/smp/$TAG/shadowmountplus.elf"
mkdir -p "$(dirname "$DEST")"

URL="$(gh_asset_url "$JSON" '^shadowmountplus\.elf$' || true)"
[ -z "$URL" ] && URL="$(gh_asset_url "$JSON" '\.elf$' || true)"
if [ -n "$URL" ]; then
  "${CURL[@]}" -o "$DEST" "$URL"
else
  ZIP_URL="$(gh_asset_url "$JSON" '\.zip$' || true)"
  if [ -n "$ZIP_URL" ]; then
    TMPZIP="$(dirname "$DEST")/_smp.zip"
    "${CURL[@]}" -o "$TMPZIP" "$ZIP_URL"
    (cd "$(dirname "$DEST")" && unzip -joq "$TMPZIP" '*shadowmountplus.elf' 2>/dev/null \
      || unzip -joq "$TMPZIP" '*.elf' 2>/dev/null)
    rm -f "$TMPZIP"
    # rename any extracted .elf to canonical name if needed
    if [ ! -f "$DEST" ]; then
      first_elf="$(find "$(dirname "$DEST")" -maxdepth 1 -name '*.elf' | head -1)"
      [ -n "$first_elf" ] && mv "$first_elf" "$DEST"
    fi
  fi
fi
if [ -s "$DEST" ]; then
  cp "$DEST" "$STAGE/shadowmount/shadowmountplus.elf"
  add_version smp "$TAG" shadowmountplus.elf "$DATE"
else
  warn "SMP fetch produced no .elf for tag $TAG"
fi

############################################################
# PS5 Linux loader
############################################################
say "Fetching PS5 Linux loader latest"
JSON="$(gh_json ps5-linux/ps5-linux-loader)"
TAG="$(gh_tag "$JSON")"
URL="$(gh_asset_url "$JSON" '\.elf$')"
if [ -n "$URL" ]; then
  DEST="$STAGE/elf-arsenal/offline-pack/linux/$TAG/ps5-linux-loader.elf"
  mkdir -p "$(dirname "$DEST")"
  "${CURL[@]}" -o "$DEST" "$URL"
  cp "$DEST" "$STAGE/elf-arsenal/dl/ps5-linux-loader.elf"
  add_version linux "$TAG" ps5-linux-loader.elf "$DATE"
fi

############################################################
# Y2JB autoloader payloads (elf-arsenal + sonic-loader-no-etahen)
############################################################
say "Fetching Y2JB autoloader release (git.etawen.dev)"
Y2JB_JSON="$(${CURL[@]} 'https://git.etawen.dev/api/v1/repos/soniciso/elf-arsenal/releases/latest' || true)"
if [ -n "$Y2JB_JSON" ]; then
  python3 -c "
import json, sys, urllib.request, os
j = json.loads(sys.stdin.read())
out = sys.argv[1]
for a in j.get('assets', []):
    n = a.get('name','')
    u = a.get('browser_download_url') or a.get('url','')
    if not u: continue
    if n in ('elf-arsenal.elf', 'sonic-loader-no-etahen.elf'):
        dest = os.path.join(out, n)
        print('  fetching', n, '->', dest)
        urllib.request.urlretrieve(u, dest)
" "$STAGE/ps5_autoloader" <<< "$Y2JB_JSON" || warn "y2jb asset fetch hit an error"
fi

############################################################
# Elf Arsenal home-screen tile pkg
############################################################
say "Fetching Elf Arsenal tile pkg"
"${CURL[@]}" -o "$STAGE/elf-arsenal/pkgs/elf-arsenal-tile.pkg" \
  "https://git.etawen.dev/soniciso/elf-arsenal/raw/branch/main/payloads/elf-arsenal-tile.pkg" \
  || warn "tile pkg fetch failed"

############################################################
# Homebrew app zips (18) from ps5-payload-dev/websrv latest
############################################################
say "Fetching homebrew app zips (~18)"
HB_BASE="https://github.com/ps5-payload-dev/websrv/releases/latest/download"
HB_LIST=(
  OffAct.zip
  PKGInstall.zip
)
for hb in "${HB_LIST[@]}"; do
  printf '  · %-22s ' "$hb"
  if "${CURL[@]}" -o "$STAGE/elf-arsenal/dl/$hb" "$HB_BASE/$hb"; then
    printf 'ok (%s)\n' "$(stat -c%s "$STAGE/elf-arsenal/dl/$hb" | numfmt --to=iec 2>/dev/null || stat -c%s "$STAGE/elf-arsenal/dl/$hb")"
  else
    printf 'FAIL\n'
    rm -f "$STAGE/elf-arsenal/dl/$hb"
  fi
done

############################################################
# Cheat repo zips
############################################################
say "Fetching cheat repo zips"
declare -A CHEAT_REPOS=(
  [etaHEN_PS5_Cheats]="https://codeload.github.com/etaHEN/PS5_Cheats/zip/refs/heads/main"
  [GoldHEN_Cheat_Repository]="https://codeload.github.com/GoldHEN/GoldHEN_Cheat_Repository/zip/refs/heads/main"
  [TeeKay87_HEN-Cheats-Collection]="https://codeload.github.com/TeeKay87/HEN-Cheats-Collection/zip/refs/heads/master"
)
for name in "${!CHEAT_REPOS[@]}"; do
  printf '  · %-32s ' "$name"
  if "${CURL[@]}" -o "$STAGE/elf-arsenal/cheats/repos/$name.zip" "${CHEAT_REPOS[$name]}"; then
    printf 'ok\n'
  else
    printf 'FAIL\n'
    rm -f "$STAGE/elf-arsenal/cheats/repos/$name.zip"
  fi
done

############################################################
# manifest.json
############################################################
say "Writing manifest"
python3 -c "
import json, sys
m = {
  'schema': 1,
  'builtAt': sys.argv[1],
  'tools': json.loads(sys.argv[2]),
}
with open(sys.argv[3], 'w') as f:
  json.dump(m, f, indent=2)
" "$DATE" "$MANIFEST_TOOLS" "$STAGE/elf-arsenal/offline-pack/manifest.json"

############################################################
# Zip it
############################################################
say "Zipping → $ZIP_OUT"
rm -f "$ZIP_OUT"
( cd "$STAGE" && zip -qr "$ZIP_OUT" . )

say "Pack tree summary"
( cd "$STAGE" && find . -type f -printf '%-12s %p\n' | sort -rn | head -20 )
echo
echo "Pack size:   $(stat -c%s "$ZIP_OUT" | numfmt --to=iec 2>/dev/null || stat -c%s "$ZIP_OUT")"
echo "Pack md5:    $(md5sum "$ZIP_OUT" | awk '{print $1}')"
echo "Stage dir:   $STAGE  (delete when done)"
echo
echo "Next steps:"
echo "  1) FTP the zip to /data/elf-arsenal-offline-pack.zip on the console"
echo "  2) Open Elf Arsenal → Settings → 🔌 Offline pack → Extract now"
echo "  3) Flip Offline mode ON so pickers serve from the local pack"
