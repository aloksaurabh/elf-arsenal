# Elf Arsenal — all-in-one PS5 payload.
#
# Bundles ftpsrv + ShadowMountPlus + a websrv-based web UI that
# can launch any title found in /system_data/priv/mms/app.db.
#
# kstuff is no longer baked in; the user installs it via Settings →
# "Install kstuff-lite + ShadowMountPlus" combo on first boot.
#
# Single build:
#   make             - elf-arsenal.elf
#
# This variant drops the etaHEN payload and the etaHEN-compatible JB
# IPC daemon (jb.c port 9028 + /download0/etahen_jailbreak watcher)
# entirely. Lapy JB Daemon (payloads/lapyjb.elf) is spawned at boot
# instead and handles the same role with no etaHEN dependency.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

VERSION_TAG := v1.6.6

EA_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

PYTHON ?= python3

BIN          := elf-arsenal.elf

SRCS := src/main.c src/websrv.c src/asset.c src/fs.c src/mime.c
SRCS += src/mdns.c src/smb.c src/appdb.c src/kmonitor.c src/cheats.c
SRCS += src/activitydb.c
SRCS += src/homebrew.c src/fan.c src/sensors.c src/config.c src/avatar.c
SRCS += src/notif_inbox.c src/dashboards.c src/translate.c src/drive_sensors.c
SRCS += src/kstuff_updater.c
SRCS += src/smp_updater.c
SRCS += src/smp_meta.c
SRCS += src/fpkg_db.c
SRCS += src/y2jb_updater.c
SRCS += src/releases.c
SRCS += src/pkgzone.c
SRCS += src/payload_registry.c
SRCS += src/plugin_loader.c
SRCS += src/xml_patches.c
SRCS += src/np.c
SRCS += src/garlic.c
SRCS += src/offact.c
SRCS += src/transfer.c
SRCS += src/jb.c
SRCS += src/dumper.c
SRCS += src/activity.c
SRCS += src/tmdb.c
SRCS += src/linux_loader.c
SRCS += src/sonic_migrate.c
SRCS += src/build_clean.c
SRCS += src/backup.c
SRCS += src/sdk_changer.c
SRCS += src/offline_pack.c
SRCS += src/ps5/sys.c src/ps5/pt.c src/ps5/elfldr.c src/ps5/hbldr.c
SRCS += src/ps5/notify.c src/ps5/http.c
SRCS += src/third_party/stb_impl.c src/third_party/cJSON.c
SRCS += src/third_party/mc4/aes.c src/third_party/mc4/base64.c
SRCS += src/third_party/mc4/mc4decrypter.c
SRCS += src/third_party/miniz.c src/zipread.c

# sqlite3.c is built separately (relaxed warnings) and linked as .o
SQLITE_OBJ := src/third_party/sqlite3.o

