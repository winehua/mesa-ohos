# OpenHarmony Documentation

**Status: DRAFT** (Pending validation on target devices)

## Architecture Overview

Mesa 3D is designed to handle a broad range of graphics APIs and drivers efficiently. It achieves this by sharing components such as interfaces, compiler optimization passes, and more. This modular approach enhances flexibility and maintainability.

Below, we explore the key layers of Mesa 3D’s architecture and their roles.

### 1. GL Dispatch

```c
glGenBuffers(..);
glDrawArrays(..);
```

Every OpenGL application operates with at least one **GL context**, which stores all GL-related states, including textures, buffer objects, and enables. Each OpenGL API call (e.g., `glGenBuffers(..)`) must act according to the current context.

For applications with multiple GL contexts, Mesa ensures the correct context is associated with every function call. This dispatch mechanism is optimized for minimal overhead, as every OpenGL API call relies on it.

For further details, refer to the official documentation: [Mesa Dispatch](https://docs.mesa3d.org/dispatch.html)

### 2. Mesa Core

```c
void GLAPIENTRY
_mesa_GenBuffers(GLsizei n, GLuint *buffers)
{
   GET_CURRENT_CONTEXT(ctx);
   create_buffers_err(ctx, n, buffers, false);
}
```

Mesa Core is the primary implementation layer for rendering APIs like OpenGL and OpenGLES. It handles:

- **API Implementation**: Supports OpenGL and OpenGLES functionalities.
- **API Validation**: Ensures that API calls adhere to specifications.
- **Context Management**: Tracks and manages GL states.
- **Version & Extension Exposure**: Determines and exposes supported API versions and extensions.

### 3. State Tracker

The **State Tracker** acts as an intermediary between Mesa Core and the **Gallium3D** interface. It transforms high-level Mesa Core functionality into a more flexible format, enabling driver optimizations. The state tracker reduces immediate-mode functionality, allowing better performance tuning.

### 4. Gallium3D

Gallium3D is a hardware abstraction layer that simplifies graphics driver development. It provides a structured API for managing:

- **Graphics State Management**: Converts Mesa state (e.g., blend modes, texture states) into hardware-ready operations.
- **Drawing Operations**: Maps OpenGL commands (`glDrawArrays`, `glDrawPixels`, etc.) into hardware commands.
- **Device Agnostic API**: Enables easier cross-platform driver development for OpenGL, OpenGLES, and OpenCL.

Gallium3D serves as the primary API for writing OpenGL/OpenGLES/OpenCL drivers, making it a crucial component for graphics driver development.

### 5. Zink: Gallium-to-Vulkan Translation

Zink is a Gallium driver that translates Gallium API calls into **Vulkan** calls. It effectively treats the Vulkan API as hardware, allowing applications to leverage Vulkan’s capabilities through the Gallium3D interface.

## Conclusion

Mesa 3D provides a modular, flexible approach to handling multiple graphics APIs and drivers. Its layered architecture ensures efficient dispatching, optimized state management, and cross-platform compatibility. The integration of Gallium3D and drivers like Zink further enhances its adaptability, making Mesa 3D a robust solution for modern graphics rendering.

---

For further reading, refer to the official Mesa documentation: [Mesa 3D](https://docs.mesa3d.org)

## Compile Guide

### Install all needed build deps

```
cd ~
wget https://github.com/mesonbuild/meson/releases/download/1.7.0/meson-1.7.0.tar.gz ; tar -xvzf meson-1.7.0.tar.gz ; cd meson-1.7.0/ ;  ln -s meson.py meson

pip install mako pyyaml
PATH=~/meson-1.7.0/:$PATH
```

```
cd /home/openharmony/workdir/third_party/mesa3d

python3 ohos/build_ohos64.py  /home/openharmony/workdir rpi4 /home/openharmony/workdir/third_party/mesa3d

```

 - rpi4 .. can be any arm64 traget.
 - `/home/openharmony/workdir`is the base folder, where OHOS is located.


## Deploy into demo application

 ```
cp /home/openharmony/workdir/third_party/mesa3d/build-ohos/install/lib/libEGL.so.1 entry/libs/arm64-v8a/
cp /home/openharmony/workdir/third_party/mesa3d/build-ohos/install/lib/libGLESv2.so.2 entry/libs/arm64-v8a/
cp /home/openharmony/workdir/third_party/mesa3d/build-ohos/install/lib/libgallium-25.1.0-devel.so entry/libs/arm64-v8a/
 ```
