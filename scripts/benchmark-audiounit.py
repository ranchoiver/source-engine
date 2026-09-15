#!/usr/bin/env python3
"""Compare actual backend revisions with identical fake-platform benchmark code.

This measures transport CPU costs on this host, not device latency or a full
engine mixer. Runs alternate baseline/candidate order to reduce drift bias.
"""
import argparse
import json
import hashlib
import os
from pathlib import Path
import platform
import shlex
import shutil
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref',default='84a5813182c10d7333bd029bdb531bb349fc8dd5')
    parser.add_argument('--candidate',type=Path,default=ROOT/'engine/audio/snd_dev_mac_audiounit.cpp')
    parser.add_argument('--repetitions',type=int,default=7)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.repetitions<3: parser.error('use at least three repetitions')
    compiler=shlex.split(os.environ.get('CXX','c++'))
    baseline=subprocess.check_output(['git','show',args.baseline_ref+':engine/audio/snd_dev_mac_audiounit.cpp'],cwd=ROOT,text=True)
    results={'baseline':[], 'candidate':[]}
    concurrent={'baseline':[], 'candidate':[]}
    with tempfile.TemporaryDirectory(prefix='audiounit-bench-') as tmp:
        exes={}
        for label,source in [('baseline',baseline),('candidate',args.candidate.read_text())]:
            work=Path(tmp)/label
            shutil.copytree(ROOT/'scripts/tests/audioqueue',work/'audioqueue')
            shutil.copytree(ROOT/'scripts/tests/audiounit',work/'audiounit')
            unit=work/'audiounit'
            (unit/'snd_dev_mac_audiounit.cpp').write_text(source)
            exe=work/'benchmark'
            subprocess.run(compiler+['-std=c++11','-O2','-pthread','-I',str(unit),'-I',str(work/'audioqueue'),str(unit/'audiounit_test.cpp'),'-o',str(exe)],check=True)
            exes[label]=exe
        for repetition in range(args.repetitions):
            for label in (['baseline','candidate'] if repetition%2==0 else ['candidate','baseline']):
                output=subprocess.check_output([str(exes[label]),'benchmark'],text=True,timeout=60)
                results[label].append([json.loads(line) for line in output.splitlines() if line.startswith('{')])
                output=subprocess.check_output([str(exes[label]),'benchmark-concurrent'],text=True,timeout=60)
                concurrent[label].append(next(json.loads(line)['million_frames_per_second'] for line in output.splitlines() if line.startswith('{')))
    summaries=[]
    for index,frames in enumerate([32,128,512,4096]):
        checksums={run[index]['checksum'] for runs in results.values() for run in runs}
        if len(checksums)!=1: raise RuntimeError('Benchmark output differs between runs/revisions')
        summary={'frames':frames}
        for metric in ['pair_mean_ns','callback_median_ns','callback_p99_ns']:
            for label in results:
                summary[label+'_'+metric]=statistics.median(run[index][metric] for run in results[label])
        summaries.append(summary)
    report={'platform':platform.platform(),'compiler':subprocess.check_output(compiler+['--version'],text=True).splitlines()[0],
            'baseline_ref':args.baseline_ref,'candidate_source_sha256':hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
            'harness_sha256':hashlib.sha256((ROOT/'scripts/tests/audiounit/audiounit_test.cpp').read_bytes()).hexdigest(),'repetitions':args.repetitions,'summary':summaries,'runs':results,'concurrent_runs_mframes_per_second':concurrent,
            'limits':'Fake mixer/platform, no scheduling cadence, no CPU affinity; clock-call overhead included. These are host microbenchmarks, not game FPS, audible latency or macOS performance.'}
    text=json.dumps(report,indent=2)+'\n'
    if args.output: args.output.write_text(text)
    print(json.dumps(summaries,indent=2))
    print('Concurrent Mframes/s:', {label:statistics.median(values) for label,values in concurrent.items()})

if __name__=='__main__': main()
