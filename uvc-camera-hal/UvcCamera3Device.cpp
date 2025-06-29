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

#define LOG_TAG "UvcCamera3Device"
#include <log/log.h>
#include <cutils/properties.h> // For property_get

#include "UvcCamera3Device.h"
#include <hardware/camera_common.h> // For camera_metadata_entry_t
#include <system/camera_metadata.h> // For actual metadata tags
#include <utils/Trace.h> // For ATRACE_CALL
#include <map> // For std::map in initStaticMetadata

namespace android {

// Initialize static sOps member
camera3_device_ops_t UvcCamera3Device::sOps = {
    .initialize = UvcCamera3Device::initialize_device,
    .configure_streams = UvcCamera3Device::configure_streams_device,
    .register_stream_buffers = nullptr, // Deprecated in HAL3.2+, use configure_streams
    .construct_default_request_settings = UvcCamera3Device::construct_default_request_settings_device,
    .process_capture_request = UvcCamera3Device::process_capture_request_device,
    .get_metadata_vendor_tag_ops = nullptr, // No vendor tags for now
    .dump = UvcCamera3Device::dump_device,
    .flush = UvcCamera3Device::flush_device,
    .reserved = {0},
};

// --- UvcCamera3Device::RequestThread Implementation ---
UvcCamera3Device::RequestThread::RequestThread(UvcCamera3Device* parent) :
    Thread(/*canCallJava*/false), mParent(parent), mExitRequested(false) {
    ALOGV("RequestThread created");
}

UvcCamera3Device::RequestThread::~RequestThread() {
    ALOGV("RequestThread destroyed");
    if (!mExitRequested) { // Ensure thread is properly exited
        requestExitAndWait();
    }
}

void UvcCamera3Device::RequestThread::requestExitAndWait() {
    ALOGI("RequestThread::requestExitAndWait called for camera ID %d", mParent ? mParent->mCameraId : -1);
    Mutex::Autolock lock(mRequestLock);
    mExitRequested = true;
    mRequestCond.signal();
    // Unlock before join
    lock.unlock();

    status_t status = Thread::requestExit(); // Base class method
    if (status != OK) {
        ALOGE("RequestThread::requestExit failed with status %d", status);
    }
    status = Thread::join(); // Base class method
    if (status != OK) {
        ALOGE("RequestThread::join failed with status %d", status);
    }
    ALOGI("RequestThread exited for camera ID %d", mParent ? mParent->mCameraId : -1);
}


void UvcCamera3Device::RequestThread::enqueueRequest(camera3_capture_request_t* request) {
    Mutex::Autolock lock(mRequestLock);
    if (mExitRequested) {
        ALOGW("RequestThread: Not enqueueing request for frame %u, exit requested.", request ? request->frame_number : 0);
        // TODO: Handle buffer release for dropped request properly by notifying parent
        if (mParent && request) {
            // mParent->sendErrorNotify(request->frame_number, CAMERA3_MSG_ERROR_REQUEST, CAMERA3_STREAM_ID_INVALID);
            // mParent->releaseRequestBuffers(request); // A method to release buffers of a request
        }
        return;
    }
    if (request) {
        mRequestQueue.push(request);
        mRequestCond.signal();
    }
}

status_t UvcCamera3Device::RequestThread::flushRequests() {
    Mutex::Autolock lock(mRequestLock);
    ALOGI("RequestThread: Flushing %zu requests for camera ID %d", mRequestQueue.size(), mParent ? mParent->mCameraId : -1);
    while (!mRequestQueue.empty()) {
        camera3_capture_request_t* request = mRequestQueue.front();
        mRequestQueue.pop();
        if (mParent && request) {
            // Notify parent to send error for flushed request
            // This needs to be carefully implemented in the parent to handle buffer states
            ALOGW("RequestThread: Flushed request for frame %u. Parent needs to handle buffer release and error notification.", request->frame_number);
             mParent->sendErrorNotify(request->frame_number, CAMERA3_MSG_ERROR_REQUEST /*, invalid stream*/);
            // The framework expects buffers from flushed requests to be returned with CAMERA3_BUFFER_STATUS_ERROR
            // This should be handled by the main class when processing the flush command.
        }
    }
    return OK;
}

bool UvcCamera3Device::RequestThread::threadLoop() {
    camera3_capture_request_t* request = nullptr;
    {
        Mutex::Autolock lock(mRequestLock);
        while (mRequestQueue.empty() && !mExitRequested) {
            status_t res = mRequestCond.waitRelative(mRequestLock, kRequestWaitTimeoutNs);
            if (res == TIMED_OUT) {
                // TODO: Handle timeout, e.g., check if parent is still active or if we should idle differently
                ALOGV("RequestThread: Timed out waiting for request.");
                // Continue to check mExitRequested
            }
        }

        if (mExitRequested) {
            ALOGV("RequestThread: Exiting loop for camera ID %d.", mParent ? mParent->mCameraId : -1);
            // Clear any remaining requests on exit
            while(!mRequestQueue.empty()) {
                camera3_capture_request_t* req = mRequestQueue.front();
                mRequestQueue.pop();
                ALOGW("RequestThread: Discarding request for frame %u on exit.", req->frame_number);
                // TODO: Notify parent for proper buffer release and error.
                 if (mParent && req) {
                    mParent->sendErrorNotify(req->frame_number, CAMERA3_MSG_ERROR_REQUEST);
                 }
            }
            return false; // Exit thread
        }
        request = mRequestQueue.front();
        mRequestQueue.pop();
    }

    if (request && mParent) {
        ALOGV("RequestThread: Processing request for frame %u on camera ID %d", request->frame_number, mParent->mCameraId);
        mParent->processOneCaptureRequest(request);
    }
    return true;
}

const nsecs_t UvcCamera3Device::RequestThread::kRequestWaitTimeoutNs = 2000000000LL; // 2 seconds

// --- UvcCamera3Device Implementation ---
UvcCamera3Device::UvcCamera3Device(int cameraId, const std::string& devicePath, std::unique_ptr<V4L2Device> v4l2Device) :
    mCameraId(cameraId),
    mDevicePath(devicePath),
    mCallbackOps(nullptr),
    mState(State::CLOSED),
    mV4l2Device(std::move(v4l2Device)),
    mFormatConverter(std::make_unique<FormatConverter>()),
    mStaticInfo(nullptr),
    mCurrentV4L2Format(0),
    mCurrentV4L2Width(0),
    mCurrentV4L2Height(0) {

    ALOGI("UvcCamera3Device created for ID %d, path %s", mCameraId, mDevicePath.c_str());
    memset(&mDevice, 0, sizeof(mDevice));
    mDevice.common.tag = HARDWARE_DEVICE_TAG;
    // Match HAL_MODULE_INFO_SYM or higher supported (e.g. CAMERA_DEVICE_API_VERSION_3_4 if HAL_MODULE_API_VERSION_2_5)
    mDevice.common.version = CAMERA_DEVICE_API_VERSION_3_3;
    mDevice.common.module = const_cast<hw_module_t*>(&HAL_MODULE_INFO_SYM.common);
    mDevice.common.close = close_device;
    mDevice.ops = &sOps;
    mDevice.priv = this;

    if (!mV4l2Device || !mV4l2Device->isOpen()) {
        ALOGE("UvcCamera3Device ID %d: V4L2Device is not valid or not open!", mCameraId);
        mState = State::ERROR;
    } else {
        mState = State::OPENED;
        mSupportedV4L2Formats = mV4l2Device->enumFormats();
        if (mSupportedV4L2Formats.empty()) {
            ALOGE("UvcCamera3Device ID %d: No V4L2 formats supported by device %s!", mCameraId, mDevicePath.c_str());
            // This is a critical failure for device usability.
            // Consider setting state to ERROR, though initStaticMetadata might also catch this.
        }
    }
    initStaticMetadata();
}

UvcCamera3Device::~UvcCamera3Device() {
    ALOGI("UvcCamera3Device destroyed for ID %d, path %s", mCameraId, mDevicePath.c_str());
    if (mRequestThread != nullptr) {
        mRequestThread->requestExitAndWait(); // Ensure thread is joined
        mRequestThread.clear(); // Release strong pointer
    }
    closeDevice();
    if (mStaticInfo) {
        free_camera_metadata(mStaticInfo);
        mStaticInfo = nullptr;
    }
}

hw_device_t* UvcCamera3Device::getHwDevice() {
    return &mDevice.common;
}

status_t UvcCamera3Device::initialize(const camera3_callback_ops_t* callbackOps) {
    ATRACE_CALL();
    ALOGI("UvcCamera3Device ID %d: initialize", mCameraId);
    Mutex::Autolock lock(mLock);

    if (!callbackOps) {
        ALOGE("UvcCamera3Device ID %d: Null callback ops provided!", mCameraId);
        return -EINVAL;
    }
    if (mState != State::OPENED) {
        ALOGE("UvcCamera3Device ID %d: Initialize called in wrong state %d", mCameraId, static_cast<int>(mState));
        return (mState == State::ERROR) ? -ENODEV : -EPIPE;
    }
    if (!mV4l2Device || !mV4l2Device->isOpen()) {
        ALOGE("UvcCamera3Device ID %d: V4L2 device unusable.", mCameraId);
        mState = State::ERROR;
        return -ENODEV;
    }
     if (mSupportedV4L2Formats.empty() && mStaticInfo == nullptr) { // Double check if static info also failed
        ALOGE("UvcCamera3Device ID %d: V4L2 device has no supported formats or static info failed.", mCameraId);
        mState = State::ERROR;
        return -ENODEV;
    }


    mCallbackOps = callbackOps;
    mState = State::READY;

    mRequestThread = new RequestThread(this);
    status_t res = mRequestThread->run(("UVCReqThr" + std::to_string(mCameraId)).c_str());
    if (res != OK) {
        ALOGE("UvcCamera3Device ID %d: Could not start RequestThread: %s (%d)", mCameraId, strerror(-res), res);
        mRequestThread.clear();
        mState = State::ERROR;
        return res;
    }

    ALOGI("UvcCamera3Device ID %d: Initialized successfully, state READY.", mCameraId);
    return OK;
}

void UvcCamera3Device::closeDevice() {
    ALOGI("UvcCamera3Device ID %d: closeDevice", mCameraId);
    Mutex::Autolock lock(mLock);

    if (mState == State::CLOSED) {
        ALOGI("UvcCamera3Device ID %d: Already closed.", mCameraId);
        return;
    }

    State previousState = mState;
    mState = State::CLOSED; // Mark as closing early to prevent new operations

    if (mRequestThread != nullptr) {
        ALOGI("UvcCamera3Device ID %d: Requesting RequestThread exit...", mCameraId);
        // Unlock before join if RequestThread could call back into UvcCamera3Device methods that take mLock
        // However, RequestThread::threadLoop primarily calls processOneCaptureRequest which might take its own locks.
        // The current structure seems okay for RequestThread to not deadlock with mLock.
        mRequestThread->requestExitAndWait();
        mRequestThread.clear();
        ALOGI("UvcCamera3Device ID %d: RequestThread exited and cleared.", mCameraId);
    }

    if (mV4l2Device && mV4l2Device->isOpen()) {
        if (previousState == State::STREAMING || previousState == State::CONFIGURED) {
            ALOGI("UvcCamera3Device ID %d: Stopping V4L2 stream and releasing buffers.", mCameraId);
            mV4l2Device->streamOff();
            // Unmapping is done by requestBuffers(0) or V4L2Device destructor
            mV4l2Device->requestBuffers(0); // Release all V4L2 buffers
        }
        // mV4l2Device is unique_ptr, its destructor will call its own closeDevice if not already called.
        // No need to explicitly call mV4l2Device->closeDevice() here unless specific ordering is required
        // before other member cleanup.
    }

    mConfiguredStreams.clear();
    mCallbackOps = nullptr;
    ALOGI("UvcCamera3Device ID %d: Closed.", mCameraId);
}


// --- Static HAL method wrappers ---
int UvcCamera3Device::close_device(struct hw_device_t* device) {
    if (!device) {
        ALOGE("close_device: Null device pointer");
        return -EINVAL;
    }
    UvcCamera3Device* self = reinterpret_cast<UvcCamera3Device*>(
        reinterpret_cast<camera3_device_t*>(device)->priv
    );
    if (!self) {
        ALOGE("close_device: Null UvcCamera3Device private data");
        return -EINVAL;
    }
    self->closeDevice();
    return 0;
}

int UvcCamera3Device::initialize_device(const struct camera3_device *d,
                                       const camera3_callback_ops_t *callback_ops) {
    UvcCamera3Device* self = reinterpret_cast<UvcCamera3Device*>(d->priv);
    return self->initialize(callback_ops);
}

int UvcCamera3Device::configure_streams_device(const struct camera3_device *d,
                                              camera3_stream_configuration_t *stream_list) {
    UvcCamera3Device* self = reinterpret_cast<UvcCamera3Device*>(d->priv);
    return self->configureStreams(stream_list);
}

const camera_metadata_t* UvcCamera3Device::construct_default_request_settings_device(
                                              const struct camera3_device *d, int type) {
    UvcCamera3Device* self = reinterpret_cast<UvcCamera3Device*>(d->priv);
    return self->constructDefaultRequestSettings(type);
}

int UvcCamera3Device::process_capture_request_device(const struct camera3_device *d,
                                                    camera3_capture_request_t *request) {
    UvcCamera3Device* self = reinterpret_cast<UvcCamera3Device*>(d->priv);
    return self->processCaptureRequest(request);
}

void UvcCamera3Device::dump_device(const struct camera3_device *d, int fd) {
    UvcCamera3Device* self = reinterpret_cast<UvcCamera3Device*>(d->priv);
    self->dump(fd);
}

int UvcCamera3Device::flush_device(const struct camera3_device *d) {
    UvcCamera3Device* self = reinterpret_cast<UvcCamera3Device*>(d->priv);
    return self->flush();
}


// --- Implementation for core HAL methods ---
void UvcCamera3Device::initStaticMetadata() {
    ALOGI("UvcCamera3Device ID %d: initStaticMetadata", mCameraId);
    if (mStaticInfo) { // Should not happen if constructor calls this once
        free_camera_metadata(mStaticInfo);
        mStaticInfo = nullptr;
    }

    if (!mV4l2Device || mSupportedV4L2Formats.empty()) {
        ALOGE("Cannot initialize static metadata: V4L2 device not ready or no formats found.");
        // Allocate a minimal metadata so mStaticInfo is not null, but it will be mostly empty.
        mStaticInfo = allocate_camera_metadata(5, 30);
        if (!mStaticInfo) {
             ALOGE("Failed to allocate even minimal static metadata!");
             mState = State::ERROR; // Critical failure
             return;
        }
        uint8_t hardwareLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
        add_camera_metadata_entry(mStaticInfo, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hardwareLevel, 1);
        uint8_t caps[] = {ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE};
        add_camera_metadata_entry(mStaticInfo, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, caps, sizeof(caps));
        int32_t partialResultCount = 1;
        add_camera_metadata_entry(mStaticInfo, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialResultCount, 1);
        // This device will likely fail any real app usage.
        return;
    }

    // Approximate entry and data counts. These can be tuned.
    // Number of entries: ~20 base + (num_formats * num_sizes * (1 stream_config + N fps_configs)) + num_controls
    // Data size: sum of data for these entries.
    size_t approx_entries = 50 + mSupportedV4L2Formats.size() * 5; // Rough estimate
    size_t approx_data = 2048 + mSupportedV4L2Formats.size() * 100; // Rough estimate
    mStaticInfo = allocate_camera_metadata(approx_entries, approx_data);
    if (!mStaticInfo) {
        ALOGE("Failed to allocate static metadata (entries=%zu, data=%zu)!", approx_entries, approx_data);
        mState = State::ERROR;
        return;
    }

    // Order of adding entries doesn't strictly matter but grouping helps readability.
    // Goldfish HAL reference: hardware/google/camera/devices/EmulatedCamera/hwl/EmulatedSensor.cpp

    // ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL
    uint8_t hardwareLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED; // Or FULL if more features are supported
    add_camera_metadata_entry(mStaticInfo, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hardwareLevel, 1);

    // ANDROID_LENS_FACING & ANDROID_SENSOR_ORIENTATION
    uint8_t lensFacing = ANDROID_LENS_FACING_EXTERNAL; // Default for UVC. Could be FRONT.
    char prop_facing[PROPERTY_VALUE_MAX];
    property_get("vendor.camera.uvc.facing", prop_facing, "external"); // "external" or "front"
    if (strcmp(prop_facing, "front") == 0) {
        lensFacing = ANDROID_LENS_FACING_FRONT;
    }
    add_camera_metadata_entry(mStaticInfo, ANDROID_LENS_FACING, &lensFacing, 1);

    int32_t orientation = 0;
    char prop_orientation[PROPERTY_VALUE_MAX];
    property_get("vendor.camera.uvc.orientation", prop_orientation, "0");
    orientation = atoi(prop_orientation);
    add_camera_metadata_entry(mStaticInfo, ANDROID_SENSOR_ORIENTATION, &orientation, 1);

    // ANDROID_REQUEST_AVAILABLE_CAPABILITIES
    std::vector<uint8_t> capsVector;
    capsVector.push_back(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE);
    // TODO: Check V4L2 controls to determine if MANUAL_SENSOR, etc., can be added.
    // if (supports_manual_exposure_via_v4l2) capsVector.push_back(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
    add_camera_metadata_entry(mStaticInfo, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, capsVector.data(), capsVector.size());

    // ANDROID_REQUEST_PARTIAL_RESULT_COUNT
    int32_t partialResultCount = 1;
    add_camera_metadata_entry(mStaticInfo, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialResultCount, 1);

    // ANDROID_REQUEST_PIPELINE_MAX_DEPTH (Number of V4L2 buffers + internal stages)
    // Should be at least 2. (V4L2 buffers + 1 for current processing + 1 for framework)
    uint8_t pipelineMaxDepth = static_cast<uint8_t>(mV4l2Device->getMappedBuffers().size() > 0 ? mV4l2Device->getMappedBuffers().size() : 4);
    if (pipelineMaxDepth < 3) pipelineMaxDepth = 3; // A safe minimum
    add_camera_metadata_entry(mStaticInfo, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipelineMaxDepth, 1);

    // ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS {RAW, PROC, JPEG}
    // Example: 0 RAW, 2 YUV (PROC), 1 JPEG
    int32_t maxOutputStreams[] = {0, 2, 1};
    add_camera_metadata_entry(mStaticInfo, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, maxOutputStreams, 3);


    // Stream Configurations, Min Frame Durations, Stall Durations
    std::vector<int32_t> streamConfigsData;
    std::vector<int64_t> minFrameDurationsData;
    std::vector<int64_t> stallDurationsData;

    std::map<uint32_t, int32_t> v4l2ToHalFormatMap = {
        {V4L2_PIX_FMT_YUYV, HAL_PIXEL_FORMAT_YCbCr_422_I},
        // MJPEG is not directly a HAL_PIXEL_FORMAT for output streams other than BLOB (JPEG)
        // {V4L2_PIX_FMT_MJPEG, HAL_PIXEL_FORMAT_BLOB}, // Only for JPEG output, handled separately or via BLOB
        {V4L2_PIX_FMT_YUV420, HAL_PIXEL_FORMAT_YCBCR_420_888}, // V4L2_PIX_FMT_YUV420 is I420
        {V4L2_PIX_FMT_NV12, HAL_PIXEL_FORMAT_YCBCR_420_888},   // NV12 is YCbCr_420_888
        {V4L2_PIX_FMT_NV21, HAL_PIXEL_FORMAT_YCrCb_420_SP},   // NV21 (HAL_PIXEL_FORMAT_YCRCB_420_SP)
    };

    // Helper to add a stream configuration if not already present (format, w, h)
    auto addUniqueStreamConfig = [&](int32_t format, int32_t w, int32_t h, const std::vector<float>& fpsList) {
        bool exists = false;
        for (size_t i = 0; i < streamConfigsData.size(); i += 4) {
            if (streamConfigsData[i] == format && streamConfigsData[i+1] == w && streamConfigsData[i+2] == h) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            streamConfigsData.push_back(format);
            streamConfigsData.push_back(w);
            streamConfigsData.push_back(h);
            streamConfigsData.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);

            for (float fps : fpsList) {
                if (fps > 0) {
                    minFrameDurationsData.push_back(format);
                    minFrameDurationsData.push_back(w);
                    minFrameDurationsData.push_back(h);
                    minFrameDurationsData.push_back(static_cast<int64_t>(1e9 / fps));

                    stallDurationsData.push_back(format);
                    stallDurationsData.push_back(w);
                    stallDurationsData.push_back(h);
                    stallDurationsData.push_back(0); // UVC stall is typically 0
                }
            }
        }
    };


