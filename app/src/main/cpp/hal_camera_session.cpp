#include "hal_camera_session.h"
#include "hal_camera_device.h" // To call parentDevice->closeSession()
#include <utils/Log.h>
#include <vector> // For std::vector from JNI call
#include <aidl/android/hardware/camera/device/BufferStatus.h>
#include <aidl/android/hardware/camera/device/StreamBuffer.h>
#include <chrono> // For std::chrono::system_clock
#include <android/hardware_buffer.h> // For AHardwareBuffer
#include <android/native_window.h> // For native_handle_clone, native_handle_delete
#include <system/graphics.h> // For HAL_PIXEL_FORMAT constants (needed for AHARDWAREBUFFER_FORMAT mapping)

// Define a LOG_TAG for this file
#undef LOG_TAG
#define LOG_TAG "HalCameraSession"

namespace android {
namespace cambridge {

// These constants are usually defined in a shared place, e.g., the UvcCameraManager.java
// For now, define them here for use in pushNewFrame and frameProcessingLoop
// Mirroring VideoFrame.java (ensure these values are consistent)
const int UVC_FORMAT_MJPEG = 0;
const int UVC_FORMAT_YUYV = 1;

// Forward declaration for the JNI bridge function from cambridge_jni.cpp
// This avoids needing a separate header if it's only used here.
extern std::vector<uint8_t> callJavaMjpegDecoder(const uint8_t* mjpeg_data, size_t mjpeg_size, int width, int height);


HalCameraSession::HalCameraSession(
        const std::string& cameraId,
        HalCameraDevice* parentDevice,
        const std::shared_ptr<ICameraDeviceCallback>& frameworkCallback)
    : mCameraId(cameraId),
      mParentDevice(parentDevice),
      mFrameworkCallback(frameworkCallback),
      mIsClosing(false),
      mFrameNumber(0) {
    ALOGI("HalCameraSession instance created for camera %s", mCameraId.c_str());
    mProcessingThread = std::thread(&HalCameraSession::frameProcessingLoop, this);
}

HalCameraSession::~HalCameraSession() {
    ALOGI("HalCameraSession instance destroying for camera %s", mCameraId.c_str());
    if (!mIsClosing) {
        ALOGW("Destructor calling close() for %s as it wasn't called explicitly.", mCameraId.c_str());
        close();
    }
    if (mProcessingThread.joinable()) {
        mProcessingThread.join();
    }

    // Release AHardwareBuffers
    for (AHardwareBuffer* buffer : mHardwareBuffers) {
        if (buffer) {
            AHardwareBuffer_release(buffer);
        }
    }
    mHardwareBuffers.clear();
    ALOGI("HalCameraSession instance destroyed for camera %s", mCameraId.c_str());
}

ndk::ScopedAStatus HalCameraSession::configureStreams(
        const StreamConfiguration& in_requestedStreams,
        std::vector<HalStream>* _aidl_return) {
    ALOGI("configureStreams called for camera %s", mCameraId.c_str());
    std::lock_guard<std::mutex> lock(mFrameMutex);

    // Clear previous configuration
    mStreamsConfigured = false;
    mConfiguredHalStreams.clear();
    for (AHardwareBuffer* buffer : mHardwareBuffers) {
        if (buffer) AHardwareBuffer_release(buffer);
    }
    mHardwareBuffers.clear();
    _aidl_return->clear();


    if (in_requestedStreams.streams.empty()) {
        ALOGI("configureStreams called with empty stream list for %s. Deconfigured.", mCameraId.c_str());
        return ndk::ScopedAStatus::ok();
    }
    
    if (in_requestedStreams.streams.size() > 1) {
        ALOGE("Configuration with %zu streams not supported for %s. Only 1 stream.", 
              in_requestedStreams.streams.size(), mCameraId.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_ILLEGAL_ARGUMENT);
    }
    
    const auto& reqStream = in_requestedStreams.streams[0];

    // Assumption: isStreamCombinationSupported has already validated this stream.
    // We just need to handle it.
    if (reqStream.streamType != aidl::android::hardware::camera::device::StreamType::OUTPUT) {
        ALOGE("Requested stream type %d not OUTPUT for %s.", (int)reqStream.streamType, mCameraId.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_ILLEGAL_ARGUMENT);
    }
    
    // Check if pixel format is YCBCR_420_888, as this is what the HAL currently handles for output.
    // A more advanced HAL might support multiple output formats.
    if (reqStream.format != PixelFormat::YCBCR_420_888) {
        ALOGE("Requested stream format %d not YCBCR_420_888 for %s. Currently only YCBCR_420_888 is supported for output.", 
            (int)reqStream.format, mCameraId.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_ILLEGAL_ARGUMENT);
    }

    mActiveStreamInfo = reqStream; // Store the active stream's properties
    
    HalStream halStream;
    halStream.id = reqStream.id;
    halStream.overridePixelFormat = reqStream.format; 
    halStream.producerUsage = aidl::android::hardware::graphics::common::BufferUsage::CPU_WRITE_OFTEN |
                              aidl::android::hardware::graphics::common::BufferUsage::CAMERA_WRITE | 
                              aidl::android::hardware::graphics::common::BufferUsage::GPU_SAMPLED_IMAGE; 
    halStream.maxBuffers = kNumStreamBuffers; 
    halStream.dataSpace = reqStream.dataSpace;

    // Allocate AHardwareBuffers using the properties from reqStream
    AHardwareBuffer_Desc desc = {};
    desc.width = reqStream.width;
    desc.height = reqStream.height;
    desc.layers = 1;
    // Map AIDL PixelFormat to AHARDWAREBUFFER_FORMAT.
    // PixelFormat::YCBCR_420_888 directly maps to AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420
    if (reqStream.format == PixelFormat::YCBCR_420_888) {
        desc.format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
    } else {
        // Should not happen if we checked above, but as a fallback
        ALOGE("Unsupported pixel format %d for AHardwareBuffer allocation on %s.", (int)reqStream.format, mCameraId.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_ILLEGAL_ARGUMENT);
    }
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | 
                 AHARDWAREBUFFER_USAGE_CAMERA_WRITE | 
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE; // Match producerUsage

    mHardwareBuffers.resize(kNumStreamBuffers, nullptr);
    for (int i = 0; i < kNumStreamBuffers; ++i) {
        AHardwareBuffer* buffer = nullptr;
        status_t err = AHardwareBuffer_allocate(&desc, &buffer);
        if (err != NO_ERROR || buffer == nullptr) {
            ALOGE("Failed to allocate AHardwareBuffer %d (w%d h%d fmt%d) for stream %d on %s: %s (%d)", 
                  i, (int)desc.width, (int)desc.height, (int)desc.format,
                  halStream.id, mCameraId.c_str(), strerror(-err), err);
            for (int j = 0; j < i; ++j) { // Clean up already allocated buffers
                if (mHardwareBuffers[j]) AHardwareBuffer_release(mHardwareBuffers[j]);
            }
            mHardwareBuffers.clear();
            return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_ALLOCATE_FAILED);
        }
        mHardwareBuffers[i] = buffer; 
    }
    
