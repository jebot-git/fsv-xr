# Native model preview dependencies

`gltf-dependencies.json` pins vendored sources and their SHA-256 hashes.

- cgltf (`cgltf/cgltf.h`): Johannes Kuhlmann and contributors, MIT license
  embedded at the end of the header; glTF parsing and accessor decoding.
- stb_image (`stb/stb_image.h`): Sean Barrett and contributors, public domain
  or MIT license embedded at the end of the header; only PNG/JPEG compiled.
- Native EGL setup (`fltk/model-egl.cpp`) adapted from the MIT de-xr native
  graphics adapter in OpenBrush Native, originating in Ghostty XR/FLTerm XR.
  Its copyright and permission notice is in `LICENSE.native-graphics`.

These sources are compiled into the FLTK preview. No downloads occur at runtime.
The new native model implementation uses MIT source notices; the existing
browser retains its original notices and licenses.
