#pragma once

#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <aidl/android/hardware/camera/device/ICameraDevice.h>
#include <map>
#include <string>
#include <memory>
#include <mutex> // Added for std::mutex
#include "hal_camera_session.h"

// Forward declare HalCameraDevice if its header isn't created in this step
// class HalCameraDevice; 
// Forward declaration for HalCameraDevice. Full include will be in .cpp
namespace android {
namespace cambridge {
class HalCameraDevice; 
}
}

class HalCameraSession;

namespace android {
namespace cambridge {

using ::aidl::android::hardware::camera::common::CameraDeviceStatus;
using ::aidl::android::hardware::camera::provider::ICameraProviderCallback;
using ::aidl::android::hardware::camera::device::ICameraDevice;

class HalCameraProvider : public ::aidl::android::hardware::camera::provider::BnCameraProvider {
public:
    HalCameraProvider();
    ~HalCameraProvider() override;

    // --- AIDL ICameraProvider methods ---
    ndk::ScopedAStatus setCallback(const std::shared_ptr<ICameraProviderCallback>& in_callback) override;
    ndk::ScopedAStatus getCameraIdList(std::vector<std::string>* _aidl_return) override;
    ndk::ScopedAStatus getCameraDeviceInterface(const std::string& in_cameraDeviceName, std::shared_ptr<ICameraDevice>* _aidl_return) override;
    // ndk::ScopedAStatus getConcurrentCameraStreamingMode(StreamingMode* _aidl_return) override;
    // ndk::ScopedAStatus isSetTorchModeAvailable(bool* _aidl_return) override;
    // ndk::ScopedAStatus notifyDeviceStateChange(int64_t in_deviceState) override; // This is for physical device changes, not used here

    // --- Custom methods ---
    void initialize(); 
    void cleanup();    
    void signalDeviceAvailable(const std::string& cameraId, bool available); 
    void onDeviceClosed(const std::string& cameraId); // Called by HalCameraDevice
    std::shared_ptr<HalCameraSession> getActiveSessionForCameraId(const std::string& cameraId);

    // Add stubs for all required pure virtual methods from the AIDL base class
    ndk::ScopedAStatus getVendorTags(std::vector<::aidl::android::hardware::camera::common::VendorTagSection>* _aidl_return) override;
    ndk::ScopedAStatus notifyDeviceStateChange(int64_t in_deviceState) override;
    ndk::ScopedAStatus getConcurrentCameraIds(std::vector<::aidl::android::hardware::camera::provider::ConcurrentCameraIdCombination>* _aidl_return) override;
    ndk::ScopedAStatus isConcurrentStreamCombinationSupported(const std::vector<::aidl::android::hardware::camera::provider::CameraIdAndStreamCombination>& in_configs, bool* _aidl_return) override;

private:
    std::shared_ptr<ICameraProviderCallback> mFrameworkCallback;
    std::string mVirtualCameraId; // e.g., "0"
    std::shared_ptr<HalCameraDevice> mCameraDeviceInstance; 
    bool mIsDeviceAvailable; 
    std::mutex mLock; 

    // Helper to create and cache HalCameraDevice instance, called with mLock held
    std::shared_ptr<ICameraDevice> getOrCreateCameraDeviceInternal(const std::string& cameraDeviceName);
};

} // namespace cambridge
} // namespace android