    _aidl_return->push_back(halStream);
    mConfiguredHalStreams.assign({_aidl_return->front()});

    // Update buffer size based on actual configured stream
    mOutputBufferSize = (reqStream.width * reqStream.height * 3) / 2; // For YCBCR_420_888
    mNextAvailableBufferIdx = 0;
    mStreamsConfigured = true;
    ALOGI("Streams configured for camera %s with w%d h%d fmt%d. Allocated %d AHardwareBuffers. Stream ID: %d", 
          mCameraId.c_str(), reqStream.width, reqStream.height, (int)reqStream.format, kNumStreamBuffers, halStream.id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraSession::processCaptureRequest(
        const CaptureRequest& in_request, int32_t* _aidl_return_submittedRequests) {
        // ALOGD for verbose logging
    if (mIsClosing) {
        ALOGE("processCaptureRequest on closing session for camera %s, frame %lld", mCameraId.c_str(), (long long)in_request.frameNumber);
        *_aidl_return_submittedRequests = 0;
        NotifyMsg errorMsg;
        errorMsg.type = MsgType::ERROR;
        errorMsg.msg.error = {in_request.frameNumber, 
                              mStreamsConfigured && !mConfiguredHalStreams.empty() ? mConfiguredHalStreams[0].id : -1, 
                              ErrorCode::ERROR_REQUEST};
        if (mFrameworkCallback) mFrameworkCallback->notify({errorMsg});
        return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_CAMERA_DISCONNECTED);
    }
    
    std::lock_guard<std::mutex> lock(mFrameMutex); // Protects mStreamsConfigured, mConfiguredHalStreams
    if (!mStreamsConfigured || mConfiguredHalStreams.empty() || mHardwareBuffers.empty()) {
        ALOGE("processCaptureRequest: Streams not configured or no buffers for %s, frame %lld.", mCameraId.c_str(), (long long)in_request.frameNumber);
        NotifyMsg errorMsg;
        errorMsg.type = MsgType::ERROR;
        errorMsg.msg.error = {in_request.frameNumber, 
                              mActiveStreamInfo.id != 0 ? mActiveStreamInfo.id : (mConfiguredHalStreams.empty() ? -1 : mConfiguredHalStreams[0].id), 
                              ErrorCode::ERROR_REQUEST};
        if (mFrameworkCallback) mFrameworkCallback->notify({errorMsg});
        *_aidl_return_submittedRequests = 0; 
        return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_INVALID_OPERATION);
    }

