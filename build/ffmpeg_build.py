"""Shared FFmpeg rebuild driver; outputs are always unvalidated new artifacts."""
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def extract_source(archive, destination, prefix, expected):
    """Extract verified regular-file git archives, also on Python 3.10."""
    if not re.fullmatch(r'[A-Za-z0-9_.-]+', prefix) or prefix in ('.', '..'):
        raise RuntimeError('Invalid source archive root')
    if digest(archive) != expected:
        raise RuntimeError('Source archive checksum mismatch: ' + str(archive))
    with tarfile.open(archive, 'r:*') as bundle:
        members = bundle.getmembers()
        seen = set()
        for member in members:
            name = member.name.rstrip('/')
            path = PurePosixPath(name)
            if (not name or '\\' in name or ':' in name or path.is_absolute() or
                    path.as_posix() != name or '..' in path.parts or
                    not path.parts or path.parts[0] != prefix):
                raise RuntimeError('Source archive member escapes its root: ' + member.name)
            if name in seen or not (member.isdir() or member.isfile()):
                raise RuntimeError('Duplicate or linked/non-regular source member: ' + member.name)
            seen.add(name)
        destination.mkdir(parents=True, exist_ok=False)
        for member in members:
            output = destination / member.name
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
            else:
                output.parent.mkdir(parents=True, exist_ok=True)
                with bundle.extractfile(member) as source, output.open('xb') as target:
                    shutil.copyfileobj(source, target)
                output.chmod(member.mode & 0o777)
    root = destination / prefix
    if not root.is_dir():
        raise RuntimeError('Source archive root is missing')
    return root


def snapshot_checkout(path, commit, work, name):
    prefix = name + '-' + commit
    archive = work / (prefix + '.tar')
    subprocess.run(['git', '-C', str(path), 'archive', '--format=tar', '--prefix=' + prefix + '/',
                    commit, '-o', str(archive)], check=True)
    sha = digest(archive)
    root = extract_source(archive, work / ('source-' + name), prefix, sha)
    return root, {'commit': commit, 'archive': archive.name, 'sha256': sha, 'kind': 'clean-git-archive'}


def checked_tool(name, env):
    found = shutil.which(str(name), path=env['PATH'])
    if not found:
        raise RuntimeError('Missing build tool: ' + str(name))
    path = Path(found).absolute()
    result = subprocess.run([str(path), '--version'], env=env, capture_output=True, text=True, check=True)
    return {'path': str(path), 'resolved_path': str(path.resolve()), 'sha256': digest(path),
            'version': (result.stdout + result.stderr).strip()}


