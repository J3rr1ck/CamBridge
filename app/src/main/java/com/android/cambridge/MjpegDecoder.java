package com.android.cambridge;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaCodecInfo;
import android.util.Log;
import java.io.IOException;
import java.nio.ByteBuffer;

public class MjpegDecoder {
    private static final String TAG = "MjpegDecoder";
    private static final String MIME_TYPE_MJPEG = "video/mjpeg";
    private MediaCodec mCodec;
    private int mWidth;
    private int mHeight;
    private byte[] mDecodedFrameData; // To store the latest decoded frame

    public MjpegDecoder() {
        // Constructor can be empty or initialize some members
    }

    // Initializes the decoder for a specific resolution.
    // Returns true on success, false on failure.
    public boolean configure(int width, int height) {
        Log.d(TAG, "Configuring MjpegDecoder for " + width + "x" + height);
        mWidth = width;
        mHeight = height;
        try {
            // Ensure any previous instance is released
            if (mCodec != null) {
                mCodec.stop();
                mCodec.release();
                mCodec = null;
            }

            mCodec = MediaCodec.createDecoderByType(MIME_TYPE_MJPEG);
            MediaFormat format = MediaFormat.createVideoFormat(MIME_TYPE_MJPEG, width, height);
            // Request flexible YUV420 output, common for AHardwareBuffer
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            
            mCodec.configure(format, null /* surface */, null /* crypto */, 0 /* flags: decoder */);
            mCodec.start();
            Log.d(TAG, "MediaCodec for MJPEG decoding started.");
            return true;
        } catch (IOException | IllegalStateException e) {
            Log.e(TAG, "Failed to configure MediaCodec for MJPEG: " + e.getMessage());
            if (mCodec != null) {
                mCodec.release();
                mCodec = null;
            }
            return false;
        }
    }

    // Decodes a single MJPEG frame.
    // Returns the decoded YUV data as a byte array, or null on failure or if not configured.
    // The returned byte array is expected to be in a YUV420 format (e.g. NV12 or I420 planar).
    public byte[] decode(byte[] mjpegFrame) {
        if (mCodec == null || mjpegFrame == null) {
            Log.e(TAG, "Decoder not configured or input frame is null.");
            return null;
        }

        try {
            int inputBufferIndex = mCodec.dequeueInputBuffer(10000); // 10ms timeout
            if (inputBufferIndex >= 0) {
                ByteBuffer inputBuffer = mCodec.getInputBuffer(inputBufferIndex);
                if (inputBuffer != null) {
                    inputBuffer.clear();
                    inputBuffer.put(mjpegFrame);
                    mCodec.queueInputBuffer(inputBufferIndex, 0, mjpegFrame.length, 
                                           System.nanoTime() / 1000 /* presentation time Us */, 0);
                } else {
                     Log.e(TAG, "getInputBuffer returned null");
                     return null; // Should not happen
                }
            } else {
                Log.w(TAG, "Failed to dequeue input buffer. Index: " + inputBufferIndex);
                return null; // Try again later or indicate error
            }

            MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
            int outputBufferIndex = mCodec.dequeueOutputBuffer(bufferInfo, 10000); // 10ms timeout

            if (outputBufferIndex >= 0) {
                ByteBuffer outputBuffer = mCodec.getOutputBuffer(outputBufferIndex);
                if (outputBuffer != null) {
                    // Ensure mDecodedFrameData is large enough
                    if (mDecodedFrameData == null || mDecodedFrameData.length != bufferInfo.size) {
                        mDecodedFrameData = new byte[bufferInfo.size];
                    }
                    outputBuffer.get(mDecodedFrameData, 0, bufferInfo.size);
                    mCodec.releaseOutputBuffer(outputBufferIndex, false);
                    return mDecodedFrameData;
                } else {
                     Log.e(TAG, "getOutputBuffer returned null");
                     mCodec.releaseOutputBuffer(outputBufferIndex, false); // Still release
                     return null;
                }
            } else if (outputBufferIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat newFormat = mCodec.getOutputFormat();
                Log.i(TAG, "Decoder output format changed: " + newFormat);
                // You might need to re-query width/height/stride/color format here
                // For YUV420Flexible, the actual layout (NV12, I420, etc.) can be found from newFormat
            } else if (outputBufferIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                Log.w(TAG, "dequeueOutputBuffer timed out.");
            } else {
                Log.w(TAG, "dequeueOutputBuffer returned: " + outputBufferIndex);
            }
        } catch (IllegalStateException e) {
            Log.e(TAG, "MediaCodec error during decoding: " + e.getMessage());
            // Codec might be in an error state, may need reset/reconfigure
            return null;
        }
        return null; // No frame decoded in this attempt
    }

    // Releases MediaCodec resources.
    public void release() {
        Log.d(TAG, "Releasing MjpegDecoder.");
        if (mCodec != null) {
            try {
                mCodec.stop();
            } catch (IllegalStateException e) {
                Log.e(TAG, "Error stopping codec: " + e.getMessage());
            }
            try {
                mCodec.release();
            } catch (IllegalStateException e) {
                Log.e(TAG, "Error releasing codec: " + e.getMessage());
            }
            mCodec = null;
        }
    }

    /**
     * Static JNI bridge method.
     * This method is called from native C++ code.
     * It creates an MjpegDecoder instance, configures it, decodes one frame, and releases.
     * This is inefficient for continuous streaming but good for a JNI bridge.
     * A more optimized approach would keep the MjpegDecoder instance alive.
     *
     * @param mjpegData MJPEG encoded frame data.
     * @param width Frame width.
     * @param height Frame height.
     * @return Decoded YUV data as byte array, or null on failure.
     */
    public static byte[] decodeMjpegFrameFromNative(byte[] mjpegData, int width, int height) {
        Log.d(TAG, "decodeMjpegFrameFromNative called for " + width + "x" + height);
        MjpegDecoder decoder = new MjpegDecoder();
        if (!decoder.configure(width, height)) {
            Log.e(TAG, "Failed to configure decoder in static JNI bridge method.");
            decoder.release(); // ensure release even if configure fails
            return null;
        }
        byte[] yuvData = decoder.decode(mjpegData);
        decoder.release(); // Release after single frame decode
        
        if (yuvData != null) {
            Log.d(TAG, "Successfully decoded MJPEG frame to YUV, size: " + yuvData.length);
        } else {
            Log.e(TAG, "Failed to decode MJPEG frame in static JNI bridge method.");
        }
        return yuvData;
    }
}
