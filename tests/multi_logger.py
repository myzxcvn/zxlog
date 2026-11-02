#!/usr/bin/env python3
"""End-to-end multi-logger isolation, ownership and macro contract checks."""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import decode
import extract_dictionary


def run(*args):
    try:
        result = subprocess.run(args, text=True, capture_output=True, timeout=60)
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(f'{args} timed out: stdout={exc.stdout!r}; stderr={exc.stderr!r}') from exc
    if result.returncode or result.stderr:
        raise AssertionError(f'{args}: {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result.stdout


def segments(root, name, suffix):
    base = name + suffix
    found = [p for p in root.glob(base + '*')
             if p.name == base or re.fullmatch(re.escape(base) + r'\.\d+', p.name)]
    return sorted(found, key=lambda p: int(p.name[len(base) + 1:]) if p.name != base else 0,
                  reverse=True)


def main(a):
    exe = str(Path(a.exe).resolve())
    cpp_decoder = str(Path(a.cpp_decoder).resolve())
    dictionary = str(Path(a.dict).resolve())
    build_id, entries = decode.load_dictionary(dictionary)
    assert sum(e[1] == 'custom {}' for e in entries.values()) == 1, 'one call site must serve both loggers'
    image = Path(exe).read_bytes()
    assert extract_dictionary.elf_section(image, 'zxlog_identity') == build_id
    try:
        extract_dictionary.elf_section(image, 'zxlog_meta')
    except ValueError:
        pass
    else:
        raise AssertionError('multi-logger deployment image retains metadata')
    assert b'custom level {}' not in image and b'filtered {}' not in image

    work = Path(a.work).resolve()
    cases = work / 'cases'
    if cases.exists():
        shutil.rmtree(cases)
    cases.mkdir(parents=True)

    def records(root, name, only=None):
        result = []
        if a.mode != 'TEXT' and only != 'text':
            for path in segments(root, name, '.blog'):
                decoded = [(level, message) for _, level, message in decode.iter_logs(path, build_id, entries)]
                cpp = run(cpp_decoder, '--dict', dictionary, '--input', str(path))
                cpp_records = re.findall(r'\[(TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL)\] (.*)', cpp)
                assert cpp_records == decoded, (path, cpp_records, decoded)
                result.extend(decoded)
        if a.mode != 'BINARY' and only != 'binary':
            for path in segments(root, name, '.log'):
                lines = path.read_text().splitlines()
                decoded = [re.fullmatch(r'\[\+\d+\.\d+\] \[(\w+)\] (.*)', line) for line in lines]
                assert all(decoded), (path, lines)
                result.extend(match.groups() for match in decoded)
        return result

    roots = {}
    for scenario in ('isolation', 'diagnostics', 'periodic', 'text_limits', 'encoding', 'churn', 'capacity', 'thread_churn', 'close', 'conflicts', 'rotation', 'resource_budget', 'custom_sink', 'shared_dispatcher', 'failing_sink'):
        root = cases / scenario
        root.mkdir()
        roots[scenario] = root
        output = run(exe, scenario, dictionary, str(root))
        for line in output.splitlines():
            name, accepted, dropped, rotations = line.split()
            accepted, dropped, rotations = int(accepted), int(dropped), int(rotations)
            data = records(root, name)
            if scenario == 'rotation' and name == 'rotate':
                assert 0 < len(data) <= accepted and rotations > 0
            else:
                assert len(data) == accepted, (scenario, name, accepted, len(data))
            assert not any('filtered' in message for _, message in data)
            if scenario not in ('capacity', 'close', 'encoding', 'resource_budget'):
                assert dropped == 0, (scenario, output)

    root = roots['isolation']
    global_data, a_data, b_data = (records(root, name) for name in ('global', 'a', 'b'))
    assert {m for _, m in a_data if m.startswith('custom ')} == {
        *(f'custom {i}' for i in range(10)), *(f'custom level {i}' for i in range(6))}
    assert {m for _, m in b_data if m.startswith('custom ')} == {
        *(f'custom {i}' for i in range(100, 110)), 'custom while global disabled'}
    assert {m for _, m in global_data if m.startswith('global thread ')} == {
        f'global thread {t} {i}' for t in range(4) for i in range(25)}
    for name, data in (('a', a_data), ('b', b_data)):
        assert {m for _, m in data if ' thread ' in m} == {
            f'{name} thread {t} {i}' for t in range(4) for i in range(25)}
    for i, level in enumerate(decode.LEVELS):
        assert (level, f'level {i}') in global_data
        assert (level, f'custom level {i}') in a_data
    assert all(('INFO', message) in a_data for message in ('pointer', 'reference', 'resolved'))
    assert ('INFO', 'adapter once') in a_data
    assert ('ERROR', 'accepted 1') in a_data
    assert ('INFO', 'after other closed') in b_data
    assert records(root, 'shared') == [('INFO', 'shared')]
    assert records(root, 'temporary') == [('INFO', 'temporary')]
    if a.mode == 'RUNTIME':
        assert records(root, 'b', 'text') == [('INFO', 'switched text')]
        assert ('INFO', 'switched binary') in records(root, 'b', 'binary')
        assert ('INFO', 'global stays binary') in records(root, 'global', 'binary')
        assert ('INFO', 'other stays binary') in records(root, 'a', 'binary')
        assert records(root, 'global', 'text') == records(root, 'a', 'text') == []

    assert records(roots['encoding'], 'encoding') == [('INFO', 'adapter once'),
                                                       ('INFO', 'after rejected records')]
    if a.mode != 'BINARY':
        assert records(roots['text_limits'], 'text_limits') == [
            ('INFO', 'before oversized text'), ('INFO', 'after oversized text')]
        assert records(roots['text_limits'], 'text_budget') == [('INFO', 'budget survives')]
    for name, messages in {'timer': ['timer event'], 'manual': ['manual event'],
                           'severity': ['buffered before error', 'severity event'],
                           'sync_timer': ['sync timer event'], 'sync_level': ['sync level event']}.items():
        assert [m for _, m in records(roots['periodic'], name)] == messages
    for i in range(1, 5):
        assert ('INFO', 'recovered') in records(roots['diagnostics'], f'fault{i}')
    if a.mode != 'BINARY':
        assert records(roots['diagnostics'], 'format_error') == [('INFO', 'after bad format')]
    assert records(roots['diagnostics'], 'producer_limit') == [('INFO', 'owns producer slot')]
    churn = records(roots['churn'], 'churn')
    last = int(churn[-1][1].split()[-1])
    assert len(churn) == 3 and churn == [('INFO', f'generation {i}') for i in range(last - 2, last + 1)]
    assert records(roots['close'], 'survivor') == [('INFO', f'survivor {i}') for i in range(200)]
    assert records(roots['conflicts'], 'live') == [('INFO', 'live'), ('INFO', 'replacement')]
    assert records(roots['conflicts'], 'retry') == [('INFO', 'retry succeeds')]
    assert records(roots['conflicts'], 'copy_failure') == [('INFO', 'after copy failure')]
    for name, offset in (('a', 0), ('b', 100)):
        assert records(roots['thread_churn'], name) == [('INFO', f'custom {offset + i}') for i in range(40)]
    assert records(roots['resource_budget'], 'small_queue') == [('INFO', 'small 7')]
    assert records(roots['custom_sink'], 'with_sink') == [('INFO', 'sink 19')]
    assert records(roots['shared_dispatcher'], 'dispatch_a') == [
        ('INFO', f'shared a {i}') for i in range(100)]
    assert records(roots['shared_dispatcher'], 'dispatch_b') == [
        *(( 'INFO', f'shared b {i}') for i in range(100)),
        ('INFO', 'shared after close')]
    root = roots['rotation']
    suffix = '.log' if a.mode == 'TEXT' else '.blog'
    files = segments(root, 'rotate', suffix)
    assert 1 <= len(files) <= 3 and all(p.stat().st_size <= 1800 for p in files)
    assert records(root, 'other') == [('INFO', f'other batch {i}') for i in range(9, 200, 10)]

    # Both forms must reject invalid formats and unsupported level tokens.
    for call in ('ZX_LOG(INFO, "{:x}", 1.5)', 'ZX_LOG(logger, INFO, "{:x}", 1.5)',
                 'ZX_LOG(logger, UNKNOWN, "bad level")'):
        source = '#include "zxlog/zxlog.h"\nvoid f(){ auto &logger = zxlog::Logger::instance(); ' + call + '; }'
        result = subprocess.run(['g++', '-std=c++17', '-fsyntax-only', '-I' + str(ROOT / 'include'),
                                 '-I' + str(ROOT / 'third_party/fmt/include'), '-x', 'c++', '-'],
                                input=source, text=True, capture_output=True, timeout=60)
        assert result.returncode != 0, call
        if '{:x}' in call:
            assert 'fmt' in result.stderr.lower(), result.stderr
    Path(a.stamp).write_text(f'PASS multi-logger {a.mode}\n')
    print(f'PASS multi-logger {a.mode}: isolation, lifecycle, TLS limits, conflicts, rotation, decoding')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--dict', required=True)
    parser.add_argument('--cpp-decoder', required=True)
    parser.add_argument('--mode', required=True, choices=('TEXT', 'BINARY', 'RUNTIME'))
    parser.add_argument('--work', required=True)
    parser.add_argument('--stamp', required=True)
    main(parser.parse_args())