def execute_build(work, roots, provenance, target, flags, jobs=8, cross_prefix=None,
                  cc=None, make='make', pkg_config='pkg-config', nasm='nasm'):
    """Build in a new workspace, recording failed steps and actual tool identity."""
    if os.name == 'nt':
        raise RuntimeError('Build FFmpeg under Linux/WSL; use the result on its target OS')
    if jobs < 1:
        raise RuntimeError('--jobs must be positive')
    if target not in ('windows', 'linux') or (target == 'windows' and not cross_prefix):
        raise RuntimeError('Windows builds require an explicit MinGW cross prefix')
    if ('--disable-gpl' not in flags or '--disable-nonfree' not in flags or
            any(flag in flags for flag in ['--enable-gpl', '--enable-nonfree'])):
        raise RuntimeError('The release recipe requires GPL and nonfree components disabled')
    env = os.environ.copy()
    ignored = ['CC', 'CXX', 'LD', 'AR', 'AS', 'NM', 'RANLIB', 'STRIP', 'CFLAGS', 'CXXFLAGS',
               'CPPFLAGS', 'LDFLAGS', 'MAKEFLAGS', 'MFLAGS', 'CONFIG_SITE', 'CPATH',
               'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX',
               'COMPILER_PATH', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR', 'PKG_CONFIG_SYSROOT_DIR']
    for key in ignored:
        env.pop(key, None)
    if cross_prefix:
        env['PATH'] = str(Path(cross_prefix).parent) + os.pathsep + env.get('PATH', '')
    env['LC_ALL'] = 'C'
    prefix, build, headers = work / 'ffmpeg-install', work / 'build', work / 'nvcodec-install'
    receipt = {'schema': 1, 'status': 'preparing', 'validated': False, 'target': target,
               'started_utc': datetime.now(timezone.utc).isoformat(), 'sources': provenance,
               'host': {'platform': platform.platform(), 'python': sys.version,
                        'os_release': Path('/etc/os-release').read_text() if Path('/etc/os-release').exists() else None},
               'driver_sha256': digest(__file__), 'commands': [], 'logs': {},
               'environment_policy': {'cleared_variables': ignored, 'LC_ALL': 'C'}, 'tools': {}}
    receipt_path = work / 'build-receipt.json'

    def run(command, cwd, name):
        command = list(map(str, command))
        receipt['status'] = name
        receipt['commands'].append({'command': command, 'cwd': str(cwd), 'log': name + '.log'})
        write_json(receipt_path, receipt)
        with (work / (name + '.log')).open('w') as log:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        receipt['logs'][name + '.log'] = digest(work / (name + '.log'))
        if result.returncode:
            raise RuntimeError(name + ' failed with exit ' + str(result.returncode) + '; see ' + str(work / (name + '.log')))

    try:
        build.mkdir(exist_ok=False)
        prefix.mkdir(exist_ok=False)
        cc = cc or ((cross_prefix + 'gcc-win32') if cross_prefix else 'gcc')
        for name, command in {'cc': cc, 'host_cc': 'cc', 'make': make, 'pkg_config': pkg_config, 'nasm': nasm}.items():
            receipt['tools'][name] = checked_tool(command, env)
        compiler = receipt['tools']['cc']['path']
        for name in ['ar', 'nm', 'ranlib', 'strip', 'ld', 'as']:
            command = subprocess.check_output([compiler, '-print-prog-name=' + name], env=env, text=True).strip()
            receipt['tools'][name] = checked_tool(command, env)
        tools = {name: item['path'] for name, item in receipt['tools'].items()}
        env['PKG_CONFIG_PATH'] = str(headers / 'lib/pkgconfig')
        # No implicit external packages in this fixed minimal recipe.
        env['PKG_CONFIG_LIBDIR'] = str(headers / 'lib/pkgconfig')
        recipe = [str(roots['ffmpeg'] / 'configure'), '--prefix=' + str(prefix), *flags,
                  '--cc=' + compiler, '--host-cc=' + tools['host_cc'], '--pkg-config=' + tools['pkg_config'],
                  '--x86asmexe=' + tools['nasm'], '--ar=' + tools['ar'], '--nm=' + tools['nm'],
                  '--ranlib=' + tools['ranlib'], '--strip=' + tools['strip']]
        if cross_prefix:
            recipe += ['--cross-prefix=' + cross_prefix]
        run([tools['make'], 'install', 'PREFIX=' + str(headers)], roots['nv-codec-headers'], 'headers')
        run(recipe, build, 'configure')
        run([tools['make'], '-j', str(jobs)], build, 'compile')
        run([tools['make'], 'install'], build, 'install')
        receipt['configuration'] = {}
        for name in ['config.h', 'config_components.h', 'ffbuild/config.mak', 'ffbuild/config.log']:
            file = build / name
            if not file.is_file():
                raise RuntimeError('Missing build configuration: ' + name)
            receipt['configuration'][name] = {'bytes': file.stat().st_size, 'sha256': digest(file)}
        receipt['outputs'] = {file.relative_to(prefix).as_posix(): {'bytes': file.stat().st_size, 'sha256': digest(file)}
                              for file in sorted(prefix.rglob('*')) if file.is_file()}
        for component in ['avcodec', 'avutil', 'swscale']:
            if 'lib/lib' + component + '.a' not in receipt['outputs']:
                raise RuntimeError('Expected static archive was not installed: ' + component)
        receipt['status'] = 'built-unvalidated'
        receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
        write_json(receipt_path, receipt)
        write_json(prefix / 'build-manifest.json', {
            'schema': 2, 'ffmpeg_commit': provenance['ffmpeg']['commit'],
            'nvcodec_commit': provenance['nv-codec-headers']['commit'], 'target': target,
            'configure': recipe, 'libraries': sorted(name for name in receipt['outputs'] if name.endswith('.a')),
            'build_receipt_sha256': digest(receipt_path), 'build_receipt': receipt})
        print('FFMPEG_BUILT_UNVALIDATED', prefix, flush=True)
        return prefix
    except Exception as error:
        receipt['failed_step'] = receipt['status']
        receipt['status'] = 'failed'
        receipt['error'] = str(error)
        write_json(receipt_path, receipt)
        raise