# Read-only SQLite profile. We use 10 sqlite3_* APIs (prepare/bind_text/
# step/column_*/finalize/reset/busy_timeout/errmsg/close) and 1 PRAGMA
# (table_info). src/third_party/sqlite3.{c,h} is a custom amalgamation
# regenerated from sqlite-src-3460100 with these same OMITs so the LEMON
# parser, keywordhash, and opcode tables are all built without the
# omitted features. Regen recipe: tools/regen-sqlite.sh
SQLITE_DEFS := -DSQLITE_THREADSAFE=2
SQLITE_DEFS += -DSQLITE_DEFAULT_MEMSTATUS=0
SQLITE_DEFS += -DSQLITE_DQS=0
SQLITE_DEFS += -DSQLITE_OMIT_DEPRECATED
SQLITE_DEFS += -DSQLITE_OMIT_LOAD_EXTENSION
SQLITE_DEFS += -DSQLITE_OMIT_AUTHORIZATION
SQLITE_DEFS += -DSQLITE_OMIT_TRACE
SQLITE_DEFS += -DSQLITE_OMIT_PROGRESS_CALLBACK
SQLITE_DEFS += -DSQLITE_OMIT_DECLTYPE
SQLITE_DEFS += -DSQLITE_OMIT_GET_TABLE
SQLITE_DEFS += -DSQLITE_OMIT_INCRBLOB
SQLITE_DEFS += -DSQLITE_OMIT_TCL_VARIABLE
SQLITE_DEFS += -DSQLITE_OMIT_SHARED_CACHE
SQLITE_DEFS += -DSQLITE_OMIT_LOCALTIME
SQLITE_DEFS += -DSQLITE_OMIT_UTF16
SQLITE_DEFS += -DSQLITE_OMIT_DESERIALIZE
SQLITE_DEFS += -DSQLITE_OMIT_LOOKASIDE
SQLITE_DEFS += -DSQLITE_OMIT_COMPLETE
SQLITE_DEFS += -DSQLITE_OMIT_AUTOMATIC_INDEX
# Schema-parsing OMITs (TRIGGER, VIEW, AUTOINCREMENT, FOREIGN_KEY) are
# intentionally NOT defined here — sqlite re-parses every CREATE statement
# from sqlite_master on DB open, and Sony's app.db uses both AUTOINCREMENT
# and FOREIGN KEY clauses. Dropping those parsers fails the open with
# SQLITE_ERROR and every query returns 500.
SQLITE_DEFS += -DSQLITE_OMIT_CTE
SQLITE_DEFS += -DSQLITE_OMIT_WINDOWFUNC
SQLITE_DEFS += -DSQLITE_OMIT_VACUUM
SQLITE_DEFS += -DSQLITE_OMIT_VIRTUALTABLE
SQLITE_DEFS += -DSQLITE_OMIT_REINDEX
SQLITE_DEFS += -DSQLITE_OMIT_EXPLAIN
SQLITE_DEFS += -DSQLITE_OMIT_ALTERTABLE
SQLITE_DEFS += -DSQLITE_OMIT_ANALYZE
SQLITE_DEFS += -DSQLITE_OMIT_DATETIME_FUNCS
SQLITE_DEFS += -DSQLITE_OMIT_JSON
SQLITE_DEFS += -DSQLITE_DEFAULT_FOREIGN_KEYS=0
SQLITE_DEFS += -DSQLITE_UNTESTABLE

CFLAGS := -Os -Wall -Werror -Isrc -Isrc/third_party
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -flto
# C payload, no exceptions / stack unwinding — drop .eh_frame tables.
CFLAGS += -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
CFLAGS += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config zlib --cflags`
CFLAGS += -DVERSION_TAG=\"$(VERSION_TAG)\"
CFLAGS += -DEA_VERSION=\"$(EA_VERSION)\"
CFLAGS += -DEA_AUTOLAUNCH_HBL
CFLAGS += -DEA_NO_ETAHEN
CFLAGS += $(SQLITE_DEFS)

LDFLAGS := -Wl,--gc-sections -flto

LDADD  := -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil -lScePad
LDADD  += -lSceSsl -lSceHttp -lSceRegMgr
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libmicrohttpd --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config microdns --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libsmb2 --libs`
LDADD  += `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config zlib --libs`

