#!/usr/bin/env python3
"""Build pkg-source/sce_sys/param.sfo for the Sonic Loader tile PKG.

Reads the same fields param.json carries and emits a clean SFO with
ONLY the keys prospero-pub-cmd needs. The PSVue sample SFO we started
from had 7 SERVICE_ID_ADDCONT_ADD_* DLC slots that don't apply here;
this rebuilds without them.

deeplinkUri is intentionally absent — it's a PS5-native field that
lives in param.json only. The PS5 PKG installer reads both files.

Usage:
  python3 build_sfo.py
"""
import struct
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent / "sce_sys" / "param.sfo"

# -----------------------------------------------------------------
# field table — order matters, prospero-pub-cmd keeps keys
# alphabetical. Each entry: (KEY, FMT, MAX_LEN, VALUE)
# FMT 0x0004 = utf-8 (no null pad), 0x0204 = utf-8 null-terminated,
# 0x0404 = u32 LE.
# -----------------------------------------------------------------
FIELDS = [
    ("APP_TYPE",            0x0404, 4,   1),                  # 1 = paid app
    ("APP_VER",             0x0204, 8,   "01.00"),
    ("ATTRIBUTE",           0x0404, 4,   1),                  # mirrors param.json
    ("CATEGORY",            0x0204, 4,   "gd"),               # gd = game digital
    ("CONTENT_ID",          0x0204, 48,  "IV9999-PSPS69691_00-ELFARSENAL000001"),
    ("DISP_LOCATION_1",     0x0404, 4,   1),
    ("DISP_LOCATION_2",     0x0404, 4,   1),
    ("DOWNLOAD_DATA_SIZE",  0x0404, 4,   0),
    ("FORMAT",              0x0204, 4,   "obs"),
    ("PARENTAL_LEVEL",      0x0404, 4,   1),
    ("PUBTOOLINFO",         0x0204, 512, "creation_date=2026-05-06,c_date=20260506,c_time=0,sdk_ver=1000051,st_type=digital50,img0_l0_size=0,img0_l1_size=0,img0_pc_size=18000000"),
    ("SYSTEM_VER",          0x0404, 4,   0),
    ("TITLE",               0x0204, 128, "Elf Arsenal"),
    ("TITLE_ID",            0x0204, 12,  "PSPS69691"),
    ("VERSION",             0x0204, 8,   "01.00"),
]


def build_sfo(fields):
    """Pack FIELDS into a complete .sfo blob."""
    n = len(fields)
    # Header is 0x14 bytes; index table is 16 bytes per entry.
    header_size = 0x14
    index_size  = 16 * n

    # Build key table.
    key_blob = b""
    key_offsets = []
    for key, _, _, _ in fields:
        key_offsets.append(len(key_blob))
        key_blob += key.encode("ascii") + b"\x00"
    # Pad to 4-byte alignment.
    while len(key_blob) % 4:
        key_blob += b"\x00"

    # Build data table — each entry gets its full max_len slot,
    # zero-padded so the offsets are stable.
    data_blob = b""
    data_offsets = []
    lengths = []
    for _, fmt, max_len, val in fields:
        data_offsets.append(len(data_blob))
        if fmt == 0x0404:
            # u32 LE, max_len always 4.
            payload = struct.pack("<I", int(val))
            length = 4
        elif fmt in (0x0004, 0x0204):
            payload = val.encode("utf-8") + b"\x00"
            if len(payload) > max_len:
                raise ValueError(f"value too long for {val!r}: {len(payload)} > {max_len}")
            length = len(payload)
            payload = payload.ljust(max_len, b"\x00")
        else:
            raise ValueError(f"unknown fmt 0x{fmt:04x}")
        lengths.append(length)
        data_blob += payload

    key_table_offset  = header_size + index_size
    data_table_offset = key_table_offset + len(key_blob)

    # Header.
    out = bytearray()
    out += b"\x00PSF"                                  # magic
    out += struct.pack("<I", 0x0101)                   # version
    out += struct.pack("<I", key_table_offset)
    out += struct.pack("<I", data_table_offset)
    out += struct.pack("<I", n)

    # Index table.
    for i, (_, fmt, max_len, _) in enumerate(fields):
        out += struct.pack(
            "<HHIII",
            key_offsets[i],
            fmt,
            lengths[i],
            max_len,
            data_offsets[i],
        )

    # Key + data tables.
    out += key_blob
    out += data_blob
    return bytes(out)


def main():
    blob = build_sfo(FIELDS)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(blob)
    print(f"wrote {OUT} ({len(blob)} bytes, {len(FIELDS)} fields)")


if __name__ == "__main__":
    main()
