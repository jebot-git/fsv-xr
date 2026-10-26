#!/usr/bin/env python3
"""Generated format, deformation, texture and malformed-input regressions."""
import base64
import copy
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

runner = str(Path(sys.argv[1]).resolve())
env = dict(os.environ,LIBGL_ALWAYS_SOFTWARE='1')

def png():
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',2,2,8,6,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+b'\x00\xff\x00\xff'*2)*2))+chunk(b'IEND',b'')

class Fixture:
    def __init__(self):
        self.data=bytearray()
        self.doc={'asset':{'version':'2.0'},'bufferViews':[],'accessors':[],
                  'meshes':[{'primitives':[{'attributes':{}}]}], 'nodes':[{'mesh':0}],
                  'scenes':[{'nodes':[0]}],'scene':0}
    def view(self, data):
        self.data.extend(b'\0'*(-len(self.data)%4))
        v={'buffer':0,'byteOffset':len(self.data),'byteLength':len(data)}
        self.data.extend(data)
        self.doc['bufferViews'].append(v)
        return len(self.doc['bufferViews'])-1
    def attr(self, values, shape, component=5126):
        n={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[shape]
        fmt={5126:'f',5123:'H',5121:'B'}[component]
        view=self.view(struct.pack('<'+str(len(values))+fmt,*values))
        a={'bufferView':view,'componentType':component,'count':len(values)//n,'type':shape}
        if shape=='VEC3':
            a.update(min=[min(values[i::3]) for i in range(3)],max=[max(values[i::3]) for i in range(3)])
        self.doc['accessors'].append(a)
        return len(self.doc['accessors'])-1
    def triangle(self):
        self.primitive=self.doc['meshes'][0]['primitives'][0]
        self.primitive['attributes']['POSITION']=self.attr([-.5,-.5,0,.5,-.5,0,0,.5,0],'VEC3')
        return self
    def save(self, path):
        self.doc['buffers']=[{'byteLength':len(self.data),'uri':'data:application/octet-stream;base64,'+base64.b64encode(self.data).decode()}]
        path.write_text(json.dumps(self.doc))
        return path

def call(*args,good=True):
    p=subprocess.run([runner,*map(str,args)],env=env,capture_output=True,text=True,timeout=10)
    assert (p.returncode==0)==good,p.stdout+p.stderr
    return p.stdout

def close(actual,expected):
    assert len(actual)==len(expected)
    assert all(math.isclose(a,b,abs_tol=.0001) for a,b in zip(actual,expected)),(actual,expected)

with tempfile.TemporaryDirectory(prefix='fsvr-formats-') as temporary:
    root=Path(temporary)
    file=root/'fixture.gltf'
    texture=root/'green.png'
    texture.write_bytes(png())
    f=Fixture().triangle()
    f.primitive['attributes']['TEXCOORD_1']=f.attr([0,0,1,0,.5,1],'VEC2')
    f.doc['images']=[{'uri':'green.png'}]
    f.doc['textures']=[{'source':0}]
    f.doc['materials']=[{'doubleSided':False,'extensions':{'KHR_materials_unlit':{}},
                        'pbrMetallicRoughness':{'baseColorTexture':{'index':0,'texCoord':1,'extensions':{'KHR_texture_transform':{'offset':[.2,.3],'scale':[.5,.5]}}}}}]
    f.primitive['material']=0
    f.doc['nodes'][0]['scale']=[-1,1,1]
    f.save(file)
    cache=json.loads(call('cache',file))
    assert cache['initial_uploads']==3 and cache['final_uploads']==3,cache
    assert cache['seconds']<1,cache
    print('PASS 1000 static frames reuse GPU geometry: '+str(cache))
    info=json.loads(call('inspect',file))
    assert info['images']==1 and info['triangles']==1
    ppm=root/'green.ppm'
    call('render',file,ppm)
    pixels=ppm.read_bytes().split(b'\n',3)[3]
    green=sum(g>180 and r<30 and b<30 for r,g,b in zip(pixels[0::3],pixels[1::3],pixels[2::3]))
    assert green>20000,green
    # Masked fragments disappear; blend produces a visible intermediate value.
    f.doc['materials'][0]['alphaMode']='MASK'
    f.doc['materials'][0]['pbrMetallicRoughness']['baseColorFactor']=[1,1,1,.1]
    f.save(file);call('render',file,ppm)
    assert ppm.read_bytes().split(b'\n',3)[3]!=pixels
    print('PASS real GPU texture, UV1/transform, mirrored winding and alpha-mask rendering')
    # Accessor without a base buffer is zero-filled then sparse overrides apply.
    f=Fixture().triangle()
    values=f.view(struct.pack('<9f',-.5,-.5,0,.5,-.5,0,0,.5,0))
    indices=f.view(bytes([0,1,2]))
    f.doc['accessors'][0].pop('bufferView')
    f.doc['accessors'][0]['sparse']={'count':3,'indices':{'bufferView':indices,'componentType':5121},'values':{'bufferView':values}}
    f.save(file)
    close(json.loads(call('inspect',file))['draws'][0][2],[0,.5,0])
    print('PASS sparse accessor positions')
    # Morph displacement followed by skeletal translation, animated together.
    f=Fixture().triangle()
    f.primitive['targets']=[{'POSITION':f.attr([0,.2,0]*3,'VEC3')}]
    f.doc['meshes'][0]['weights']=[1]
    f.primitive['attributes']['JOINTS_0']=f.attr([0,0,0,0]*3,'VEC4',5121)
    f.primitive['attributes']['WEIGHTS_0']=f.attr([1,0,0,0]*3,'VEC4')
    f.doc['nodes']=[{'mesh':0,'skin':0},{'translation':[.2,0,0]}]
    f.doc['scenes'][0]['nodes']=[0,1]
    f.doc['skins']=[{'joints':[1]}]
    times=f.attr([0,1],'SCALAR')
    motion=f.attr([.2,0,0,.6,0,0],'VEC3')
    weights=f.attr([0,1],'SCALAR')
    f.doc['animations']=[{'samplers':[{'input':times,'output':motion},{'input':times,'output':weights}],
                          'channels':[{'sampler':0,'target':{'node':1,'path':'translation'}},
                                      {'sampler':1,'target':{'node':0,'path':'weights'}}]}]
    f.save(file)
    close(json.loads(call('inspect',file,.5))['draws'][0][0],[-.1,-.4,0])
    call('render',file,ppm,.5)
    f.doc['animations'][0]['samplers'][0]['interpolation']='STEP'
    f.save(file)
    close(json.loads(call('inspect',file,.5))['draws'][0][0],[-.3,-.4,0])
    cubic=f.attr([0,0,0,.2,0,0,0,0,0,0,0,0,.6,0,0,0,0,0],'VEC3')
    f.doc['animations'][0]['samplers'][0].update(output=cubic,interpolation='CUBICSPLINE')
    f.save(file)
    close(json.loads(call('inspect',file,.5))['draws'][0][0],[-.1,-.4,0])
    print('PASS GPU skin/morph and LINEAR, STEP, CUBICSPLINE animation')
    # Reject a resource that otherwise blocks indefinitely waiting for a writer.
    os.mkfifo(root/'blocked.bin')
    f=Fixture().triangle();f.save(file)
    f.doc['buffers'][0]['uri']='blocked.bin';file.write_text(json.dumps(f.doc))
    call('inspect',file,good=False)
    f.doc['buffers'][0]['uri']='https://example.invalid/model.bin';file.write_text(json.dumps(f.doc))
    call('inspect',file,good=False)
    f=Fixture().triangle();f.doc['nodes'][0]['scale']=[1e30]*3
    f.doc['nodes'].append({'children':[0],'scale':[1e30]*3});f.doc['scenes'][0]['nodes']=[1]
    f.save(file);call('inspect',file,good=False)
    call('xr',file,'nan',good=False)
    print('PASS FIFO, remote URI, transform overflow and invalid duration rejection')
