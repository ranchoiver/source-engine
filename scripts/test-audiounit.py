#!/usr/bin/env python3
"""Compile and exercise the real AudioUnit backend with a fake CoreAudio boundary."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
TESTS = ['pcm-order', 'wrap-copy', 'partial-underrun', 'long-underrun',
         'tiny-mixahead', 'zero-mixahead', 'mix-budget-bounds', 'silence-flag', 'engine-rebase', 'engine-clock-rebase', 'movie-transition', 'signed-arithmetic', 'counter-wrap',
         'short-buffer', 'zero-capacity', 'null-storage', 'scaled-null-storage', 'bad-layout',
         'clear-buffer', 'stop-failure', 'nested-pause', 'start-failure',
         'device-change', 'change-during-recovery', 'create-failure',
         'failed-pause-recovery', 'restart-grace', 'no-callback-recovery', 'underrun-not-stall', 'late-listener',
         'movie-silence', 'fifo-full', 'allocation-failure', 'max-slice-failure', 'large-slice', 'spsc-stress']

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'engine/audio/snd_dev_mac_audiounit.cpp')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--sanitize',action='store_true')
    mode.add_argument('--thread-sanitize',action='store_true')
    parser.add_argument('--native-syntax',action='store_true')
    parser.add_argument('--benchmark',action='store_true')
    parser.add_argument('--test',choices=TESTS)
    args=parser.parse_args()
    compiler=shlex.split(os.environ.get('CXX','c++'))
    with tempfile.TemporaryDirectory(prefix='audiounit-tests-') as tmp:
        work=Path(tmp)
        shutil.copytree(ROOT/'scripts/tests/audioqueue',work/'audioqueue')
        shutil.copytree(ROOT/'scripts/tests/audiounit',work/'audiounit')
        unit=work/'audiounit'
        shutil.copy2(args.source,unit/'snd_dev_mac_audiounit.cpp')
        flags=['-std=c++11','-Wall','-Wextra','-Wno-unused-parameter','-O2','-g','-pthread']
        if args.sanitize:
            flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        if args.thread_sanitize:
            flags+=['-fsanitize=thread','-fno-omit-frame-pointer']
        include=['-I',str(unit),'-I',str(work/'audioqueue')]
        exe=work/'audiounit-tests'
        subprocess.run(compiler+flags+include+[str(unit/'audiounit_test.cpp'),'-o',str(exe)],check=True)
        failed=[]
        for name in ([args.test] if args.test else TESTS):
            if subprocess.run([str(exe),name],timeout=60).returncode: failed.append(name)
        if args.benchmark:
            subprocess.run([str(exe),'benchmark'],check=True)
        if args.native_syntax:
            if sys.platform!='darwin': parser.error('--native-syntax requires macOS')
            for folder in ['AudioToolbox','AudioUnit','CoreAudio']:
                shutil.rmtree(unit/folder)
            shutil.rmtree(work/'audioqueue/AudioToolbox')
            # Compile the production translation unit for both supported Macs.
            for arch in ['arm64','x86_64']:
                subprocess.run(compiler+['-std=c++11','-Wall','-Wextra','-Wno-unused-parameter',
                    '-arch',arch,'-fsyntax-only']+include+[str(unit/'snd_dev_mac_audiounit.cpp')],check=True)
        if failed:
            print('Failed: '+', '.join(failed),file=sys.stderr)
            return 1
        print('Passed {} AudioUnit regressions.'.format(1 if args.test else len(TESTS)))
    return 0

if __name__=='__main__': sys.exit(main())
