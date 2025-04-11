# CamBridge: UVC Camera to Android Camera HAL Bridge

A system-level Android application that enables USB UVC cameras to be used with standard Android camera applications like Google Meet, Zoom, and other apps that use the Camera API/Camera2 API.

## Overview

CamBridge acts as a bridge between USB Video Class (UVC) cameras and the Android Camera Hardware Abstraction Layer (HAL). When a UVC camera is connected via USB, CamBridge:

1. Detects the UVC camera
2. Opens a connection to the camera and starts streaming video
3. Creates a virtual camera in the Android Camera HAL
4. Forwards video frames from the UVC camera to the virtual camera

This enables apps to use the external USB camera as if it were a built-in camera device.

## Features

- Automatic detection of UVC cameras when connected
- Support for common UVC video formats (MJPEG, YUYV)
- Format conversion as needed for the Camera HAL
- Registration of the camera with the Android Camera Service
- Low-latency video streaming

## Requirements

- Android 13 or later (built against platform sources)
- Platform signing keys
- Needs to be built into the Android ROM as a system app
- Device must support USB Host mode
- USB UVC-compliant camera
- Correct JNI build dependencies (e.g., `libhardware`, `libutils`, `liblog`, `libcutils`, `libcamera_metadata`, `camera_headers`) configured in `Android.bp`.

## Integration into Android Platform Build

This application is designed to be built as part of the Android platform. To include it in your Android build:

1. Copy the CamBridge directory to `packages/apps/CamBridge` in your Android source tree
2. Add the package to your device's build configuration by adding these lines to your device makefile:

```make
PRODUCT_PACKAGES += \
    CamBridge \
    privapp_whitelist_com.android.cambridge
```

3. Ensure the `app/Android.bp` file correctly defines the `android_app` and `cc_library_shared` modules with appropriate dependencies.
4. Build the Android platform with this app included.

## Implementation Details

### Major Components

- **CamBridgeService**: Main service that detects USB events and manages the bridge
- **UvcCameraManager**: Handles USB communication with UVC cameras
- **VirtualCameraManager**: Manages the virtual camera and interaction with Camera HAL
- **VirtualCameraProviderService**: Implements the camera provider interface

### Native Component (JNI)

CamBridge includes a native JNI component (`libcambridge_jni.so`) defined in `app/Android.bp` and implemented in `app/src/main/jni/`. This component is crucial for bridging the Java application layer with the native Camera HAL.

- **`cambridge_jni.cpp`**: Provides the JNI entry points callable from the Java side.
- **`virtual_camera_hal.cpp`**: Implements the core logic for creating and managing a virtual camera device. It currently interacts with the Camera HAL using older **HAL1-style interfaces** (`camera_module_t`, `camera_device_t`, etc.).

### Platform Permissions

CamBridge requires several privileged system permissions, which are included in the `privapp_whitelist_com.android.cambridge.xml` file. Ensure this file is correctly placed and included in the build.

## Build Status (as of last update)

- The Java application components are defined.
- The native JNI component (`libcambridge_jni.so`) **compiles successfully** after resolving build system configuration issues (dependencies, header paths) and C++ compilation errors.
- **Runtime testing is required** to verify the correct interaction between the Java services, the JNI layer, and the Android Camera HAL, including frame delivery and callback handling.

## Customization

### Camera Configuration

The app *may* create an external camera configuration file (e.g., at `/data/vendor/camera/external_camera_config.xml` or similar) to define the capabilities of the virtual camera. This mechanism needs verification and potential customization based on specific UVC camera capabilities.

### Video Format Conversion

The `VideoFrame` class includes a basic format conversion mechanism. In a production environment, you may want to implement more efficient conversion using:

- Hardware accelerated codecs via `MediaCodec`
- Native libraries like `libyuv`
- OpenGL/Vulkan for GPU-accelerated conversion

## Testing

Since this is a system-level application requiring platform signing and native components, testing must be done on a device where you have full control over the system image.

1. Build and flash your custom Android ROM including CamBridge
2. Connect a UVC camera to your Android device
3. Monitor system logs (`logcat`) for messages from `CamBridgeService`, `VirtualCameraHAL`, etc., to verify detection and initialization
4. Open a camera app (e.g., Google Camera, or a test app) and check if the "Virtual UVC Camera" appears in the camera selector
5. Test basic streaming functionality

## TODO / Future Improvements

- [ ] **HAL Migration:** Migrate the native implementation from Camera HAL1 to a modern version (HAL3+ using AIDL or HIDL) for better performance, features, and compatibility with the Camera2 API.
- [ ] **Runtime Testing & Debugging:** Thoroughly test and debug the entire pipeline on target hardware. Verify frame timestamps, buffer handling, and callback invocation.
- [ ] **UVC Feature Support:** Implement handling for more UVC controls (brightness, contrast, focus, zoom, etc.) and expose them through the HAL.
- [ ] **Performance Optimization:** Profile the frame processing pipeline (USB read -> JNI -> HAL delivery) and optimize bottlenecks. Implement efficient format conversion if needed (e.g., MJPEG -> NV21/YV12).
- [ ] **Multi-Camera Support:** Enhance logic to correctly handle multiple connected UVC devices.
- [ ] **Dynamic Configuration:** Improve virtual camera configuration based on actual UVC device capabilities reported via USB descriptors, rather than relying solely on a static XML.
- [ ] **Concurrency & Threading:** Review and ensure robust thread safety in the JNI layer, especially for buffer management and callbacks.
- [ ] **Camera2 Compliance:** Test extensively with Camera2 API test suites and various applications to ensure the virtual HAL implementation is compliant.
- [ ] **SELinux:** Review and refine SELinux policies for stricter access control if required.
- [ ] **Error Handling:** Improve error handling and reporting throughout the application and JNI layer (e.g., USB disconnects, unsupported formats, HAL errors).
- [ ] **Code Quality:** Refactor native C++ code, improve logging, and add comments where necessary.

## Limitations

- Requires deep platform integration and platform signing keys
- Must be built as part of the Android platform source
- Updates typically require system OTA updates
- Compatibility may vary depending on the specific UVC camera implementation
- Current implementation uses older Camera HAL1 interfaces

## License

This project is licensed under the Apache License 2.0.

## Acknowledgments

This implementation is based on information from:
- Android Camera HAL documentation
- USB Video Class specifications
- Android USB Host API documentation 