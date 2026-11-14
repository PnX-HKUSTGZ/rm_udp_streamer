#!/usr/bin/env python3
"""Build the pinned, LGPL, static HEVC dependency on Linux (Windows via MinGW)."""
import argparse
import json
import os
from pathlib import Path
import subprocess

FFMPEG_COMMIT = '1041abdc962f4cc4f394aa8de9dc5236c0c3b9e7'  # n8.1.3
NVCODEC_COMMIT = 'f2053071ea93137255b6dcf42e9b4c9e0965a44e'


def run(command, cwd=None, env=None, log=None):
    print(' '.join(map(str, command)), flush=True)
    with open(log, 'w') if log else open(os.devnull, 'w') as output:
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=env,
                                stdout=output if log else None, stderr=subprocess.STDOUT)
    if result.returncode:
        if log:
            print(Path(log).read_text(errors='replace')[-8000:], flush=True)
        raise RuntimeError(f'Command failed with exit {result.returncode}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--target', choices=['linux', 'windows'], required=True)
    parser.add_argument('--cross-tools', type=Path, help='Extracted MinGW tools root (contains usr/bin)')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    cache = args.cache.resolve()
    source = cache / 'ffmpeg-source'
    headers = cache / 'nv-codec-headers'
    for path, revision in [(source, FFMPEG_COMMIT), (headers, NVCODEC_COMMIT)]:
        actual = subprocess.check_output(['git', '-C', path, 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise RuntimeError(f'{path} revision {actual} != pinned {revision}')
    prefix = cache / ('ffmpeg-static-' + args.target)
    build = cache / ('ffmpeg-build-' + args.target)
    sdk_headers = cache / 'nvcodec-sdk'
    prefix.mkdir(exist_ok=True)
    build.mkdir(exist_ok=True)
    run(['make', 'install', 'PREFIX=' + str(sdk_headers)], cwd=headers)
    env = os.environ.copy()
    env['PKG_CONFIG_PATH'] = str(sdk_headers / 'lib/pkgconfig')
    if args.cross_tools:
        env['PATH'] = str(args.cross_tools.resolve() / 'usr/bin') + ':' + env['PATH']
    command = [source / 'configure', '--prefix=' + str(prefix), '--disable-autodetect',
               '--disable-shared', '--enable-static', '--enable-pic', '--disable-programs',
               '--disable-doc', '--disable-debug', '--disable-everything', '--disable-avformat',
               '--disable-avdevice', '--disable-avfilter', '--disable-swresample',
               '--enable-avcodec', '--enable-avutil', '--enable-swscale', '--enable-decoder=hevc',
               '--enable-parser=hevc', '--pkg-config=pkg-config', '--enable-ffnvcodec', '--enable-cuda', '--enable-nvdec',
               '--disable-vulkan', '--disable-gpl', '--disable-nonfree', '--enable-optimizations']
    if args.target == 'windows':
        if not args.cross_tools:
            raise RuntimeError('--cross-tools required for Windows build')
        tools = args.cross_tools.resolve() / 'usr/bin'
        command += ['--target-os=mingw32', '--arch=x86_64', '--enable-cross-compile',
                    '--cross-prefix=' + str(tools / 'x86_64-w64-mingw32-'),
                    '--cc=' + str(tools / 'x86_64-w64-mingw32-gcc-win32'),
                    '--enable-d3d11va', '--enable-hwaccel=hevc_d3d11va,hevc_d3d11va2,hevc_nvdec']
    else:
        command += ['--enable-pthreads', '--enable-hwaccel=hevc_nvdec']
    run(command, cwd=build, env=env, log=build / 'configure.log')
    run(['make', '-j', str(args.jobs)], cwd=build, env=env, log=build / 'build.log')
    run(['make', 'install'], cwd=build, env=env, log=build / 'install.log')
    (prefix / 'build-manifest.json').write_text(json.dumps({
        'ffmpeg_commit': FFMPEG_COMMIT, 'nvcodec_commit': NVCODEC_COMMIT,
        'target': args.target, 'configure': list(map(str, command)),
        'libraries': [str(p.relative_to(prefix)) for p in sorted(prefix.glob('lib/*.a'))],
    }, indent=2))
    print('STATIC_FFMPEG_READY', prefix, flush=True)


if __name__ == '__main__':
    main()
