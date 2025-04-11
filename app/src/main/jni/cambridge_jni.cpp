#include <jni.h>
#include <string>
#include <android/log.h>
#include <hardware/hardware.h>
#include <hardware/camera_common.h>
#include "virtual_camera_hal.h"

#define LOG_TAG "CamBridge-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Global references
static JavaVM* gJavaVM = nullptr;
static VirtualCameraHAL* gVirtualCameraHAL = nullptr;

// Function to get JNIEnv (no longer needed)
/*
static JNIEnv* getJNIEnv() {
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        ALOGE("Failed to get JNIEnv");
        return nullptr;
    }
    return env;
}
*/

extern "C" JNIEXPORT jlong JNICALL
Java_com_android_cambridge_VirtualCameraProviderService_initializeNative(
        JNIEnv* env, jobject instance) {
    
    // Create virtual camera HAL instance
    VirtualCameraHAL* hal = new VirtualCameraHAL();
    if (!hal->initialize()) {
        LOGE("Failed to initialize virtual camera HAL");
        delete hal;
        return 0;
    }
    
    // Save global reference
    if (gVirtualCameraHAL == nullptr) {
        gVirtualCameraHAL = hal;
    }
    
    LOGI("Virtual camera HAL initialized successfully");
    return reinterpret_cast<jlong>(hal);
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_cambridge_VirtualCameraProviderService_cleanupNative(
        JNIEnv* env, jobject instance, jlong nativeContext) {
    
    if (nativeContext == 0) {
        LOGE("Invalid native context");
        return;
    }
    
    VirtualCameraHAL* hal = reinterpret_cast<VirtualCameraHAL*>(nativeContext);
    hal->cleanup();
    
    if (hal == gVirtualCameraHAL) {
        gVirtualCameraHAL = nullptr;
    }
    
    delete hal;
    LOGI("Virtual camera HAL cleaned up");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_android_cambridge_VirtualCameraManager_pushVideoFrameNative(
        JNIEnv* env, jobject instance, jlong nativeContext, 
        jbyteArray frameData, jint width, jint height, jint format) {
    
    if (nativeContext == 0 || gVirtualCameraHAL == nullptr) {
        LOGE("Invalid native context or HAL not initialized");
        return JNI_FALSE;
    }
    
    // Get the byte array elements
    jbyte* data = env->GetByteArrayElements(frameData, nullptr);
    jsize dataLength = env->GetArrayLength(frameData);
    
    // Push the frame to the virtual camera
    bool success = gVirtualCameraHAL->pushVideoFrame(
        reinterpret_cast<uint8_t*>(data), 
        static_cast<size_t>(dataLength),
        static_cast<int>(width),
        static_cast<int>(height),
        static_cast<int>(format)
    );
    
    // Release the byte array
    env->ReleaseByteArrayElements(frameData, data, JNI_ABORT);
    
    return success ? JNI_TRUE : JNI_FALSE;
}

// JNI OnLoad function - called when the library is loaded
jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    gJavaVM = vm;
    LOGI("JNI library loaded");
    return JNI_VERSION_1_6;
}

// JNI OnUnload function - called when the library is unloaded
void JNI_OnUnload(JavaVM* vm, void* reserved) {
    if (gVirtualCameraHAL != nullptr) {
        delete gVirtualCameraHAL;
        gVirtualCameraHAL = nullptr;
    }
    gJavaVM = nullptr;
    LOGI("JNI library unloaded");
} 