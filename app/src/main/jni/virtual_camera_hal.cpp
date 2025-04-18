#include "virtual_camera_hal.h"
#include <android/log.h>
#include <hardware/hardware.h>
#include <hardware/camera_common.h>
#include <hardware/camera.h>
#include <system/camera_metadata.h>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <hardware/camera_common.h>
#include <hardware/hardware.h>
#include <camera/CameraMetadata.h>
#include <string.h> // For memset

// Remove local LOG_TAG definition
// #define LOG_TAG "VirtualCameraHAL"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Virtual camera parameters
#define VIRTUAL_CAMERA_ID 99  // Using a high ID to avoid conflicts
#define FRAME_BUFFER_COUNT 4  // Number of frame buffers to allocate

// Static references
static camera_module_callbacks_t* g_callbacks = nullptr;

// Define the HAL module methods structure
static hw_module_methods_t mCameraModuleMethods = {
    .open = VirtualCameraHAL::openCameraHAL
};

// Define the HAL device operations structure
static camera_device_ops_t mCameraOps = {
    .set_preview_window = VirtualCameraHAL::set_preview_window,
    .set_callbacks = VirtualCameraHAL::set_callbacks,
    .enable_msg_type = VirtualCameraHAL::enable_msg_type,
    .disable_msg_type = VirtualCameraHAL::disable_msg_type,
    .msg_type_enabled = VirtualCameraHAL::msg_type_enabled,
    .start_preview = VirtualCameraHAL::start_preview,
    .stop_preview = VirtualCameraHAL::stop_preview,
    .preview_enabled = VirtualCameraHAL::preview_enabled,
    .store_meta_data_in_buffers = VirtualCameraHAL::store_meta_data_in_buffers,
    .start_recording = VirtualCameraHAL::start_recording,
    .stop_recording = VirtualCameraHAL::stop_recording,
    .recording_enabled = VirtualCameraHAL::recording_enabled,
    .release_recording_frame = VirtualCameraHAL::release_recording_frame,
    .auto_focus = VirtualCameraHAL::auto_focus,
    .cancel_auto_focus = VirtualCameraHAL::cancel_auto_focus,
    .take_picture = VirtualCameraHAL::take_picture,
    .cancel_picture = VirtualCameraHAL::cancel_picture,
    .set_parameters = VirtualCameraHAL::set_parameters,
    .get_parameters = VirtualCameraHAL::get_parameters,
    .put_parameters = VirtualCameraHAL::put_parameters,
    .send_command = VirtualCameraHAL::send_command,
    .release = VirtualCameraHAL::release,
    .dump = VirtualCameraHAL::dump,
};

// Helper to get VirtualCameraHAL instance from camera_device
static inline VirtualCameraHAL* getHalInstance(camera_device_t* device) {
    return reinterpret_cast<VirtualCameraHAL*>(device->priv);
}

