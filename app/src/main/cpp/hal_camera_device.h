#pragma once

#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceSession.h>
#include <system/camera_metadata.h> // Use the public camera metadata API
#include <aidl/android/hardware/camera/device/CameraMetadata.h>
#include <aidl/android/hardware/camera/common/CameraResourceCost.h>
#include <aidl/android/hardware/camera/device/ICameraInjectionSession.h>
#include <camera/CameraMetadata.h>
// #include <android/hardware/camera/common/include/android/hardware/camera/common/CameraMetadata.h> // REMOVED: Not available in AOSP, use <system/camera_metadata.h> if needed

// Forward declare HalCameraProvider and HalCameraSession
namespace android {
namespace cambridge {
class HalCameraProvider; // From previous step
class HalCameraSession;  // Will be implemented next

using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::ICameraDeviceSession;
using ::aidl::android::hardware::camera::device::StreamConfiguration;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::common::CameraResourceCost;
using ::aidl::android::hardware::camera::device::ICameraInjectionSession;


class HalCameraDevice : public ::aidl::android::hardware::camera::device::BnCameraDevice {
public:
    HalCameraDevice(const std::string& cameraId, HalCameraProvider* parentProvider);
    ~HalCameraDevice() override;

    // --- AIDL ICameraDevice methods ---
    ndk::ScopedAStatus getCameraCharacteristics(CameraMetadata* _aidl_return) override;
    ndk::ScopedAStatus getPhysicalCameraCharacteristics(const std::string& in_physicalCameraId, CameraMetadata* _aidl_return) override;
    ndk::ScopedAStatus getResourceCost(CameraResourceCost* _aidl_return) override;
    ndk::ScopedAStatus open(const std::shared_ptr<ICameraDeviceCallback>& in_callback,
                            std::shared_ptr<ICameraDeviceSession>* _aidl_return) override;
    ndk::ScopedAStatus openInjectionSession(const std::shared_ptr<ICameraDeviceCallback>& in_callback,
                            std::shared_ptr<ICameraInjectionSession>* _aidl_return) override;
    ndk::ScopedAStatus setTorchMode(bool in_enabled) override;
    ndk::ScopedAStatus turnOnTorchWithStrengthLevel(int32_t in_torchStrength) override;
    ndk::ScopedAStatus getTorchStrengthLevel(int32_t* _aidl_return) override;
    ndk::ScopedAStatus dumpState(const ::ndk::ScopedFileDescriptor& in_fd);
    ndk::ScopedAStatus isStreamCombinationSupported(const StreamConfiguration& in_streams, 
                                                    bool* _aidl_return) override;
    
    // --- Custom methods ---
    void initializeCharacteristics(); // Helper to build static metadata
    void closeSession(); // Called by HalCameraSession when it closes
    std::shared_ptr<HalCameraSession> getActiveSession(); // New method

private:
    const std::string mCameraId;
    HalCameraProvider* mParentProvider; // Weak_ptr might be safer if lifecycle complex
    CameraMetadata mStaticCharacteristics;
    std::shared_ptr<HalCameraSession> mCurrentSession;
    std::mutex mLock; // For protecting session creation/access
};

} // namespace cambridge
} // namespace android
