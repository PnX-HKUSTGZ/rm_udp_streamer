#!/usr/bin/env python3
"""Build an independently identified Linux Mono template with the Wayland stop fix.

Creates a new work directory, template archive and developer lock. It never
changes the official lock, editor executable or installed export templates.
Recorded artifacts still require package/playback/exit/performance validation.
"""
import argparse
import copy
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile

COMMIT = 'ed1daf0bf001b61586d9930840f2f1394092c079'
SOURCE_SHA256 = 'e607e9985e1c201bc9cdc1aec8a120f0c3f53b9603f1f828e2b748534a2471ef'
ORIGINAL_SHA256 = '7d20a8705e3ab1be89ec757de18aeb0c0859bda190a78ed9a16d63ccb5aad9ab'
PATCHED_SHA256 = '5bbd53deca8f574f248eea11fd34edcc52edd24f1fa68dc2e18c1f1e882fb8d1'
ASSET = 'Godot_v4.7.2-stable_mono_export_templates.tpz'


def digest(path, algorithm='sha256'):
    result = hashlib.new(algorithm)
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''): result.update(block)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-lock', type=Path, default=Path(__file__).with_name('dependencies.lock.json'))
    parser.add_argument('--source-archive', type=Path, required=True, help='Pinned Godot GitHub source tar.gz')
    parser.add_argument('--official-templates', type=Path, required=True)
    parser.add_argument('--scons-package', type=Path, required=True, help='Isolated directory containing SCons 4.8.1')
    parser.add_argument('--dotnet', type=Path, required=True, help='Pinned .NET 8.0.428 executable')
    parser.add_argument('--godot', type=Path, required=True, help='Pinned upstream Mono editor for packing the license probe')
    parser.add_argument('--cxx', type=Path, default=Path('/usr/bin/g++-13'))
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/gcc-13'))
    parser.add_argument('--work', type=Path, required=True, help='New directory; existing paths are refused')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    if sys.platform != 'linux' or not 1 <= args.jobs <= 64:
        raise RuntimeError('This recipe requires Linux and 1..64 build jobs')
    base = args.base_lock.resolve(strict=True)
    lock = json.loads(base.read_text(encoding='utf-8'))
    if lock['godot']['engine_commit'] != COMMIT or lock['godot'].get('template_variant'):
        raise RuntimeError('Use the pinned upstream engine lock, optionally with rebuilt FFmpeg')
    source_archive = args.source_archive.resolve(strict=True)
    official = args.official_templates.resolve(strict=True)
    original_asset = lock['godot']['assets'][ASSET]
    if digest(source_archive) != SOURCE_SHA256:
        raise RuntimeError('Godot source archive does not match the pinned source SHA256')
    if official.stat().st_size != original_asset['size'] or digest(official, 'sha512') != original_asset['sha512']:
        raise RuntimeError('Original template archive does not match the base lock')
    env = os.environ.copy()
    # Do not import unrelated user packages or compiler flags into this variant.
    for name in ['CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'SCONSFLAGS', 'CC', 'CXX']:
        env.pop(name, None)
    env.update(PYTHONPATH=str(args.scons_package.resolve(strict=True)), PYTHONNOUSERSITE='1',
               DOTNET_ROOT=str(args.dotnet.resolve(strict=True).parent), GODOT_VERSION_STATUS='stable-rm-wayland')
    env['PATH'] = env['DOTNET_ROOT'] + os.pathsep + env.get('PATH', '')
    def version(command): return subprocess.check_output([str(x) for x in command], env=env, text=True).strip()
    tools = {}
    editor = args.godot.resolve(strict=True)
    if digest(editor) != lock['godot']['linux_sha256']:
        raise RuntimeError('License probe editor differs from the pinned upstream editor')
    tools['godot_editor'] = {'path': str(editor), 'sha256': digest(editor)}
    for name, path, arguments, expected in [
        ('cxx', args.cxx, ['-dumpfullversion'], '13.3.0'), ('cc', args.cc, ['-dumpfullversion'], '13.3.0'),
        ('dotnet', args.dotnet, ['--version'], lock['dotnet']['sdk'])]:
        path = path.resolve(strict=True)
        value = version([path, *arguments])
        if value != expected: raise RuntimeError(name + ' version mismatch: ' + value)
        tools[name] = {'path': str(path), 'version': value, 'sha256': digest(path)}
    scons_version = version([sys.executable, '-c', 'import SCons; print(SCons.__version__)'])
    if scons_version != '4.8.1': raise RuntimeError('This recipe pins SCons 4.8.1')
    tools['python'] = {'path': sys.executable, 'version': sys.version, 'sha256': digest(sys.executable)}
    tools['strip'] = {'path': '/usr/bin/strip', 'sha256': digest('/usr/bin/strip'), 'version': version(['/usr/bin/strip', '--version']).splitlines()[0]}
    scons_files = {f.relative_to(args.scons_package).as_posix(): digest(f)
                   for f in sorted((args.scons_package/'SCons').rglob('*.py'))}
    tools['scons'] = {'version': scons_version, 'python_sources': scons_files}
    work = args.work.resolve(); work.mkdir(parents=True, exist_ok=False)
    receipt = {'schema': 1, 'status': 'building', 'validated': False, 'started': time.time(),
               'source_commit': COMMIT, 'source_url': 'https://codeload.github.com/godotengine/godot/tar.gz/' + COMMIT,
               'source_sha256': SOURCE_SHA256, 'base_lock_sha256': digest(base),
               'base_template': original_asset, 'tools': tools, 'scons_version': scons_version,
               'build_platform': platform.platform(), 'build_machine': platform.machine(),
               'recipe_sha256': digest(__file__), 'steps': [],
               'scope': 'Developer Linux release template only. The Linux debug template remains upstream; other platforms are absent.'}
    def save():
        (work / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    def run(name, command, cwd=None, timeout=3600):
        row = {'name': name, 'command': [str(x) for x in command]}; receipt['steps'].append(row); save()
        with (work / (name + '.log')).open('w', encoding='utf-8') as output:
            result = subprocess.run(row['command'], cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
        row['exit'] = result.returncode; save(); result.check_returncode()
    save()
    try:
        with tarfile.open(source_archive, 'r:gz') as archive:
            for member in archive.getmembers():
                name = Path(member.name)
                if name.is_absolute() or '..' in name.parts or name.parts[0] != 'godot-' + COMMIT:
                    raise RuntimeError('Unexpected Godot source archive path')
            archive.extractall(work, filter='data')
        source = work / ('godot-' + COMMIT)
        cpp = source / 'platform/linuxbsd/wayland/wayland_thread.cpp'
        if digest(cpp) != ORIGINAL_SHA256: raise RuntimeError('Upstream Wayland source differs')
        patch = Path(__file__).parent / 'patches/godot-wayland-stop.patch'
        shutil.copy2(patch, work / 'wayland-stop.patch')
        receipt['patch_sha256'] = digest(patch)
        run('apply-patch', ['git', 'apply', '--check', str(patch.resolve())], source)
        run('patch', ['git', 'apply', str(patch.resolve())], source)
        if digest(cpp) != PATCHED_SHA256: raise RuntimeError('Patched Wayland source differs')
        receipt['patched_source_sha256'] = digest(cpp)
        shutil.copy2(cpp, work / 'wayland_thread.cpp')
        run('compile', [sys.executable, '-m', 'SCons', 'platform=linuxbsd', 'target=template_release', 'arch=x86_64',
                        'module_mono_enabled=yes', 'debug_symbols=yes', 'optimize=speed', 'lto=none',
                        'CC=' + str(args.cc.resolve()), 'CXX=' + str(args.cxx.resolve()), '-j' + str(args.jobs)], source)
        binaries = [f for f in (source / 'bin').glob('godot.linuxbsd.template_release.x86_64.mono*') if f.is_file() and os.access(f, os.X_OK)]
        if len(binaries) != 1: raise RuntimeError('Expected one compiled Mono template')
        binary = work / 'linux_release.x86_64'
        run('strip-debug', ['/usr/bin/strip', '--strip-debug', '-o', binary, binaries[0]])
        binary.chmod(0o755)
        receipt['unstripped_sha256'] = digest(binaries[0]); receipt['template_sha256'] = digest(binary)
        probe = work / 'notices-project'; probe.mkdir()
        (probe / 'project.godot').write_text('config_version=5\n[application]\nconfig/name="Engine license extraction"\nrun/main_scene="res://main.tscn"\n', encoding='utf-8')
        script = Path(__file__).with_name('engine_notices.gd').read_text(encoding='utf-8')
        script = script.replace('extends SceneTree', 'extends Node').replace('func _initialize():', 'func _ready():').replace('quit(', 'get_tree().quit(')
        (probe / 'notices.gd').write_text(script, encoding='utf-8')
        (probe / 'main.tscn').write_text('[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://notices.gd" id="1"]\n[node name="Notices" type="Node"]\nscript = ExtResource("1")\n', encoding='utf-8')
        # Release templates reject --path/--script. Use an adjacent packed
        # project so the unmodified release settings remain in effect.
        packer = Path(__file__).with_name('pack_engine_notices.gd')
        env['RM_NOTICES_PROJECT'] = str(probe)
        env['RM_NOTICES_PACK'] = str(probe / 'Notices.pck')
        run('pack-notices', [editor, '--headless', '--path', probe, '--script', packer], timeout=60)
        receipt['notices_probe'] = {name: digest(probe / name) for name in ['project.godot', 'notices.gd', 'main.tscn', 'Notices.pck']}
        receipt['notices_packer_sha256'] = digest(packer)
        shutil.copy2(binary, probe / 'Notices.x86_64')
        env['RM_ENGINE_NOTICES'] = str(work / 'engine-notices.json')
        run('notices', [probe / 'Notices.x86_64', '--headless', '--audio-driver', 'Dummy'], probe, timeout=60)
        notices = json.loads((work / 'engine-notices.json').read_text())
        if not notices.get('license') or not notices.get('copyright') or not notices.get('third_party_licenses'):
            raise RuntimeError('The actual rebuilt engine did not provide a complete license inventory')
        receipt['status'] = 'built-unvalidated'; receipt['finished'] = time.time(); save()
        additions = {'rm-engine-notices.json': work / 'engine-notices.json', 'rm-engine-build.json': work / 'build-receipt.json',
                     'rm-wayland-stop.patch': work / 'wayland-stop.patch', 'rm-wayland_thread.cpp': work / 'wayland_thread.cpp'}
        archive_path = work / 'godot-mono-linux-rm-wayland.tpz'
        with zipfile.ZipFile(official) as upstream, zipfile.ZipFile(archive_path, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
            def add(name, stream):
                info = zipfile.ZipInfo('templates/' + name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED; info.external_attr = 0o100755 << 16
                with archive.open(info, 'w', force_zip64=True) as destination: shutil.copyfileobj(stream, destination)
            for name in ['version.txt', 'linux_debug.x86_64']:
                with upstream.open('templates/' + name) as stream: add(name, stream)
            for name, path in {'linux_release.x86_64': binary, **additions}.items():
                with path.open('rb') as stream: add(name, stream)
        updated = copy.deepcopy(lock)
        updated['dependency_variant'] = {'kind': 'developer', 'base_lock_sha256': digest(base), 'validated': False,
                                          'base_variant': lock.get('dependency_variant'), 'meaning': 'Rebuilt Wayland release template; validation remains required.'}
        updated['godot']['assets'][ASSET] = {'url': archive_path.as_uri(), 'size': archive_path.stat().st_size, 'sha512': digest(archive_path, 'sha512')}
        updated['godot']['template_variant'] = {'kind': 'rm-wayland-stop', 'platform': 'linux', 'release_sha256': digest(binary),
                                              'materials': {name: {'sha256': digest(path), 'bytes': path.stat().st_size} for name, path in additions.items()}}
        (work / 'dependencies.developer.lock.json').write_text(json.dumps(updated, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({'template_archive': str(archive_path), 'dependency_lock': str(work / 'dependencies.developer.lock.json'), 'validated': False}))
    except BaseException as error:
        receipt.update(status='failed', error=repr(error)); save(); raise


if __name__ == '__main__': main()
