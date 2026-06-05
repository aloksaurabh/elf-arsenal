#!/usr/bin/env python3
"""
Prototype for the PS4-fpkg app.db repair (task 12).

Reconstructs the app.db rows a real PS4 fpkg install creates, purely from
on-disk data (param.sfo + appmeta art + pkg size) — no PSN/network. Uses the
"cid:local" concept path (conceptId=0) like homebrew / offline-installed games,
so it works after a Sony safe-mode DB rebuild.

Validated offline against a real capture:
  /tmp/app.db.before  — snapshot that ALREADY contains CUSA19279 (a cid:local
                        PS4 game) — our ground-truth template.
  /tmp/c19279.sfo     — that title's param.sfo.

Run: python3 tools/fpkg_rebuild_prototype.py
Once the STABLE fields match, the same logic is ported to C in src/smp_meta.c.

NOTE: volatile fields (timestamps, *_index, sync_index, ts= cache-busters) are
expected to differ — a real install uses "now" + running counters, which is
exactly what the C port will do. We validate the STABLE (SFO/computed/constant)
fields only.
"""
import sqlite3, json, struct, sys, os

# ---- param.sfo parser ------------------------------------------------------
def parse_sfo(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x00PSF', d[:4]
    kt, dt, n = struct.unpack_from('<III', d, 8)
    out = {}
    for i in range(n):
        ko, fmt, ln, ml, do = struct.unpack_from('<HHIII', d, 20 + i*16)
        k = d[kt+ko:].split(b'\x00', 1)[0].decode('utf-8', 'replace')
        raw = d[dt+do:dt+do+ln]
        out[k] = struct.unpack('<I', raw[:4])[0] if fmt == 0x0404 \
                 else raw.split(b'\x00', 1)[0].decode('utf-8', 'replace')
    return out

# ---- derived helpers -------------------------------------------------------
# CATEGORY -> categoryType. gd=game(61440). Extend from observed rows as needed.
CATEGORY_TYPE = {"gd": 61440, "gde": 61440, "gp": 61440, "ac": 0}
def view_category(cat):       return "app" if cat == "gde" else "game"
def hubapp_uri(tid, cat):     return ("psmediahub:main?titleId="+tid) if cat=="gde" \
                                     else ("pshome:gamehub?titleId="+tid)
# Fixed magic constants observed identically across PT (cid:scp) and CUSA19279
# (cid:local) — i.e. install-source independent.
INSTALL_VERSION   = 289074801081843713      # 0x0403000000000001
PATH_ICON0_MAGIC  = -8070450532247928832    # 0x9000000000000000 (signed)
PATH_PIC0_MAGIC   = -6917529027641081856    # 0xA000000000000000 (signed)
PRIMARY_TITLE_SORT= 132101
DISPLAY_LOCATION  = 138
METADATA_ID       = "prior:internal:0"
SORT_PRIORITY     = 100

def build_appinfo_stable(sfo, tid):
    """The non-volatile AppInfoJson fields, as {key: data}."""
    cat = sfo.get("CATEGORY", "gd")
    f = {}
    # straight SFO passthroughs
    for k in ("APP_TYPE","APP_VER","ATTRIBUTE","ATTRIBUTE2","CATEGORY",
              "CONTENT_ID","DOWNLOAD_DATA_SIZE","FORMAT","PARENTAL_LEVEL",
              "REMOTE_PLAY_KEY_ASSIGN","SYSTEM_VER","TITLE","TITLE_ID","VERSION"):
        if k in sfo: f[k] = sfo[k]
    for i in range(1, 8):
        f[f"SERVICE_ID_ADDCONT_ADD_{i}"] = int(sfo.get(f"SERVICE_ID_ADDCONT_ADD_{i}", 0) or 0)
    # computed
    f["CATEGORY_TYPE"]   = CATEGORY_TYPE.get(cat, 61440)
    f["DEEPLINK_URI"]    = f"psgm:play?id={tid}"
    f["HUBAPP_URI"]      = hubapp_uri(tid, cat)
    f["DISPLAYLOCATION"] = DISPLAY_LOCATION
    f["METADATA_ID"]     = METADATA_ID
    f["_install_version"]= INSTALL_VERSION
    f["_local_concept_id"]= f"cid:local:{tid}"
    f["_metadata_path"]  = f"/user/appmeta/{tid}"
    f["_org_path"]       = f"/user/app/{tid}"
    f["_path_icon0_info"]= PATH_ICON0_MAGIC
    f["_path_pic0_info"] = PATH_PIC0_MAGIC
    f["_primary_title_sort"] = PRIMARY_TITLE_SORT
    f["_sort_priority"]  = SORT_PRIORITY
    f["_ps_platform"]    = 1
    f["_uninstallable"]  = 1
    f["_install_sub_status"] = 1
    return f

# ---- validation against the real CUSA19279 row -----------------------------
def main():
    if not (os.path.exists("/tmp/app.db.before") and os.path.exists("/tmp/c19279.sfo")):
        print("missing /tmp/app.db.before or /tmp/c19279.sfo"); return 1
    sfo = parse_sfo("/tmp/c19279.sfo")
    tid = sfo["TITLE_ID"]
    mine = build_appinfo_stable(sfo, tid)

    db = sqlite3.connect("/tmp/app.db.before"); c = db.cursor()
    aij = c.execute("SELECT AppInfoJson FROM tbl_contentinfo WHERE titleId=?", (tid,)).fetchone()[0]
    real = {x['key']: x['data'] for x in json.loads(aij)['field_list']}

    VOLATILE = lambda k: (k.startswith('#_') or 'time' in k.lower()
                          or 'index' in k.lower() or k=='sync_index'
                          or k.startswith('_path_') and k.endswith('_time_stamp'))
    print(f"=== validating reconstructed STABLE AppInfoJson for {tid} ===")
    ok=bad=0
    for k, v in sorted(mine.items()):
        rv = real.get(k, "<MISSING IN REAL>")
        match = str(rv) == str(v)
        ok += match; bad += (not match)
        if not match:
            print(f"  MISMATCH {k}: mine={v!r}  real={rv!r}")
    # report real stable keys we didn't generate
    missing = [k for k in real if not VOLATILE(k) and k not in mine
               and not k.startswith('_') and not k.startswith('USER_DEFINED')
               and not k.startswith('DISP_LOCATION')]
    print(f"\n  matched {ok} stable fields, {bad} mismatched")
    if missing:
        print(f"  real stable keys we still need to emit: {missing}")
    db.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