    for (const auto& fmt_info : mSupportedV4L2Formats) {
        // 1. List direct V4L2 formats if they map to a HAL format
        auto halFormatIt = v4l2ToHalFormatMap.find(fmt_info.pixel_format);
        if (halFormatIt != v4l2ToHalFormatMap.end()) {
            addUniqueStreamConfig(halFormatIt->second, fmt_info.width, fmt_info.height, fmt_info.frame_rates);
        }

        // 2. If V4L2 supports MJPEG, list HAL_PIXEL_FORMAT_BLOB (for JPEG) for those sizes
        if (fmt_info.pixel_format == V4L2_PIX_FMT_MJPEG) {
            addUniqueStreamConfig(HAL_PIXEL_FORMAT_BLOB, fmt_info.width, fmt_info.height, fmt_info.frame_rates);
        }

        // 3. If V4L2 supports YUYV or MJPEG, assume we can convert to YCbCr_420_888
        //    and list YCbCr_420_888 for those sizes. This is crucial for preview/video.
        if (fmt_info.pixel_format == V4L2_PIX_FMT_YUYV || fmt_info.pixel_format == V4L2_PIX_FMT_MJPEG) {
            addUniqueStreamConfig(HAL_PIXEL_FORMAT_YCbCr_420_888, fmt_info.width, fmt_info.height, fmt_info.frame_rates);
        }
    }

