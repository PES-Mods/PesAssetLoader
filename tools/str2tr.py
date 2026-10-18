"""
str2tr.py -- dump the game's string tables (.str files) into one .tr file for PesAssetLoader's text loader.

    python str2tr.py [folder] [--out text.tr] [--lang e] [--id-format split|full|hex]

Reads every .str file in the folder (default: the current one) and writes text.tr there. Each text becomes
one line "table|string text" ("table|string/short text" when it has a short version), tables in id order,
each under a comment naming the files it was found in. A table found in several files is written once
(a text that differs between them is written as found first, with a comment).

The form of the id is set by ID_FORMAT below or --id-format: "split" = TABLEID|STRINGID[/SHORT], "full" =
FULLID[/SHORT] as one decimal number (table << 16 | string id), "hex" = the same as a 0x hex number.

Texts are written as the text loader reads them: tags as in the strings editor of PesAfsEditor ([color=...],
[spacing=...], [font=...], [button=...], [\\uXXXX], [\\xNN]), \\n for a new line, \\\\ for a backslash, and
"quotes" around a text with spaces at either end. --lang puts a language line ([e], ...) at the top.

.str files: a (compressed) folder chunk of string tables (0x0400 chunks):
    table: uint32 numStrings, uint32 tableId, { uint16 id, uint16 shortId, uint16 size, uint16 length,
           uint32 offset } [numStrings], then the UTF-8 strings, null terminated
"""

import argparse
import glob
import os
import re
import struct
import sys
import zlib

# How a text id is written; the text loader reads all of them. --id-format overrides this.
#   'split': TABLEID|STRINGID[/SHORT]      e.g.  25|142/143
#   'full' : FULLID[/SHORT], decimal       e.g.  1638542/143     (FULLID = table << 16 | string id)
#   'hex'  : FULLID[/SHORT], 0x hex        e.g.  0x19008E/143
ID_FORMAT = 'full'

SIG_TABLE = 0x0400
FOLDER_TYPES = (0x0600, 0x0000)     # folder, single file (low 16 bits; bit 16: compressed)


def read_chunk(data, offset, name, tables):
    """Collect the string tables of the chunk at `offset`: {table id: [(id, short id, text)]}."""
    if offset + 32 > len(data):
        return
    sig, csize, dsize = struct.unpack_from('<III', data, offset)
    if sig == SIG_TABLE:
        read_table(data[offset + 32:offset + 32 + csize], name, tables)
        return
    if sig & 0xFFFF not in FOLDER_TYPES or sig >> 16 > 1:
        return
    if struct.unpack_from('<I', data, offset + 16)[0]:
        print('%s: encrypted chunk, skipped' % name, file=sys.stderr)
        return
    body = data[offset + 32:offset + 32 + csize]
    if sig >> 16:
        body = zlib.decompress(body)
    if sig & 0xFFFF == 0x0000:
        read_chunk(body, 0, name, tables)
        return
    count, offsets_at = struct.unpack_from('<II', body, 0)
    for i in range(count):
        entry = struct.unpack_from('<I', body, offsets_at + 4 * i)[0]
        if entry:
            read_chunk(body, entry, name, tables)


def read_table(payload, name, tables):
    count, table_id = struct.unpack_from('<II', payload, 0)
    entries = []
    for i in range(count):
        sid, short, size, _length, offset = struct.unpack_from('<HHHHI', payload, 8 + 12 * i)
        text = payload[offset:offset + size - 1].decode('utf-8', 'replace')
        entries.append((sid, short, text))
    tables.setdefault(table_id, []).append((name, entries))


# ---------------------------------------------------------------------------
# texts as .tr lines

MARK = '\x11'
BUTTON_KEYS = 'jklnopq'
TAG_RE = re.compile(
    r'\[(?:color=[0-9a-fA-F]{8}|spacing=-?\d{1,3}|font=[0-9A-Za-z]{1,4}|button=[' + BUTTON_KEYS + r'][a-z]'
    r'|\\u[0-9a-fA-F]{4}|\\x[0-9a-fA-F]{2})\]')


def is_hex(s):
    return all(c in '0123456789abcdefABCDEF' for c in s)


