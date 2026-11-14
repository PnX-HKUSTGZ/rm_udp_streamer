#!/usr/bin/env python3
"""Relink the supplied application objects with original or modified FFmpeg."""
import argparse,json,os,shutil,subprocess
from pathlib import Path

def main():
    base=Path(__file__).resolve().parent
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ffmpeg-root',type=Path,help='Prefix with lib/libavcodec.a, libswscale.a and libavutil.a')
    p.add_argument('--cxx',help='Matching G++ compiler; see relink-manifest.json')
    p.add_argument('--output',type=Path,required=True,help='New output DLL/SO; never overwrites the running package')
    a=p.parse_args();m=json.loads((base/'relink-manifest.json').read_text());windows=os.name=='nt'
    if windows!=(m['platform']=='windows'):raise RuntimeError('Run the relinker on the target OS')
    if a.output.exists():raise RuntimeError('Choose a new output filename')
    cxx=a.cxx or shutil.which('g++');ffmpeg=(a.ffmpeg_root.resolve() if a.ffmpeg_root else base/'ffmpeg')
    if not cxx:raise RuntimeError('Install the documented G++ toolchain or use --cxx')
    version=subprocess.check_output([cxx,'-dumpfullversion'],text=True).strip()
    if version!=m['compiler_version']:raise RuntimeError('Expected G++ '+m['compiler_version']+', got '+version)
    command=[cxx,*m['link_flags'],'-o',str(a.output.resolve())]
    command += [str(base/'objects'/name) for name in m['objects']]
    command += [str(base/'libraries'/name) for name in m['application_libraries']]
    if windows:command+=['-lpthread','-lws2_32']
    command += [str(ffmpeg/'lib'/('lib'+name+'.a')) for name in ['avcodec','swscale','avutil']]
    command += m['system_libraries']
    a.output.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(command,check=True)
    print('RELINKED',a.output)
if __name__=='__main__':main()
