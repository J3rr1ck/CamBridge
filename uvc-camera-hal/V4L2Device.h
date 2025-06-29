/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UVC_CAMERA_HAL_V4L2DEVICE_H
#define UVC_CAMERA_HAL_V4L2DEVICE_H

#include <string>
#include <vector>
#include <linux/videodev2.h> // For V4L2 structures
#include <utils/Mutex.h>

namespace android {

struct V4L2Buffer {
    void* start = nullptr;
    size_t length = 0;
    // Add other buffer related info if needed, like offset for planar
};

struct V4L2FormatInfo {
    uint32_t pixel_format; // V4L2_PIX_FMT_*
    uint32_t width;
    uint32_t height;
    std::vector<float> frame_rates; // Store as actual rates (fps)
};

class V4L2Device {
public:
    V4L2Device(const std::string& devicePath);
    ~V4L2Device();

    bool openDevice();
    void closeDevice();
    bool isOpen() const;

    bool queryCaps(struct v4l2_capability* caps);
    std::vector<V4L2FormatInfo> enumFormats(); // Enumerates all formats and their supported sizes/rates

    // Get/Set current format
    bool getFormat(struct v4l2_format* format);
    bool setFormat(uint32_t pixelFormat, uint32_t width, uint32_t height);
    // Try to set a specific frame rate if supported by the driver for current format
    bool setFrameRate(float frameRate);


    bool requestBuffers(int count, enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE);
    bool queryBuffer(int index, struct v4l2_buffer* v4l2buf, enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE);
    bool mmapBuffers();
    void unmapBuffers();

    bool queueBuffer(int index, const struct v4l2_plane* planes = nullptr, int num_planes = 0);
    bool dequeueBuffer(struct v4l2_buffer* v4l2buf, struct v4l2_plane* planes = nullptr, int num_planes = 0);

    bool streamOn();
    bool streamOff();

    bool getControl(uint32_t id, int32_t* value);
    bool setControl(uint32_t id, int32_t value);
    std::vector<struct v4l2_queryctrl> queryControls(); // Query all available controls
    std::vector<struct v4l2_querymenu> queryControlMenuItems(uint32_t control_id, const struct v4l2_queryctrl& ctrl);


    const std::string& getDevicePath() const { return mDevicePath; }
    const std::vector<V4L2Buffer>& getMappedBuffers() const { return mMappedBuffers; }
    uint32_t getCurrentPixelFormat() const { return mCurrentFormat.fmt.pix.pixelformat; }
    uint32_t getCurrentWidth() const { return mCurrentFormat.fmt.pix.width; }
    uint32_t getCurrentHeight() const { return mCurrentFormat.fmt.pix.height; }


private:
    int xioctl(int fd, unsigned long request, void* arg); // Helper for ioctl calls

    std::string mDevicePath;
    int mDeviceFd = -1;
    std::vector<V4L2Buffer> mMappedBuffers;
    struct v4l2_format mCurrentFormat; // Stores the currently set format on the device
    enum v4l2_buf_type mBufferType;
    int mNumBuffers = 0;

    android::Mutex mLock; // Protects access to device operations if needed from multiple threads

    // Disable copy and assign
    V4L2Device(const V4L2Device&) = delete;
    V4L2Device& operator=(const V4L2Device&) = delete;
};

} // namespace android

#endif // UVC_CAMERA_HAL_V4L2DEVICE_H
