#!/usr/bin/env python3
"""Archive the exact clean FFmpeg/nv-codec source trees for the release kit."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();lock=json.loads((Path(__file__).parent/'dependencies.lock.json').read_text())
    a.output.mkdir(parents=True,exist_ok=True);manifest={}
    for name,directory,entry in [('ffmpeg','ffmpeg-source','ffmpeg'),('nv-codec-headers','nv-codec-headers','nv_codec_headers')]:
        source=a.cache/directory;revision=lock[entry]['commit']
        if subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()!=revision:raise RuntimeError('Wrong source revision: '+name)
        if subprocess.check_output(['git','-C',str(source),'status','--porcelain'],text=True).strip():raise RuntimeError('Unrecorded source changes: '+name)
        prefix=name+'-'+revision;archive=a.output/(prefix+'.tar.gz')
        subprocess.run(['git','-C',str(source),'archive','--format=tar.gz','--prefix='+prefix+'/',str(revision),'-o',str(archive.resolve())],check=True)
        manifest[name]={'commit':revision,'repository':lock[entry]['repository'],'archive':archive.name,'prefix':prefix,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest()}
    (a.output/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('SOURCE_KIT_READY',a.output)
if __name__=='__main__':main()