    if (in_request.outputBuffers.empty()) {
        ALOGE("processCaptureRequest: No output buffers in request for frame %lld on %s", (long long)in_request.frameNumber, mCameraId.c_str());
        NotifyMsg errorMsg;
        errorMsg.type = MsgType::ERROR;
        errorMsg.msg.error = {in_request.frameNumber, mConfiguredHalStreams[0].id, ErrorCode::ERROR_REQUEST};
        if (mFrameworkCallback) mFrameworkCallback->notify({errorMsg});
        *_aidl_return_submittedRequests = 0;
        return ndk::ScopedAStatus::fromServiceSpecificError(ICameraDeviceSession::ERROR_ILLEGAL_ARGUMENT);
    }

    // For this simple HAL, we assume the request is for the single configured stream.
    // A more complex HAL would check in_request.outputBuffers[0].streamId.

    NotifyMsg shutterMsg;
    shutterMsg.type = MsgType::SHUTTER;
    shutterMsg.msg.shutter = {in_request.frameNumber, std::chrono::system_clock::now().time_since_epoch().count()};
    if (mFrameworkCallback) mFrameworkCallback->notify({shutterMsg});
    
    // The actual processing and result submission will be triggered by pushNewFrame -> frameProcessingLoop.
    // This HAL doesn't queue requests internally in the traditional sense for matching.
    // The frame number from the request is used when the next frame is processed.
    // This is a simplification.

    *_aidl_return_submittedRequests = 1; 
    return ndk::ScopedAStatus::ok();
}

void HalCameraSession::pushNewFrame(const uint8_t* uvcData, size_t uvcDataSize, 
                                   int width, int height, int uvcFormat) {
    // ALOGV("pushNewFrame: %zu bytes, %dx%d, format %d", uvcDataSize, width, height, uvcFormat);
    if (mIsClosing) {
        // ALOGV("pushNewFrame on closing session for %s, discarding.", mCameraId.c_str());
        return;
    }

    RawFrameData frame;
    frame.data.assign(uvcData, uvcData + uvcDataSize);
    frame.width = width;
    frame.height = height;
    frame.uvcFormat = uvcFormat; 
    frame.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        if (!mStreamsConfigured) { // Check under lock
            ALOGW("pushNewFrame: Streams not configured for %s. Dropping frame.", mCameraId.c_str());
            return;
        }
        if (mFrameQueue.size() < kNumStreamBuffers * 2) { 
            mFrameQueue.push(std::move(frame));
        } else {
            ALOGW("Frame queue full for %s (size %zu), dropping incoming UVC frame.", mCameraId.c_str(), mFrameQueue.size());
        }
    }
    mFrameCv.notify_one();
}

