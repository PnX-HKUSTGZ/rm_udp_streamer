#!/usr/bin/env python3
"""Rebuild the shipped LGPL sources on Linux/WSL, without an SDK download."""
import argparse,json,os
from pathlib import Path
from ffmpeg_build import digest, execute_build, extract_source

def main():
    base=Path(__file__).resolve().parent
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True,help='New work directory for source, build and install files')
    p.add_argument('--cross-prefix',help='For Windows: prefix ending in x86_64-w64-mingw32-')
    p.add_argument('--cc',help='Explicit C compiler; its version and digest are recorded')
    p.add_argument('--make',default='make')
    p.add_argument('--pkg-config',default='pkg-config')
    p.add_argument('--nasm',default='nasm')
    p.add_argument('--jobs',type=int,default=8)
    a=p.parse_args()
    if os.name=='nt':raise RuntimeError('Build FFmpeg under Linux/WSL, then run the Windows relinker on Windows')
    if a.work.exists():raise RuntimeError('Choose a new work directory')
    manifest=json.loads((base/'sources/source-manifest.json').read_text());recipe=json.loads((base/'ffmpeg/build-manifest.json').read_text())
    if recipe['target']=='windows' and not a.cross_prefix:raise RuntimeError('Windows FFmpeg needs --cross-prefix and a MinGW cross compiler')
    if a.jobs<1:raise RuntimeError('--jobs must be positive')
    a.work=a.work.resolve();a.work.mkdir(parents=True)
    roots={};provenance={}
    for name,key in [('ffmpeg','ffmpeg_commit'),('nv-codec-headers','nvcodec_commit')]:
        item=manifest[name]
        if item['commit']!=recipe[key]:raise RuntimeError('Release source revision differs from recipe: '+name)
        if Path(item['archive']).name!=item['archive'] or '/' in item['archive'] or '\\' in item['archive']:
            raise RuntimeError('Invalid source archive name')
        archive=base/'sources'/item['archive']
        roots[name]=extract_source(archive,a.work/('source-'+name),item['prefix'],item['sha256'])
        provenance[name]={'commit':item['commit'],'archive':item['archive'],'sha256':digest(archive),'kind':'verified-release-archive'}
    replaced=('--prefix=','--cross-prefix=','--cc=','--host-cc=','--pkg-config=','--x86asmexe=','--ar=','--nm=','--ranlib=','--strip=')
    flags=[s for s in recipe['configure'][1:] if not s.startswith(replaced)]
    prefix=execute_build(a.work,roots,provenance,recipe['target'],flags,a.jobs,a.cross_prefix,
                         a.cc,a.make,a.pkg_config,a.nasm)
    print('To modify FFmpeg, edit',roots['ffmpeg'],'then run make and make install in',a.work/'build',flush=True)
    print('After manual edits the original build receipt no longer describes the modified outputs; record them separately before validation.',flush=True)
if __name__=='__main__':main()
