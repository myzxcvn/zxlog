#!/usr/bin/env python3
"""zxlog v0.4 integration + durability tests against actual executables."""
import argparse
import binascii
import importlib.util
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def command(*args, expect_success=True):
    p = subprocess.run(args, text=True, capture_output=True)
    if (p.returncode == 0) != expect_success:
        raise AssertionError(f'unexpected exit {p.returncode} for {args}\n'
                             f'stdout={p.stdout}\nstderr={p.stderr}')
    return p

def decoder_module():
    spec = importlib.util.spec_from_file_location('zxlog_decoder', ROOT / 'tools' / 'decode.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def do_test(a):
    # GN passes filenames relative to the build directory. subprocess searches
    # PATH for bare names, so resolve executable paths before launching them.
    for name in ('demo', 'unit', 'cpp_decoder'):
        value = getattr(a, name)
        if value:
            setattr(a, name, str(Path(value).resolve()))
    work_root = Path(a.work)
    work_root.mkdir(parents=True, exist_ok=True)
    work = work_root / 'cases'
    if work.exists():
        shutil.rmtree(work)  # only the integration harness's own scratch directory
    work.mkdir(parents=True)
    command(a.unit)
    dec = decoder_module()
    build_id, entries = dec.load_dictionary(a.dict)
    assert len(entries) >= 19, f'missing log sites: {len(entries)}'
    mode = {'TEXT': 'text', 'BINARY': 'binary', 'RUNTIME': 'switch'}[a.mode]
    prefix = work / f'run_{mode}'
    status = command(a.demo, mode, a.dict, str(prefix)).stdout
    assert 'io_errors=0' in status and 'sync_errors=0' in status, status
    if a.mode != 'BINARY':
        content = prefix.with_suffix('.log').read_text(encoding='utf-8')
        assert 'Phase: text' in content if a.mode == 'RUNTIME' else 'Motor speed: 1200' in content
        assert "Aligned: '   LiDAR'" in content if a.mode == 'TEXT' else True
        assert 'Manual args: second first' in content if a.mode == 'TEXT' else True
        assert 'Strong type: 00002A' in content if a.mode == 'TEXT' else True
        assert 'Dynamic width: [    go]' in content if a.mode == 'TEXT' else True
        assert 'Signed exp: +3.250e+00' in content if a.mode == 'TEXT' else True
        assert 'Should not execute' not in content
    if a.mode == 'TEXT':
        # A torn trailing text line is truncated before archiving on restart.
        previous = prefix.with_suffix('.log')
        original_text = previous.read_bytes()
        previous.write_bytes(original_text + b'incomplete')
        second = command(a.demo, 'text', a.dict, str(prefix)).stdout
        assert 'repaired_bytes=10' in second, second
        assert Path(str(previous) + '.1').read_bytes() == original_text
    if a.mode != 'TEXT':
        binary = prefix.with_suffix('.blog')
        decoded = command(sys.executable, str(ROOT / 'tools' / 'decode.py'),
                          '--dict', a.dict, '--input', str(binary)).stdout
        assert 'Motor speed: 1200, current: 1.52' in decoded
        assert "Aligned: '   LiDAR'" in decoded
        assert 'Manual args: second first' in decoded
        assert 'Strong type: 00002A' in decoded
        assert 'Dynamic width: [    go]' in decoded
        assert 'Signed exp: +3.250e+00' in decoded
        assert 'Should not execute' not in decoded
        assert ('Phase: binary' in decoded) == (a.mode == 'RUNTIME')
        if a.cpp_decoder:
            cpp = command(a.cpp_decoder, '--dict', a.dict, '--input', str(binary)).stdout
            assert 'Motor speed: 1200, current: 1.52' in cpp
            assert 'Signed exp: +3.250e+00' in cpp
            assert 'Dynamic width: [    go]' in cpp
        raw = binary.read_bytes()
        assert raw[:8] == b'ZXLOG02\0' and len(raw) >= 64
        assert int.from_bytes(raw[60:64], 'little') == binascii.crc32(raw[:60])

        # CRC detects even valid-length corruption; recover salvages later records.
        mutated = bytearray(raw)
        first_size = int.from_bytes(mutated[64:68], 'little')
        mutated[64 + first_size - 1] ^= 0xff
        corrupt = work / 'corrupted.blog'
        corrupt.write_bytes(mutated)
        command(sys.executable, str(ROOT / 'tools/decode.py'), '--dict', a.dict,
                '--input', str(corrupt), expect_success=False)
        recovered = command(sys.executable, str(ROOT / 'tools/decode.py'),
                            '--recover', '--dict', a.dict, '--input', str(corrupt)).stdout
        if a.cpp_decoder:
            command(a.cpp_decoder, '--dict', a.dict, '--input', str(corrupt),
                    expect_success=False)
            cpp_recovered = command(a.cpp_decoder, '--recover', '--dict', a.dict,
                                    '--input', str(corrupt)).stdout
            assert 'Sensor ID: 002a' in cpp_recovered
        assert 'Sensor ID: 002a' in recovered

        # Recover truncated final record (nothing fabricated from torn data).
        torn = work / 'torn.blog'
        torn.write_bytes(raw[:-5])
        command(sys.executable, str(ROOT / 'tools/decode.py'), '--dict', a.dict,
                '--input', str(torn), expect_success=False)
        command(sys.executable, str(ROOT / 'tools/decode.py'), '--recover',
                '--dict', a.dict, '--input', str(torn))

        # Wrong dictionary and damaged header are always fatal, even --recover.
        bad_dict = bytearray(Path(a.dict).read_bytes())
        bad_dict[12] ^= 0xff
        bad = work / 'wrong.zdict'
        bad.write_bytes(bad_dict)
        command(sys.executable, str(ROOT / 'tools/decode.py'), '--dict', str(bad),
                '--input', str(binary), expect_success=False)
        broken_header = work / 'broken-header.blog'
        broken_header.write_bytes(bytes([raw[0] ^ 1]) + raw[1:])
        command(sys.executable, str(ROOT / 'tools/decode.py'), '--recover',
                '--dict', a.dict, '--input', str(broken_header), expect_success=False)

        # Restart after torn append repairs the valid prefix and archives it.
        resume = work / 'resume'
        command(a.demo, 'binary', a.dict, str(resume))
        live = Path(str(resume) + '.blog')
        original = live.read_bytes()
        live.write_bytes(original + b'\x32\x00\x00')
        status2 = command(a.demo, 'binary', a.dict, str(resume)).stdout
        assert 'repaired_bytes=3' in status2, status2
        archived = Path(str(resume) + '.blog.1')
        assert archived.is_file() and archived.read_bytes() == original
        assert len(list(dec.iter_logs(archived, build_id, entries))) == 13
        assert len(list(dec.iter_logs(live, build_id, entries))) == 13

        # A legacy v1 active segment is preserved through a v0.4 upgrade.
        ready_id = next((event for event, e in entries.items()
                         if e[1] == 'Ready' and not e[2]), None)
        assert ready_id is not None
        legacy_prefix = work / 'legacy'
        old = Path(str(legacy_prefix) + '.blog')
        legacy_header = (b'ZXLOG01\0' + struct.pack('<HH', 1, 48) +
                         build_id + struct.pack('<QQI', 1, 2, 0))
        old.write_bytes(legacy_header + struct.pack('<IIQ', 16, ready_id, 3))
        command(a.demo, 'binary', a.dict, str(legacy_prefix))
        old_archive = Path(str(old) + '.1')
        assert old_archive.exists()
        assert len(list(dec.iter_logs(old_archive, build_id, entries))) == 1
        assert old.read_bytes().startswith(b'ZXLOG02\0')

        # Torn v2 file header: no valid event, discard incomplete header only.
        header_prefix = work / 'partial_header'
        partial = Path(str(header_prefix) + '.blog')
        partial.write_bytes(b'ZXLOG02\0' + b'\x02\x00\x40\x00')
        result = command(a.demo, 'binary', a.dict, str(header_prefix)).stdout
        assert 'repaired_bytes=12' in result
        assert partial.read_bytes().startswith(b'ZXLOG02\0')

        # Never silently overwrite files with unrecognized headers.
        unknown_prefix = work / 'unknown'
        unknown = Path(str(unknown_prefix) + '.blog')
        unknown.write_bytes(b'not a zxlog file')
        command(a.demo, 'binary', a.dict, str(unknown_prefix), expect_success=False)
        assert unknown.read_bytes() == b'not a zxlog file'

        # Multiple producer SPSC queues, bounded loss, intact record boundaries.
        stress = work / 'stress'
        report = command(a.demo, 'stress', a.dict, str(stress)).stdout
        match = re.search(r'accepted=(\d+) dropped=(\d+) io_errors=(\d+)', report)
        assert match is not None, report
        accepted, dropped, io_errors = map(int, match.groups())
        assert accepted + dropped == 2013 and io_errors == 0
        messages = list(dec.iter_logs(str(stress) + '.blog', build_id, entries))
        assert len(messages) == accepted
        assert all('worker=' not in msg or ' sequence=' in msg for _, _, msg in messages)

        # Thread-local SPSC ring reclamation after thread exit.
        churn = work / 'churn'
        report = command(a.demo, 'churn', a.dict, str(churn)).stdout
        assert 'dropped=0' in report, report
        recovered_churn = list(dec.iter_logs(str(churn) + '.blog', build_id, entries))
        assert len([msg for _, _, msg in recovered_churn if 'churn worker=' in msg]) == 20

        # Disk retention: active + 2 archives, each file <= quota.
        rotation = work / 'rotate'
        stale = Path(str(rotation) + '.blog.99')
        stale.write_bytes(b'old archive should be pruned')
        report = command(a.demo, 'rotate', a.dict, str(rotation)).stdout
        assert not stale.exists(), 'old archives beyond quota remain'
        count = int(re.search(r'rotations=(\d+)', report).group(1))
        assert count > 0, report
        segments = list(work.glob('rotate.blog*'))
        assert 1 <= len(segments) <= 3, segments
        assert all(f.stat().st_size <= 1800 for f in segments)
        for file in segments:
            list(dec.iter_logs(file, build_id, entries))

    # GN deploy image must not contain embedded dictionary section.
    spec = importlib.util.spec_from_file_location('extract_dictionary', ROOT / 'tools/extract_dictionary.py')
    ext = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ext)
    demo_bytes = Path(a.demo).read_bytes()
    try:
        ext.elf_section(demo_bytes, 'zxlog_meta')
    except ValueError:
        pass
    else:
        raise AssertionError('deployable image still contains static log dictionary')
    # A missing section alone does not prove stripping worked: older GCC can
    # place template-local LogSites in .rodata instead of the requested section.
    assert b'Motor speed: {}, current: {:.2f}' not in demo_bytes, \
        'deployable image still contains a LogSite format string'
    assert b'Should not execute: {}' not in demo_bytes, \
        'deployable image still contains a filtered LogSite format string'

    # A C++17 fmt error is a compile-time failure, not deferred to a log event.
    invalid = '#include "zxlog/zxlog.h"\nvoid f(){ ZX_LOG(INFO, "{:x}", 1.5); }\n'
    bad = subprocess.run(['g++', '-std=c++17', '-fsyntax-only',
                         '-I' + str(ROOT / 'include'),
                         '-I' + str(ROOT / 'third_party/fmt/include'),
                         '-x', 'c++', '-'], input=invalid, text=True, capture_output=True)
    assert bad.returncode != 0 and 'fmt' in bad.stderr.lower(), bad.stderr

    for result_type, exception_spec, message in (
            ('int', 'noexcept', 'return type must match Wire'),
            ('std::uint64_t', '', 'to_wire must be noexcept')):
        source = ('#include "zxlog/zxlog.h"\nstruct Custom {};\n'
                  'namespace zxlog { template<> struct LogAdapter<Custom> {'
                  'using Wire=std::uint64_t; static ' + result_type +
                  ' to_wire(const Custom&) ' + exception_spec + ' { return 1; }};}\n'
                  'void f(){ ZX_LOG(INFO, "{}", Custom{}); }')
        result = subprocess.run(['g++', '-std=c++17', '-fsyntax-only',
                                 '-I' + str(ROOT / 'include'),
                                 '-I' + str(ROOT / 'third_party/fmt/include'), '-x', 'c++', '-'],
                                input=source, text=True, capture_output=True)
        assert result.returncode != 0 and message in result.stderr, result.stderr

    Path(a.stamp).write_text(f'PASS zxlog v0.4 {a.mode}\n', encoding='utf-8')
    print(f'PASS zxlog v0.4: {a.mode}')

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--demo', required=True)
    ap.add_argument('--unit', required=True)
    ap.add_argument('--dict', required=True)
    ap.add_argument('--mode', required=True, choices=['TEXT', 'BINARY', 'RUNTIME'])
    ap.add_argument('--work', required=True)
    ap.add_argument('--stamp', required=True)
    ap.add_argument('--cpp-decoder')
    do_test(ap.parse_args())
