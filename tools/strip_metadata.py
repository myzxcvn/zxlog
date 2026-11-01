#!/usr/bin/env python3
"""Produce a runtime image without its build-only ELF log dictionary section."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from extract_dictionary import elf_section


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--input', required=True)
    p.add_argument('--output', required=True)
    p.add_argument('--dict', dest='dictionary', required=True)
    p.add_argument('--metadata-json', required=True)
    p.add_argument('--manifest', required=True)
    a = p.parse_args()
    meta = json.loads(Path(a.metadata_json).read_text())
    raw = Path(a.input).read_bytes()
    dictionary = Path(a.dictionary).read_bytes()
    identity = bytes.fromhex(meta['build_id'])
    if (hashlib.sha256(raw).hexdigest() != meta['identified_elf_sha256'] or
            hashlib.sha256(dictionary).hexdigest() != meta['dictionary_sha256'] or
            elf_section(raw, 'zxlog_identity') != identity or dictionary[12:28] != identity):
        raise ValueError('ELF, dictionary and extraction manifest do not match')
    objcopy = shutil.which('objcopy') or shutil.which('llvm-objcopy')
    if not objcopy:
        raise SystemExit('objcopy or llvm-objcopy is required to strip metadata')
    Path(a.output).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([objcopy, '--remove-section', 'zxlog_meta', a.input, a.output], check=True)
    Path(a.output).chmod(Path(a.output).stat().st_mode | 0o111)
    deployed = Path(a.output).read_bytes()
    if elf_section(deployed, 'zxlog_identity') != identity:
        raise ValueError('stripping changed runtime identity')
    try:
        elf_section(deployed, 'zxlog_meta')
    except ValueError:
        pass
    else:
        raise ValueError('deployment image still contains metadata')
    manifest = {key: meta[key] for key in ('build_id', 'source_build_id', 'raw_elf_sha256',
                                          'identified_elf_sha256', 'dictionary_sha256')}
    manifest['deployed_elf_sha256'] = hashlib.sha256(deployed).hexdigest()
    Path(a.manifest).write_text(json.dumps(manifest, indent=2) + '\n')

if __name__ == '__main__':
    main()
