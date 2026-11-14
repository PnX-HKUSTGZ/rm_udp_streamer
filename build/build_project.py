#!/usr/bin/env python3
"""Build the receiver using the dependency cache prepared in M0."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=repo.parent / '.tools/rm-video')
    parser.add_argument('--cmake', help='Explicit CMake executable')
    parser.add_argument('--dotnet', help='Explicit .NET SDK executable')
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    cache = args.cache.resolve()
    windows = os.name == 'nt'
    platform = 'windows' if windows else 'linux'
    cmake = args.cmake or shutil.which('cmake')
    if not cmake and windows:
        cmake = str(cache / 'python-tools/cmake/data/bin/cmake.exe')
    if not cmake:
        raise RuntimeError('CMake is missing; prepare the build tools or pass --cmake')
    output = repo / 'out' / platform
    command = [cmake, '-S', str(repo / 'native'), '-B', str(output), '-DCMAKE_BUILD_TYPE=Release',
               '-DGODOT_CPP_DIR=' + (cache / 'godot-cpp').as_posix(),
               '-DFFMPEG_ROOT=' + (cache / ('ffmpeg-static-' + platform)).as_posix()]
    if windows:
        compiler = shutil.which('g++')
        make = shutil.which('mingw32-make')
        if not compiler or not make:
            raise RuntimeError('The verified Windows build requires MinGW g++ and mingw32-make')
        command += ['-G', 'MinGW Makefiles', '-DCMAKE_CXX_COMPILER=' + Path(compiler).as_posix(),
                    '-DCMAKE_MAKE_PROGRAM=' + Path(make).as_posix()]
    subprocess.run(command, check=True)
    subprocess.run([cmake, '--build', str(output), '--parallel', str(args.jobs)], check=True)
    if not args.native_only:
        dotnet = args.dotnet or shutil.which('dotnet')
        env = os.environ.copy()
        local = cache / 'dotnet-8.0.428/dotnet'
        if not dotnet and local.exists():
            dotnet = str(local)
            env['DOTNET_ROOT'] = str(local.parent)
        if not dotnet:
            raise RuntimeError('.NET SDK 8.0.428 is missing; pass --dotnet')
        subprocess.run([dotnet, 'build', str(repo / 'RmUdpStreamer.csproj')], env=env, check=True)

if __name__ == '__main__':
    main()
