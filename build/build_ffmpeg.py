#!/usr/bin/env python3
"""Rebuild pinned LGPL FFmpeg in a new directory (Windows via MinGW)."""
import argparse
import os
from pathlib import Path
import subprocess
from ffmpeg_build import execute_build, snapshot_checkout

FFMPEG_COMMIT = '1041abdc962f4cc4f394aa8de9dc5236c0c3b9e7'  # n8.1.3
NVCODEC_COMMIT = 'f2053071ea93137255b6dcf42e9b4c9e0965a44e'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--target', choices=['linux', 'windows'], required=True)
    parser.add_argument('--cross-tools', type=Path, help='Extracted MinGW tools root (contains usr/bin)')
    parser.add_argument('--work', type=Path, help='New build directory; the input cache is never overwritten')
    parser.add_argument('--cc', help='Explicit C compiler; its version and digest are recorded')
    parser.add_argument('--make', default='make')
    parser.add_argument('--pkg-config', default='pkg-config')
    parser.add_argument('--nasm', help='Explicit NASM; otherwise use cross-tools/usr/bin/nasm or PATH')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--verify-only', action='store_true', help='Check pinned, clean source checkouts without building')
    args = parser.parse_args()
    cache = args.cache.resolve()
    source = cache / 'ffmpeg-source'
    headers = cache / 'nv-codec-headers'
    for path, revision in [(source, FFMPEG_COMMIT), (headers, NVCODEC_COMMIT)]:
        actual = subprocess.check_output(['git', '-C', path, 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise RuntimeError(f'{path} revision {actual} != pinned {revision}')
        dirty = subprocess.check_output(['git', '-C', path, 'status', '--porcelain', '--untracked-files=normal'], text=True)
        if dirty.strip():
            raise RuntimeError(f'{path} contains unrecorded source changes:\n{dirty}')
    if args.verify_only:
        print('Pinned source checkouts are clean', flush=True)
        return
    if os.name == 'nt':
        parser.error('Build FFmpeg under Linux/WSL')
    if args.target == 'windows' and not args.cross_tools:
        parser.error('--cross-tools is required for Windows')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    work = (args.work or cache / ('ffmpeg-rebuild-' + args.target)).absolute()
    work.mkdir(parents=True, exist_ok=False)
    roots, provenance = {}, {}
    for name, path, commit in [('ffmpeg', source, FFMPEG_COMMIT), ('nv-codec-headers', headers, NVCODEC_COMMIT)]:
        roots[name], provenance[name] = snapshot_checkout(path, commit, work, name)
    command = ['--disable-autodetect',
               '--disable-shared', '--enable-static', '--enable-pic', '--disable-programs',
               '--disable-doc', '--disable-debug', '--disable-everything', '--disable-avformat',
               '--disable-avdevice', '--disable-avfilter', '--disable-swresample',
               '--enable-avcodec', '--enable-avutil', '--enable-swscale', '--enable-decoder=hevc',
               '--enable-parser=hevc', '--enable-ffnvcodec', '--enable-cuda', '--enable-nvdec',
               '--disable-vulkan', '--disable-gpl', '--disable-nonfree', '--enable-optimizations']
    if args.target == 'windows':
        command += ['--target-os=mingw32', '--arch=x86_64', '--enable-cross-compile',
                    '--enable-d3d11va', '--enable-hwaccel=hevc_d3d11va,hevc_d3d11va2,hevc_nvdec']
    else:
        command += ['--enable-pthreads', '--enable-hwaccel=hevc_nvdec']
    cross = str(args.cross_tools.resolve() / 'usr/bin/x86_64-w64-mingw32-') if args.target == 'windows' else None
    nasm = args.nasm or (str(args.cross_tools.resolve() / 'usr/bin/nasm') if args.cross_tools else 'nasm')
    execute_build(work, roots, provenance, args.target, command, args.jobs, cross,
                  args.cc, args.make, args.pkg_config, nasm)


if __name__ == '__main__':
    main()
