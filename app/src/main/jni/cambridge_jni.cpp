#include <jni.h>
#include <string>
#include <vector> // For std::vector
#include <memory> // For std::shared_ptr
#include <android/log.h>
#include <android/binder_manager.h> // For AServiceManager_addService
#include <android/binder_process.h>  // For ABinderProcess_startThreadPool

// HAL specific headers
#include "hal_camera_provider.h" // Assuming this is the main entry point for HAL
#include "hal_camera_device.h"   // For potential direct access or casting if needed
#include "hal_camera_session.h"  // For pushNewFrame

// Using namespace for convenience if types are within it
using namespace android::cambridge;

#define LOG_TAG "CamBridge-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM* gJavaVM = nullptr;
// Store the provider context globally or pass it around.
// For simplicity, if VirtualCameraProviderService is a singleton in Java and only one provider exists,
// a global static shared_ptr might be okay, but passing context is cleaner.

extern "C" JNIEXPORT jlong JNICALL
Java_com_android_cambridge_VirtualCameraProviderService_initializeNative(
        JNIEnv* env, jobject /* this */) {
    
    std::shared_ptr<HalCameraProvider> provider = ndk::SharedRefBase::make<HalCameraProvider>();
    if (!provider) {
        LOGE("Failed to create HalCameraProvider");
        return 0;
    }
    provider->initialize(); // Call any internal initialization

    // Register the provider with Android's ServiceManager
    // The service name must be unique and match what CameraService expects for external providers.
    // Format: "android.hardware.camera.provider.ICameraProvider/your_unique_provider_name"
    const std::string serviceName = "android.hardware.camera.provider.ICameraProvider/cambridge"; 
    binder_status_t status = AServiceManager_addService(provider->asBinder().get(), serviceName.c_str());

    if (status != STATUS_OK) {
        LOGE("Failed to register HalCameraProvider service '%s'. Status: %d", serviceName.c_str(), status);
        // Provider might still be usable if not registered, but CameraService won't find it.
        // Depending on requirements, might want to cleanup provider here.
        // For now, let it exist but log failure.
    } else {
        LOGI("HalCameraProvider service '%s' registered successfully.", serviceName.c_str());
    }
    
    // Store the shared_ptr in a way that Java can hold onto it.
    // We return a raw pointer to a new shared_ptr instance on the heap.
    // Java side will store this jlong and pass it back for cleanup.
    std::shared_ptr<HalCameraProvider>* providerPtr = new std::shared_ptr<HalCameraProvider>(provider);
    LOGI("HalCameraProvider initialized and context created: %p", providerPtr);
    return reinterpret_cast<jlong>(providerPtr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_cambridge_VirtualCameraProviderService_cleanupNative(
        JNIEnv* env, jobject /* this */, jlong providerContext) {
    
    if (providerContext == 0) {
        LOGE("cleanupNative: Invalid provider context (null)");
        return;
    }
    
    LOGI("Cleaning up HalCameraProvider with context: %lld", providerContext);
    std::shared_ptr<HalCameraProvider>* providerPtr = reinterpret_cast<std::shared_ptr<HalCameraProvider>*>(providerContext);
    
    if (providerPtr && *providerPtr) {
        (*providerPtr)->cleanup(); // Call any internal cleanup
        // The shared_ptr itself will be deleted, decrementing ref count.
    } else {
        LOGE("cleanupNative: Provider context %lld did not yield a valid provider.", providerContext);
    }
    
    delete providerPtr; // Delete the heap-allocated shared_ptr holder
    LOGI("HalCameraProvider context cleaned up.");
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_cambridge_UvcCameraManager_notifyHalProviderDeviceAvailable(
        JNIEnv* env, jobject /* this */, jlong providerContext, 
        jstring javaCameraId, jboolean available) {
    
    if (providerContext == 0) {
        LOGE("notifyHalProviderDeviceAvailable: Invalid provider context (null)");
        return;
    }
    std::shared_ptr<HalCameraProvider>* providerPtr = reinterpret_cast<std::shared_ptr<HalCameraProvider>*>(providerContext);
    if (!providerPtr || !(*providerPtr)) {
         LOGE("notifyHalProviderDeviceAvailable: Provider context %lld did not yield a valid provider.", providerContext);
        return;
    }

    const char* cameraIdStr = env->GetStringUTFChars(javaCameraId, nullptr);
    if (!cameraIdStr) {
        LOGE("notifyHalProviderDeviceAvailable: Failed to get camera ID string from Java");
        return; // Or throw exception
    }
    std::string cameraId(cameraIdStr);
    env->ReleaseStringUTFChars(javaCameraId, cameraIdStr);

    LOGI("Notifying HAL provider: Camera ID '%s' is %s", cameraId.c_str(), available ? "available" : "unavailable");
    (*providerPtr)->signalDeviceAvailable(cameraId, available);
}


extern "C" JNIEXPORT jboolean JNICALL
Java_com_android_cambridge_UvcCameraManager_pushVideoFrameNative(
        JNIEnv* env, jobject /* this */, jlong providerContext, jstring javaCameraId,
        jbyteArray frameData, jint width, jint height, jint format) {
    
    if (providerContext == 0) {
        LOGE("pushVideoFrameNative: Invalid provider context (null)");
        return JNI_FALSE;
    }
    std::shared_ptr<HalCameraProvider>* providerPtr = reinterpret_cast<std::shared_ptr<HalCameraProvider>*>(providerContext);
     if (!providerPtr || !(*providerPtr)) {
         LOGE("pushVideoFrameNative: Provider context %lld did not yield a valid provider.", providerContext);
        return JNI_FALSE;
    }

    const char* cameraIdStrChars = env->GetStringUTFChars(javaCameraId, nullptr);
    if (!cameraIdStrChars) {
        LOGE("pushVideoFrameNative: Failed to get camera ID string from Java");
        return JNI_FALSE;
    }
    std::string cameraIdStr(cameraIdStrChars);
    env->ReleaseStringUTFChars(javaCameraId, cameraIdStrChars);

    std::shared_ptr<HalCameraSession> session = (*providerPtr)->getActiveSessionForCameraId(cameraIdStr);

    if (!session) {
        // LOGD might be too noisy if frames arrive before session is ready
        // ALOGV("pushVideoFrameNative: No active session for camera ID '%s'. Frame dropped.", cameraIdStr.c_str());
        return JNI_FALSE;
    }
    
    jbyte* uvcDataBytes = env->GetByteArrayElements(frameData, nullptr);
    if (!uvcDataBytes) {
        LOGE("pushVideoFrameNative: Failed to get byte array elements from frameData for camera %s", cameraIdStr.c_str());
        return JNI_FALSE;
    }
    jsize dataLength = env->GetArrayLength(frameData);
    
    // ALOGV("Pushing frame to session for camera %s: %dx%d, format %d, size %d", 
    //       cameraIdStr.c_str(), width, height, format, dataLength);
    session->pushNewFrame(
        reinterpret_cast<const uint8_t*>(uvcDataBytes), 
        static_cast<size_t>(dataLength),
        width, height, format);
    
    env->ReleaseByteArrayElements(frameData, uvcDataBytes, JNI_ABORT); // JNI_ABORT: no copy back
    
    return JNI_TRUE;
}

jint JNI_OnLoad(JavaVM* vm, void* /* reserved */) {
    gJavaVM = vm;
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("JNI_OnLoad: Failed to get JNIEnv");
        return -1;
    }

    // Start the Binder thread pool for this process.
    // This is necessary for the HAL service (HalCameraProvider) to handle incoming Binder calls
    // from CameraService or other clients.
    // It should generally be called once per process that hosts AIDL services.
    ABinderProcess_startThreadPool();
    LOGI("JNI library loaded and Binder thread pool started.");
    
    return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM* vm, void* /* reserved */) {
    // Note: Cleanup of provider instances should happen via VirtualCameraProviderService.cleanupNative
    // before the library is unloaded, if possible.
    // If ABinderProcess_stopThreadPool() or similar is needed, it could go here,
    // but typically not required as process termination handles it.
    gJavaVM = nullptr;
    LOGI("JNI library unloaded.");
}

// This is a C++ callable function, not a JNI method of a Java class
std::vector<uint8_t> callJavaMjpegDecoder(const uint8_t* mjpeg_data, size_t mjpeg_size, int width, int height) {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (gJavaVM == nullptr) {
        LOGE("gJavaVM is null in callJavaMjpegDecoder");
        return {};
    }

    int getEnvStat = gJavaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvStat == JNI_EDETACHED) {
        if (gJavaVM->AttachCurrentThread(&env, nullptr) != 0) {
            LOGE("Failed to attach current thread to JavaVM");
            return {};
        }
        attached = true;
    } else if (getEnvStat == JNI_EVERSION) {
        LOGE("JNI version not supported");
        return {};
    } // JNI_OK means env is valid

    if (env == nullptr) {
        LOGE("JNIEnv is null after GetEnv/AttachCurrentThread");
        if (attached) gJavaVM->DetachCurrentThread();
        return {};
    }

    jbyteArray javaMjpegData = env->NewByteArray(mjpeg_size);
    if (javaMjpegData == nullptr) {
        LOGE("Failed to allocate NewByteArray for MJPEG data");
        if (attached) gJavaVM->DetachCurrentThread();
        return {};
    }
    env->SetByteArrayRegion(javaMjpegData, 0, mjpeg_size, reinterpret_cast<const jbyte*>(mjpeg_data));

    jclass mjpegDecoderClass = env->FindClass("com/android/cambridge/MjpegDecoder");
    if (mjpegDecoderClass == nullptr) {
        LOGE("Failed to find MjpegDecoder class");
        env->DeleteLocalRef(javaMjpegData);
        if (attached) gJavaVM->DetachCurrentThread();
        return {};
    }

    jmethodID decodeMethod = env->GetStaticMethodID(mjpegDecoderClass, 
                                                    "decodeMjpegFrameFromNative", 
                                                    "([BII)[B");
    if (decodeMethod == nullptr) {
        LOGE("Failed to find MjpegDecoder.decodeMjpegFrameFromNative static method");
        env->DeleteLocalRef(mjpegDecoderClass);
        env->DeleteLocalRef(javaMjpegData);
        if (attached) gJavaVM->DetachCurrentThread();
        return {};
    }

    jbyteArray javaYuvDataArray = (jbyteArray)env->CallStaticObjectMethod(mjpegDecoderClass, 
                                                                        decodeMethod, 
                                                                        javaMjpegData, 
                                                                        width, 
                                                                        height);
    
    std::vector<uint8_t> yuv_vector;
    if (javaYuvDataArray != nullptr) {
        jsize yuv_len = env->GetArrayLength(javaYuvDataArray);
        jbyte* yuv_bytes = env->GetByteArrayElements(javaYuvDataArray, nullptr);
        if (yuv_bytes) {
            yuv_vector.assign(yuv_bytes, yuv_bytes + yuv_len);
            env->ReleaseByteArrayElements(javaYuvDataArray, yuv_bytes, JNI_ABORT);
        } else {
            LOGE("Failed to get byte array elements from YUV data");
            // yuv_vector will be empty
        }
        env->DeleteLocalRef(javaYuvDataArray);
    } else {
        LOGE("Java MjpegDecoder.decodeMjpegFrameFromNative returned null");
        // yuv_vector will be empty
    }
    
    env->DeleteLocalRef(mjpegDecoderClass);
    env->DeleteLocalRef(javaMjpegData);

    if (attached) {
        gJavaVM->DetachCurrentThread();
    }
    return yuv_vector;
}