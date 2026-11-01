#!/usr/bin/env python3
"""Generate one build identity shared by the executable and the log dictionary.

For the demo, inputs cover all application and zxlog sources and GN options.
Production integrations should provide all files/options that can affect LogSites.
"""
import argparse
import hashlib
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--header', required=True)
    ap.add_argument('--id-file', required=True)
    ap.add_argument('--mode', required=True)
    ap.add_argument('sources', nargs='+')
    a = ap.parse_args()
    h = hashlib.sha256()
    h.update(b'zxlog-build-id-v1\0')
    h.update(a.mode.encode() + b'\0')
    for name in sorted(a.sources):
        src = Path(name)
        h.update(src.as_posix().encode() + b'\0')
        h.update(hashlib.sha256(src.read_bytes()).digest())
    build_id = h.digest()[:16]
    header = Path(a.header)
    header.parent.mkdir(parents=True, exist_ok=True)
    body = ('#pragma once\n#include <array>\n#include <cstdint>\n'
            'namespace zxlog::build {\n'
            'inline constexpr std::array<std::uint8_t, 16> kBuildId{{'
            + ', '.join(f'0x{x:02x}' for x in build_id)
            + '}};\n}\n')
    header.write_text(body, encoding='utf-8')
    file = Path(a.id_file)
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(build_id.hex() + '\n', encoding='ascii')
    print(build_id.hex())

if __name__ == '__main__':
    main()