    if (!streamConfigsData.empty()) {
        add_camera_metadata_entry(mStaticInfo, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, streamConfigsData.data(), streamConfigsData.size());
    }
    if (!minFrameDurationsData.empty()) {
        add_camera_metadata_entry(mStaticInfo, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, minFrameDurationsData.data(), minFrameDurationsData.size());
    }
    if (!stallDurationsData.empty()) {
        add_camera_metadata_entry(mStaticInfo, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stallDurationsData.data(), stallDurationsData.size());
    }

    // ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES (subset of JPEG sizes or common ones)
    std::vector<int32_t> jpegThumbnailSizes = {0, 0, 160, 120, 320, 240}; // Common, plus (0,0) for no thumbnail
    // TODO: Add actual small JPEG sizes if available from BLOB stream configs
    add_camera_metadata_entry(mStaticInfo, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, jpegThumbnailSizes.data(), jpegThumbnailSizes.size());

    // ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES
    // Derive from min/max FPS found across all YUV/Preview compatible formats/sizes
    std::vector<int32_t> fpsRanges;
    std::set<std::pair<int32_t, int32_t>> uniqueFpsRanges;
    for(size_t i=0; i < minFrameDurationsData.size(); i+=4) {
        // Consider formats suitable for preview/video for FPS ranges (e.g. YCbCr_420_888)
        if (minFrameDurationsData[i] == HAL_PIXEL_FORMAT_YCbCr_420_888) {
            int64_t minDurationNs = minFrameDurationsData[i+3];
            if (minDurationNs > 0) {
                int32_t fps = static_cast<int32_t>(round(1e9 / minDurationNs));
                uniqueFpsRanges.insert({fps, fps}); // For now, list exact FPS values as ranges
                // A more robust way is to find min/max FPS for each size and then create ranges.
                // Example: {15,15}, {30,30} or a combined {15,30} if device supports it smoothly.
            }
        }
    }
    if (uniqueFpsRanges.empty()) { uniqueFpsRanges.insert({15,30}); } // Fallback
    for(const auto& range : uniqueFpsRanges) {
        fpsRanges.push_back(range.first);
        fpsRanges.push_back(range.second);
    }
    add_camera_metadata_entry(mStaticInfo, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fpsRanges.data(), fpsRanges.size());

