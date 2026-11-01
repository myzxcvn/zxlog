#!/usr/bin/env python3
"""Produce a runtime image without its build-only ELF log dictionary section."""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--input', required=True)
    p.add_argument('--output', required=True)
    a = p.parse_args()
    objcopy = shutil.which('objcopy') or shutil.which('llvm-objcopy')
    if not objcopy:
        raise SystemExit('objcopy or llvm-objcopy is required to strip metadata')
    Path(a.output).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([objcopy, '--remove-section', 'zxlog_meta', a.input, a.output], check=True)
    Path(a.output).chmod(Path(a.output).stat().st_mode | 0o111)

if __name__ == '__main__':
    main()
