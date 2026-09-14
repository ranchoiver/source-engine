#!/usr/bin/env python3
"""Run the production AudioQueue backend against a deterministic fake platform.

Requires Python 3 and a GCC/Clang-compatible C++ compiler (CXX overrides it).
--native-syntax also checks the backend against Apple's real SDK on macOS.
No game assets, engine build, audio devices, or third-party Python modules needed.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
TESTS = ['prime-order', 'early-buffer-return', 'tiny-mixahead', 'long-session',
         'counter-wrap', 'signed-counter-boundary', 'stop-failure', 'dispose-after-stop-failure',
         'pause-stays-paused', 'nested-pause', 'restart-grace', 'enqueue-failure',
         'prime-failure', 'start-failure', 'device-change-cooldown', 'create-failure',
         'stall-backoff', 'paint-frontier']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true', help='Enable address and undefined-behavior sanitizers')
    parser.add_argument('--native-syntax', action='store_true', help='Also compile against the macOS SDK')
    parser.add_argument('--source', type=Path, default=ROOT / 'engine/audio/snd_dev_mac_audioqueue.cpp',
                        help='Backend source to test; supports reproducing failures on earlier revisions')
    args = parser.parse_args()
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    if not shutil.which(compiler[0]):
        parser.error('C++ compiler unavailable; set CXX to g++ or clang++')
    with tempfile.TemporaryDirectory(prefix='audioqueue-tests-') as tmp:
        work = Path(tmp)
        shutil.copytree(ROOT / 'scripts/tests/audioqueue', work, dirs_exist_ok=True)
        shutil.copy2(args.source, work / 'snd_dev_mac_audioqueue.cpp')
        flags = ['-std=c++11', '-Wall', '-Wextra', '-Wno-unused-parameter', '-O1', '-g']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        exe = work / ('audioqueue-tests.exe' if os.name=='nt' else 'audioqueue-tests')
        subprocess.run(compiler+flags+['-I',str(work),str(work/'audioqueue_test.cpp'),'-o',str(exe)], check=True)
        failed = []
        for name in TESTS:
            if subprocess.run([str(exe),name]).returncode:
                failed.append(name)
        if args.native_syntax:
            if sys.platform != 'darwin':
                parser.error('--native-syntax requires macOS and the Xcode command-line tools')
            shutil.rmtree(work / 'AudioToolbox')
            subprocess.run(compiler+flags+['-fsyntax-only','-I',str(work),str(work/'snd_dev_mac_audioqueue.cpp')], check=True)
        if failed:
            print('Failed: '+', '.join(failed), file=sys.stderr)
            return 1
        print('Passed {} AudioQueue behavioral regressions.'.format(len(TESTS)))
    return 0

if __name__ == '__main__':
    sys.exit(main())
