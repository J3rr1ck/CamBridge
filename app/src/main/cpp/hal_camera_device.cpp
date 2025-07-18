#include "hal_camera_device.h"
#include "hal_camera_session.h" // Will be created next
#include "hal_camera_provider.h" 
#include <utils/Log.h>
#include <hardware/camera3.h>     // For camera3_device_ops_t::construct_default_request_settings
#include <system/camera_metadata.h>
#include <cstring>

// Define a LOG_TAG for this file
#undef LOG_TAG
#define LOG_TAG "HalCameraDevice"

namespace android {
namespace cambridge {

// Example: Define some basic camera characteristics
const int32_t kDefaultWidth = 640;
const int32_t kDefaultHeight = 480;
// HAL_PIXEL_FORMAT_YCBCR_420_888 is typically from system/core/include/system/graphics.h
// For AIDL, this is mapped to aidl::android::hardware::graphics::common::PixelFormat::YCBCR_420_888
const auto kDefaultPixelFormat = aidl::android::hardware::graphics::common::PixelFormat::YCBCR_420_888;
const int32_t kDefaultFps = 30;

HalCameraDevice::HalCameraDevice(const std::string& cameraId, HalCameraProvider* parentProvider)
    : mCameraId(cameraId), mParentProvider(parentProvider), mCurrentSession(nullptr) {
    ALOGI("HalCameraDevice instance created for ID: %s", mCameraId.c_str());
    initializeCharacteristics();
}

HalCameraDevice::~HalCameraDevice() {
    ALOGI("HalCameraDevice instance destroyed for ID: %s", mCameraId.c_str());
    std::shared_ptr<HalCameraSession> sessionToClose;
    {
        std::lock_guard<std::mutex> lock(mLock);
        sessionToClose = mCurrentSession;
        mCurrentSession.reset(); // Release our reference
    }

    if (sessionToClose) {
        // The close method of HalCameraSession might try to call back into HalCameraDevice::closeSession
        // which would re-lock mLock. To avoid potential deadlocks or re-entrancy issues if
        // HalCameraSession::close() is complex, we call it outside the lock.
        // However, HalCameraSession::close() is designed to be simple and primarily notify its callback.
        // For safety, ensure HalCameraSession::close() doesn't call back in a way that re-locks mLock here.
        // A typical pattern is that HalCameraSession::close() signals its client (CameraService)
        // and then the client releases its reference to HalCameraSession.
        // Our HalCameraSession::close() implementation will be simple.
         sessionToClose->close(); 
    }

    if (mParentProvider) {
        mParentProvider->onDeviceClosed(mCameraId);
    }
}

void HalCameraDevice::initializeCharacteristics() {
    ALOGI("Initializing static characteristics for camera %s", mCameraId.c_str());
    // Estimate the number of entries and data count needed
    const size_t kEntryCount = 32;
    const size_t kDataCount = 256;
    camera_metadata_t* metadata = allocate_camera_metadata(kEntryCount, kDataCount);
    if (!metadata) {
        ALOGE("Failed to allocate camera metadata");
        return;
    }

    int32_t lensFacing = ANDROID_LENS_FACING_EXTERNAL;
    add_camera_metadata_entry(metadata, ANDROID_LENS_FACING, &lensFacing, 1);

    int32_t sensorOrientation = 0;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_ORIENTATION, &sensorOrientation, 1);

    uint8_t hardwareLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
    add_camera_metadata_entry(metadata, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hardwareLevel, 1);

    std::vector<int32_t> streamConfigs;
    // Config for 640x480
    streamConfigs.push_back(static_cast<int32_t>(kDefaultPixelFormat));
    streamConfigs.push_back(kDefaultWidth);
    streamConfigs.push_back(kDefaultHeight);
    streamConfigs.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
    // Config for 1280x720
    streamConfigs.push_back(static_cast<int32_t>(kDefaultPixelFormat));
    streamConfigs.push_back(1280);
    streamConfigs.push_back(720);
    streamConfigs.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
    // Config for 1920x1080
    streamConfigs.push_back(static_cast<int32_t>(kDefaultPixelFormat));
    streamConfigs.push_back(1920);
    streamConfigs.push_back(1080);
    streamConfigs.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, streamConfigs.data(), streamConfigs.size() / 4);