// Constructor
VirtualCameraHAL::VirtualCameraHAL()
    : mNotifyCb(nullptr),
      mDataCb(nullptr),
      mDataCbTimestamp(nullptr),
      mRequestMemory(nullptr),
      mCallbackCookie(nullptr),
      mInitialized(false),
      mPreviewEnabled(false),
      mRecordingEnabled(false),
      mMsgTypeEnabled(0) // Correct order
{
    LOGD("VirtualCameraHAL constructor");
    // Initialize device info
    memset(&mDeviceInfo, 0, sizeof(mDeviceInfo));
    mDeviceInfo.cameraId = VIRTUAL_CAMERA_ID;
    mDeviceInfo.cameraName = "Virtual UVC Camera";
    mDeviceInfo.staticMetadata = nullptr;

    // Initialize camera_module_t fields
    mDeviceInfo.cameraModule.common.tag = HARDWARE_MODULE_TAG;
    mDeviceInfo.cameraModule.common.module_api_version = CAMERA_MODULE_API_VERSION_2_4; // Example version
    mDeviceInfo.cameraModule.common.hal_api_version = HARDWARE_HAL_API_VERSION;
    mDeviceInfo.cameraModule.common.id = CAMERA_HARDWARE_MODULE_ID; // Use standard ID
    mDeviceInfo.cameraModule.common.name = "Virtual Camera HAL";
    mDeviceInfo.cameraModule.common.author = "CamBridge";
    mDeviceInfo.cameraModule.common.methods = &mCameraModuleMethods;
    memset(&mDeviceInfo.cameraModule.reserved, 0, sizeof(mDeviceInfo.cameraModule.reserved));

    // Set methods
    mDeviceInfo.cameraModule.get_number_of_cameras = []() { return 1; }; // Lambda for simplicity
    mDeviceInfo.cameraModule.get_camera_info = VirtualCameraHAL::static_get_camera_info; // Use wrapper
    mDeviceInfo.cameraModule.set_callbacks = VirtualCameraHAL::setCallbacks;
    mDeviceInfo.cameraModule.open_legacy = nullptr; // Not supporting legacy cameras
    mDeviceInfo.cameraModule.set_torch_mode = nullptr; // Not supporting torch
    mDeviceInfo.cameraModule.init = nullptr;
    mDeviceInfo.cameraModule.get_vendor_tag_ops = nullptr;

    // Create virtual camera device
    if (!createVirtualCameraDevice()) {
        LOGE("Failed to create virtual camera device");
        return;
    }
    
    // Setup camera static metadata
    setupStaticMetadata();
    
    // Allocate frame buffers
    if (!allocateFrameBuffers(FRAME_BUFFER_COUNT)) {
        LOGE("Failed to allocate frame buffers");
        return;
    }
    
    // Register camera with HAL
    if (!registerCameraWithHAL()) {
        LOGE("Failed to register camera with HAL");
        return;
    }
    
    mInitialized = true;
    LOGI("Virtual camera HAL initialized successfully");
}

// Destructor
VirtualCameraHAL::~VirtualCameraHAL() {
    cleanup();
}

// Initialize the virtual camera HAL
bool VirtualCameraHAL::initialize() {
    LOGE("VirtualCameraHAL::initialize() called but should be unused!");
    return false; // Return error if called
}

// Cleanup resources
void VirtualCameraHAL::cleanup() {
    if (!mInitialized) {
        return;
    }
    
    // Free frame buffers
    freeFrameBuffers();
    
    // Free static metadata
    if (mDeviceInfo.staticMetadata != nullptr) {
        free_camera_metadata(mDeviceInfo.staticMetadata);
        mDeviceInfo.staticMetadata = nullptr;
    }
    
    mInitialized = false;
    LOGI("Virtual camera HAL cleaned up");
}

// Push a video frame from Java to the native side
bool VirtualCameraHAL::pushVideoFrame(const uint8_t* data, size_t size, int width, int height, int format) {
    if (!mInitialized || !mPreviewEnabled) {
        return false;
    }
    
    // Get an available buffer
    FrameBuffer* buffer = getAvailableBuffer();
    if (buffer == nullptr) {
        LOGD("No available buffer for frame");
        return false;
    }
    
    // Copy frame data
    if (buffer->size < size) {
        LOGE("Frame buffer too small (%zu < %zu)", buffer->size, size);
        returnBuffer(buffer);
        return false;
    }
    
    // Copy frame data to buffer
    memcpy(buffer->data, data, size);
    buffer->width = width;
    buffer->height = height;
    buffer->format = format;
    buffer->timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Deliver frame to callbacks
    deliverFrameToCallbacks(buffer);
    
    // Return buffer to pool
    returnBuffer(buffer);
    return true;
}

