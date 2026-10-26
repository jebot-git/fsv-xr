#!/usr/bin/env python3
"""Isolated OpenXR controller trace with real EGL stereo rendering."""
import base64
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='fsvr-stereo-test-') as temporary:
    root = Path(temporary)
    # A cube permits both near grabs and two-hand acquisition around its centre.
    positions = [(x*.5,y*.5,z*.5) for z in (-1,1) for y in (-1,1) for x in (-1,1)]
    indices = [0,2,1,1,2,3,4,5,6,5,7,6,0,1,4,1,5,4,2,6,3,3,6,7,0,4,2,2,4,6,1,3,5,3,7,5]
    data = struct.pack('<24f',*(v for p in positions for v in p)) + struct.pack('<36H',*indices)
    asset = {
        'asset': {'version':'2.0'},
        'buffers':[{'byteLength':len(data),'uri':'data:application/octet-stream;base64,'+base64.b64encode(data).decode()}],
        'bufferViews':[{'buffer':0,'byteLength':96},{'buffer':0,'byteOffset':96,'byteLength':72}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':8,'type':'VEC3','min':[-.5]*3,'max':[.5]*3},
                     {'bufferView':1,'componentType':5123,'count':36,'type':'SCALAR'}],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0},'indices':1,'material':0}]}],
        'materials':[{'pbrMetallicRoughness':{'baseColorFactor':[.8,.3,.1,1]},'doubleSided':True}],
        'nodes':[{'mesh':0}], 'scenes':[{'nodes':[0]}], 'scene':0,
    }
    model = root/'cube.gltf'
    model.write_text(json.dumps(asset))
    report = root/'trace.json'
    environment = dict(os.environ, LD_PRELOAD=str(Path(sys.argv[2]).resolve()),
                       LIBGL_ALWAYS_SOFTWARE='1', XR_RUNTIME_JSON='/nonexistent/isolated-test.json')
    result = subprocess.run([sys.argv[1],'xr',str(model),'5',str(report)],
                            env=environment,text=True,capture_output=True,timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
    data = json.loads(report.read_text())
    assert data['overlay'] is True
    assert data['rendered_stereo_frames'] >= 80, data
    assert data['focused_frames'] >= 80, data
    assert data['tracked_left'] >= 80 and data['tracked_right'] >= 80, data
    assert data['grab_frames'] >= 40, data
    for actual, expected in zip(data['position'], [.1,1.68,-1.4]):
        assert math.isclose(actual,expected,abs_tol=.002), data
    assert math.isclose(data['scale'],1.3,abs_tol=.002), data
    orientation = data['orientation']
    assert all(math.isclose(abs(v),.5,abs_tol=.002) for v in orientation), data
    print('PASS Touch bindings, one/two-hand move/turn/scale, focus/tracking loss, B close and real stereo GPU frames')
    print(json.dumps(data,sort_keys=True))
