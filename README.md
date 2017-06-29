# module\_openvr

A OpenVR module for OSPRay and HTC Vive, currently just a 360 image viewer app.
Uses OSPRay to render panoramic 360 images and displays them in VR.

## Building

First clone the module into OSPRay's `modules/` directory.

```
cd $OSPRAY/modules
git clone git@github.com:ospray/module_openvr.git
```

Then cd back to your OSPRay build directory and run CMake with the module enabled
via `-DOSPRAY_MODULE_OPENVR=ON`. The GLM and SDL2 libraries are required to
build the module and the OpenVR SDK is required for VR support.
If these libraries are installed in non-standard locations (or you're on Windows)
you can specify the root directories of each library when running CMake.
For example:

```
cmake <other OSPRay params> \
	-DOSPRAY_MODULE_OPENVR=ON \
	-DGLM_DIR=<..> -DSDL2_DIR=<..> -DOPENVR_DIR=<..>
```

If you don't have the OpenVR SDK on your system or don't specify
`OPENVR_DIR` the app will build to just show the desktop mirror view
with a fixed viewpoint.

## Running

The module currently only contains the `osp360` app which will use OSPRay
to render a panoramic 360 image of a loaded model and then display this
in VR as an environmeny map, allowing you to look around. To run pass the
model to be loaded:

```
./osp360 <path to model>
```

**Note:** Currently the camera position is hard-coded for the
Crytek Sponza model from the [OSPRay demos page](http://www.ospray.org/demos.html).