// Create and initialize the virtual camera device
bool VirtualCameraHAL::createVirtualCameraDevice() {
    LOGD("Creating virtual camera device");
    
    // Initialize camera_module_t
    // Note: Some fields were initialized in the constructor already
    // Need to ensure all necessary function pointers are set
    // camera_module_t* module = &mDeviceInfo.cameraModule; // Redundant if done in constructor
    
    // Initialize camera_device_t
    camera_device_t* device = &mDeviceInfo.cameraDevice;
    memset(device, 0, sizeof(camera_device_t));
    
    device->common.tag = HARDWARE_DEVICE_TAG;
    device->common.version = CAMERA_DEVICE_API_VERSION_1_0; // Using HAL1 device API
    device->common.module = (hw_module_t*)&mDeviceInfo.cameraModule;
    device->common.close = closeCamera; // Assign the static close function
    
    // Assign HAL1 device operations
    device->ops = &mCameraOps; // Use defined ops struct
    
    device->priv = this; // Store pointer to the VirtualCameraHAL instance

    LOGD("Virtual camera device created");
    return true;
}

// Setup static metadata for the virtual camera
void VirtualCameraHAL::setupStaticMetadata() {
    // Create metadata
    camera_metadata_t* metadata = allocate_camera_metadata(30, 500);
    if (metadata == nullptr) {
        LOGE("Failed to allocate camera metadata");
        return;
    }
    
    // Add camera facing direction (external)
    uint8_t facing = CAMERA_FACING_EXTERNAL;
    add_camera_metadata_entry(metadata, ANDROID_LENS_FACING, &facing, 1);
    
    // Add sensor orientation (0 degrees)
    int32_t orientation = 0;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_ORIENTATION, &orientation, 1);
    
    // Add supported preview sizes (common resolutions)
    std::vector<int32_t> availableSizes = {
        // Width, Height pairs
        1920, 1080,  // Full HD
        1280, 720,   // HD
        640, 480,    // VGA
        320, 240     // QVGA
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, 
                             availableSizes.data(), availableSizes.size());
    
    // Add supported frame rates
    std::vector<int32_t> fpsRanges = {
        // Min, Max pairs (in frames per second)
        15, 30,  // 15-30 fps
        30, 30   // Fixed 30 fps
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, 
                             fpsRanges.data(), fpsRanges.size());
    
    // Set the static metadata
    mDeviceInfo.staticMetadata = metadata;
}

// Register the virtual camera with the Android HAL
bool VirtualCameraHAL::registerCameraWithHAL() {
    if (g_callbacks == nullptr) {
        LOGE("Camera callbacks not registered");
        return false;
    }
    
    // Notify the camera service about the new camera device
    camera_device_status_t status = CAMERA_DEVICE_STATUS_PRESENT;
    g_callbacks->camera_device_status_change(g_callbacks, VIRTUAL_CAMERA_ID, status);
    
    LOGI("Virtual camera registered with HAL, ID: %d", VIRTUAL_CAMERA_ID);
    return true;
}

// Allocate frame buffers
bool VirtualCameraHAL::allocateFrameBuffers(int count) {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    
    // Free existing buffers if any
    freeFrameBuffers();
    
    // Allocate new buffers
    mFrameBuffers.resize(count);
    const size_t bufferSize = 1920 * 1080 * 4; // Max buffer size (Full HD RGBA)
    
    for (int i = 0; i < count; i++) {
        mFrameBuffers[i].data = new uint8_t[bufferSize];
        if (mFrameBuffers[i].data == nullptr) {
            LOGE("Failed to allocate frame buffer %d", i);
            freeFrameBuffers();
            return false;
        }
        
        mFrameBuffers[i].size = bufferSize;
        mFrameBuffers[i].width = 0;
        mFrameBuffers[i].height = 0;
        mFrameBuffers[i].format = 0;
        mFrameBuffers[i].timestamp = 0;
        mFrameBuffers[i].inUse = false;
    }
    
    LOGI("Allocated %d frame buffers", count);
    return true;
}

// Free frame buffers
void VirtualCameraHAL::freeFrameBuffers() {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    
    for (auto& buffer : mFrameBuffers) {
        if (buffer.data != nullptr) {
            delete[] buffer.data;
            buffer.data = nullptr;
        }
    }
    
    mFrameBuffers.clear();
}

