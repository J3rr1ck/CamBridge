#pragma once

#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/StreamConfiguration.h>
#include <aidl/android/hardware/camera/device/CaptureRequest.h>
#include <aidl/android/hardware/camera/device/CaptureResult.h>
#include <aidl/android/hardware/camera/device/NotifyMsg.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
// #include <android/hardware/camera/common/include/android/hardware/camera/common/CameraMetadata.h> // REMOVED: Not available in AOSP, use <system/camera_metadata.h> if needed
#include <system/camera_metadata.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <android/hardware_buffer.h> // For AHardwareBuffer

// libyuv includes
#include "libyuv/convert.h"
#include "libyuv/planar_functions.h"

#include <aidl/android/hardware/camera/device/BufferCache.h>

// Forward declare HalCameraDevice
namespace android {
namespace cambridge {
class HalCameraDevice;

using ::aidl::android::hardware::camera::device::BnCameraDeviceSession;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::StreamConfiguration;
using ::aidl::android::hardware::camera::device::Stream;
using ::aidl::android::hardware::camera::device::CaptureRequest;
using ::aidl::android::hardware::camera::device::CaptureResult;
using ::aidl::android::hardware::camera::device::NotifyMsg;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using ::aidl::android::hardware::camera::device::HalStream;
using ::aidl::android::hardware::camera::device::BufferCache;


// Simple structure for raw frames coming from JNI
struct RawFrameData {
    std::vector<uint8_t> data;
    int width;
    int height;
    int uvcFormat; // e.g., VideoFrame.FORMAT_YUYV, VideoFrame.FORMAT_MJPEG
    uint64_t timestamp; // Optional: capture timestamp
};

class HalCameraSession : public BnCameraDeviceSession {
public:
    HalCameraSession(const std::string& cameraId,
                     HalCameraDevice* parentDevice,
                     const std::shared_ptr<ICameraDeviceCallback>& frameworkCallback);
    ~HalCameraSession() override;

    // --- AIDL ICameraDeviceSession methods ---
    ndk::ScopedAStatus configureStreams(const StreamConfiguration& in_requestedStreams,
                                        std::vector<HalStream>* _aidl_return) override;
    ndk::ScopedAStatus processCaptureRequest(
        const std::vector<CaptureRequest>& in_requests,
        const std::vector<BufferCache>& in_cachesToRemove,
        int32_t* _aidl_return) override;
    ndk::ScopedAStatus flush() override;
    ndk::ScopedAStatus close() override;
    
    // --- Custom methods ---
    // Called by JNI to push a new frame
    void pushNewFrame(const uint8_t* uvcData, size_t uvcDataSize, 
                      int width, int height, int uvcFormat);

private:
    void frameProcessingLoop();
    // Updated signature
    bool convertYUYVToI420(const uint8_t* yuyvData, int width, int height, 
                           uint8_t* i420Y, int yStride, uint8_t* i420U, int uStride, uint8_t* i420V, int vStride);

    const std::string mCameraId;
    HalCameraDevice* mParentDevice; // Not owning
    std::shared_ptr<ICameraDeviceCallback> mFrameworkCallback;

    std::vector<HalStream> mConfiguredHalStreams;
    // For simplicity, assume one output stream, fixed properties
    Stream mActiveStreamInfo; // Stores the currently configured stream's properties
    bool mStreamsConfigured = false;
    // int mDefaultWidth = 640; // Replaced by mActiveStreamInfo.width
    // int mDefaultHeight = 480; // Replaced by mActiveStreamInfo.height
    // PixelFormat mDefaultPixelFormat = PixelFormat::YCBCR_420_888; // Replaced by mActiveStreamInfo.format
    size_t mOutputBufferSize = 0; // Calculated based on mActiveStreamInfo

    // Frame processing thread
    std::thread mProcessingThread;
    std::mutex mFrameMutex;
    std::condition_variable mFrameCv;
    std::queue<RawFrameData> mFrameQueue;
    bool mIsClosing = false;

    // Buffer management for the output stream using AHardwareBuffer
    std::vector<AHardwareBuffer*> mHardwareBuffers; // Store raw pointers, manage lifecycle
    int mNextAvailableBufferIdx = 0;
    const int kNumStreamBuffers = 4; // Number of buffers for the output stream

    uint32_t mFrameNumber = 0;
};

} // namespace cambridge
} // namespace android