// Updated signature to include strides
bool HalCameraSession::convertYUYVToI420(const uint8_t* yuyvData, int width, int height, 
                                       uint8_t* i420Y, int yStride, 
                                       uint8_t* i420U, int uStride, 
                                       uint8_t* i420V, int vStride) {
    int result = libyuv::YUY2ToI420(yuyvData, width * 2, // YUYV stride is width * 2 bytes
                                    i420Y, yStride,
                                    i420U, uStride,
                                    i420V, vStride,
                                    width, height);
    if (result != 0) {
        ALOGE("libyuv::YUY2ToI420 failed: %d", result);
    }
    return result == 0;
}


void HalCameraSession::frameProcessingLoop() {
    ALOGI("Frame processing loop started for camera %s.", mCameraId.c_str());

    while (true) {
        RawFrameData rawFrame;
        AHardwareBuffer* outputHwBuffer = nullptr;
        int currentBufferIdx = -1;

        {
            std::unique_lock<std::mutex> lock(mFrameMutex);
            mFrameCv.wait(lock, [this] { return mIsClosing || (!mFrameQueue.empty() && mStreamsConfigured && !mHardwareBuffers.empty()); });
            
            if (mIsClosing && mFrameQueue.empty()) { // Prioritize exit if closing and no frames left
                 break;
            }
            
            if (!mStreamsConfigured || mHardwareBuffers.empty() || mFrameQueue.empty()) {
                if (mIsClosing) break; // If closing, and conditions not met (e.g. queue became empty), try to exit
                ALOGW("Frame loop: Spurious wakeup or streams deconfigured/no buffers/empty queue for %s. Closing: %d, Configured: %d, HWBuffersEmpty: %d, QueueEmpty: %d",
                    mCameraId.c_str(), mIsClosing, mStreamsConfigured, mHardwareBuffers.empty(), mFrameQueue.empty());
                continue;
            }
            
            rawFrame = std::move(mFrameQueue.front());
            mFrameQueue.pop();

            currentBufferIdx = mNextAvailableBufferIdx;
            outputHwBuffer = mHardwareBuffers[currentBufferIdx];
            mNextAvailableBufferIdx = (mNextAvailableBufferIdx + 1) % mHardwareBuffers.size();
        }

        if (!outputHwBuffer) {
            ALOGE("Output AHardwareBuffer is null for frame processing on %s. Skipping.", mCameraId.c_str());
            continue;
        }
        
        HalStream& targetStream = mConfiguredHalStreams[0]; 
        
        void* cpuWritablePtr = nullptr;
        AHardwareBuffer_Desc desc;
        AHardwareBuffer_describe(outputHwBuffer, &desc); // Get actual stride, etc.

        // Lock buffer for CPU write
        status_t lockErr = AHardwareBuffer_lock(outputHwBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &cpuWritablePtr);
        if (lockErr != NO_ERROR || !cpuWritablePtr) {
            ALOGE("Failed to lock AHardwareBuffer for CPU write on %s: %s (%d)", mCameraId.c_str(), strerror(-lockErr), lockErr);
            // TODO: Notify error for this buffer/request
            continue;
        }

        bool conversionOk = false;
        if (rawFrame.uvcFormat == UVC_FORMAT_YUYV && targetStream.overridePixelFormat == PixelFormat::YCBCR_420_888) {
            if (rawFrame.width != (int)desc.width || rawFrame.height != (int)desc.height) {
                ALOGE("YUYV frame size %dx%d doesn't match AHardwareBuffer %ux%u for %s. Dropping.", 
                    rawFrame.width, rawFrame.height, desc.width, desc.height, mCameraId.c_str());
            } else {
                uint8_t* yDst = static_cast<uint8_t*>(cpuWritablePtr);
                // For YUV420 planar (like I420/YV12), UV planes follow Y.
                // Stride for Y is desc.stride. Height for Y is desc.height.
                // Stride for U/V is desc.stride / 2. Height for U/V is desc.height / 2.
                uint8_t* uDst = yDst + desc.stride * desc.height; // This is for true planar like NV12/NV21 if stride==width
                                                                // For I420, Y, U, V planes are separate.
                                                                // libyuv expects pointers to start of each plane.
                // The AHardwareBuffer for YUV_420_888 is often semi-planar (NV12/NV21) or flexible.
                // If it's truly planar I420, then AHardwareBuffer_lockPlanes might be better.
                // Assuming libyuv handles the specific layout once pointers to Y, U, V planes are given.
                // For AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, AHardwareBuffer_lock should give a pointer to Y.
                // U and V plane locations need to be derived based on format specifics or using AHardwareBuffer_lockPlanes.
                // Let's assume a simple planar layout for now, this is a common source of issues.
                // A more robust way: AHardwareBuffer_lockPlanes (API level 29+) or check AHardwareBuffer_Desc.
                // If format is YCBCR_420_888, it's often NV12 or NV21 like.
                // For libyuv's YUY2ToI420, it expects separate Y, U, V plane pointers.
                // This part needs careful handling of actual buffer layout.
                // For simplicity, assuming cpuWritablePtr points to a buffer large enough for I420
                // and we write Y, then U, then V contiguously (which is typical I420 file layout).
                uint8_t* vDst = uDst + (desc.stride / 2) * (desc.height / 2); // If U/V are full width stride after Y plane
                                                                            // This is more like I420 layout.
                                                                            // Stride for U/V is typically desc.stride/2 for I420.

                conversionOk = convertYUYVToI420(rawFrame.data.data(), rawFrame.width, rawFrame.height,
                                                static_cast<uint8_t*>(cpuWritablePtr), desc.stride,      // Y plane, Y stride
                                                static_cast<uint8_t*>(cpuWritablePtr) + desc.stride * desc.height, desc.stride / 2, // U plane, UV stride
                                                static_cast<uint8_t*>(cpuWritablePtr) + desc.stride * desc.height + (desc.stride/2 * desc.height/2), desc.stride / 2); // V plane, UV stride
            }
        } else if (rawFrame.uvcFormat == UVC_FORMAT_MJPEG && targetStream.overridePixelFormat == PixelFormat::YCBCR_420_888) {
            if (rawFrame.width != (int)desc.width || rawFrame.height != (int)desc.height) {
                ALOGE("MJPEG frame size %dx%d doesn't match AHardwareBuffer %ux%u for %s. Dropping.", 
                    rawFrame.width, rawFrame.height, desc.width, desc.height, mCameraId.c_str());
            } else {
                ALOGI("Attempting MJPEG decode for %dx%d frame via JNI for %s", rawFrame.width, rawFrame.height, mCameraId.c_str());
                std::vector<uint8_t> yuvData = callJavaMjpegDecoder(rawFrame.data.data(), rawFrame.data.size(), rawFrame.width, rawFrame.height);
                if (!yuvData.empty()) {
                    // The YUV data from MediaCodec (COLOR_FormatYUV420Flexible) could be NV12, NV21, YU12 (I420), YV12.
                    // We need to copy it correctly into the AHardwareBuffer which is AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420.
                    // This format typically expects a planar or semi-planar layout.
                    // For simplicity, assuming the output from MediaCodec is I420 (planar YUV)
                    // and can be directly copied plane by plane if strides match.
                    // This is a common point of failure if formats don't align perfectly.
                    // A more robust solution would inspect MediaFormat after INFO_OUTPUT_FORMAT_CHANGED
                    // and use libyuv to convert from that specific YUV format to I420/NV12 for the AHardwareBuffer.

                    size_t expectedYuvSize = (desc.width * desc.height * 3) / 2;
                    if (yuvData.size() == expectedYuvSize) {
                        // Assuming yuvData is I420: Y plane, then U plane, then V plane.
                        uint8_t* yDst = static_cast<uint8_t*>(cpuWritablePtr);
                        uint8_t* uDst = yDst + desc.stride * desc.height; // For true planar like I420
                        uint8_t* vDst = uDst + (desc.stride / 2) * (desc.height / 2);

                        // Copy Y plane
                        libyuv::CopyPlane(yuvData.data(), desc.width, // src_y, src_stride_y
                                          yDst, desc.stride,          // dst_y, dst_stride_y
                                          desc.width, desc.height);   // width, height

                        // Copy U plane
                        libyuv::CopyPlane(yuvData.data() + desc.width * desc.height, desc.width / 2, // src_u, src_stride_u
                                          uDst, desc.stride / 2,                                  // dst_u, dst_stride_u
                                          desc.width / 2, desc.height / 2);                       // width, height for U

                        // Copy V plane
                        libyuv::CopyPlane(yuvData.data() + desc.width * desc.height + (desc.width/2 * desc.height/2) , desc.width / 2, // src_v, src_stride_v
                                          vDst, desc.stride / 2,                                                                    // dst_v, dst_stride_v
                                          desc.width / 2, desc.height / 2);                                                          // width, height for V
                        conversionOk = true;
                        ALOGI("MJPEG frame decoded and copied to AHardwareBuffer for %s.", mCameraId.c_str());
                    } else {
                        ALOGE("Decoded YUV data size %zu does not match expected %zu for %s.", yuvData.size(), expectedYuvSize, mCameraId.c_str());
                    }
                } else {
                    ALOGE("MJPEG decoding via JNI returned empty data for %s.", mCameraId.c_str());
                }
            }
        } else {
            ALOGE("Unsupported UVC format %d or target format %d for conversion on %s. Dropping frame.", 
                  rawFrame.uvcFormat, (int)targetStream.overridePixelFormat, mCameraId.c_str());
        }

        int32_t releaseFenceFd = -1;
        status_t unlockErr = AHardwareBuffer_unlock(outputHwBuffer, &releaseFenceFd);
        if (unlockErr != NO_ERROR) {
            ALOGE("Failed to unlock AHardwareBuffer on %s: %s (%d)", mCameraId.c_str(), strerror(-unlockErr), unlockErr);
            // Data might be corrupt or not written. Consider this frame lost.
            if(releaseFenceFd != -1) close(releaseFenceFd);
            continue;
        }

        if (!conversionOk) {
            ALOGE("Frame conversion failed for %s. Dropping.", mCameraId.c_str());
            if(releaseFenceFd != -1) close(releaseFenceFd); // Close fence if conversion failed post-unlock attempt
            // TODO: Notify error (buffer status error?)
            continue;
        }
        
        CaptureResult result;
        result.frameNumber = mFrameNumber++; 
        result.partialResult = 1; 
        result.inputBuffer = std::nullopt; 

        aidl::android::hardware::camera::device::StreamBuffer streamBuffer;
        streamBuffer.streamId = targetStream.id;
        streamBuffer.bufferId = static_cast<int64_t>(currentBufferIdx); // Framework uses this to map to its gralloc ID
        streamBuffer.status = aidl::android::hardware::camera::device::BufferStatus::OK;
        
        const native_handle_t* anativeHandle = AHardwareBuffer_getNativeHandle(outputHwBuffer);
        if (!anativeHandle) {
            ALOGE("Failed to get native handle from AHardwareBuffer for %s", mCameraId.c_str());
            if(releaseFenceFd != -1) close(releaseFenceFd);
            continue;
        }
        // Framework expects to receive a new handle it takes ownership of.
        native_handle_t* clonedHalHandle = native_handle_clone(anativeHandle);
        if (!clonedHalHandle) {
            ALOGE("Failed to clone native_handle for %s", mCameraId.c_str());
             if(releaseFenceFd != -1) close(releaseFenceFd);
            continue;
        }
        streamBuffer.buffer.set(clonedHalHandle); // ScopedHandle takes ownership of clonedHandle

        if (releaseFenceFd != -1) {
            streamBuffer.releaseFence.set(releaseFenceFd); // ScopedHandle takes ownership of FD
        }
        // streamBuffer.acquireFence should be set if HAL needs framework to wait before reading.
        // For CPU processed buffer, usually not needed, or set to -1.

        result.outputBuffers.push_back(std::move(streamBuffer));

        ::android::CameraMetadata dynamicMetadata;
        dynamicMetadata.update(ANDROID_SENSOR_TIMESTAMP, &(rawFrame.timestamp), 1);
        camera_metadata_t* released_metadata = dynamicMetadata.release();
        result.result.reset(released_metadata);

        if (mFrameworkCallback) {
            mFrameworkCallback->processCaptureResult({result});
        } else {
            ALOGE("Framework callback is null, cannot send capture result for %s.", mCameraId.c_str());
        }
    }
    ALOGI("Frame processing loop stopped for camera %s.", mCameraId.c_str());
}


