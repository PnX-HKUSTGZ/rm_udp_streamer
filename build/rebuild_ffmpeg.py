#!/usr/bin/env python3
"""Rebuild the shipped LGPL sources on Linux/WSL, without an SDK download."""
import argparse,json,os,subprocess,tarfile,hashlib
from pathlib import Path

def main():
    base=Path(__file__).resolve().parent
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True,help='New work directory for source, build and install files')
    p.add_argument('--cross-prefix',help='For Windows: prefix ending in x86_64-w64-mingw32-')
    p.add_argument('--jobs',type=int,default=8)
    a=p.parse_args()
    if os.name=='nt':raise RuntimeError('Build FFmpeg under Linux/WSL, then run the Windows relinker on Windows')
    if a.work.exists():raise RuntimeError('Choose a new work directory')
    manifest=json.loads((base/'sources/source-manifest.json').read_text());recipe=json.loads((base/'ffmpeg/build-manifest.json').read_text())
    a.work=a.work.resolve();a.work.mkdir(parents=True)
    roots={}
    for name,item in manifest.items():
        archive=base/'sources'/item['archive']
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=item['sha256']:raise RuntimeError('Source archive checksum mismatch')
        with tarfile.open(archive,'r:gz') as bundle:
            bundle.extractall(a.work,filter='data')
        roots[name]=a.work/item['prefix']
    headers=a.work/'nvcodec-install';prefix=a.work/'ffmpeg-install';build=a.work/'build';build.mkdir()
    subprocess.run(['make','install','PREFIX='+str(headers)],cwd=roots['nv-codec-headers'],check=True)
    env=os.environ.copy();env['PKG_CONFIG_PATH']=str(headers/'lib/pkgconfig')
    flags=[s for s in recipe['configure'][1:] if not s.startswith(('--prefix=','--cross-prefix=','--cc='))]
    if recipe['target']=='windows':
        if not a.cross_prefix:raise RuntimeError('Windows FFmpeg needs --cross-prefix and a MinGW cross compiler')
        flags+=['--cross-prefix='+a.cross_prefix,'--cc='+a.cross_prefix+'gcc-win32']
    command=[str(roots['ffmpeg']/'configure'),'--prefix='+str(prefix),*flags]
    subprocess.run(command,cwd=build,env=env,check=True)
    print('Sources are extracted in',roots['ffmpeg'],flush=True)
    print('To make changes, edit those files and rerun make in',build,flush=True)
    subprocess.run(['make','-j',str(a.jobs)],cwd=build,env=env,check=True)
    subprocess.run(['make','install'],cwd=build,env=env,check=True)
    print('FFMPEG_READY',prefix)
if __name__=='__main__':main()
