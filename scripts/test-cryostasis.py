#!/usr/bin/env python3
"""Compile the production bloom/capture functions and blur shader against a CPU renderer.

No game, GPU or external packages needed. The independent reference uses tightly
sized images, rather than the engine's packed subregions. This catches stale
border reads, rounded-extent drift and one-pixel downsampling mistakes.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source', type=Path, default=ROOT)
    args = parser.parse_args()
    source = args.source
    cpp = (source / 'game/client/viewpostprocess.cpp').read_text()
    functions = cpp[cpp.index('static void SetCryostasisMaterialTexture'):cpp.index('CON_COMMAND( hl2_cryostasis')]
    shader = (source / 'materialsystem/stdshaders/cryostasis_magichdr_blur_ps2x.fxc').read_text()
    shader = shader[shader.index('sampler BaseTextureSampler'):]
    shader = shader.replace(' : register( s0 )', '').replace(' : register( c0 )', '').replace(' : register( c1 )', '')
    shader = shader.replace(' : TEXCOORD0', '').replace(' : COLOR', '')
    for swizzle in ('xy', 'zw'):
        shader = shader.replace('.' + swizzle, '.' + swizzle + '()')
    shader = shader.replace('float4 main(', 'float4 BlurShader(')
    fixture = (ROOT / 'scripts/tests/cryostasis/renderer.cpp').read_text()
    fixture = fixture.replace('// INSERT_SHADER', shader).replace('// INSERT_PRODUCTION', functions)
    with tempfile.TemporaryDirectory(prefix='cryostasis-test-') as directory:
        path = Path(directory)
        (path / 'test.cpp').write_text(fixture)
        flags = ['-std=c++11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(['c++', *flags, '-I', str(source / 'public'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    main()
