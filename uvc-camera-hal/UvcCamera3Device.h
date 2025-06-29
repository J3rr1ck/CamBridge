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

#ifndef UVC_CAMERA_HAL_UVCCAMERA3DEVICE_H
#define UVC_CAMERA_HAL_UVCCAMERA3DEVICE_H

#include <hardware/camera3.h>
#include <utils/Mutex.h>
#include <utils/Thread.h>
#include <utils/Condition.h>
#include <queue>
#include <string>
#include <vector>
#include <map>
#include <memory> // For std::unique_ptr
#include "V4L2Device.h"     // Assuming V4L2Device.h is in the same directory
#include "FormatConverter.h" // Assuming FormatConverter.h for conversions

namespace android {

// Forward declare CameraMetadata if not fully included from camera_metadata.h
// For this example, assume it's available via <hardware/camera_metadata.h>
// which is included by <hardware/camera3.h>

class UvcCamera3Device {
public:
    UvcCamera3Device(int cameraId, const std::string& devicePath, std::unique_ptr<V4L2Device> v4l2Device);
    ~UvcCamera3Device();

    // Initialize the device (called after open)
    status_t initialize(const camera3_callback_ops_t* callbackOps);
    void closeDevice(); // Full cleanup

    // Camera HAL3 device operations
    status_t configureStreams(camera3_stream_configuration_t* stream_list);
    status_t registerStreamBuffers(const camera3_stream_buffer_set_t* buffer_set); // Usually for HAL < 3.3
    const camera_metadata_t* constructDefaultRequestSettings(int type);
    status_t processCaptureRequest(camera3_capture_request_t* request);
    void dump(int fd);
    status_t flush();

    int getCameraId() const { return mCameraId; }
    const std::string& getDevicePath() const { return mDevicePath; }
    hw_device_t* getHwDevice(); // To return common.hw_device_t

    // Static callback from hw_device_t->close
    static int close_device(struct hw_device_t* device);


private:
    // Internal states
    enum class State {
        CLOSED,   // Initial state or after close
        OPENED,   // After open_device by factory, before initialize by framework
        READY,    // After initialize by framework, before configure_streams
        CONFIGURED, // After successful configure_streams
        STREAMING, // Actively streaming
        ERROR
    };

    // Request processing thread
    class RequestThread : public Thread {
    public:
        RequestThread(UvcCamera3Device* parent);
        ~RequestThread();
        void enqueueRequest(camera3_capture_request_t* request);
        status_t flushRequests(); // Clear pending requests
        void requestExitAndWait();

    private:
        virtual bool threadLoop() override;
        UvcCamera3Device* mParent;
        android::Mutex mRequestLock;
        android::Condition mRequestCond;
        std::queue<camera3_capture_request_t*> mRequestQueue;
        bool mExitRequested;
    };

    // Helper methods
    void initStaticMetadata();
    void processOneCaptureRequest(camera3_capture_request_t* request);
    void sendShutterNotify(uint32_t frameNumber, uint64_t timestamp);
    void sendErrorNotify(uint32_t frameNumber, camera3_error_msg_code_t error);
    void sendCaptureResult(camera3_capture_result_t* result);
    bool selectBestV4L2Format(const std::vector<camera3_stream_t*>& streams,
                              uint32_t* v4l2PixelFormat,
                              uint32_t* v4l2Width,
                              uint32_t* v4l2Height);

    // Member variables
    const int mCameraId;
    const std::string mDevicePath;
    camera3_device_t mDevice; // HAL device structure
    const camera3_callback_ops_t* mCallbackOps;

    android::Mutex mLock; // General lock for synchronizing access to device state
    State mState;

    std::unique_ptr<V4L2Device> mV4l2Device;
    std::unique_ptr<FormatConverter> mFormatConverter;
    sp<RequestThread> mRequestThread;

    camera_metadata_t* mStaticInfo; // Static camera characteristics
    std::vector<V4L2FormatInfo> mSupportedV4L2Formats;


    // Stream configuration
    struct StreamInfo {
        camera3_stream_t* stream;
        // Add any other per-stream info needed, e.g., internal buffers for conversion
        bool configured;
        // If this stream requires intermediate buffers for format conversion
        std::vector<std::unique_ptr<uint8_t[]>> intermediateBuffers;
        size_t intermediateBufferSize;

        StreamInfo() : stream(nullptr), configured(false), intermediateBufferSize(0) {}
    };
    std::map<camera3_stream_t*, StreamInfo> mConfiguredStreams;
    uint32_t mCurrentV4L2Format; // The V4L2 format currently configured on mV4l2Device
    uint32_t mCurrentV4L2Width;
    uint32_t mCurrentV4L2Height;


    // Disable copy and assign
    UvcCamera3Device(const UvcCamera3Device&) = delete;
    UvcCamera3Device& operator=(const UvcCamera3Device&) = delete;

    // Static device ops
    static camera3_device_ops_t sOps;
};

} // namespace android

#endif // UVC_CAMERA_HAL_UVCCAMERA3DEVICE_H