    // TODO: Query V4L2 controls using mV4l2Device->queryControls() and map them
    // Example for brightness (AE compensation) if V4L2_CID_BRIGHTNESS maps to exposure.
    // struct v4l2_queryctrl brightness_ctrl_info; // get from queryControls()
    // if (found_brightness_control) {
    //    int32_t exp_comp_range[] = {brightness_ctrl_info.minimum, brightness_ctrl_info.maximum};
    //    add_camera_metadata_entry(mStaticInfo, ANDROID_CONTROL_AE_COMPENSATION_RANGE, exp_comp_range, 2);
    //    camera_metadata_rational_t exp_comp_step = {brightness_ctrl_info.step, 1}; // Assuming step is integer
    //    add_camera_metadata_entry(mStaticInfo, ANDROID_CONTROL_AE_COMPENSATION_STEP, &exp_comp_step, 1);
    // }
    // Similarly for AWB modes (from V4L2_CID_WHITE_BALANCE_TEMPERATURE or V4L2_CID_AUTO_WHITE_BALANCE + menu items)
    // and AF modes (V4L2_CID_FOCUS_ABSOLUTE, V4L2_CID_FOCUS_AUTO). Most UVC are fixed or manual focus.

    uint8_t afModes[] = {ANDROID_CONTROL_AF_MODE_OFF};
    add_camera_metadata_entry(mStaticInfo, ANDROID_CONTROL_AF_AVAILABLE_MODES, afModes, sizeof(afModes));
    float minFocusDist = 0.0f; // Fixed focus at infinity
    add_camera_metadata_entry(mStaticInfo, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &minFocusDist, 1);
    float focalLengths[] = {3.0f}; // Example fixed focal length
    add_camera_metadata_entry(mStaticInfo, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, focalLengths, sizeof(focalLengths)/sizeof(float));


