#!/usr/bin/env python3
# encoding: utf-8
# Copyright (C) 2024 John Törnblom
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not see
# <http://www.gnu.org/licenses/>.

import argparse
import string
import mimetypes
import zlib


tmpl_raw = string.Template('''
void asset_register(const char*, void*, unsigned long, const char*);

static unsigned char data[] = $data;

__attribute__((constructor)) static void
constructor(void) {
  asset_register("/$path", data, sizeof(data), "$mime");
}
''')


tmpl_gz = string.Template('''
void asset_register_gz(const char*, const void*, unsigned long, unsigned long, const char*);

static const unsigned char data[] = $data;

__attribute__((constructor)) static void
constructor(void) {
  asset_register_gz("/$path", data, sizeof(data), $raw_size, "$mime");
}
''')


# Worth-it threshold: only use the gz path when it saves more than this many
# bytes. Below the threshold the per-asset zlib state + decompression buffer
# overhead isn't worth a few hundred bytes of disk.
GZ_MIN_SAVING = 256


def gen_data(buf):
    yield '{\n  '
    for n, b in enumerate(buf, 1):
        yield hex(b)
        yield ', '
        if n % 16 == 0:
            yield '\n  '
    yield '\n}'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--path', default=None)
    parser.add_argument('FILE')
    args = parser.parse_args()

    if args.path is None:
        args.path = args.FILE

    with open(args.FILE, mode='rb') as f:
        raw = f.read()

    mime = mimetypes.guess_type(args.path)[0] or ''
    gz = zlib.compress(raw, 9)

    if len(raw) - len(gz) > GZ_MIN_SAVING:
        data = ''.join(gen_data(gz))
        print(tmpl_gz.substitute(data=data, path=args.path,
                                 raw_size=len(raw), mime=mime))
    else:
        data = ''.join(gen_data(raw))
        print(tmpl_raw.substitute(data=data, path=args.path, mime=mime))
