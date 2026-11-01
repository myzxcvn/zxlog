#!/usr/bin/env python3
"""Generate a source fingerprint and a runtime linked-identity accessor.

For the demo, inputs cover all application and zxlog sources and GN options.
Production integrations should provide all files/options that can affect LogSites.
The final ID is assigned by extract_dictionary.py after linking, never here.
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
    body = ('#pragma once\n#include "zxlog/identity.h"\n'
            'namespace zxlog::build {\n'
            'inline const auto kBuildId = ::zxlog::linked_build_id();\n}\n')
    header.write_text(body, encoding='utf-8')
    file = Path(a.id_file)
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(build_id.hex() + '\n', encoding='ascii')
    print(build_id.hex())

if __name__ == '__main__':
    main()
