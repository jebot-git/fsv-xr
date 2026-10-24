# In-window stereo 3D

The FLTK browser uses Comic Chat's in-place GPU stereo transport. In an active
DomeD XR presentation, it packs the entire window into two eye images, keeping
controls at zero disparity. Only the filesystem's spatial viewport receives
per-eye rendering and reprojection through the compositor's actual cylinder.
Tracked eyes are used when available, with a 64 mm head-relative fallback.
Pointer rays follow the reprojected viewport; orbit, pan, zoom and selection
remain available. No separate OpenXR session or presenter is started.

**View → Stereo 3D** enables or disables depth and saves the choice alongside
sorting preferences. It defaults to enabled. With depth disabled, identical
mono views still fill both eyes. Ordinary desktop presentation stays mono.
The existing standalone glTF **Open in XR** command is independent.

DomeD must recognize `org.fsvxr.fsvr.Sbs` and provide
`GET /api/integrations/fsvr`, the version-1 Comic Chat geometry/eye contract.
Build both projects in Release mode. Updated compositor binaries take effect
on a later session start; do not restart an active desktop for this feature.

Run the isolated Wayland integration check with:

```sh
cmake -S . -B build-fltk -DCMAKE_BUILD_TYPE=Release \
  -DFSVR_STEREO_TEST_COMPOSITOR=/absolute/path/to/DomeD/target/release/dome-compositor
cmake --build build-fltk -j3
ctest --test-dir build-fltk -R in-window-stereo --output-on-failure
```

The test starts a private Xvfb, session bus and nested DomeD, injects eye tracking,
and checks actual GPU output, matching chrome, scene disparity, pointer rays,
depth toggling, desktop return and saved preferences. Its `stereo-qa/stereo.ppm`
capture is beside the test executable. This does not validate headset optics
or tracking latency; the compositor monitor polls outside the UI thread.
