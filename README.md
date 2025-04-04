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

- Android 13 or later
- Platform signing keys
- Needs to be built into the Android ROM as a system app
- Device must support USB Host mode
- USB UVC-compliant camera

## Integration into Android Platform Build

This application is designed to be built as part of the Android platform. To include it in your Android build:

1. Copy the CamBridge directory to `packages/apps/CamBridge` in your Android source tree
2. Add the package to your device's build configuration by adding these lines to your device makefile:

```
PRODUCT_PACKAGES += \
    CamBridge \
    privapp_whitelist_com.android.cambridge
```

3. Build the Android platform with this app included

## Implementation Details

### Major Components

- **CamBridgeService**: Main service that detects USB events and manages the bridge
- **UvcCameraManager**: Handles USB communication with UVC cameras
- **VirtualCameraManager**: Manages the virtual camera and interaction with Camera HAL
- **VirtualCameraProviderService**: Implements the camera provider interface

### Native Component

The full implementation requires a native component (JNI library) to interface with the Android Camera HAL using AIDL interfaces. The Java implementation in this repository provides the application structure, but a complete implementation would need the additional native component to be fully functional.

### Platform Permissions

CamBridge requires several privileged system permissions, which are included in the `privapp_whitelist_com.android.cambridge.xml` file.

## Customization

### Camera Configuration

The app creates an external camera configuration file at `/data/vendor/camera/external_camera_config.xml` that defines the capabilities of the virtual camera. This can be customized to match your specific UVC camera's capabilities.

### Video Format Conversion

The VideoFrame class includes a basic format conversion mechanism. In a production environment, you may want to implement more efficient conversion using:

- Hardware accelerated codecs via MediaCodec
- Native libraries like libyuv
- OpenGL/Vulkan for GPU-accelerated conversion

## Testing

Since this is a system-level application that requires platform signing, testing must be done on a device where you have full control over the system image.

1. Build and flash your custom Android ROM including CamBridge
2. Connect a UVC camera to your Android device
3. The camera should be automatically detected and registered
4. Open any camera app and select the external camera from the camera selector

## Limitations

- Requires deep platform integration
- Must be built as part of the Android platform
- Updates require system OTA updates
- May not work with all UVC cameras due to variations in UVC implementation

## License

This project is licensed under the Apache License 2.0.

## Acknowledgments

This implementation is based on information from:
- Android Camera HAL documentation
- USB Video Class specifications
- Android USB Host API documentation 