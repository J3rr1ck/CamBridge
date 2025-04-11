#ifndef VIRTUAL_CAMERA_HAL_H
#define VIRTUAL_CAMERA_HAL_H

#include <hardware/hardware.h>
#include <hardware/camera_common.h>
#include <hardware/camera.h>
#include <vector>
#include <mutex>
#include <condition_variable>

// Camera device info
struct VirtualCameraDeviceInfo {
    camera_module_t cameraModule;
    camera_device_t cameraDevice;
    uint32_t cameraId;
    std::string cameraName;
    camera_metadata_t* staticMetadata;
};

// Frame buffer structure
struct FrameBuffer {
    uint8_t* data;
    size_t size;
    int width;
    int height;
    int format;
    int64_t timestamp;
    bool inUse;
};

class VirtualCameraHAL {
public:
    VirtualCameraHAL();
    ~VirtualCameraHAL();
    
    bool initialize();
    void cleanup();
    
    // Push a video frame from Java to the native side
    bool pushVideoFrame(const uint8_t* data, size_t size, int width, int height, int format);
    
private:
    // HAL module functions
    static int openCameraHAL(const hw_module_t* module, const char* id, hw_device_t** device);
    static int closeCamera(hw_device_t* device);
    static int getCameraInfo(const camera_module_t* module, uint32_t cameraId, struct camera_info* info);
    static int static_get_camera_info(int camera_id, struct camera_info* info);
    static int setCallbacks(const camera_module_callbacks_t* callbacks);
    
    // Camera device functions
    static int set_preview_window(struct camera_device* device, struct preview_stream_ops* window);
    static void set_callbacks(struct camera_device* device, camera_notify_callback notify_cb,
                             camera_data_callback data_cb, camera_data_timestamp_callback data_cb_timestamp,
                             camera_request_memory get_memory, void* user);
    static void enable_msg_type(struct camera_device* device, int32_t msg_type);
    static void disable_msg_type(struct camera_device* device, int32_t msg_type);
    static int msg_type_enabled(struct camera_device* device, int32_t msg_type);
    static int start_preview(struct camera_device* device);
    static void stop_preview(struct camera_device* device);
    static int preview_enabled(struct camera_device* device);
    static int store_meta_data_in_buffers(struct camera_device* device, int enable);
    static int start_recording(struct camera_device* device);
    static void stop_recording(struct camera_device* device);
    static int recording_enabled(struct camera_device* device);
    static void release_recording_frame(struct camera_device* device, const void* opaque);
    static int auto_focus(struct camera_device* device);
    static int cancel_auto_focus(struct camera_device* device);
    static int take_picture(struct camera_device* device);
    static int cancel_picture(struct camera_device* device);
    static int set_parameters(struct camera_device* device, const char* params);
    static char* get_parameters(struct camera_device* device);
    static void put_parameters(struct camera_device* device, char* params);
    static int send_command(struct camera_device* device, int32_t cmd, int32_t arg1, int32_t arg2);
    static void release(struct camera_device* device);
    static int dump(struct camera_device* device, int fd);
    
    // Helper methods
    bool createVirtualCameraDevice();
    void setupStaticMetadata();
    bool registerCameraWithHAL();
    
    // Get the virtual camera device instance from the camera device pointer
    static VirtualCameraHAL* getInstance(struct camera_device* device);
    
    // Frame buffer management
    bool allocateFrameBuffers(int count);
    void freeFrameBuffers();
    FrameBuffer* getAvailableBuffer();
    void returnBuffer(FrameBuffer* buffer);
    void deliverFrameToCallbacks(FrameBuffer* buffer);
    
    // Member variables
    VirtualCameraDeviceInfo mDeviceInfo;
    
    // Frame buffer variables
    std::vector<FrameBuffer> mFrameBuffers;
    std::mutex mBufferMutex;
    std::condition_variable mBufferCondition;
    
    // Callback variables
    camera_notify_callback mNotifyCb;
    camera_data_callback mDataCb;
    camera_data_timestamp_callback mDataCbTimestamp;
    camera_request_memory mRequestMemory;
    void* mCallbackCookie;
    
    // State variables
    bool mInitialized;
    bool mPreviewEnabled;
    bool mRecordingEnabled;
    int mMsgTypeEnabled;
};

#endif // VIRTUAL_CAMERA_HAL_H 