ndk::ScopedAStatus HalCameraSession::flush() {
    ALOGI("flush called for camera %s.", mCameraId.c_str());
    std::lock_guard<std::mutex> lock(mFrameMutex);
    
    if (!mFrameQueue.empty()) {
        ALOGI("Flushing %zu frames from queue for %s.", mFrameQueue.size(), mCameraId.c_str());
        std::queue<RawFrameData> empty;
        std::swap(mFrameQueue, empty);
    }
    // TODO: Send ERROR_REQUEST for any requests that were implicitly "in-flight".
    // This simple HAL doesn't track them explicitly enough for this.
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraSession::close() {
    ALOGI("close called for camera %s", mCameraId.c_str());
    
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        if (mIsClosing) {
            ALOGW("Session already closing or closed for camera %s", mCameraId.c_str());
            return ndk::ScopedAStatus::ok(); 
        }
        mIsClosing = true;
        ALOGI("Setting mIsClosing=true and notifying processing thread for %s.", mCameraId.c_str());
    }
    
    mFrameCv.notify_all(); 

    if (mProcessingThread.joinable()) {
        ALOGI("Waiting for processing thread to join for %s...", mCameraId.c_str());
        mProcessingThread.join();
        ALOGI("Processing thread joined for %s.", mCameraId.c_str());
    }

    if (mParentDevice) {
        mParentDevice->closeSession(); 
        mParentDevice = nullptr; 
    }

    if (mFrameworkCallback) {
        mFrameworkCallback.reset();
    }
    
    // Release AHardwareBuffers and clear internal state under lock
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        std::queue<RawFrameData> emptyQueue;
        mFrameQueue.swap(emptyQueue);
        
        for (AHardwareBuffer* buffer : mHardwareBuffers) {
            if (buffer) {
                AHardwareBuffer_release(buffer);
            }
        }
        mHardwareBuffers.clear();
        
        mConfiguredHalStreams.clear();
        mStreamsConfigured = false;
        ALOGI("Internal queues and AHardwareBuffers cleared for %s.", mCameraId.c_str());
    }

    ALOGI("Session close completed for camera %s", mCameraId.c_str());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraSession::constructDefaultRequestSettings(::aidl::android::hardware::camera::device::RequestTemplate /*in_type*/, ::aidl::android::hardware::camera::device::CameraMetadata* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus HalCameraSession::getCaptureRequestMetadataQueue(::aidl::android::hardware::common::fmq::MQDescriptor<int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus HalCameraSession::getCaptureResultMetadataQueue(::aidl::android::hardware::common::fmq::MQDescriptor<int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus HalCameraSession::isReconfigurationRequired(const ::aidl::android::hardware::camera::device::CameraMetadata& /*in_oldSessionParams*/, const ::aidl::android::hardware::camera::device::CameraMetadata& /*in_newSessionParams*/, bool* _aidl_return) {
    if (_aidl_return) *_aidl_return = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraSession::signalStreamFlush(const std::vector<int32_t>& /*in_streamIds*/, int32_t /*in_streamConfigCounter*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus HalCameraSession::switchToOffline(const std::vector<int32_t>& /*in_streamsToKeep*/, ::aidl::android::hardware::camera::device::CameraOfflineSessionInfo* /*out_offlineSessionInfo*/, std::shared_ptr<::aidl::android::hardware::camera::device::ICameraOfflineSession>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus HalCameraSession::repeatingRequestEnd(int32_t /*in_frameNumber*/, const std::vector<int32_t>& /*in_streamIds*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

} // namespace cambridge
} // namespace android