// Get an available buffer from the pool
FrameBuffer* VirtualCameraHAL::getAvailableBuffer() {
    std::unique_lock<std::mutex> lock(mBufferMutex);
    
    // Find an available buffer
    for (auto& buffer : mFrameBuffers) {
        if (!buffer.inUse) {
            buffer.inUse = true;
            return &buffer;
        }
    }
    
    // Wait for a buffer to become available (with timeout)
    bool bufferAvailable = mBufferCondition.wait_for(lock, std::chrono::milliseconds(100),
        [this]() -> bool {
            for (auto& buffer : mFrameBuffers) {
                if (!buffer.inUse) {
                    return true;
                }
            }
            return false;
        });
    
    if (!bufferAvailable) {
        return nullptr;
    }
    
    // Find the available buffer
    for (auto& buffer : mFrameBuffers) {
        if (!buffer.inUse) {
            buffer.inUse = true;
            return &buffer;
        }
    }
    
    return nullptr;
}

// Return a buffer to the pool
void VirtualCameraHAL::returnBuffer(FrameBuffer* buffer) {
    if (buffer == nullptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mBufferMutex);
    buffer->inUse = false;
    mBufferCondition.notify_one();
}

// Deliver a frame to the camera callbacks
void VirtualCameraHAL::deliverFrameToCallbacks(FrameBuffer* buffer) {
    if (buffer == nullptr || mDataCb == nullptr || mRequestMemory == nullptr) {
        return;
    }
    
    // Check if preview messages are enabled
    if (!(mMsgTypeEnabled & CAMERA_MSG_PREVIEW_FRAME)) {
        return;
    }
    
    // Allocate memory for the frame
    camera_memory_t* mem = mRequestMemory(-1, buffer->size, 1, nullptr);
    if (mem == nullptr || mem->data == nullptr) {
        LOGE("Failed to allocate memory for frame");
        return;
    }
    
    // Copy frame data to memory
    memcpy(mem->data, buffer->data, buffer->size);
    
    // Deliver the frame to the callback
    mDataCb(CAMERA_MSG_PREVIEW_FRAME, mem, 0, nullptr, mCallbackCookie);
    
    // Release memory
    mem->release(mem);
}

// Static HAL module functions
int VirtualCameraHAL::openCameraHAL(const hw_module_t* module, const char* id, hw_device_t** device) {
    if (id == nullptr || device == nullptr) {
        return -EINVAL;
    }
    
    // Check if the requested ID matches our virtual camera
    int cameraId = atoi(id);
    if (cameraId != VIRTUAL_CAMERA_ID) {
        LOGE("Invalid camera ID: %d", cameraId);
        return -ENODEV;
    }
    
    // Find the VirtualCameraHAL instance
    VirtualCameraHAL* halInstance = reinterpret_cast<VirtualCameraHAL*>(
        const_cast<hw_module_t*>(module)->dso);
    
    if (halInstance == nullptr) {
        LOGE("No HAL instance found");
        return -ENODEV;
    }
    
    // Return the camera device
    *device = &halInstance->mDeviceInfo.cameraDevice.common;
    return 0;
}

int VirtualCameraHAL::closeCamera(hw_device_t* device) {
    if (device == nullptr) {
        return -EINVAL;
    }
    
    // Get the camera device
    camera_device_t* cameraDevice = reinterpret_cast<camera_device_t*>(device);
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(cameraDevice);
    if (hal == nullptr) {
        return -EINVAL;
    }
    
    // Perform cleanup
    hal->mPreviewEnabled = false;
    hal->mRecordingEnabled = false;
    
    return 0;
}

