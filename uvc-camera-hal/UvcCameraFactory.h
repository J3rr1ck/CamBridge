/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UVC_CAMERA_HAL_UVCAMERAFACTORY_H
#define UVC_CAMERA_HAL_UVCAMERAFACTORY_H

#include <hardware/camera_common.h>
#include <hardware/camera3.h>
#include <utils/Mutex.h>
#include <vector>
#include <string>
#include <memory> // For std::unique_ptr

// Forward declaration
namespace android {
class UvcCamera3Device;
class V4L2Device;

struct UvcCameraInfo {
    std::string device_path; // e.g., /dev/video0
    std::string card_name;   // From v4l2_capability
    std::string bus_info;    // Persistent identifier (e.g., USB bus path)
    int camera_id;           // HAL camera ID
};

class UvcCameraFactory {
public:
    UvcCameraFactory();
    ~UvcCameraFactory();

    int getNumberOfCameras() const;
    int getCameraInfo(int cameraId, struct camera_info* info);
    int setCallbacks(const camera_module_callbacks_t* callbacks);
    int openDevice(const char* name, struct hw_device_t** device);
    // Add open_legacy if it needs to be supported, though typically not for new HALs

    // Static entry points for the HAL module
    static int open_dev(const struct hw_module_t* module, const char* name,
                        struct hw_device_t** device);
    static int get_number_of_cameras();
    static int get_camera_info(int camera_id, struct camera_info* info);
    static int set_callbacks(const camera_module_callbacks_t* callbacks);

private:
    void discoverCameras();
    void clearCameras();
    void onDeviceAdded(const std::string& devicePath);
    void onDeviceRemoved(const std::string& devicePath);

    // TODO: Implement hotplug detection thread
    // void hotplugThreadLoop();
    // std::thread mHotplugThread;
    // bool mHotplugThreadRunning;

    android::Mutex mLock;
    std::vector<std::unique_ptr<UvcCamera3Device>> mCameras; // Indexed by current HAL ID
    std::vector<UvcCameraInfo> mCameraInfoList; // Info about currently active cameras, also indexed by current HAL ID

    // For stable ID mapping
    std::map<std::string, int> mBusInfoToHalIdMap; // bus_info -> assigned HAL ID
    std::vector<bool> mHalIdAvailable;             // Tracks if a HAL ID slot is free (max MAX_CAMERAS)
    static const int MAX_CAMERAS = 4;              // Max supported cameras by this HAL instance


    const camera_module_callbacks_t* mCallbacks;

    // Hotplug detection
    std::thread mHotplugThread;
    std::atomic<bool> mHotplugThreadRunning;
    void hotplugThreadLoop();
    // For inotify (conceptual, not fully implemented here)
    // int mInotifyFd;
    // int mInotifyWd;


    static UvcCameraFactory& getInstance();

    // Disable copy and assign
    UvcCameraFactory(const UvcCameraFactory&) = delete;
    UvcCameraFactory& operator=(const UvcCameraFactory&) = delete;
};

} // namespace android

extern camera_module_t HAL_MODULE_INFO_SYM;

#endif // UVC_CAMERA_HAL_UVCAMERAFACTORY_H