def format_id(table_id, sid, short=0, id_format=None):
    """The id of a text as a .tr id token, in the form of ID_FORMAT (or `id_format`)."""
    id_format = id_format or ID_FORMAT
    if id_format == 'split':
        token = '%d|%d' % (table_id, sid)
    else:
        full = (table_id << 16) | sid
        token = '0x%X' % full if id_format == 'hex' else '%d' % full
    return token + ('/%d' % short if short else '')


def to_tr(text):
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == MARK:
            key = text[i + 1:i + 2]
            if key == 'c' and len(text) >= i + 10 and is_hex(text[i + 2:i + 10]):
                out.append('[color=%s]' % text[i + 2:i + 10].lower())
                i += 10
                continue
            if key == 'w' and len(text) >= i + 4 and is_hex(text[i + 2:i + 4]):
                v = int(text[i + 2:i + 4], 16)
                out.append('[spacing=%d]' % (v - 0x100 if v > 0x7F else v))
                i += 4
                continue
            if key == 'm' and len(text) >= i + 5 and text[i + 2:i + 5].isalnum():
                out.append('[font=%s]' % text[i + 2:i + 5])
                i += 5
                continue
            if key and key in BUTTON_KEYS and 'a' <= text[i + 2:i + 3] <= 'z':
                out.append('[button=%s]' % text[i + 1:i + 3])
                i += 3
                continue
        if 0xE000 <= ord(c) <= 0xF8FF:
            out.append('[\\u%04X]' % ord(c))
        elif c == '\n':
            out.append('\\n')
        elif c == '\\':
            out.append('\\\\')
        elif c == '[' and TAG_RE.match(text, i):
            out.append('[\\x5b]')           # a bracket that would read as a tag
        elif ord(c) < 0x20 or ord(c) == 0x7F:
            out.append('[\\x%02x]' % ord(c))
        else:
            out.append(c)
        i += 1
    s = ''.join(out)
    if not s or s != s.strip(' \t') or (len(s) >= 2 and s[0] == '"' and s[-1] == '"'):
        s = '"%s"' % s
    return s


def main():
    ap = argparse.ArgumentParser(description='Dump .str string tables into one .tr file.')
    ap.add_argument('folder', nargs='?', default='.', help='folder with the .str files (default: current)')
    ap.add_argument('--out', default='text.tr', help='output file, relative to the folder (default: text.tr)')
    ap.add_argument('--lang', help='language line at the top, e.g. e')
    ap.add_argument('--id-format', choices=('split', 'full', 'hex'), default=ID_FORMAT,
                    help='form of the text ids: split = TABLEID|STRINGID[/SHORT], full = FULLID[/SHORT] (decimal), '
                         'hex = FULLID[/SHORT] (0x hex); default: %s' % ID_FORMAT)
    args = ap.parse_args()
    id_format = args.id_format

    tables = {}
    files = sorted(glob.glob(os.path.join(args.folder, '*.str')), key=str.lower)
    for path in files:
        name = os.path.basename(path)
        try:
            data = open(path, 'rb').read()
            read_chunk(data, 0, name, tables)
        except (struct.error, zlib.error, IndexError) as e:
            print('%s: cannot read (%s)' % (name, e), file=sys.stderr)
    if not tables:
        print('no string tables found in %d .str files' % len(files), file=sys.stderr)
        return 1

    lines = ['; %d string tables from %d .str files in %s' % (len(tables), len(files), os.path.abspath(args.folder))]
    if args.lang:
        lines += ['', '[%s]' % args.lang]
    count = 0
    for table_id in sorted(tables):
        found = tables[table_id]
        names = []
        for name, _ in found:
            if name not in names:
                names.append(name)
        lines += ['', '; table %d (%s)' % (table_id, ', '.join(names))]
        first = {}
        for name, entries in found:
            for sid, short, text in entries:
                if sid not in first:
                    first[sid] = (short, text)
                elif first[sid] != (short, text):
                    lines.append('; %s differs in %s: %s' % (format_id(table_id, sid, 0, id_format), name, to_tr(text)))
        for sid, (short, text) in first.items():
            lines.append('%s %s' % (format_id(table_id, sid, short, id_format), to_tr(text)))
            count += 1

    out = os.path.join(args.folder, args.out)
    with open(out, 'w', encoding='utf-8-sig', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d texts in %d tables' % (out, count, len(tables)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
