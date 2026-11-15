#!/usr/bin/env python3
"""Record a new, unvalidated lock for rebuilt pinned FFmpeg dependencies.

The official lock is never overwritten. Recording identity is not validation;
the new inputs need the same playback, color, ownership and release checks.
"""
import argparse
import copy
import json
from pathlib import Path
from dependency_inputs import digest, inventory_ffmpeg


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-lock', type=Path, default=repo / 'build/dependencies.lock.json')
    parser.add_argument('--ffmpeg', type=Path, required=True, help='Freshly rebuilt FFmpeg prefix including its build-manifest.json')
    parser.add_argument('--platform', choices=['windows', 'linux'], required=True)
    parser.add_argument('--output', type=Path, required=True, help='New developer lock file')
    args = parser.parse_args()
    base = args.base_lock.resolve(strict=True); output = args.output.resolve()
    if output == base or output.exists():
        raise RuntimeError('Choose a new developer lock path; existing locks are never overwritten')
    lock = json.loads(base.read_text(encoding='utf-8'))
    prefix = args.ffmpeg.resolve(strict=True)
    inputs = inventory_ffmpeg(prefix)
    recipe = json.loads((prefix / 'build-manifest.json').read_text(encoding='utf-8'))
    if recipe.get('target') != args.platform:
        raise RuntimeError('FFmpeg recipe target does not match requested platform')
    if recipe.get('ffmpeg_commit') != lock['ffmpeg']['commit'] or recipe.get('nvcodec_commit') != lock['nv_codec_headers']['commit']:
        raise RuntimeError('Source revisions differ from the base lock; record and review new source pins before recording rebuilt artifacts')
    configure = recipe.get('configure', [])
    if '--disable-gpl' not in configure or '--disable-nonfree' not in configure or '--enable-gpl' in configure or '--enable-nonfree' in configure:
        raise RuntimeError('The receiver release recipe requires GPL and nonfree components to remain disabled')
    updated = copy.deepcopy(lock)
    updated['schema'] = 2
    updated.setdefault('ffmpeg_inputs', {})[args.platform] = inputs
    updated['ffmpeg'][args.platform] = recipe
    archive_prefix = 'ffmpeg-static-' + args.platform + '/'
    for name in list(updated['archive_sha256']):
        if name.replace('\\', '/').startswith(archive_prefix):
            del updated['archive_sha256'][name]
    for component in ['avcodec', 'avutil', 'swscale']:
        name = 'lib/lib' + component + '.a'
        updated['archive_sha256'][archive_prefix + name] = inputs['files'][name]['sha256']
    updated['dependency_variant'] = {'kind': 'developer', 'base_lock_sha256': digest(base),
                                     'rebuilt_platform': args.platform, 'validated': False,
                                     'meaning': 'Recorded artifact identity only; revalidation is required.'}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as destination:
        json.dump(updated, destination, indent=2)
        destination.write('\n')
    print(json.dumps({'developer_lock': str(output), 'sha256': digest(output), 'validated': False}, indent=2))


if __name__ == '__main__':
    main()