    std::vector<int64_t> minFrameDurations;
    // Duration for 640x480 @ 30fps
    minFrameDurations.push_back(static_cast<int64_t>(kDefaultPixelFormat));
    minFrameDurations.push_back(kDefaultWidth);
    minFrameDurations.push_back(kDefaultHeight);
    minFrameDurations.push_back(1000000000LL / kDefaultFps); // 33.3ms
    // Duration for 1280x720 @ 30fps
    minFrameDurations.push_back(static_cast<int64_t>(kDefaultPixelFormat));
    minFrameDurations.push_back(1280);
    minFrameDurations.push_back(720);
    minFrameDurations.push_back(1000000000LL / kDefaultFps); // 33.3ms
    // Duration for 1920x1080 @ 30fps
    minFrameDurations.push_back(static_cast<int64_t>(kDefaultPixelFormat));
    minFrameDurations.push_back(1920);
    minFrameDurations.push_back(1080);
    minFrameDurations.push_back(1000000000LL / kDefaultFps); // 33.3ms
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, minFrameDurations.data(), minFrameDurations.size() / 4);
    
    std::vector<int64_t> stallDurations;
    // Stall for 640x480
    stallDurations.push_back(static_cast<int64_t>(kDefaultPixelFormat));
    stallDurations.push_back(kDefaultWidth);
    stallDurations.push_back(kDefaultHeight);
    stallDurations.push_back(0); // No stall
    // Stall for 1280x720
    stallDurations.push_back(static_cast<int64_t>(kDefaultPixelFormat));
    stallDurations.push_back(1280);
    stallDurations.push_back(720);
    stallDurations.push_back(0); // No stall
    // Stall for 1920x1080
    stallDurations.push_back(static_cast<int64_t>(kDefaultPixelFormat));
    stallDurations.push_back(1920);
    stallDurations.push_back(1080);
    stallDurations.push_back(0); // No stall
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stallDurations.data(), stallDurations.size() / 4);

    // Sensor active array size (based on largest resolution)
    int32_t activeArraySize[] = {0, 0, 1920, 1080}; // left, top, width, height
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArraySize, sizeof(activeArraySize)/sizeof(int32_t));

    // AE available target FPS ranges
    std::vector<int32_t> aeTargetFpsRanges = {15, 30, 30, 30}; // {min1,max1, min2,max2 ...}
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, aeTargetFpsRanges.data(), aeTargetFpsRanges.size() / 4);

    // AF available modes
    std::vector<uint8_t> afModes;
    afModes.push_back(ANDROID_CONTROL_AF_MODE_OFF);
    afModes.push_back(ANDROID_CONTROL_AF_MODE_AUTO);
    afModes.push_back(ANDROID_CONTROL_AF_MODE_MACRO);
    afModes.push_back(ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO);
    afModes.push_back(ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE);
    // Add EDOF if it makes sense for a virtual camera, often not.
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_AVAILABLE_MODES, afModes.data(), afModes.size());
    
    // AWB available modes
    std::vector<uint8_t> awbModes;
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_OFF);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_AUTO);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_INCANDESCENT);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_FLUORESCENT);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_WARM_FLUORESCENT);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_DAYLIGHT);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_TWILIGHT);
    awbModes.push_back(ANDROID_CONTROL_AWB_MODE_SHADE);
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModes.data(), awbModes.size());

    // JPEG Thumbnail Sizes
    std::vector<int32_t> jpegThumbnailSizes;
    jpegThumbnailSizes.push_back(0); jpegThumbnailSizes.push_back(0); // Mandatory: 0,0 for no thumbnail
    jpegThumbnailSizes.push_back(160); jpegThumbnailSizes.push_back(120);
    jpegThumbnailSizes.push_back(320); jpegThumbnailSizes.push_back(240);
    add_camera_metadata_entry(metadata, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, jpegThumbnailSizes.data(), jpegThumbnailSizes.size() / 4);

    uint8_t requestCapabilities[] = {ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE};
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, requestCapabilities, sizeof(requestCapabilities));
    
    int32_t partialResultCount = 1; 
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialResultCount, 1);

    uint8_t pipelineMaxDepth = 4; 
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipelineMaxDepth, 1);
    
    int32_t syncMaxLatency = ANDROID_SYNC_MAX_LATENCY_PER_FRAME_CONTROL;
    add_camera_metadata_entry(metadata, ANDROID_SYNC_MAX_LATENCY, &syncMaxLatency, 1);

    // Available request keys (none for now, beyond mandatory)
    // Available result keys (none for now, beyond mandatory)
    // Available characteristics keys (populated above)

    size_t size = get_camera_metadata_size(metadata);
    mStaticCharacteristics.metadata.clear();
    mStaticCharacteristics.metadata.resize(size);
    memcpy(mStaticCharacteristics.metadata.data(), metadata, size);
    free_camera_metadata(metadata);

    ALOGI("Static characteristics initialized for %s. Entry count: %zu", mCameraId.c_str(), get_camera_metadata_entry_count(reinterpret_cast<const camera_metadata_t*>(mStaticCharacteristics.metadata.data())));
}

