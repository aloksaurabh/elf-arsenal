#!/usr/bin/env bash
# Regenerate src/third_party/sqlite3.{c,h} with the SQLite OMIT set used
# by elf-arsenal. The LEMON parser, keyword hash, and opcode tables are
# all rebuilt with the same defines so parser-tied OMITs (TRIGGER, VIEW,
# CTE, WINDOWFUNC, VACUUM, ATTACH, VIRTUALTABLE, ...) actually take
# effect. Requires: gcc, tclsh8.6, curl, unzip.
#
# Keep the OPTS list here in sync with SQLITE_DEFS in the Makefile.

set -euo pipefail

VERSION=3460100
URL="https://www.sqlite.org/2024/sqlite-src-${VERSION}.zip"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO=$(cd "$(dirname "$0")/.." && pwd)

# Keep on one line — sqlite's main.mk passes $(OPTS) verbatim to gcc
# and to lemon, so embedded newlines break the invocation.
# NOTE: TRIGGER / VIEW / AUTOINCREMENT / FOREIGN_KEY are intentionally
# kept ON. sqlite re-parses every CREATE TABLE/INDEX/TRIGGER/VIEW from
# sqlite_master when opening a DB, and Sony's app.db schema uses
# AUTOINCREMENT and FOREIGN KEY clauses — dropping those parsers fails
# the open with SQLITE_ERROR and every query returns 500.
OPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_INCRBLOB -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOCALTIME -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_DESERIALIZE -DSQLITE_OMIT_LOOKASIDE -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_AUTOMATIC_INDEX -DSQLITE_OMIT_CTE -DSQLITE_OMIT_WINDOWFUNC -DSQLITE_OMIT_VACUUM -DSQLITE_OMIT_VIRTUALTABLE -DSQLITE_OMIT_REINDEX -DSQLITE_OMIT_EXPLAIN -DSQLITE_OMIT_ALTERTABLE -DSQLITE_OMIT_ANALYZE -DSQLITE_OMIT_DATETIME_FUNCS -DSQLITE_OMIT_JSON"

cd "$WORK"
echo "[regen-sqlite] downloading $URL"
curl -fsSL "$URL" -o src.zip
unzip -q src.zip
cd "sqlite-src-${VERSION}"

echo "[regen-sqlite] configuring"
./configure --disable-tcl --disable-readline --quiet

echo "[regen-sqlite] building amalgamation with OMITs"
OPTS="$OPTS" make sqlite3.c >/dev/null

echo "[regen-sqlite] installing into $REPO/src/third_party/"
cp sqlite3.c sqlite3.h "$REPO/src/third_party/"
echo "[regen-sqlite] done. Lines: $(wc -l < "$REPO/src/third_party/sqlite3.c") sqlite3.c"
