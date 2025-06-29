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

#ifndef UVC_CAMERA_HAL_FORMATCONVERTER_H
#define UVC_CAMERA_HAL_FORMATCONVERTER_H

#include <cstdint> // For uint8_t etc.
#include <vector>
#include <linux/videodev2.h> // For V4L2_PIX_FMT_*

// Forward declare jpeglib structs if needed, or include the header
// For libjpeg-turbo, it's usually <jpeglib.h>
struct jpeg_decompress_struct; // Forward declaration example

namespace android {

class FormatConverter {
public:
    FormatConverter();
    ~FormatConverter();

    // Converts MJPEG to YUYV (V4L2_PIX_FMT_YUYV)
    // Output buffer (outYuyv) must be pre-allocated by the caller.
    // Returns true on success, false on failure.
    bool mjpegToYuyv(const uint8_t* mjpegSrc, size_t mjpegSize,
                     uint32_t width, uint32_t height,
                     uint8_t* outYuyv, size_t outYuyvSize);

    // Converts MJPEG to a planar YUV420 format (like I420: YYYY...UU...VV...)
    // Output buffer (outYuv420p) must be pre-allocated.
    // yStride, uStride, vStride are the strides for Y, U, V planes.
    bool mjpegToYuv420p(const uint8_t* mjpegSrc, size_t mjpegSize,
                        uint32_t width, uint32_t height,
                        uint8_t* outYuv420p, size_t outYuv420pSize,
                        uint32_t yStride, uint32_t uStride, uint32_t vStride);

    // Converts YUYV (V4L2_PIX_FMT_YUYV) to NV21 (YYYY...VUVU...)
    // Output buffer (outNv21) must be pre-allocated.
    bool yuyvToNv21(const uint8_t* yuyvSrc, uint32_t width, uint32_t height,
                    uint8_t* outNv21, size_t outNv21Size);

    // Converts YUYV (V4L2_PIX_FMT_YUYV) to I420 (Planar YUV420: YYYY...UU...VV...)
    // Output buffer (outI420) must be pre-allocated.
    bool yuyvToI420(const uint8_t* yuyvSrc, uint32_t width, uint32_t height,
                    uint8_t* outI420, size_t outI420Size,
                    uint32_t yStride, uint32_t uStride, uint32_t vStride);


    // Add other conversions as needed, e.g.:
    // - YUYV to RGB
    // - Specific YUV420 planar (I420) to YUV420 semi-planar (NV21/NV12)

    // Helper to calculate buffer size for common formats
    static size_t getBufferSize(uint32_t width, uint32_t height, uint32_t format);


private:
    // For libjpeg-turbo, you might have a decompress object as a member
    // if you want to reuse it across calls, but for simplicity,
    // it can be created and destroyed within each mjpegTo* method.
    // struct jpeg_decompress_struct mJpegDecompressInfo;
    // struct jpeg_error_mgr mJpegErrorMgr;

    // Disable copy and assign
    FormatConverter(const FormatConverter&) = delete;
    FormatConverter& operator=(const FormatConverter&) = delete;
};

} // namespace android

#endif // UVC_CAMERA_HAL_FORMATCONVERTER_H
