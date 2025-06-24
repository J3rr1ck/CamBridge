#pragma once

#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceSession.h>
#include <camera_metadata_hidden.h> // For camera_metadata_t utilities if needed
#include <android/hardware/camera/common/include/android/hardware/camera/common/CameraMetadata.h> // For CameraMetadata type

// Forward declare HalCameraProvider and HalCameraSession
namespace android {
namespace cambridge {
class HalCameraProvider; // From previous step
class HalCameraSession;  // Will be implemented next

using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::ICameraDeviceSession;
using ::aidl::android::hardware::camera::device::StreamConfiguration;
using ::aidl::android::hardware::camera::common::CameraMetadata;


class HalCameraDevice : public ::aidl::android::hardware::camera::device::BnCameraDevice {
public:
    HalCameraDevice(const std::string& cameraId, HalCameraProvider* parentProvider);
    ~HalCameraDevice() override;

    // --- AIDL ICameraDevice methods ---
    ndk::ScopedAStatus getCameraCharacteristics(CameraMetadata* _aidl_return) override;
    ndk::ScopedAStatus open(const std::shared_ptr<ICameraDeviceCallback>& in_callback,
                            std::shared_ptr<ICameraDeviceSession>* _aidl_return) override;
    ndk::ScopedAStatus setTorchMode(bool in_enabled) override;
    ndk::ScopedAStatus dumpState(const ::ndk::ScopedFileDescriptor& in_fd) override;
    ndk::ScopedAStatus isStreamCombinationSupported(const StreamConfiguration& in_streams, 
                                                    bool* _aidl_return) override;
    
    // --- Custom methods ---
    void initializeCharacteristics(); // Helper to build static metadata
    void closeSession(); // Called by HalCameraSession when it closes
    std::shared_ptr<HalCameraSession> getActiveSession(); // New method

private:
    const std::string mCameraId;
    HalCameraProvider* mParentProvider; // Weak_ptr might be safer if lifecycle complex
    ::android::hardware::camera::common::CameraMetadata mStaticCharacteristics;
    std::shared_ptr<HalCameraSession> mCurrentSession;
    std::mutex mLock; // For protecting session creation/access
};

} // namespace cambridge
} // namespace android