    uint8_t aeModes[] = {ANDROID_CONTROL_AE_MODE_ON};
    add_camera_metadata_entry(mStaticInfo, ANDROID_CONTROL_AE_AVAILABLE_MODES, aeModes, sizeof(aeModes));

    uint8_t awbModes[] = {ANDROID_CONTROL_AWB_MODE_AUTO};
    add_camera_metadata_entry(mStaticInfo, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModes, sizeof(awbModes));

    // ANDROID_SYNC_MAX_LATENCY
    int32_t maxLatency = ANDROID_SYNC_MAX_LATENCY_UNKNOWN; // Or PER_FRAME_CONTROL if requests are timely
    add_camera_metadata_entry(mStaticInfo, ANDROID_SYNC_MAX_LATENCY, &maxLatency, 1);

    // Sensor Info
    int32_t maxV4L2Width = 0, maxV4L2Height = 0;
    for (const auto& fmt_info : mSupportedV4L2Formats) {
        if (fmt_info.width > maxV4L2Width) maxV4L2Width = fmt_info.width;
        if (fmt_info.height > maxV4L2Height) maxV4L2Height = fmt_info.height;
    }
    if (maxV4L2Width == 0) maxV4L2Width = 640;
    if (maxV4L2Height == 0) maxV4L2Height = 480;

