#!/usr/bin/env python3
"""Verify final-image identities and reject mixed deployment artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from extract_dictionary import elf_section


def run(*args, succeeds=True):
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True, timeout=60)
    assert (result.returncode == 0) == succeeds, (args, result.stdout, result.stderr)
    return result


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main(a):
    build = Path(a.build).resolve()
    identities = []
    seed = bytes.fromhex((build / 'gen/build_id.txt').read_text().strip())
    for name, dictionary in (('zxlog_demo', 'zxlog.zdict'), ('zxlog_multi', 'multi.zdict')):
        raw = (build / (name + '_raw')).read_bytes()
        identified = (build / (name + '_identified')).read_bytes()
        deployed = (build / name).read_bytes()
        manifest = json.loads((build / (name + '.manifest.json')).read_text())
        identity = bytes.fromhex(manifest['build_id'])
        assert identity == hashlib.sha256(b'zxlog-linked-image-v1\0' + seed + raw).digest()[:16]
        assert elf_section(raw, 'zxlog_identity') == bytes(16)
        assert elf_section(identified, 'zxlog_identity') == identity
        assert elf_section(deployed, 'zxlog_identity') == identity
        assert (build / dictionary).read_bytes()[12:28] == identity
        assert manifest['source_build_id'] == seed.hex()
        assert manifest['raw_elf_sha256'] == sha(raw)
        assert manifest['identified_elf_sha256'] == sha(identified)
        assert manifest['deployed_elf_sha256'] == sha(deployed)
        assert manifest['dictionary_sha256'] == sha((build / dictionary).read_bytes())
        identities.append(identity)
    assert identities[0] != identities[1], 'different linked programs share an identity'

    with tempfile.TemporaryDirectory(prefix='zxlog-artifacts-') as temporary:
        scratch = Path(temporary)
        mode = 'text' if a.mode == 'TEXT' else 'binary'
        raw_result = run(build / 'zxlog_demo_raw', mode, build / 'zxlog.zdict', scratch / 'raw',
                         succeeds=False)
        assert 'unfinalized ELF' in raw_result.stderr
        assert not list(scratch.glob('raw.*'))
        if a.mode != 'BINARY':
            wrong = run(build / 'zxlog_demo', mode, build / 'multi.zdict', scratch / 'wrong',
                        succeeds=False)
            assert 'build ID mismatch' in wrong.stderr
            assert not list(scratch.glob('wrong.*'))
        if a.mode != 'TEXT':
            run(build / 'zxlog_demo', mode, build / 'zxlog.zdict', scratch / 'correct')
            for decoder in ([sys.executable, ROOT / 'tools/decode.py'], [build / 'zxlog_decode']):
                result = run(*decoder, '--dict', build / 'multi.zdict', '--input',
                             scratch / 'correct.blog', succeeds=False)
                assert 'build ID mismatch' in result.stderr

        # Same input deterministically creates the same identity and dictionary.
        extraction = [sys.executable, ROOT / 'tools/extract_dictionary.py', '--elf',
                      build / 'zxlog_demo_raw', '--build-id-file', build / 'gen/build_id.txt',
                      '--output', scratch / 'again.zdict', '--json', scratch / 'again.json',
                      '--patched-elf', scratch / 'again']
        run(*extraction)
        assert (scratch / 'again.zdict').read_bytes() == (build / 'zxlog.zdict').read_bytes()
        assert (scratch / 'again').read_bytes() == (build / 'zxlog_demo_identified').read_bytes()
        extraction[3] = build / 'zxlog_demo_identified'
        run(*extraction, succeeds=False)

        packaging = [sys.executable, ROOT / 'tools/strip_metadata.py', '--input',
                     build / 'zxlog_demo_identified', '--output', scratch / 'bad-image',
                     '--dict', build / 'multi.zdict', '--metadata-json',
                     build / 'zxlog_dictionary.json', '--manifest', scratch / 'bad.json']
        run(*packaging, succeeds=False)
        assert not (scratch / 'bad-image').exists()
        corrupted = bytearray((build / 'zxlog.zdict').read_bytes())
        corrupted[-1] ^= 1
        (scratch / 'corrupt.zdict').write_bytes(corrupted)
        packaging[7] = scratch / 'corrupt.zdict'
        run(*packaging, succeeds=False)
        assert not (scratch / 'bad-image').exists()

    Path(a.stamp).write_text('PASS final ELF identities and artifact matching\n')
    print('PASS final ELF identities, mixed-dictionary rejection and deployment manifests')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--mode', required=True)
    parser.add_argument('--stamp', required=True)
    main(parser.parse_args())
