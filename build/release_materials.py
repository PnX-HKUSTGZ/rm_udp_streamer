"""Assemble complete notices and the LGPL relinking kit into an export."""
import hashlib,json,os,shutil,subprocess,tarfile
from pathlib import Path
from ffmpeg_link import hardware_libraries

def sha(path):
    result=hashlib.sha256()
    with Path(path).open('rb') as source:
        for part in iter(lambda:source.read(1024*1024),b''):result.update(part)
    return result.hexdigest()

def prepare(repo,cache,output,engine,env,lock,windows,engine_materials=None):
    platform='windows' if windows else 'linux';notices=output/'licenses';notices.mkdir(exist_ok=True)
    if engine_materials:
        # These notices came from the actual rebuilt template, not the editor.
        variant=lock['godot']['template_variant'];destination=notices/'engine-build';destination.mkdir()
        for name,expected in variant['materials'].items():
            source=engine_materials/name
            if sha(source)!=expected['sha256'] or source.stat().st_size!=expected['bytes']:
                raise RuntimeError('Rebuilt engine materials changed before packaging')
            shutil.copy2(source,destination/name)
        shutil.copy2(engine_materials/'rm-engine-notices.json',notices/'Godot-engine-notices.json')
    else:
        licensing_env=env.copy();licensing_env['RM_ENGINE_NOTICES']=str(notices/'Godot-engine-notices.json')
        result=subprocess.run([str(engine),'--headless','--path',str(repo),'--script',str(repo/'build/engine_notices.gd')],env=licensing_env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace')
        if result.returncode or not (notices/'Godot-engine-notices.json').is_file():raise RuntimeError('Cannot extract engine notices: '+result.stdout)
    engine_info=json.loads((notices/'Godot-engine-notices.json').read_text(encoding='utf-8'))
    if not engine_info['license'] or not engine_info['copyright'] or not engine_info['third_party_licenses']:raise RuntimeError('Incomplete engine license inventory')
    (notices/'Godot-LICENSE.txt').write_text(engine_info['license'],encoding='utf-8')
    third=engine_info['third_party_licenses']
    (notices/'Godot-THIRD-PARTY.txt').write_text('\n\n'.join(name+'\n'+str(text) for name,text in sorted(third.items())),encoding='utf-8')
    shutil.copy2(cache/'godot-cpp/LICENSE.md',notices/'godot-cpp-LICENSE.txt')
    nuget=Path(os.environ.get('NUGET_PACKAGES',str(Path.home()/'.nuget/packages')))
    runtime=nuget/('microsoft.netcore.app.runtime.'+('win-x64' if windows else 'linux-x64'))/lock['dotnet']['runtime']
    for name in ['LICENSE.TXT','THIRD-PARTY-NOTICES.TXT']:
        shutil.copy2(runtime/name,notices/('.NET-'+name))
    if windows:
        build=json.loads((repo/'out/windows/build-manifest.json').read_text(encoding='utf-8'));compiler=Path(build['tools']['cxx']['path']).parent.parent
        for name in ['gcc','mingw-w64','winpthreads']:
            shutil.copytree(compiler/'licenses'/name,notices/('runtime-'+name))
    else:
        shutil.copy2('/usr/share/doc/gcc-13-base/copyright',notices/'GCC-runtime-copyright.txt')
        for name in ['GPL-3','LGPL-2.1','LGPL-3']:shutil.copy2('/usr/share/common-licenses/'+name,notices/('GCC-'+name+'.txt'))
    kit=output/'relink';kit.mkdir();source_cache=cache/'release-sources'
    manifest=json.loads((source_cache/'source-manifest.json').read_text(encoding='utf-8'))
    for name,key in [('ffmpeg','ffmpeg'),('nv-codec-headers','nv_codec_headers')]:
        item=manifest[name]
        if item['commit']!=lock[key]['commit'] or sha(source_cache/item['archive'])!=item['sha256']:raise RuntimeError('Unpinned release source: '+name)
    (kit/'sources').mkdir()
    shutil.copy2(source_cache/'source-manifest.json',kit/'sources/source-manifest.json')
    for item in manifest.values():shutil.copy2(source_cache/item['archive'],kit/'sources'/item['archive'])
    ff=manifest['ffmpeg']
    with tarfile.open(source_cache/ff['archive'],'r:gz') as bundle:
        for name in ['COPYING.LGPLv2.1','COPYING.LGPLv3','LICENSE.md']:
            with bundle.extractfile(ff['prefix']+'/'+name) as stream:(notices/('FFmpeg-'+name)).write_bytes(stream.read())
    nv=manifest['nv-codec-headers']
    with tarfile.open(source_cache/nv['archive'],'r:gz') as bundle:
        texts=[]
        for member in bundle.getmembers():
            if member.isfile() and member.name.endswith('.h'):
                text=bundle.extractfile(member).read().decode('utf-8')
                first=text.find('/*');end=text.find('*/',first)
                if first>=0 and end>=0:texts.append(member.name+'\n'+text[first:end+2])
        if not texts:raise RuntimeError('nv-codec-headers notices missing')
        (notices/'nv-codec-headers-notices.txt').write_text('\n\n'.join(texts),encoding='utf-8')
    for name in ['relink_native.py','rebuild_ffmpeg.py','ffmpeg_build.py','ffmpeg_link.py']:shutil.copy2(repo/'build'/name,kit/name)
    shutil.copytree(cache/('ffmpeg-static-'+platform),kit/'ffmpeg')
    objects=kit/'objects';objects.mkdir();libraries=kit/'libraries';libraries.mkdir()
    build_dir=repo/'out'/platform
    suffix='.obj' if windows else '.o'
    object_names=[]
    for path in sorted((build_dir/'CMakeFiles/rm_video.dir/src/godot').glob('*'+suffix)):
        shutil.copy2(path,objects/path.name);object_names.append(path.name)
    if len(object_names)!=3:raise RuntimeError('Expected the three native bridge objects')
    godot_library='libgodot-cpp.'+platform+'.template_release.x86_64.a'
    app_libraries=['librm_media.a',godot_library,'librm_transport.a']
    for name in app_libraries:shutil.copy2(build_dir/('bin/'+name if name==godot_library else name),libraries/name)
    link_flags=['-shared','-O3','-DNDEBUG','-static-libgcc','-static-libstdc++','-s']
    if windows:link_flags+=['-static','-Wl,--no-undefined']
    else:link_flags+=['-fPIC','-Wl,--no-undefined','-Wl,--exclude-libs,ALL','-Wl,-soname,librm_video.so']
    system=['-lbcrypt','-ldxguid','-lkernel32','-luser32','-lgdi32','-lwinspool','-lshell32','-lole32','-loleaut32','-luuid','-lcomdlg32','-ladvapi32'] if windows else ['-lm','-Wl,-Bstatic','-latomic','-Wl,-Bdynamic','-ldl']
    system += hardware_libraries(kit/'ffmpeg', windows)
    m={'platform':platform,'compiler_version':'15.2.0' if windows else '13.3.0','objects':object_names,'application_libraries':app_libraries,'link_flags':link_flags,'system_libraries':system,'files':{}}
    for file in sorted(kit.rglob('*')):
        if file.is_file():m['files'][file.relative_to(kit).as_posix()]=sha(file)
    (kit/'relink-manifest.json').write_text(json.dumps(m,indent=2)+'\n',encoding='utf-8')
    # Source for the native application adapter is included for inspection and rebuilding.
    source_out=kit/'application-source';source_out.mkdir()
    for name in ['native','build']:
        shutil.copytree(repo/name,source_out/name,ignore=shutil.ignore_patterns('__pycache__'))
    (kit/'README.txt').write_text(
        'FFmpeg n8.1.3 corresponding source and native relinking materials\n\n'
        'FFmpeg is LGPL-2.1-or-later, configured without GPL or nonfree components. See ../licenses.\n'
        'sources contains complete, pinned FFmpeg and nv-codec-headers source archives and checksums.\n'
        'objects and libraries contain the application/native bridge object code needed to link a modified FFmpeg.\n'
        'No application EULA in this package restricts modification or reverse engineering to debug LGPL-library modifications.\n'
        'The application authors retain their other rights; these materials do not change the application source license.\n\n'
        '1. Install Python 3.12+ and the compiler version in relink-manifest.json.\n'
        '2. On the target OS, run: python relink_native.py --output /new/path/'+('rm_video.dll' if windows else 'librm_video.so')+'\n'
        '3. To rebuild/modify FFmpeg, use Linux/WSL with make, pkg-config, NASM and GCC:\n'
        '   python3 rebuild_ffmpeg.py --work /new/work/path'+(' --cross-prefix /path/to/x86_64-w64-mingw32-' if windows else '')+'\n'
        '   The fresh build records source/tool identities, logs and installed file digests in build-receipt.json.\n'
        '   Rebuilt artifacts are unvalidated; they never overwrite the official dependency cache.\n'
        '   Edit the extracted FFmpeg source, then rerun make and make install in its build directory.\n'
        '   Manual edits invalidate the original build receipt; record the modified inputs and outputs before validation.\n'
        '   Original cross tools are MinGW GCC 13.2 win32; native Linux is GCC 13.3.\n'
        '   VAAPI builds additionally need libva-dev and libdrm-dev (Ubuntu); matching system runtime libraries and GPU drivers are not bundled.\n'
        '4. Run relink_native.py --ffmpeg-root /path/to/ffmpeg-install --output /new/path/library.\n'
        '5. Close the receiver, keep a backup, then replace the matching native library in the package.\n'
        '   Windows uses rm_video.dll beside the executable; Linux uses librm_video.so beside the executable.\n'
        '   Keep the same x86_64 architecture and FFmpeg ABI; incompatible modifications may require rebuilding application-source/native.\n'
        'Drivers, compiler tools and operating-system libraries are not included. The original configure command is in ffmpeg/build-manifest.json.\n',encoding='utf-8')
    (notices/'THIRD_PARTY.txt').write_text(
        'Godot 4.7.2 Mono: MIT and bundled third-party licenses; complete inventory in Godot-engine-notices.json.\n'
        'godot-cpp: MIT. .NET 8.0.34: MIT and .NET-THIRD-PARTY-NOTICES.TXT.\n'
        'FFmpeg n8.1.3: LGPL-2.1-or-later. Corresponding source, build recipe and relinkable application objects are in ../relink.\n'
        'nv-codec-headers: see the original per-header notices and complete source archive. NVIDIA driver is an OS dependency, not redistributed.\n'
        'VAAPI builds use distribution libva/libva-drm/libdrm and vendor media drivers as operating-system dependencies; these are not redistributed.\n'
        'Compiler runtimes: GCC runtime exception and associated notices; Windows additionally includes MinGW-w64/winpthreads notices.\n'
        'Application source rights are unchanged. No license here claims ownership of third-party components.\n',encoding='utf-8')
    shutil.copy2(build_dir/'build-manifest.json',output/'build-manifest.json')
