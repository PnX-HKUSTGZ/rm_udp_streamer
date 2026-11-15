"""Inventory and verify every file in a prepared FFmpeg build prefix.

Relative names make a verified prefix relocatable. The original build recipe
is provenance; CMake receives the current prefix explicitly when it builds.
"""
import hashlib
import json
import re
import stat
from pathlib import Path, PurePosixPath


def linked(path):
    # Path.is_junction is unavailable in older supported Python versions.
    return path.is_symlink() or bool(getattr(path.lstat(), 'st_file_attributes', 0) &
                                    getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def input_name(name):
    """Only portable, prefix-relative input paths belong in this manifest."""
    if not isinstance(name, str) or not name or '\\' in name or ':' in name:
        raise ValueError('Invalid dependency input name: ' + repr(name))
    path = PurePosixPath(name)
    if path.is_absolute() or name != path.as_posix() or any(part in ('.', '..') for part in path.parts):
        raise ValueError('Dependency input escapes its prefix: ' + repr(name))
    if name != 'build-manifest.json' and path.parts[0] not in ('include', 'lib'):
        raise ValueError('Unexpected dependency input: ' + name)
    return name


def inventory_ffmpeg(prefix):
    prefix = Path(prefix).resolve(strict=True)
    files = {}
    folded = set()
    roots = [prefix / 'include', prefix / 'lib']
    for root in roots:
        if not root.is_dir():
            raise RuntimeError('Missing FFmpeg input directory: ' + str(root))
    candidates = [prefix / 'build-manifest.json']
    for root in roots:
        if linked(root):
            raise RuntimeError('Linked dependency input directory: ' + str(root))
        candidates.extend(sorted(root.rglob('*')))
    for file in candidates:
        if linked(file):
            raise RuntimeError('Linked dependency input is not allowed: ' + str(file))
        if file.is_dir():
            continue
        if not file.is_file():
            raise RuntimeError('Missing or non-regular FFmpeg input: ' + str(file))
        name = input_name(file.relative_to(prefix).as_posix())
        if name.casefold() in folded:
            raise RuntimeError('Case-ambiguous dependency input: ' + name)
        folded.add(name.casefold())
        files[name] = {'bytes': file.stat().st_size, 'sha256': digest(file)}
    for name in ['lib/libavcodec.a', 'lib/libavutil.a', 'lib/libswscale.a']:
        if name not in files:
            raise RuntimeError('Missing FFmpeg archive: ' + name)
    return {'schema': 1, 'files': files}


def validate_input_manifest(expected):
    if not isinstance(expected, dict):
        raise RuntimeError('Invalid dependency input manifest')
    if expected.get('schema') != 1 or not isinstance(expected.get('files'), dict):
        raise RuntimeError('Dependency lock lacks a complete FFmpeg input manifest')
    folded = set()
    for name, item in expected['files'].items():
        input_name(name)
        if name.casefold() in folded:
            raise RuntimeError('Case-ambiguous manifest path: ' + name)
        folded.add(name.casefold())
        if not isinstance(item, dict) or set(item) != {'bytes', 'sha256'}:
            raise RuntimeError('Invalid dependency input metadata: ' + name)
        if type(item['bytes']) is not int or item['bytes'] < 0 or not isinstance(item['sha256'], str) or not re.fullmatch(r'[0-9a-f]{64}', item['sha256']):
            raise RuntimeError('Invalid dependency input digest/size: ' + name)


def verify_ffmpeg_inputs(prefix, expected):
    validate_input_manifest(expected)
    actual = inventory_ffmpeg(prefix)
    want = expected['files']; found = actual['files']
    missing = sorted(set(want) - set(found))
    added = sorted(set(found) - set(want))
    changed = sorted(name for name in set(want) & set(found) if want[name] != found[name])
    if missing or added or changed:
        raise RuntimeError('FFmpeg inputs differ from lock: ' + json.dumps(
            {'missing': missing, 'unexpected': added, 'changed': changed}, ensure_ascii=False))
    return actual