ASSETS   := $(wildcard assets/*)
GEN_SRCS := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

# Sub-payloads bundled in, gzip-compressed. Each payloads/<x> is compressed
# to gen/payloads/<x>.gz at build time and embedded via INCASSET_GZ in
# src/ps5/sys.c, then inflated on first spawn. kstuff.elf is intentionally
# absent (installed at runtime via Settings); etaHEN absent (Lapy JB takes
# its place). KEEP THIS LIST IN SYNC with the INCASSET_GZ entries in sys.c.
EMBED_PAYLOADS := payloads/ftpsrv.elf payloads/zftpd.elf
EMBED_PAYLOADS += payloads/klogsrv.elf payloads/backpork.elf
EMBED_PAYLOADS += payloads/np-restore-account.elf
EMBED_PAYLOADS += payloads/garlic-worker.elf payloads/garlic-savemgr.elf
EMBED_PAYLOADS += payloads/nanodns.elf payloads/ps5-app-dumper.elf
EMBED_PAYLOADS += payloads/dpi.elf payloads/smp_icon.png
EMBED_PAYLOADS += payloads/lapyjb.elf
EMBED_PAYLOADS += payloads/trophy-unlocker-all.elf
EMBED_PAYLOADS += payloads/trophy-unlocker-uds.elf
EMBED_PAYLOADS += payloads/trophy-unlock-now.elf
EMBED_PAYLOADS += payloads/backup-helper.elf payloads/fw_probe.elf
EMBED_PAYLOADS += payloads/sdk-changer.elf
EMBED_PAYLOADS += payloads/ps5-linux-loader.elf
EMBED_PAYLOADS += payloads/cheatrunner.elf
EMBED_PAYLOADS += payloads/fpkg-guard.elf
EMBED_PAYLOADS += payloads/ps5-fw-spoof.elf
EMBED_PAYLOADS += payloads/dpiv2.elf

EMBED_GZ := $(patsubst payloads/%,gen/payloads/%.gz,$(EMBED_PAYLOADS))

all: $(BIN)

gen:
	mkdir -p gen

gen/payloads:
	mkdir -p gen/payloads

clean:
	rm -rf $(BIN) gen $(SQLITE_OBJ)

gen/%.c: assets/% | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

# Compress each embedded payload; INCASSET_GZ in sys.c .incbin's these.
gen/payloads/%.gz: payloads/% | gen/payloads
	gzip -9 -c $< > $@

# ps5-fw-spoof: build via its CMake against our SDK, strip, stage into payloads/.
# Rebuilt when its source changes (we patch it to accept a target version argv).
payloads/ps5-fw-spoof.elf: external/ps5-fw-spoof/source/main.c
	cd external/ps5-fw-spoof && \
	  cmake -B build -DCMAKE_TOOLCHAIN_FILE=$(PS5_PAYLOAD_SDK)/toolchain/prospero.cmake \
	    -DPROSPERO=1 -DPAYLOAD_DEPLOY=/bin/true >/dev/null && \
	  cmake --build build >/dev/null
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all \
	  external/ps5-fw-spoof/build/ps5-fw-spoof.elf
	cp external/ps5-fw-spoof/build/ps5-fw-spoof.elf $@

# OMIT_* leaves a couple of variables technically unused inside sqlite3.c;
# the warnings are not real bugs, silence them only for this file.
$(SQLITE_OBJ): src/third_party/sqlite3.c
	$(CC) $(CFLAGS) -Wno-unused-variable -Wno-unused-but-set-variable \
	    -Wno-error -c -o $@ $<


CHEATRUNNER_GH_API := https://api.github.com/repos/notmaj0r/CheatRunner/releases

payloads/cheatrunner.elf:
	@url=$$(curl -fsSL '$(CHEATRUNNER_GH_API)' | \
	    $(PYTHON) -c "import sys,json; d=json.load(sys.stdin); \
	    print(next(a['browser_download_url'] for r in d \
	              for a in r.get('assets',[]) if a['name'].endswith('.elf')))"); \
	 echo "[cheatrunner] fetching $$url"; \
	 curl -fsSL "$$url" -o $@

# Rebuild DPI from source (current SDK → extends FW support beyond 10.60).
payloads/dpi.elf: payloads-src/dpi/main.c
	$(MAKE) -C payloads-src/dpi PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) install

# DPI v2 — public HTTP bridge on port 12800, forwards to Arsenal's install path.
payloads/dpiv2.elf: payloads-src/dpiv2/main.c
	$(MAKE) -C payloads-src/dpiv2 PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) install

$(BIN): $(SRCS) $(GEN_SRCS) $(SQLITE_OBJ) $(EMBED_GZ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(GEN_SRCS) $(SQLITE_OBJ) $(LDADD)
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

# Standalone FPKG guard daemon — protect, auto-snapshot, sweep orphans.
# Bundled in EMBED_PAYLOADS so Arsenal auto-spawns it at boot.
payloads/fpkg-guard.elf: src/fpkg_guard.c src/fpkg_db.c $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/fpkg_guard.c src/fpkg_db.c $(SQLITE_OBJ) \
	    -lkernel_sys -lSceSystemService
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

deploy: $(BIN)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

deploy-guard: payloads/fpkg-guard.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

.PHONY: all deploy deploy-guard clean