    int32_t activeArraySize[] = {0, 0, maxV4L2Width, maxV4L2Height};
    add_camera_metadata_entry(mStaticInfo, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArraySize, sizeof(activeArraySize)/sizeof(int32_t));
    int32_t pixelArraySize[] = {maxV4L2Width, maxV4L2Height};
    add_camera_metadata_entry(mStaticInfo, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArraySize, sizeof(pixelArraySize)/sizeof(int32_t));

    // Timestamp source
    uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    add_camera_metadata_entry(mStaticInfo, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource, 1);

    // Default face detect mode
    uint8_t faceDetectModes[] = {ANDROID_STATISTICS_FACE_DETECT_MODE_OFF};
    add_camera_metadata_entry(mStaticInfo, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, faceDetectModes, sizeof(faceDetectModes));
    int32_t maxFaceCount = 0;
    add_camera_metadata_entry(mStaticInfo, ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &maxFaceCount, 1);


    sort_camera_metadata(mStaticInfo);

    ALOGI("UvcCamera3Device ID %d: initStaticMetadata completed.", mCameraId);
    // For debugging:
    // dump_camera_metadata(mStaticInfo, /*verbosity*/ 1, /*indentation*/ 2);
    ALOGI("Static metadata for camera %d initialized with %d entries.", mCameraId, get_camera_metadata_entry_count(mStaticInfo));
}

