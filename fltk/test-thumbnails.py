#!/usr/bin/env python3
"""Bounded decode tests: no user pictures, no display, no original-sized buffers."""
import json,struct,subprocess,sys,tempfile,zlib
from pathlib import Path
from PIL import Image
binary=Path(sys.argv[1]).resolve()
def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
def png(path,w,h):
 with path.open('wb') as f:
  f.write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0)))
  compressor=zlib.compressobj();row=b'\0'+bytes([230,40,20])*w
  for y in range(h):
   block=compressor.compress(row)
   if block:f.write(chunk(b'IDAT',block))
  f.write(chunk(b'IDAT',compressor.flush())+chunk(b'IEND',b''))
def decode(path):
 result=json.loads(subprocess.check_output([str(binary),str(path)],text=True,timeout=12))
 assert result['peak_rss_kib']<100*1024,result
 if result['ok']:assert 0<result['width']<=256 and 0<result['height']<=96,result
 return result
with tempfile.TemporaryDirectory(prefix='fsv-thumbnails-') as folder:
 root=Path(folder)
 large=root/'100-megapixels.png';png(large,10000,10000)
 result=decode(large);assert result['ok'] and result['first']==[230,40,20,255],result
 print('PASS 100 MP PNG streams scanlines with bounded output and RSS')
 photo=root/'32-megapixels.jpg';Image.new('RGB',(8192,4096),(230,40,20)).save(photo,quality=90)
 result=decode(photo);assert result['ok'] and result['decoder_width']<=1024<result['source_width'],result
 print('PASS JPEG decoder downscales before rasterization')
 oriented=root/'orientation.jpg';picture=Image.new('RGB',(640,480),(10,50,200));exif=picture.getexif();exif[0x112]=6;picture.save(oriented,exif=exif)
 result=decode(oriented);assert result['ok'] and result['width']<result['height'],result
 for suffix in ('png','gif','bmp','jpg'):
  path=root/('small.'+suffix);Image.new('RGB',(640,480),(10,50,200)).save(path)
  result=decode(path);assert result['ok'],suffix
  if suffix!='jpg':assert result['first']==[10,50,200,255],result
 # A huge GIF logical canvas with a tiny first frame must not allocate the canvas.
 gif=root/'large-canvas.gif';Image.new('P',(1,1)).save(gif)
 data=bytearray(gif.read_bytes());data[6:10]=struct.pack('<HH',65535,65535);gif.write_bytes(data)
 assert decode(gif)['ok']
 # Large uncompressed BMP is sparse: read only output sample rows.
 bmp=root/'large.bmp';w=h=10000;stride=w*3;size=54+stride*h
 with bmp.open('wb') as f:
  f.write(b'BM'+struct.pack('<IHHI',size,0,0,54)+struct.pack('<IiiHHIIiiII',40,w,h,1,24,0,stride*h,0,0,0,0));f.truncate(size)
 assert decode(bmp)['ok']
 for name,data in [('invalid.png',b'not an image'),('truncated.png',large.read_bytes()[:80]),('bad.jpg',b'\xff\xd8'+b'\0'*200)]:
  path=root/name;path.write_bytes(data);assert not decode(path)['ok']
 print('PASS PNG/JPEG/GIF/BMP, oversized GIF canvas, sparse BMP and malformed inputs')