int VirtualCameraHAL::getCameraInfo(const camera_module_t* module, uint32_t cameraId, struct camera_info* info) {
    LOGD("getCameraInfo called for camera %d", cameraId);
    if (!module || !info) {
        return -EINVAL;
    }
    
    // Check if the requested ID matches our virtual camera
    if (cameraId != VIRTUAL_CAMERA_ID) {
        return -ENODEV;
    }
    
    // Find the HAL instance
    VirtualCameraHAL* hal = reinterpret_cast<VirtualCameraHAL*>(
        const_cast<camera_module_t*>(module)->common.dso);
    
    if (hal == nullptr || !hal->mInitialized) {
        return -ENODEV;
    }
    
    // Fill in the camera info
    info->facing = CAMERA_FACING_EXTERNAL;
    info->orientation = 0;
    info->device_version = CAMERA_DEVICE_API_VERSION_1_0;
    info->static_camera_characteristics = hal->mDeviceInfo.staticMetadata;
    
    return 0;
}

int VirtualCameraHAL::setCallbacks(const camera_module_callbacks_t* callbacks) {
    LOGD("setCallbacks called");
    if (callbacks == nullptr) {
        return -EINVAL;
    }
    
    // Store the callbacks
    g_callbacks = const_cast<camera_module_callbacks_t*>(callbacks);
    return 0;
}

// Camera device functions
int VirtualCameraHAL::set_preview_window(struct camera_device* device, struct preview_stream_ops* window) {
    if (device == nullptr) {
        return -EINVAL;
    }
    
    // Nothing to do for our virtual camera implementation
    return 0;
}

void VirtualCameraHAL::set_callbacks(struct camera_device* device, camera_notify_callback notify_cb,
                                     camera_data_callback data_cb, camera_data_timestamp_callback data_cb_timestamp,
                                     camera_request_memory get_memory, void* user) {
    if (device == nullptr) {
        return;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return;
    }
    
    // Store the callbacks
    hal->mNotifyCb = notify_cb;
    hal->mDataCb = data_cb;
    hal->mDataCbTimestamp = data_cb_timestamp;
    hal->mRequestMemory = get_memory;
    hal->mCallbackCookie = user;
}

void VirtualCameraHAL::enable_msg_type(struct camera_device* device, int32_t msg_type) {
    if (device == nullptr) {
        return;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return;
    }
    
    // Enable the message type
    hal->mMsgTypeEnabled |= msg_type;
}

void VirtualCameraHAL::disable_msg_type(struct camera_device* device, int32_t msg_type) {
    if (device == nullptr) {
        return;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return;
    }
    
    // Disable the message type
    hal->mMsgTypeEnabled &= ~msg_type;
}

int VirtualCameraHAL::msg_type_enabled(struct camera_device* device, int32_t msg_type) {
    if (device == nullptr) {
        return 0;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return 0;
    }
    
    // Check if the message type is enabled
    return (hal->mMsgTypeEnabled & msg_type) ? 1 : 0;
}

int VirtualCameraHAL::start_preview(struct camera_device* device) {
    if (device == nullptr) {
        return -EINVAL;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return -EINVAL;
    }
    
    // Start preview
    hal->mPreviewEnabled = true;
    LOGI("Started preview");
    return 0;
}

void VirtualCameraHAL::stop_preview(struct camera_device* device) {
    if (device == nullptr) {
        return;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return;
    }
    
    // Stop preview
    hal->mPreviewEnabled = false;
    LOGI("Stopped preview");
}

int VirtualCameraHAL::preview_enabled(struct camera_device* device) {
    if (device == nullptr) {
        return 0;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return 0;
    }
    
    // Return preview state
    return hal->mPreviewEnabled ? 1 : 0;
}

int VirtualCameraHAL::store_meta_data_in_buffers(struct camera_device* device, int enable) {
    // We don't support metadata buffers in this implementation
    return -EINVAL;
}

int VirtualCameraHAL::start_recording(struct camera_device* device) {
    if (device == nullptr) {
        return -EINVAL;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return -EINVAL;
    }
    
    // Start recording
    hal->mRecordingEnabled = true;
    LOGI("Started recording");
    return 0;
}

void VirtualCameraHAL::stop_recording(struct camera_device* device) {
    if (device == nullptr) {
        return;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return;
    }
    
    // Stop recording
    hal->mRecordingEnabled = false;
    LOGI("Stopped recording");
}