const camera_metadata_t* UvcCamera3Device::constructDefaultRequestSettings(int type) {
    ATRACE_CALL();
    ALOGV("UvcCamera3Device ID %d: constructDefaultRequestSettings type %d", mCameraId, type);
    Mutex::Autolock lock(mLock);

    if (mState == State::ERROR || !mStaticInfo) {
        ALOGE("Device in error state or no static metadata for constructDefaultRequestSettings");
        return nullptr;
    }
    if (type < 0 || type >= CAMERA3_TEMPLATE_COUNT) {
        ALOGE("Invalid request template type: %d", type);
        return nullptr;
    }

    CameraMetadata requestTemplate;

    // Start with a minimal set of common request settings.
    // These are often required for any request to be valid.
    requestTemplate.update(ANDROID_CONTROL_MODE, (uint8_t[]){ANDROID_CONTROL_MODE_AUTO}, 1);
    requestTemplate.update(ANDROID_CONTROL_EFFECT_MODE, (uint8_t[]){ANDROID_CONTROL_EFFECT_MODE_OFF}, 1);
    requestTemplate.update(ANDROID_CONTROL_SCENE_MODE, (uint8_t[]){ANDROID_CONTROL_SCENE_MODE_DISABLED}, 1); // Usually for UVC
    requestTemplate.update(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, (uint8_t[]){ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF}, 1); // UVC no stabilization

    // AF mode (typically OFF for UVC, unless specific V4L2 focus controls are mapped)
    uint8_t afMode = ANDROID_CONTROL_AF_MODE_OFF;
    camera_metadata_ro_entry entry = find_camera_metadata_ro_entry(mStaticInfo, ANDROID_CONTROL_AF_AVAILABLE_MODES, nullptr);
    if (entry.count > 0) { // If available modes are listed, pick one (e.g. the first or AUTO if available)
        bool foundOff = false;
        for(size_t i=0; i < entry.count; ++i) if(entry.data.u8[i] == ANDROID_CONTROL_AF_MODE_OFF) foundOff = true;
        if(foundOff) afMode = ANDROID_CONTROL_AF_MODE_OFF;
        else afMode = entry.data.u8[0]; // Default to first available if OFF not found
    }
    requestTemplate.update(ANDROID_CONTROL_AF_MODE, &afMode, 1);


    // AE mode (typically ON for UVC auto-exposure)
    uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    entry = find_camera_metadata_ro_entry(mStaticInfo, ANDROID_CONTROL_AE_AVAILABLE_MODES, nullptr);
     if (entry.count > 0) {
        bool foundOn = false;
        for(size_t i=0; i < entry.count; ++i) if(entry.data.u8[i] == ANDROID_CONTROL_AE_MODE_ON) foundOn = true;
        if(foundOn) aeMode = ANDROID_CONTROL_AE_MODE_ON;
        else aeMode = entry.data.u8[0];
    }
    requestTemplate.update(ANDROID_CONTROL_AE_MODE, &aeMode, 1);
    // Default AE target FPS range (use the first one available from static_info)
    entry = find_camera_metadata_ro_entry(mStaticInfo, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, nullptr);
    if (entry.count >= 2) { // Must have at least one pair {min_fps, max_fps}
        int32_t defaultFpsRange[] = {entry.data.i32[0], entry.data.i32[1]};
        requestTemplate.update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, defaultFpsRange, 2);
    } else { // Fallback if not in static info (should be)
        int32_t defaultFpsRange[] = {15, 30};
        requestTemplate.update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, defaultFpsRange, 2);
    }
    int32_t aeComp = 0; // Default 0 exposure compensation
    requestTemplate.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &aeComp, 1);


    // AWB mode (typically AUTO for UVC)
    uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    entry = find_camera_metadata_ro_entry(mStaticInfo, ANDROID_CONTROL_AWB_AVAILABLE_MODES, nullptr);
    if (entry.count > 0) {
        bool foundAuto = false;
        for(size_t i=0; i < entry.count; ++i) if(entry.data.u8[i] == ANDROID_CONTROL_AWB_MODE_AUTO) foundAuto = true;
        if(foundAuto) awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
        else awbMode = entry.data.u8[0];
    }
    requestTemplate.update(ANDROID_CONTROL_AWB_MODE, &awbMode, 1);

    // Default JPEG quality and thumbnail size
    uint8_t jpegQuality = 90;
    requestTemplate.update(ANDROID_JPEG_QUALITY, &jpegQuality, 1);
    uint8_t thumbQuality = 90;
    requestTemplate.update(ANDROID_JPEG_THUMBNAIL_QUALITY, &thumbQuality, 1);
    entry = find_camera_metadata_ro_entry(mStaticInfo, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, nullptr);
    if (entry.count >= 2 && !(entry.data.i32[0] == 0 && entry.data.i32[1] == 0)) { // Use a non-(0,0) size
        int32_t thumbSize[] = {entry.data.i32[entry.count-2], entry.data.i32[entry.count-1]}; // Pick last available or a fixed small one
        for(size_t i=0; i < entry.count; i+=2) { // Prefer 320x240 or similar if available
            if (entry.data.i32[i] == 320 && entry.data.i32[i+1] == 240) {
                thumbSize[0] = 320; thumbSize[1] = 240; break;
            }
        }
        requestTemplate.update(ANDROID_JPEG_THUMBNAIL_SIZE, thumbSize, 2);
    } else {
        int32_t thumbSize[] = {320, 240}; // Fallback
        requestTemplate.update(ANDROID_JPEG_THUMBNAIL_SIZE, thumbSize, 2);
    }


    // Template-specific settings
    uint8_t intent = ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM;
    switch (type) {
        case CAMERA3_TEMPLATE_PREVIEW:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
            break;
        case CAMERA3_TEMPLATE_STILL_CAPTURE:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
            // For still capture, might want specific AE/AF triggers if supported
            // requestTemplate.update(ANDROID_CONTROL_AF_TRIGGER, (uint8_t[]){ANDROID_CONTROL_AF_TRIGGER_IDLE}, 1);
            // requestTemplate.update(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, (uint8_t[]){ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE}, 1);
            break;
        case CAMERA3_TEMPLATE_VIDEO_RECORD:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
            break;
        case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
            break;
        case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;
            // ZSL usually implies specific AE/AWB/AF modes and possibly HW support
            break;
        case CAMERA3_TEMPLATE_MANUAL:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
            requestTemplate.update(ANDROID_CONTROL_MODE, (uint8_t[]){ANDROID_CONTROL_MODE_OFF}, 1); // Manual control
            // TODO: Add default manual sensor settings if MANUAL_SENSOR capability exists
            // e.g., ANDROID_SENSOR_EXPOSURE_TIME, ANDROID_SENSOR_SENSITIVITY
            break;
        default:
            ALOGW("Unknown template type: %d, using CUSTOM intent", type);
            intent = ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM;
            break;
    }
    requestTemplate.update(ANDROID_CONTROL_CAPTURE_INTENT, &intent, 1);

    // Add an empty request.outputStreams - framework will fill this in the actual request.
    // This tag is not typically part of default request settings but is essential for a full request.
    // However, the default settings should be valid on their own.

    // Add an empty request.inputStreams
    // Not usually needed for default settings unless for specific reprocessing templates.

    // The framework expects a list of output streams to be included in a request.
    // This is NOT part of default settings. Default settings define controls.
    // The actual request from framework will have output_buffers and thus streams.
    // int32_t defaultStreamId = 0; // This is incorrect usage here.
    // requestTemplate.update(ANDROID_REQUEST_OUTPUT_STREAMS, &defaultStreamId, 1);
    // requestTemplate.update(ANDROID_REQUEST_INPUT_STREAMS, &defaultStreamId, 0); // No input stream

    // Required request keys (even if just with default values)
    // According to spec, these should be present in every request.
    // If not set by user, HAL should use these defaults.
    // int32_t requestId = 0; // Framework sets this. Not for default template.
    // requestTemplate.update(ANDROID_REQUEST_ID, &requestId, 1);
    // int32_t frameCount = -1; // Framework sets this. Not for default template.
    // requestTemplate.update(ANDROID_REQUEST_FRAME_COUNT, &frameCount, 1);


    camera_metadata_t *final_template = requestTemplate.release();
    // dump_camera_metadata(final_template, 1, 2); // For debugging
    return final_template;
}


status_t UvcCamera3Device::configureStreams(camera3_stream_configuration_t* stream_list) {
