#include "hal_camera_provider.h"
#include "hal_camera_device.h" // This will be created in a subsequent step
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <utils/Log.h> // For ALOGE, ALOGI, etc.

// Define a LOG_TAG for this file
#undef LOG_TAG
#define LOG_TAG "HalCameraProvider"

namespace android {
namespace cambridge {

// Assuming these constants are defined in the ICameraProvider AIDL or a common place
// For now, let's use generic negative values if not.
// Actual values should come from generated AIDL headers or common error definitions.
// For example, ICameraProvider.aidl might have:
// const int ERROR_ILLEGAL_ARGUMENT = 1;
// const int ERROR_CAMERA_IN_USE = 7; // From CameraService.h (legacy)
// const int ERROR_DEVICE_UNAVAILABLE = 2; // Example
// For NDK ScopedAStatus, specific errors are often negative.
// Let's use some placeholder negative values for service specific errors.
constexpr int32_t SERVICE_ERROR_ILLEGAL_ARGUMENT = -EINVAL; // Or a custom enum
constexpr int32_t SERVICE_ERROR_CAMERA_IN_USE = -EBUSY;   // Or a custom enum
constexpr int32_t SERVICE_ERROR_DEVICE_UNAVAILABLE = -ENODEV; // Or a custom enum


HalCameraProvider::HalCameraProvider()
    : mFrameworkCallback(nullptr),
      mVirtualCameraId("0"),
      mCameraDeviceInstance(nullptr),
      mIsDeviceAvailable(false) {
    ALOGI("HalCameraProvider instance created. VirtualCameraId: %s", mVirtualCameraId.c_str());
}

HalCameraProvider::~HalCameraProvider() {
    ALOGI("HalCameraProvider instance destroyed.");
    cleanup();
}

void HalCameraProvider::initialize() {
    ALOGI("HalCameraProvider initialized.");
    // mCameraDeviceInstance is created on-demand in getCameraDeviceInterface
}

void HalCameraProvider::cleanup() {
    ALOGI("HalCameraProvider cleaning up.");
    std::lock_guard<std::mutex> lock(mLock);
    if (mCameraDeviceInstance) {
        // If HalCameraDevice has a specific shutdown method, call it.
        // For now, just reset the shared_ptr. If the device is open,
        // its client (CameraService) should have closed it.
        mCameraDeviceInstance.reset();
        ALOGI("Reset HalCameraDevice instance.");
    }
    if (mFrameworkCallback) {
        mFrameworkCallback.reset();
        ALOGI("Reset framework callback.");
    }
}

ndk::ScopedAStatus HalCameraProvider::setCallback(const std::shared_ptr<ICameraProviderCallback>& in_callback) {
    ALOGI("setCallback called.");
    std::lock_guard<std::mutex> lock(mLock);
    if (in_callback == nullptr) {
        ALOGW("Framework callback is null. Clearing existing callback.");
        mFrameworkCallback.reset(); // Clear if it was already set
        // The spec allows null callback to unregister, so this is not an error.
        // However, if a null callback is problematic for operation, one might return an error.
        // For now, accept it.
    } else {
        ALOGI("Framework callback set.");
    }
    mFrameworkCallback = in_callback;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraProvider::getCameraIdList(std::vector<std::string>* _aidl_return) {
    ALOGI("getCameraIdList called.");
    std::lock_guard<std::mutex> lock(mLock);
    _aidl_return->clear();
    if (mIsDeviceAvailable) {
        _aidl_return->push_back(mVirtualCameraId);
        ALOGI("Device %s is available, returning its ID.", mVirtualCameraId.c_str());
    } else {
        ALOGI("No devices available to list.");
    }
    ALOGI("Returning %zu camera IDs.", _aidl_return->size());
    return ndk::ScopedAStatus::ok();
}

// Internal helper, assumes mLock is held
std::shared_ptr<ICameraDevice> HalCameraProvider::getOrCreateCameraDeviceInternal(const std::string& cameraDeviceName) {
    if (cameraDeviceName != mVirtualCameraId) {
        ALOGE("getOrCreateCameraDeviceInternal: Requested camera ID %s does not match virtual camera ID %s",
              cameraDeviceName.c_str(), mVirtualCameraId.c_str());
        return nullptr;
    }

    if (!mCameraDeviceInstance) {
        ALOGI("Creating new HalCameraDevice instance for ID %s", cameraDeviceName.c_str());
        // Note: The HalCameraDevice needs to be castable to ICameraDevice.
        // ndk::SharedRefBase::make will create a std::shared_ptr<HalCameraDevice>.
        // This will be implicitly converted to std::shared_ptr<ICameraDevice> if HalCameraDevice inherits ICameraDevice.
        // The HalCameraDevice constructor takes (cameraId, *this_provider).
        mCameraDeviceInstance = ndk::SharedRefBase::make<HalCameraDevice>(cameraDeviceName, this);
        if (!mCameraDeviceInstance) {
             ALOGE("Failed to create HalCameraDevice for ID %s", cameraDeviceName.c_str());
        }
    } else {
        ALOGI("Returning existing HalCameraDevice instance for ID %s", cameraDeviceName.c_str());
    }
    return mCameraDeviceInstance;
}


ndk::ScopedAStatus HalCameraProvider::getCameraDeviceInterface(const std::string& in_cameraDeviceName,
                                                              std::shared_ptr<ICameraDevice>* _aidl_return) {
    ALOGI("getCameraDeviceInterface called for camera: %s", in_cameraDeviceName.c_str());
    std::lock_guard<std::mutex> lock(mLock);

    if (in_cameraDeviceName != mVirtualCameraId) {
        ALOGE("Camera ID %s not recognized. Expected %s.", in_cameraDeviceName.c_str(), mVirtualCameraId.c_str());
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromServiceSpecificError(SERVICE_ERROR_ILLEGAL_ARGUMENT);
    }

    if (!mIsDeviceAvailable) {
        ALOGE("Camera ID %s is not available (UVC device not connected or signaled).", in_cameraDeviceName.c_str());
        *_aidl_return = nullptr;
        // This error code should align with what CameraService expects for a device that became unavailable.
        // CameraService.h uses ERROR_DISCONNECTED for some cases.
        // ICameraProvider.aidl might define specific error codes.
        return ndk::ScopedAStatus::fromServiceSpecificError(SERVICE_ERROR_DEVICE_UNAVAILABLE);
    }

    *_aidl_return = getOrCreateCameraDeviceInternal(in_cameraDeviceName);

    if (*_aidl_return == nullptr) {
        ALOGE("Failed to create or get camera device instance for %s", in_cameraDeviceName.c_str());
        // This could be due to various reasons, e.g., resource limits, internal error.
        // ERROR_CAMERA_IN_USE might be appropriate if the device cannot be opened multiple times,
        // but our current model is a cached instance.
        return ndk::ScopedAStatus::fromServiceSpecificError(SERVICE_ERROR_CAMERA_IN_USE); // Placeholder for a general failure
    }

    ALOGI("Returning ICameraDevice interface for %s", in_cameraDeviceName.c_str());
    return ndk::ScopedAStatus::ok();
}

void HalCameraProvider::signalDeviceAvailable(const std::string& cameraId, bool available) {
    std::lock_guard<std::mutex> lock(mLock);
    if (cameraId != mVirtualCameraId) {
        ALOGW("signalDeviceAvailable received for unknown camera ID %s. Ignoring.", cameraId.c_str());
        return;
    }

    if (mIsDeviceAvailable == available) {
        ALOGI("signalDeviceAvailable: No change in availability for %s (still %d).", cameraId.c_str(), available);
        return; // No change
    }
    mIsDeviceAvailable = available;
    ALOGI("signalDeviceAvailable: Camera %s is now %s.", cameraId.c_str(), available ? "PRESENT" : "NOT_PRESENT");

    if (mFrameworkCallback) {
        CameraDeviceStatus status = available ? CameraDeviceStatus::PRESENT : CameraDeviceStatus::NOT_PRESENT;
        // When multiple physical cameras map to this HAL device, ENUMERATING might be used.
        // For a single virtual camera, PRESENT/NOT_PRESENT is typical.
        ndk::ScopedAStatus cbStatus = mFrameworkCallback->cameraDeviceStatusChange(cameraId, status);
        if (cbStatus.isOk()) {
            ALOGI("Notified framework of cameraDeviceStatusChange for %s: %s", cameraId.c_str(), available ? "PRESENT" : "NOT_PRESENT");
        } else {
            ALOGE("Failed to notify framework of cameraDeviceStatusChange for %s: %s", cameraId.c_str(), cbStatus.getMessage());
        }
    } else {
        ALOGW("No framework callback set, cannot notify about device status change for %s.", cameraId.c_str());
    }

    if (!available) {
        ALOGI("Device %s is no longer available.", cameraId.c_str());
        // If the device is open (mCameraDeviceInstance exists and is active), CameraService
        // should receive the NOT_PRESENT and close the device session.
        // If we wanted to proactively tell the device instance:
        // if (mCameraDeviceInstance) {
        //    mCameraDeviceInstance->handleExternalDisconnect(); // Requires such a method in HalCameraDevice
        // }
        // For now, we don't reset mCameraDeviceInstance here. It will be reset if CameraService closes it,
        // or during cleanup. If it's attempted to be used while !mIsDeviceAvailable, getCameraDeviceInterface
        // will block it.
    }
}

void HalCameraProvider::onDeviceClosed(const std::string& cameraId) {
    std::lock_guard<std::mutex> lock(mLock);
    ALOGI("onDeviceClosed: Notification from HalCameraDevice that %s has been closed.", cameraId.c_str());
    if (cameraId == mVirtualCameraId && mCameraDeviceInstance) {
        // This means the HalCameraDevice's close() method has completed its cleanup.
        // Depending on resource management strategy, we could reset the instance.
        // For now, we keep the instance cached. If CameraService wants to re-open,
        // it will call getCameraDeviceInterface again.
        // If mIsDeviceAvailable is also false, then it might be safer to reset.
        // if (!mIsDeviceAvailable) {
        //     ALOGI("Device %s is closed and not available. Resetting instance.", cameraId.c_str());
        //     mCameraDeviceInstance.reset();
        // }
    }
}

std::shared_ptr<HalCameraSession> HalCameraProvider::getActiveSessionForCameraId(const std::string& cameraId) {
    std::lock_guard<std::mutex> lock(mLock);
    if (cameraId == mVirtualCameraId && mCameraDeviceInstance) {
        // Need to cast mCameraDeviceInstance (which is ICameraDevice) to HalCameraDevice*
        // This assumes mCameraDeviceInstance is indeed a HalCameraDevice.
        // A safer way might be to have HalCameraDevice itself register its session with the provider,
        // or have a map if multiple devices were supported.
        // For a single device instance, this direct cast is plausible if HalCameraProvider
        // exclusively creates HalCameraDevice instances.
        HalCameraDevice* device = static_cast<HalCameraDevice*>(mCameraDeviceInstance.get());
        if (device) {
            return device->getActiveSession();
        } else {
            ALOGE("getActiveSessionForCameraId: mCameraDeviceInstance for ID %s is not a HalCameraDevice or is null after cast.", cameraId.c_str());
        }
    } else {
        ALOGW("getActiveSessionForCameraId: No active or matching camera device instance for ID %s. Requested: %s, Virtual: %s", 
            cameraId.c_str(), cameraId.c_str(), mVirtualCameraId.c_str());
    }
    return nullptr;
}

// Implement correct AIDL interface methods:
ndk::ScopedAStatus HalCameraProvider::notifyDeviceStateChange(int64_t in_deviceState) {
    // This method is called when the device state changes (e.g., device rotation, fold state)
    // For this simplified HAL, we don't need to handle device state changes
    ALOGI("notifyDeviceStateChange called with device state: %ld", in_deviceState);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraProvider::getVendorTags(std::vector<::aidl::android::hardware::camera::common::VendorTagSection>* _aidl_return) {
    // This HAL doesn't define any vendor tags
    if (_aidl_return) {
        _aidl_return->clear();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraProvider::getConcurrentCameraIds(std::vector<::aidl::android::hardware::camera::provider::ConcurrentCameraIdCombination>* _aidl_return) {
    // This HAL doesn't support concurrent camera usage
    if (_aidl_return) {
        _aidl_return->clear();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HalCameraProvider::isConcurrentStreamCombinationSupported(const std::vector<::aidl::android::hardware::camera::provider::CameraIdAndStreamCombination>& in_configs, bool* _aidl_return) {
    // This HAL doesn't support concurrent stream combinations
    if (_aidl_return) {
        *_aidl_return = false;
    }
    return ndk::ScopedAStatus::ok();
}

} // namespace cambridge
} // namespace android
