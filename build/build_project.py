#!/usr/bin/env python3
"""Build the receiver using the dependency cache prepared in M0."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from dependency_inputs import verify_ffmpeg_inputs

def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''): value.update(block)
    return value.hexdigest()

def pinned_sources(cache, lock, platform):
    source = cache / 'godot-cpp'
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != lock['godot_cpp']['commit']:
        raise RuntimeError('godot-cpp revision does not match dependencies.lock.json')
    if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip():
        raise RuntimeError('godot-cpp contains unrecorded changes')
    archives = {}
    for raw, expected in lock['archive_sha256'].items():
        name = raw.replace('\\', '/')
        if not name.startswith('ffmpeg-static-' + platform + '/'): continue
        actual = digest(cache / name)
        if actual != expected: raise RuntimeError('FFmpeg archive differs from lock: ' + name)
        archives[name] = actual
    if len(archives) != 3: raise RuntimeError('Expected three pinned FFmpeg archives')
    inputs = lock.get('ffmpeg_inputs', {}).get(platform)
    if inputs is None:
        raise RuntimeError('Dependency lock has no complete FFmpeg input manifest; use the current official lock or record a new developer lock')
    inputs = verify_ffmpeg_inputs(cache / ('ffmpeg-static-' + platform), inputs)
    return {'godot_cpp': revision, 'ffmpeg_archives': archives, 'ffmpeg_inputs': inputs}

def cached_tool(output, key, require_exists=True):
    cache = output / 'CMakeCache.txt'
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(key + ':'):
                path = line.split('=', 1)[1]
                if not require_exists or Path(path).is_file(): return path
    return None

def checked_tool(path, args, expected, label):
    if not path or not Path(path).is_file(): raise RuntimeError(label + ' is missing; select its executable explicitly')
    actual = subprocess.check_output([str(path), *args], text=True).strip().splitlines()[0]
    if actual != expected: raise RuntimeError(label + ': expected ' + expected + ', got ' + actual)
    return {'path': str(Path(path).resolve()), 'version': actual, 'sha256': digest(path)}


def absolute_tool(path):
    # Preserve existing absolute spelling used by a CMake cache on Windows.
    return str(Path(path).resolve()) if path and not Path(path).is_absolute() else path


def select_build_tool(output, key, requested, fallback):
    stored = cached_tool(output, key, require_exists=False)
    available = stored if stored and Path(stored).is_file() else None
    selected = absolute_tool(requested or available or fallback)
    if stored and selected:
        try:
            if Path(stored).samefile(selected):
                # CMake treats g++.EXE and g++.exe as different compiler
                # settings even when Windows resolves both to the same file.
                return stored
        except OSError:
            pass
        if Path(stored).as_posix() != Path(selected).as_posix():
            raise RuntimeError(key + ' differs from the existing build cache; use a fresh checkout/output directory for a different tool path')
    return selected


def build_environment(dotnet=None, compiler=None, make=None):
    env = os.environ.copy()
    paths = []
    if dotnet:
        root = str(Path(dotnet).resolve().parent)
        env['DOTNET_ROOT'] = root
        paths.append(root)
    if os.name == 'nt':
        for executable in [compiler, make]:
            if executable:
                paths.append(str(Path(executable).resolve().parent))
    env['PATH'] = os.pathsep.join([*dict.fromkeys(paths), env.get('PATH', '')])
    return env

def prepare_extension_discovery(repo):
    """Register built extensions before the editor's first filesystem scan.

    The pinned editor can crash on shutdown when it first discovers an
    extension class during that scan (also reproduced with an empty class).
    Its normal startup extension list avoids the late-registration path.
    Preserve other installed extensions when preparing a new checkout.
    The managed project also seeds this list for prebuilt-library checkouts.
    """
    manifest = repo / '.godot/extension_list.cfg'
    manifest.parent.mkdir(parents=True, exist_ok=True)
    entries = manifest.read_text(encoding='utf-8').splitlines() if manifest.exists() else []
    for descriptor in sorted((repo / 'addons').rglob('*.gdextension')):
        entry = 'res://' + descriptor.relative_to(repo).as_posix()
        if entry not in entries:
            entries.append(entry)
    pending = manifest.with_suffix('.cfg.tmp')
    pending.write_text('\n'.join(entries) + '\n', encoding='utf-8')
    pending.replace(manifest)

def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=repo.parent / '.tools/rm-video')
    parser.add_argument('--cmake', help='Explicit CMake executable')
    parser.add_argument('--dotnet', help='Explicit .NET SDK executable')
    parser.add_argument('--cxx', help='Explicit compiler from the pinned toolchain')
    parser.add_argument('--make', help='Explicit GNU Make executable from the pinned toolchain')
    parser.add_argument('--dependency-lock', type=Path, help='Explicit independently recorded developer lock; defaults to the official lock')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    cache = args.cache.resolve()
    windows = os.name == 'nt'
    platform = 'windows' if windows else 'linux'
    official_lock = repo / 'build/dependencies.lock.json'
    lock_path = (args.dependency_lock or official_lock).resolve()
    lock = json.loads(lock_path.read_text(encoding='utf-8'))
    cmake = args.cmake or (str(cache / 'python-tools/cmake/data/bin/cmake.exe') if windows else '/usr/bin/cmake')
    if not cmake:
        raise RuntimeError('CMake is missing; prepare the build tools or pass --cmake')
    output = repo / 'out' / platform
    compiler = select_build_tool(output, 'CMAKE_CXX_COMPILER', args.cxx, shutil.which('g++'))
    cached_dotnet = cache / 'dotnet-8.0.428' / ('dotnet.exe' if windows else 'dotnet')
    dotnet = args.dotnet or (str(cached_dotnet) if cached_dotnet.is_file() else shutil.which('dotnet'))
    make = select_build_tool(output, 'CMAKE_MAKE_PROGRAM', args.make, shutil.which('mingw32-make' if windows else 'make'))
    cmake, compiler, dotnet, make = map(absolute_tool, [cmake, compiler, dotnet, make])
    # Validate the actual tools, rather than merely reporting a lockfile string.
    evidence = pinned_sources(cache, lock, platform)
    official = lock_path == official_lock.resolve() and not lock.get('dependency_variant')
    evidence['dependency_lock'] = {'path': str(lock_path), 'sha256': digest(lock_path),
                                   'kind': 'official' if official else 'developer'}
    evidence['tools'] = {
        'cmake': checked_tool(cmake, ['--version'], 'cmake version ' + ('4.1.3' if windows else '3.28.3'), 'CMake'),
        'cxx': checked_tool(compiler, ['-dumpfullversion'], '15.2.0' if windows else '13.3.0', 'G++'),
        'make': checked_tool(make, ['--version'], 'GNU Make ' + ('4.4.1' if windows else '4.3'), 'Make'),
    }
    if not args.native_only:
        evidence['tools']['dotnet'] = checked_tool(dotnet, ['--version'], lock['dotnet']['sdk'], '.NET SDK')
    if args.verify_only:
        print(json.dumps(evidence, indent=2)); return
    # Neither Godot nor MSBuild should discover staged export source projects.
    for name in ['out', 'dist']:
        folder = repo / name
        folder.mkdir(exist_ok=True)
        (folder / '.gdignore').touch()
    command = [cmake, '-S', str(repo / 'native'), '-B', str(output), '-DCMAKE_BUILD_TYPE=Release',
               '-DGODOT_CPP_DIR=' + (cache / 'godot-cpp').as_posix(),
               '-DFFMPEG_ROOT=' + (cache / ('ffmpeg-static-' + platform)).as_posix(),
               '-DCMAKE_CXX_COMPILER=' + Path(compiler).as_posix(),
               '-DCMAKE_MAKE_PROGRAM=' + Path(make).as_posix(),
               '-DCMAKE_CXX_FLAGS=', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG',
               '-DCMAKE_SHARED_LINKER_FLAGS=', '-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=']
    if windows:
        command += ['-G', 'MinGW Makefiles']
    evidence['configure_command'] = command
    env = build_environment(None if args.native_only else dotnet, compiler, make)
    subprocess.run(command, env=env, check=True)
    subprocess.run([cmake, '--build', str(output), '--parallel', str(args.jobs)], env=env, check=True)
    prepare_extension_discovery(repo)
    if not args.native_only:
        if not dotnet:
            raise RuntimeError('.NET SDK 8.0.428 is missing; pass --dotnet')
        subprocess.run([dotnet, 'build', str(repo / 'RmUdpStreamer.csproj')], cwd=repo, env=env, check=True)
    evidence['native_sha256'] = digest(repo / 'addons/rm_video/bin' / ('rm_video.dll' if windows else 'librm_video.so'))
    (output / 'build-manifest.json').write_text(json.dumps(evidence, indent=2) + '\n')

if __name__ == '__main__':
    main()