ndk::ScopedAStatus HalCameraDevice::getCameraCharacteristics(CameraMetadata* _aidl_return) {
    ALOGI("getCameraCharacteristics called for camera %s", mCameraId.c_str());
    if (!_aidl_return) {
        ALOGE("getCameraCharacteristics: _aidl_return is null");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (!mStaticCharacteristics.metadata.size()) {
        ALOGE("getCameraCharacteristics: mStaticCharacteristics.metadata is empty for camera %s", mCameraId.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(-ENODEV);
    }

    _aidl_return->metadata = mStaticCharacteristics.metadata;
    ALOGI("Returning characteristics for camera %s. Metadata size: %zu bytes.", mCameraId.c_str(), mStaticCharacteristics.metadata.size());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraDevice::open(const std::shared_ptr<ICameraDeviceCallback>& in_callback,
                                       std::shared_ptr<ICameraDeviceSession>* _aidl_return) {
    ALOGI("open called for camera %s", mCameraId.c_str());
    std::lock_guard<std::mutex> lock(mLock);

    if (mCurrentSession) {
        ALOGE("Camera %s is already open. Current session pointer: %p", mCameraId.c_str(), mCurrentSession.get());
        // According to AIDL spec, if camera is in use, return ERROR_CAMERA_IN_USE
        return ndk::ScopedAStatus::fromServiceSpecificError(-EBUSY);
    }

    if (in_callback == nullptr) {
        ALOGE("Framework callback (ICameraDeviceCallback) is null in open()!");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT); // Or EX_NULL_POINTER
    }

    // Create the session. The HalCameraSession constructor will need the ICameraDeviceCallback.
    auto session = ndk::SharedRefBase::make<HalCameraSession>(mCameraId, this, in_callback);
    if (!session) {
        ALOGE("Failed to create HalCameraSession for %s", mCameraId.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(-ENODEV);
    }
    
    // If HalCameraSession has an initialize method that can fail:
    // if (!session->initialize()) { // Assuming bool return for success
    //     ALOGE("Failed to initialize HalCameraSession for %s", mCameraId.c_str());
    //     return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDevice::ERROR_CAMERA_DEVICE);
    // }

    mCurrentSession = session;
    *_aidl_return = mCurrentSession;
    ALOGI("Camera %s opened successfully. New session pointer: %p", mCameraId.c_str(), mCurrentSession.get());
    return ndk::ScopedAStatus::ok();
}

void HalCameraDevice::closeSession() {
    ALOGI("HalCameraDevice::closeSession called for camera %s by its session.", mCameraId.c_str());
    std::lock_guard<std::mutex> lock(mLock);
    if (mCurrentSession) {
        // This indicates the session is done and HalCameraDevice can forget about it.
        // The actual HalCameraSession object might still exist if CameraService holds a reference.
        mCurrentSession.reset();
        ALOGI("Reference to HalCameraSession cleared for camera %s.", mCameraId.c_str());
    } else {
        ALOGW("HalCameraDevice::closeSession called but no current session for %s.", mCameraId.c_str());
    }
}


ndk::ScopedAStatus HalCameraDevice::setTorchMode(bool /*in_enabled*/) {
    // ALOGI("setTorchMode called for camera %s, enabled: %d. Not supported.", mCameraId.c_str(), in_enabled);
    // As per AIDL spec, if torch mode is not supported, this should return ERROR_ILLEGAL_ARGUMENT.
    // However, some interpretations suggest ERROR_CAMERA_DEVICE if it's a permanent lack of feature.
    // Let's use a more specific error if available, or a general one.
    // The ICameraProvider's isSetTorchModeAvailable should return false for this camera.
    // If that's the case, CameraService might not even call this.
    // For now, let's assume it can be called and we should indicate it's not supported.
    return ndk::ScopedAStatus::fromServiceSpecificError(-EINVAL); // Or -EINVAL
}

ndk::ScopedAStatus HalCameraDevice::dumpState(const ::ndk::ScopedFileDescriptor& in_fd) {
    ALOGI("dumpState called for camera %s.", mCameraId.c_str());
    if (in_fd.get() < 0) {
        ALOGE("Invalid file descriptor for dumpState.");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    
    std::string dumpString = "HalCameraDevice ID: " + mCameraId + "\n";
    {
        std::lock_guard<std::mutex> lock(mLock);
        dumpString += "  Session active: " + std::string(mCurrentSession ? "yes" : "no") + "\n";
        if (mCurrentSession) {
            // Ideally, we'd call a dump method on the session too.
            // For now, just indicate its presence.
            dumpString += "  Session ptr: " + std::to_string(reinterpret_cast<uintptr_t>(mCurrentSession.get())) + "\n";
        }
    }
    dumpString += "  Static Characteristics entry count: " +
        std::to_string(mStaticCharacteristics.metadata.size() ? get_camera_metadata_entry_count(reinterpret_cast<const camera_metadata_t*>(mStaticCharacteristics.metadata.data())) : 0) + "\n";

    if (write(in_fd.get(), dumpString.c_str(), dumpString.length()) < 0) {
        ALOGE("Failed to write dumpState to fd for camera %s: %s", mCameraId.c_str(), strerror(errno));
        // Not much we can do here, error will be ignored by caller typically.
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraDevice::isStreamCombinationSupported(
        const StreamConfiguration& in_config, bool* _aidl_return) {
    ALOGI("isStreamCombinationSupported called for camera %s", mCameraId.c_str());
    if (_aidl_return == nullptr) {
        ALOGE("isStreamCombinationSupported: _aidl_return is null");
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    // Basic validation: only one output stream configuration is supported for now.
    if (in_config.streams.size() != 1) {
        ALOGW("Stream configuration validation failed: Expected 1 stream, got %zu", in_config.streams.size());
        *_aidl_return = false;
        return ndk::ScopedAStatus::ok();
    }
    
    const auto& stream = in_config.streams[0];
    if (stream.streamType != aidl::android::hardware::camera::device::StreamType::OUTPUT) {
        ALOGW("Stream configuration validation failed: Expected OUTPUT stream type, got %d", (int)stream.streamType);
        *_aidl_return = false;
        return ndk::ScopedAStatus::ok();
    }

    // Check if the requested stream configuration is among the supported ones.
    // The mStaticCharacteristics should have ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS
    const camera_metadata_t* meta = reinterpret_cast<const camera_metadata_t*>(mStaticCharacteristics.metadata.data());
    camera_metadata_ro_entry_t entry;
    int ret = find_camera_metadata_ro_entry(meta, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry);

    bool found = false;
    if (ret == 0 && (entry.count > 0 && (entry.count % 4 == 0))) { // Each config is 4 int32_t values
        for (size_t i = 0; i < entry.count; i += 4) {
            if (static_cast<aidl::android::hardware::graphics::common::PixelFormat>(entry.data.i32[i]) == stream.format &&
                entry.data.i32[i+1] == stream.width &&
                entry.data.i32[i+2] == stream.height &&
                entry.data.i32[i+3] == ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
                // Dataspace can be tricky. For this virtual HAL, we might be lenient or expect a common default.
                // For now, let's assume if format, width, height, and type match, it's supported.
                // A more robust check would also consider stream.dataSpace.
                ALOGI("Stream combination IS supported: format %d, w %d, h %d, type OUTPUT",
                      (int)stream.format, stream.width, stream.height);
                found = true;
                break;
            }
        }
    }

    if (found) {
        *_aidl_return = true;
    } else {
        ALOGW("Stream combination NOT supported: format %d, w %d, h %d, type %d", 
            (int)stream.format, stream.width, stream.height, (int)stream.streamType);
        ALOGI("Available stream configurations:");
        if (ret == 0 && (entry.count > 0 && (entry.count % 4 == 0))) {
            for (size_t i = 0; i < entry.count; i += 4) {
                 ALOGI("  format %d, w %d, h %d, type %d (OUTPUT is %d)",
                    entry.data.i32[i], entry.data.i32[i+1], entry.data.i32[i+2], entry.data.i32[i+3],
                    ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
            }
        } else {
            ALOGI("  None or malformed in characteristics.");
        }
        *_aidl_return = false;
    }
    
    return ndk::ScopedAStatus::ok();
}

std::shared_ptr<HalCameraSession> HalCameraDevice::getActiveSession() {
    std::lock_guard<std::mutex> lock(mLock);
    return mCurrentSession;
}

ndk::ScopedAStatus HalCameraDevice::getPhysicalCameraCharacteristics(const std::string& in_physicalCameraId, CameraMetadata* _aidl_return) {
    // This HAL doesn't support physical camera characteristics
    // Physical cameras are used for multi-camera setups (e.g., wide + tele)
    ALOGI("getPhysicalCameraCharacteristics called for physical camera: %s", in_physicalCameraId.c_str());
    if (_aidl_return) {
        _aidl_return->metadata.clear();
    }
    return ndk::ScopedAStatus::fromServiceSpecificError(-ENOSYS);
}

ndk::ScopedAStatus HalCameraDevice::getResourceCost(CameraResourceCost* _aidl_return) {
    // Return a default resource cost for this camera
    if (_aidl_return) {
        _aidl_return->resourceCost = 100; // Default cost value
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraDevice::openInjectionSession(const std::shared_ptr<ICameraDeviceCallback>& in_callback, std::shared_ptr<ICameraInjectionSession>* _aidl_return) {
    // This HAL doesn't support injection sessions
    // Injection sessions are used for testing and debugging
    ALOGI("openInjectionSession called but not supported");
    if (_aidl_return) {
        _aidl_return->reset();
    }
    return ndk::ScopedAStatus::fromServiceSpecificError(-ENOSYS);
}

ndk::ScopedAStatus HalCameraDevice::turnOnTorchWithStrengthLevel(int32_t in_torchStrength) {
    // This HAL doesn't support torch functionality
    ALOGI("turnOnTorchWithStrengthLevel called with strength: %d but not supported", in_torchStrength);
    return ndk::ScopedAStatus::fromServiceSpecificError(-ENOSYS);
}

ndk::ScopedAStatus HalCameraDevice::getTorchStrengthLevel(int32_t* _aidl_return) {
    // This HAL doesn't support torch functionality
    ALOGI("getTorchStrengthLevel called but not supported");
    if (_aidl_return) {
        *_aidl_return = 0;
    }
    return ndk::ScopedAStatus::fromServiceSpecificError(-ENOSYS);
}

} // namespace cambridge
} // namespace android
