#!/usr/bin/env python3
"""Build and export a standalone package from a clean committed checkout."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time
import zipfile
from build_project import prepare_extension_discovery

def run_engine(command, env, log_path):
    """Godot may exit 0 after an export plugin error; validate its diagnostics."""
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, encoding='utf-8', errors='replace')
    log_path.write_text(result.stdout, encoding='utf-8')
    print(result.stdout, end='', flush=True)
    clean = re.sub(r'\x1b\[[0-9;]*m', '', result.stdout)
    if result.returncode or re.search(r'(?m)^\s*(?:ERROR:|SCRIPT ERROR:)', clean):
        raise RuntimeError('Godot import/export failed; see ' + str(log_path))

def validate_runtime(output, windows):
    """Require the project assembly and a self-contained .NET runtime."""
    required = ['RmUdpStreamer.dll', 'GodotSharp.dll', 'RmUdpStreamer.runtimeconfig.json',
                'coreclr.dll' if windows else 'libcoreclr.so',
                'hostfxr.dll' if windows else 'libhostfxr.so',
                'hostpolicy.dll' if windows else 'libhostpolicy.so']
    for name in required:
        if not any(file.stat().st_size > 0 for file in output.rglob(name)):
            raise RuntimeError('Incomplete self-contained .NET export: missing ' + name)
    config = next(output.rglob('RmUdpStreamer.runtimeconfig.json'))
    runtime = json.loads(config.read_text())['runtimeOptions']
    if runtime.get('framework') or runtime.get('frameworks'):
        raise RuntimeError('Export requires an external .NET runtime')

def digest(path, algorithm='sha256'):
    result=hashlib.new(algorithm)
    with path.open('rb') as source:
        for block in iter(lambda:source.read(1024*1024),b''):result.update(block)
    return result.hexdigest()

def main():
    repo=Path(__file__).resolve().parent.parent
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache',type=Path,default=repo.parent/'.tools/rm-video')
    parser.add_argument('--godot',type=Path)
    parser.add_argument('--templates',type=Path,required=True,help='Locked Godot Mono .tpz archive')
    parser.add_argument('--output',type=Path,required=True,help='New or empty package directory')
    parser.add_argument('--label',default='D1')
    args=parser.parse_args()
    cache=args.cache.resolve();output=args.output.resolve();windows=os.name=='nt';platform='windows' if windows else 'linux'
    if subprocess.check_output(['git','-C',str(repo),'status','--porcelain'],text=True).strip():
        raise RuntimeError('Commit or preserve working changes before creating a versioned export')
    commit=subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()
    lock=json.loads((repo/'build/dependencies.lock.json').read_text())
    asset=lock['godot']['assets']['Godot_v4.7.2-stable_mono_export_templates.tpz']
    archive=args.templates.resolve()
    if archive.stat().st_size!=asset['size'] or digest(archive,'sha512')!=asset['sha512']:
        raise RuntimeError('Export template archive does not match the pinned SHA512')
    templates=cache/'export-templates'/lock['godot']['version']
    templates.mkdir(parents=True,exist_ok=True)
    names=['version.txt']
    names += ['windows_'+kind+'_x86_64'+suffix for kind in ['debug','release'] for suffix in ['.exe','_console.exe']] if windows else ['linux_debug.x86_64','linux_release.x86_64']
    with zipfile.ZipFile(archive) as bundle:
        for name in names:
            # Fixed allowlist: no archive-supplied relative paths are extracted.
            with bundle.open('templates/'+name) as source,(templates/name).open('wb') as target:shutil.copyfileobj(source,target)
    engine=args.godot
    if engine is None:
        engine=repo.parent/'Godot_v4.7.2-stable_mono_win64/Godot_v4.7.2-stable_mono_win64_console.exe' if windows else cache/'godot/Godot_v4.7.2-stable_mono_linux_x86_64/Godot_v4.7.2-stable_mono_linux.x86_64'
    engine=engine.resolve()
    if output.exists() and any(output.iterdir()):raise RuntimeError('Output directory is not empty; choose a new version directory')
    output.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    dotnet=shutil.which('dotnet')
    if not windows:
        dotnet=str(cache/'dotnet-8.0.428/dotnet');env['DOTNET_ROOT']=str(Path(dotnet).parent);env['PATH']=env['DOTNET_ROOT']+':'+env['PATH']
    subprocess.run([sys.executable,str(repo/'build/build_project.py'),'--cache',str(cache)],env=env,check=True)
    stage=repo/'out/export'/(platform+'-'+str(time.time_ns()))
    stage.mkdir(parents=True)
    tracked=subprocess.check_output(['git','-C',str(repo),'ls-files','-z']).decode('utf-8').split('\0')
    for relative in filter(None,tracked):
        source=repo/relative;target=stage/relative
        target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
    binary='rm_video.dll' if windows else 'librm_video.so'
    native=stage/'addons/rm_video/bin'/binary;native.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(repo/'addons/rm_video/bin'/binary,native)
    prepare_extension_discovery(stage)
    presets=(stage/'export_presets.cfg').read_text()
    # Only the staging checkout receives machine-specific template paths.
    sections=presets.split('[preset.')
    for index in range(1,len(sections)):
        section=sections[index]
        target_platform='windows' if section.startswith('0.options]') else 'linux' if section.startswith('1.options]') else None
        if target_platform != platform:continue
        for kind in ['debug','release']:
            filename='windows_'+kind+'_x86_64.exe' if windows else 'linux_'+kind+'.x86_64'
            section=section.replace('custom_template/'+kind+'=""','custom_template/'+kind+'="'+(templates/filename).as_posix()+'"')
        sections[index]=section
    (stage/'export_presets.cfg').write_text('[preset.'.join(sections))
    subprocess.run([dotnet,'build',str(stage/'RmUdpStreamer.csproj')],env=env,check=True)
    run_engine([str(engine),'--headless','--editor','--path',str(stage),'--import','--quit'],env,stage/'import.log')
    executable=output/('RmVideoReceiver.exe' if windows else 'RmVideoReceiver.x86_64')
    run_engine([str(engine),'--headless','--path',str(stage),'--export-release','Windows' if windows else 'Linux',str(executable)],env,stage/'export.log')
    if not executable.is_file() or not (output/'RmVideoReceiver.pck').is_file():raise RuntimeError('Incomplete engine export')
    validate_runtime(output,windows)
    if not windows:executable.chmod(executable.stat().st_mode|0o111)
    shutil.copy2(repo/'build/dependencies.lock.json',output/'dependencies.lock.json')
    shutil.copy2(repo/'docs/m2-media.md',output/'CAPABILITIES.md')
    (output/'README.txt').write_text(
        'RM Video Receiver '+args.label+'\nCommit: '+commit+'\n\n'
        'Run '+executable.name+' and choose the local IPv4 listening address and UDP port (3334 by default).\n'
        'The sender uses RoboMaster HEVC UDP framing. The default Auto decoder reports the actual device and CPU transfer mode.\n'
        'Godot and .NET runtime files are included; no editor or SDK is needed. Keep all package files together.\n'
        'GPU drivers and desktop OS libraries are supplied by the operating system.\n'
        'Linux baseline: Ubuntu 24.04 x86_64, glibc 2.39 or compatible, X11/Wayland desktop, Vulkan/OpenGL drivers, fontconfig and standard .NET native dependencies.\n'
        'WSL was tested with WSLg/llvmpipe. This does not certify native Ubuntu GPU interoperability.\n'
        'Software decoding is selectable. CPU transfer is not a zero-readback GPU bridge. Performance depends on the measured configuration.\n'
        'For deployment/diagnostics append -- --start --bind 127.0.0.1 --port 3334 --decoder software\n'
        'Optional: --duration 60 --metrics new-file.ndjson --snapshot new-file.png (new files only).\n'
        'Use -- --help for all receiver options. Normal launch opens the interactive interface.\n'
        'Unsupported HDR, wide-gamut primaries and LINEAR/LOG transfer input are rejected. See CAPABILITIES.md.\n',encoding='utf-8')
    notice=output/'licenses';notice.mkdir(exist_ok=True)
    for filename in ['LICENSE.md','LICENSE.txt','LICENSE']:
        source=cache/'godot-cpp'/filename
        if source.is_file():shutil.copy2(source,notice/'godot-cpp-LICENSE.txt');break
    (notice/'THIRD_PARTY.txt').write_text(
        'Godot Engine and godot-cpp: MIT. Godot includes its own third-party notices.\n'
        '.NET runtime: MIT and its distributed third-party notices in the runtime files.\n'
        'FFmpeg n8.1.3: LGPL-2.1-or-later, static libavcodec/libavutil/libswscale in rm_video. GPL/nonfree disabled.\n'
        'Pinned source and build configuration are listed in dependencies.lock.json.\n'
        'Source/relink release materials are being completed for D3. D1 is a local validation package; do not redistribute it as the final release.\n',encoding='utf-8')
    manifest={'label':args.label,'platform':platform,'commit':commit,'template_sha512':asset['sha512'],'native_sha256':digest(native),'files':{}}
    for file in sorted(output.rglob('*')):
        if file.is_file():manifest['files'][file.relative_to(output).as_posix()]={'bytes':file.stat().st_size,'sha256':digest(file)}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print('EXPORTED',output,flush=True)

if __name__=='__main__':main()
