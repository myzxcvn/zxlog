#!/usr/bin/env python3
"""Offline zxlog binary-event decoder. Uses the build-matched .zdict file."""
import argparse
import struct
import sys
import binascii
import glob
from string import Formatter
from pathlib import Path

LEVELS = ('TRACE', 'DEBUG', 'INFO', 'WARN', 'ERROR', 'CRITICAL')
FIXED = {1:'b', 2:'B', 3:'h', 4:'H', 5:'i', 6:'I', 7:'q', 8:'Q',
         9:'f', 10:'d', 11:'B', 12:'B'}


def load_dictionary(path):
    data = Path(path).read_bytes()
    if len(data) < 36 or data[:8] != b'ZXDICT1\0' or struct.unpack_from('<HH', data, 8) != (1, 36):
        raise ValueError('invalid zxlog dictionary')
    build_id = data[12:28]
    count, reserved = struct.unpack_from('<II', data, 28)
    if count > 1_000_000 or reserved != 0:
        raise ValueError('bad dictionary entry count')
    pos = 36
    entries = {}
    for _ in range(count):
        if pos + 16 > len(data):
            raise ValueError('truncated dictionary entry')
        event_id, line, level, argc, file_n, fmt_n, _ = struct.unpack_from('<IIBBHHH', data, pos)
        pos += 16
        if pos + argc + file_n + fmt_n > len(data) or level >= len(LEVELS):
            raise ValueError('invalid dictionary entry size/level')
        types = list(data[pos:pos+argc]); pos += argc
        file = data[pos:pos+file_n].decode('utf-8'); pos += file_n
        fmt = data[pos:pos+fmt_n].decode('utf-8'); pos += fmt_n
        if event_id in entries:
            raise ValueError('duplicate LogID')
        entries[event_id] = (level, fmt, types, file, line)
    if pos != len(data):
        raise ValueError('trailing dictionary data')
    return build_id, entries


def decode_args(payload, types):
    args = []
    pos = 0
    for t in types:
        if t == 13:
            if pos + 4 > len(payload): raise ValueError('truncated string length')
            n, = struct.unpack_from('<I', payload, pos)
            pos += 4
            if pos + n > len(payload): raise ValueError('truncated string data')
            args.append(payload[pos:pos+n].decode('utf-8', errors='replace'))
            pos += n
        else:
            if t not in FIXED: raise ValueError('unknown argument type')
            fmt = '<' + FIXED[t]
            if pos + struct.calcsize(fmt) > len(payload): raise ValueError('truncated argument')
            v, = struct.unpack_from(fmt, payload, pos)
            pos += struct.calcsize(fmt)
            if t == 11: v = bool(v)
            elif t == 12: v = chr(v)
            args.append(v)
    if pos != len(payload): raise ValueError('trailing argument bytes')
    return args


class EventFormatter(Formatter):
    def get_field(self, field_name, args, kwargs):
        # Only numbered/automatic placeholders. No attribute or mapping access.
        if not field_name.isdecimal():
            raise ValueError('only numeric/automatic log fields supported')
        index = int(field_name)
        if index >= len(args):
            raise ValueError('log argument index out of range')
        return args[index], index

    def format_field(self, value, spec):
        if isinstance(value, bool) and not spec:
            return 'true' if value else 'false'
        if isinstance(value, float) and not spec:
            return format(value, '.6g')
        return format(value, spec)


def format_message(fmt, args):
    # Matches the overlapping part of C++ {fmt} and Python's format grammar,
    # including positional fields, alignment, width, precision and nested width.
    try:
        return EventFormatter().vformat(fmt, tuple(args), {})
    except (ValueError, TypeError, IndexError, KeyError) as exc:
        raise ValueError(f'unsupported log format: {exc}') from exc


def iter_logs(path, build_id, entries, recover=False):
    data = Path(path).read_bytes()
    if len(data) < 12:
        raise ValueError('truncated zxlog file header')
    magic = data[:8]
    version, header_size = struct.unpack_from('<HH', data, 8)
    if magic == b'ZXLOG02\0':
        if version != 2 or header_size != 64 or len(data) < 64:
            raise ValueError('invalid v2 binary header')
        if struct.unpack_from('<I', data, 60)[0] != binascii.crc32(data[:60]):
            raise ValueError('corrupt file header CRC32')
        min_rec, max_rec = 20, 1024
    elif magic == b'ZXLOG01\0':
        if version != 1 or header_size != 48 or len(data) < 48:
            raise ValueError('invalid legacy v1 header')
        min_rec, max_rec = 16, 512
    else:
        raise ValueError('not a zxlog binary file')
    if data[12:28] != build_id:
        raise ValueError('build ID mismatch: supply the correct dictionary')

    def record_at(pos):
        if pos + min_rec > len(data):
            raise ValueError(f'truncated record header at {pos}')
        size, event_id, stamp = struct.unpack_from('<IIQ', data, pos)
        if not min_rec <= size <= max_rec or pos + size > len(data):
            raise ValueError(f'bad/truncated record length at {pos}')
        if version == 2:
            recorded_crc, = struct.unpack_from('<I', data, pos + 16)
            calculated_crc = binascii.crc32(data[pos:pos+16])
            calculated_crc = binascii.crc32(data[pos+20:pos+size], calculated_crc)
            if calculated_crc != recorded_crc:
                raise ValueError(f'record CRC32 mismatch at {pos}')
        if event_id not in entries:
            raise ValueError(f'unknown EventID 0x{event_id:08x} at {pos}')
        level, fmt, types, _, _ = entries[event_id]
        args = decode_args(data[pos+min_rec:pos+size], types)
        return size, stamp, LEVELS[level], format_message(fmt, args)

    pos = header_size
    while pos < len(data):
        try:
            size, stamp, level, message = record_at(pos)
        except ValueError:
            if not recover:
                raise
            start = pos
            # CRC checked before accepting any candidate. A torn final record
            # is ignored; middle corruption can be scanned past at byte granularity.
            pos += 1
            while pos + min_rec <= len(data):
                try:
                    record_at(pos)
                    break
                except ValueError:
                    pos += 1
            print(f'zxlog recovery: skipped {pos - start} bad bytes in {path}', file=sys.stderr)
            if pos + min_rec > len(data):
                return
            continue
        yield stamp, level, message
        pos += size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dict', dest='dictionary', required=True)
    ap.add_argument('--input', action='append', required=True,
                    help='repeat per segment; shell globs supported')
    ap.add_argument('--output', help='write restored text here (default stdout)')
    ap.add_argument('--recover', action='store_true',
                    help='salvage valid records after CRC errors or torn writes')
    a = ap.parse_args()
    build_id, entries = load_dictionary(a.dictionary)
    paths = []
    for pattern in a.input:
        matches = glob.glob(pattern)
        if not matches:
            raise ValueError(f'log input not found: {pattern}')
        paths.extend(matches)
    output = open(a.output, 'w', encoding='utf-8') if a.output else sys.stdout
    try:
        for path in paths:
            first = None  # archive segments can have different uptime origins
            for timestamp, level, message in iter_logs(path, build_id, entries, a.recover):
                if first is None:
                    first = timestamp
                elapsed = max(timestamp - first, 0)
                output.write(f'[{elapsed / 1e9:10.3f}s] [{level}] {message}\n')
    finally:
        if a.output:
            output.close()


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as exc:
        print(f'zxlog decode error: {exc}', file=sys.stderr)
        sys.exit(1)
