#!/usr/bin/env python3
"""Restore locked FFmpeg inputs and fetch the pinned godot-cpp source checkout.

FFmpeg may come from an intact prefix, an extracted receiver release, or its
ZIP/tar archive. Build tools are selected separately by build_project.py.
Unknown cache contents are never overwritten; failed staging is retained.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tarfile
import uuid
import zipfile
from dependency_inputs import digest, input_name, validate_input_manifest, verify_ffmpeg_inputs


def publish(source, target, cache):
    """Commit a verified directory without replacing any existing target."""
    # Both rename endpoints must remain direct children of the requested cache.
    root = cache.resolve(strict=True)
    if source.resolve(strict=True).parent != root or target.resolve().parent != root:
        raise RuntimeError('Dependency publication escapes the requested cache')
    if os.name == 'nt':
        source.rename(target)  # Windows rename fails if any target exists.
        return
    library = ctypes.CDLL(None, use_errno=True)
    rename = getattr(library, 'renameat2', None)
    if rename is None:
        raise RuntimeError('Atomic no-replace directory publication requires Linux renameat2')
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(source), -100, os.fsencode(target), 1):
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(target))


def checked_copy(source, target, expected):
    target.parent.mkdir(parents=True, exist_ok=True)
    count = 0; checksum = hashlib.sha256()
    with target.open('xb') as output:
        while True:
            block = source.read(min(1024 * 1024, expected['bytes'] - count + 1))
            if not block:
                break
            count += len(block)
            if count > expected['bytes']:
                raise RuntimeError('Dependency member exceeds locked size: ' + str(target))
            checksum.update(block); output.write(block)
    if count != expected['bytes'] or checksum.hexdigest() != expected['sha256']:
        raise RuntimeError('Dependency member differs from lock: ' + str(target))


def archive_members(source, expected, stage):
    """Read only the exact input allowlist; never extract archive paths."""
    zipped = zipfile.is_zipfile(source)
    with zipfile.ZipFile(source) if zipped else tarfile.open(source, 'r:*') as archive:
        entries = archive.infolist() if zipped else archive.getmembers()
        name_of = (lambda item: item.filename) if zipped else (lambda item: item.name)
        is_directory = (lambda item: item.is_dir()) if zipped else (lambda item: item.isdir())
        candidates = [name_of(item)[:-len('build-manifest.json')] for item in entries
                      if name_of(item).endswith('relink/ffmpeg/build-manifest.json')]
        if len(candidates) != 1:
            raise RuntimeError('Release archive must contain exactly one relink/ffmpeg/build-manifest.json')
        prefix = candidates[0]; found = {}
        for item in entries:
            name = name_of(item)
            if not name.startswith(prefix) or is_directory(item):
                continue
            relative = name[len(prefix):]
            if relative != 'build-manifest.json' and not relative.startswith(('include/', 'lib/')):
                continue
            input_name(relative)
            if relative in found:
                raise RuntimeError('Duplicate dependency member: ' + relative)
            regular = stat.S_IFMT(item.external_attr >> 16) in (0, stat.S_IFREG) if zipped else item.isfile()
            if not regular:
                raise RuntimeError('Linked or non-regular dependency member: ' + relative)
            found[relative] = item
        if set(found) != set(expected['files']):
            raise RuntimeError('Release archive dependency file list differs from lock')
        for name, metadata in expected['files'].items():
            item = found[name]
            size = item.file_size if zipped else item.size
            if size != metadata['bytes']:
                raise RuntimeError('Archive member size differs from lock: ' + name)
            stream = archive.open(item) if zipped else archive.extractfile(item)
            with stream:
                checked_copy(stream, stage / name, metadata)


def restore_ffmpeg(cache, platform, source, lock):
    expected = lock.get('ffmpeg_inputs', {}).get(platform)
    if expected is None:
        raise RuntimeError('Lock has no complete FFmpeg inputs for ' + platform)
    # Validate the manifest before using any of its names as output paths.
    validate_input_manifest(expected)
    target = cache / ('ffmpeg-static-' + platform)
    if target.exists():
        verify_ffmpeg_inputs(target, expected)
        return {'status': 'already verified', 'files': len(expected['files'])}
    if source is None:
        raise RuntimeError('Pass --ffmpeg with an intact dependency prefix or receiver release')
    source = source.resolve(strict=True)
    stage = cache / ('.ffmpeg-' + platform + '-' + uuid.uuid4().hex)
    stage.mkdir()
    try:
        if source.is_dir():
            prefix = source / 'relink/ffmpeg' if (source / 'relink/ffmpeg').is_dir() else source
            verify_ffmpeg_inputs(prefix, expected)
            for name, metadata in expected['files'].items():
                with (prefix / name).open('rb') as stream:
                    checked_copy(stream, stage / name, metadata)
            origin = {'path': str(source), 'type': 'directory'}
        else:
            archive_members(source, expected, stage)
            origin = {'path': str(source), 'type': 'archive', 'sha256': digest(source)}
        verify_ffmpeg_inputs(stage, expected)
        publish(stage, target, cache)
        return {'status': 'restored', 'files': len(expected['files']), 'source': origin}
    except Exception as error:
        raise RuntimeError('Dependency restore failed; staging retained at ' + str(stage) + ': ' + str(error)) from error


def check_checkout(path, commit):
    actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != commit:
        raise RuntimeError('godot-cpp commit differs from lock: ' + str(path))
    if subprocess.check_output(['git', '-C', str(path), 'status', '--porcelain', '--untracked-files=all'], text=True).strip():
        raise RuntimeError('godot-cpp contains unrecorded changes: ' + str(path))


def prepare_godot_cpp(cache, source, lock):
    commit = lock['godot_cpp']['commit']; target = cache / 'godot-cpp'
    if target.exists():
        check_checkout(target, commit)
        return {'status': 'already verified', 'commit': commit}
    stage = cache / ('.godot-cpp-' + uuid.uuid4().hex)
    command = ['git', 'clone', '--no-checkout']
    if source is not None:
        source = source.resolve(strict=True); check_checkout(source, commit)
        command += ['--no-hardlinks', str(source)]
    else:
        command += ['--filter=blob:none', lock['godot_cpp']['repository']]
    command.append(str(stage))
    try:
        subprocess.run(command, check=True)
        subprocess.run(['git', '-C', str(stage), 'checkout', '--detach', commit], check=True)
        check_checkout(stage, commit)
        publish(stage, target, cache)
    except Exception as error:
        raise RuntimeError('godot-cpp preparation failed; staging retained at ' + str(stage)) from error
    return {'status': 'prepared', 'commit': commit}


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--platform', choices=['windows', 'linux'], default='windows' if os.name == 'nt' else 'linux')
    parser.add_argument('--ffmpeg', type=Path, help='Prefix, extracted receiver release, or release ZIP/tar archive')
    parser.add_argument('--godot-cpp-source', type=Path, help='Reuse a clean local pinned checkout instead of fetching GitHub')
    parser.add_argument('--dependency-lock', type=Path, default=repo / 'build/dependencies.lock.json')
    parser.add_argument('--components', choices=['ffmpeg', 'godot-cpp'], nargs='+', default=['ffmpeg', 'godot-cpp'])
    args = parser.parse_args()
    cache = args.cache.resolve(); cache.mkdir(parents=True, exist_ok=True)
    lock_path = args.dependency_lock.resolve(); lock = json.loads(lock_path.read_text(encoding='utf-8'))
    report = {'dependency_lock_sha256': digest(lock_path), 'platform': args.platform}
    if 'ffmpeg' in args.components:
        report['ffmpeg'] = restore_ffmpeg(cache, args.platform, args.ffmpeg, lock)
    if 'godot-cpp' in args.components:
        report['godot_cpp'] = prepare_godot_cpp(cache, args.godot_cpp_source, lock)
    receipt = cache / ('prepare-' + args.platform + '-' + uuid.uuid4().hex + '.json')
    receipt.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
