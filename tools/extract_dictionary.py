#!/usr/bin/env python3
"""Extract relocation-free zxlog_meta blobs from an ELF32/ELF64 little-endian image.

No pyelftools dependency. The extractor also fails builds on EventID collisions.
"""
import argparse
import json
import hashlib
import struct
from pathlib import Path

SITE_MAGIC = 0x4D4C585A
LEVELS = ('TRACE', 'DEBUG', 'INFO', 'WARN', 'ERROR', 'CRITICAL')
TYPES = {1: 'i8', 2: 'u8', 3: 'i16', 4: 'u16', 5: 'i32', 6: 'u32',
         7: 'i64', 8: 'u64', 9: 'f32', 10: 'f64', 11: 'bool', 12: 'char', 13: 'str'}


class MissingElfSection(ValueError):
    pass


def elf_section_range(data: bytes, section_name: str):
    if len(data) < 64:
        raise ValueError('truncated ELF header')
    if data[:4] != b'\x7fELF' or data[5] != 1:
        raise ValueError('expected little-endian ELF')
    is_64 = data[4] == 2
    if data[4] not in (1, 2):
        raise ValueError('unsupported ELF class')
    if is_64:
        shoff = struct.unpack_from('<Q', data, 40)[0]
        shentsize, shnum, names_index = struct.unpack_from('<HHH', data, 58)
        pattern = '<IIQQQQIIQQ'
    else:
        shoff = struct.unpack_from('<I', data, 32)[0]
        shentsize, shnum, names_index = struct.unpack_from('<HHH', data, 46)
        pattern = '<IIIIIIIIII'
    if shentsize < struct.calcsize(pattern) or shnum == 0 or names_index >= shnum:
        raise ValueError('unsupported/invalid ELF section header table')
    if shoff + shnum * shentsize > len(data):
        raise ValueError('truncated ELF section headers')
    headers = [struct.unpack_from(pattern, data, shoff + i * shentsize)
               for i in range(shnum)]
    _, _, _, _, str_off, str_size, *_ = headers[names_index]
    names = data[str_off:str_off + str_size]
    if len(names) != str_size:
        raise ValueError('truncated ELF names')
    for header in headers:
        name_idx = header[0]
        if name_idx >= len(names):
            continue
        name_end = names.find(b'\0', name_idx)
        if name_end == -1:
            continue
        if names[name_idx:name_end] == section_name.encode():
            _, _, _, _, start, size, *_ = header
            if start + size > len(data):
                raise ValueError('truncated ELF metadata section')
            return start, size
    raise MissingElfSection(f'ELF section {section_name!r} not found (check used/retain/KEEP)')


def elf_section(data: bytes, section_name: str) -> bytes:
    start, size = elf_section_range(data, section_name)
    return data[start:start + size]


def read_sites(data: bytes, allow_empty=False):
    entries = {}
    pos = 0
    while pos < len(data):
        if len(data) - pos < 24:
            raise ValueError(f'truncated metadata record at {pos}')
        magic, version, total, site_id, line, file_n, fmt_n, argc, level, _ = \
            struct.unpack_from('<IHHIIHHBBH', data, pos)
        if magic != SITE_MAGIC or version != 1:
            raise ValueError(f'bad metadata magic/version at offset {pos}')
        if total != 24 + max(1, argc) + file_n + fmt_n or pos + total > len(data):
            raise ValueError(f'bad metadata record length at offset {pos}')
        if not (0 <= level < len(LEVELS)):
            raise ValueError(f'invalid level at {pos}')
        types = list(data[pos + 24: pos + 24 + argc])
        if any(t not in TYPES for t in types):
            raise ValueError(f'unknown argument type at {pos}')
        fields = pos + 24 + max(1, argc)
        raw_file = data[fields:fields + file_n]
        raw_format = data[fields + file_n:fields + file_n + fmt_n]
        if not raw_file.endswith(b'\0') or not raw_format.endswith(b'\0'):
            raise ValueError(f'unterminated metadata string at {pos}')
        entry = dict(id=site_id, level=level, line=line, types=types,
                     file=raw_file[:-1].decode('utf-8'),
                     format=raw_format[:-1].decode('utf-8'))
        if site_id in entries and entries[site_id] != entry:
            raise ValueError(f'EventID collision 0x{site_id:08x}: '
                             f'{entries[site_id]} and {entry}')
        entries[site_id] = entry
        pos += total
    if not entries and not allow_empty:
        raise ValueError('no LogSite metadata found')
    return sorted(entries.values(), key=lambda entry: entry['id'])


def write_dict(path: Path, entries, build_id: bytes):
    buffer = bytearray()
    buffer.extend(b'ZXDICT1\0')
    buffer.extend(struct.pack('<HH', 1, 36))
    buffer.extend(build_id)
    buffer.extend(struct.pack('<II', len(entries), 0))
    for e in entries:
        file = e['file'].encode('utf-8')
        fmt = e['format'].encode('utf-8')
        if len(file) > 65535 or len(fmt) > 65535:
            raise ValueError('filename or format exceeds dictionary size limit')
        buffer.extend(struct.pack('<IIBBHHH', e['id'], e['line'], e['level'],
                                  len(e['types']), len(file), len(fmt), 0))
        buffer.extend(bytes(e['types']))
        buffer.extend(file)
        buffer.extend(fmt)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(buffer)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--output', required=True)
    ap.add_argument('--json', required=True)
    ap.add_argument('--build-id-file', required=True)
    ap.add_argument('--patched-elf', required=True)
    ap.add_argument('--allow-empty', action='store_true',
                    help='allow images with all log sites compiled out')
    a = ap.parse_args()
    source_id = bytes.fromhex(Path(a.build_id_file).read_text().strip())
    if len(source_id) != 16:
        raise ValueError('source build ID must have exactly 16 bytes')
    raw = Path(a.elf).read_bytes()
    start, size = elf_section_range(raw, 'zxlog_identity')
    if size != 16 or any(raw[start:start + size]):
        raise ValueError('expected one unfinalized 16-byte ELF identity')
    if Path(a.elf).resolve() == Path(a.patched_elf).resolve():
        raise ValueError('patched ELF must not overwrite the linker output')
    try:
        metadata = elf_section(raw, 'zxlog_meta')
    except MissingElfSection:
        if not a.allow_empty:
            raise
        metadata = b''
    entries = read_sites(metadata, allow_empty=a.allow_empty)
    build_id = hashlib.sha256(b'zxlog-linked-image-v1\0' + source_id + raw).digest()[:16]
    patched = raw[:start] + build_id + raw[start + size:]
    Path(a.patched_elf).parent.mkdir(parents=True, exist_ok=True)
    Path(a.patched_elf).write_bytes(patched)
    Path(a.patched_elf).chmod(Path(a.elf).stat().st_mode)
    write_dict(Path(a.output), entries, build_id)
    for e in entries:
        e['level'] = LEVELS[e['level']]
        e['types'] = [TYPES[t] for t in e['types']]
        e['id'] = f"0x{e['id']:08x}"
    Path(a.json).write_text(json.dumps({
        'schema': 'zxlog-v1', 'build_id': build_id.hex(), 'events': entries,
        'source_build_id': source_id.hex(),
        'raw_elf_sha256': hashlib.sha256(raw).hexdigest(),
        'identified_elf_sha256': hashlib.sha256(patched).hexdigest(),
        'dictionary_sha256': hashlib.sha256(Path(a.output).read_bytes()).hexdigest(),
    }, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'Extracted {len(entries)} LogSites → {a.output}')

if __name__ == '__main__':
    main()
