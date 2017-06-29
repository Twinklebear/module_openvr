OSPRay 360 Viewer App
---

A 360 viewer app for OSPRay and HTC Vive. Uses OSPRay to render panoramic 360 images
and displays them in VR. GLM, SDL2 and the OpenVR SDK are required to build the package,
if they're installed in non-standard locations (or you're on windows) you can specify
the root directories of each library. For example:

```
cmake <other OSPRay params> \
	-DOSPRAY_MODULE_360_VIEWER=ON \
	-DGLM_DIR=<..> -DSDL2_DIR=<..> -DOPENVR_DIR=<..>
```

If you don't have the OpenVR SDK on your system or don't specify
`OPENVR_DIR` the app will build to just show the desktop mirror view
with a fixed viewpoint.