int VirtualCameraHAL::recording_enabled(struct camera_device* device) {
    if (device == nullptr) {
        return 0;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return 0;
    }
    
    // Return recording state
    return hal->mRecordingEnabled ? 1 : 0;
}

void VirtualCameraHAL::release_recording_frame(struct camera_device* device, const void* opaque) {
    // Nothing to do for our implementation
}

int VirtualCameraHAL::auto_focus(struct camera_device* device) {
    if (device == nullptr) {
        return -EINVAL;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return -EINVAL;
    }
    
    // Simulate autofocus completion after a short delay
    if (hal->mNotifyCb != nullptr && (hal->mMsgTypeEnabled & CAMERA_MSG_FOCUS)) {
        // Spawn a thread to simulate autofocus
        std::thread([hal]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            hal->mNotifyCb(CAMERA_MSG_FOCUS, 1, 0, hal->mCallbackCookie);
        }).detach();
    }
    
    return 0;
}

int VirtualCameraHAL::cancel_auto_focus(struct camera_device* device) {
    // Nothing to do for our implementation
    return 0;
}

int VirtualCameraHAL::take_picture(struct camera_device* device) {
    if (device == nullptr) {
        return -EINVAL;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return -EINVAL;
    }
    
    // Simulate taking a picture (use the next frame that comes through)
    if (hal->mNotifyCb != nullptr && (hal->mMsgTypeEnabled & CAMERA_MSG_SHUTTER)) {
        hal->mNotifyCb(CAMERA_MSG_SHUTTER, 0, 0, hal->mCallbackCookie);
    }
    
    return 0;
}

int VirtualCameraHAL::cancel_picture(struct camera_device* device) {
    // Nothing to do for our implementation
    return 0;
}

int VirtualCameraHAL::set_parameters(struct camera_device* device, const char* params) {
    // We don't support parameters in this implementation
    return 0;
}

char* VirtualCameraHAL::get_parameters(struct camera_device* device) {
    // Return default parameters
    return strdup("preview-size=1280x720");
}

void VirtualCameraHAL::put_parameters(struct camera_device* device, char* params) {
    // Free the parameters
    free(params);
}

int VirtualCameraHAL::send_command(struct camera_device* device, int32_t cmd, int32_t arg1, int32_t arg2) {
    // We don't support commands in this implementation
    return -EINVAL;
}

void VirtualCameraHAL::release(struct camera_device* device) {
    if (device == nullptr) {
        return;
    }
    
    // Get the HAL instance
    VirtualCameraHAL* hal = getHalInstance(device);
    if (hal == nullptr) {
        return;
    }
    
    // Stop preview and recording
    hal->mPreviewEnabled = false;
    hal->mRecordingEnabled = false;
    
    LOGI("Camera released");
}

int VirtualCameraHAL::dump(struct camera_device* device, int fd) {
    // We don't support dumping in this implementation
    return 0;
}

// Static wrapper for get_camera_info
int VirtualCameraHAL::static_get_camera_info(int camera_id, struct camera_info* info) {
    // We need access to the module or assume properties for camera_id 0
    // For simplicity, assuming camera_id is always 0 for this HAL
    if (camera_id != 0) {
        return -EINVAL; // Invalid camera ID
    }
    // Call the original function or reconstruct info here
    // Since the original needs 'module', which we don't have here,
    // we'll populate the 'info' struct directly for camera 0.
    info->facing = CAMERA_FACING_BACK; // Example: virtual camera faces back
    info->orientation = 0; // Example: landscape orientation
    info->device_version = CAMERA_DEVICE_API_VERSION_1_0; // Using HAL1 device version for example
    // The static_camera_characteristics field is for HAL3+, leave null for HAL1
    info->static_camera_characteristics = nullptr;
    // info->resource_cost and info->conflicting_devices might need specific values

    LOGD("static_get_camera_info called for camera %d", camera_id);
    return 0; // Success 
} 