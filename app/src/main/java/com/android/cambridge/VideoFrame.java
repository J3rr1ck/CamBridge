package com.android.cambridge;

import java.nio.ByteBuffer;

/**
 * Represents a video frame from a UVC camera.
 */
public class VideoFrame {
    // Supported formats
    public static final int FORMAT_MJPEG = 0;
    public static final int FORMAT_YUYV = 1;
    public static final int FORMAT_NV21 = 2;
    public static final int FORMAT_YUV420 = 3;
    
    // Frame data
    public ByteBuffer data;
    
    // Frame metadata
    public int width;
    public int height;
    public int format;
    public long timestamp;
    
    public VideoFrame() {
        timestamp = System.nanoTime();
    }
    
    /**
     * Converts the frame to a different format if needed.
     * 
     * @param targetFormat The target format (one of the FORMAT_* constants)
     * @return A new VideoFrame in the target format, or this frame if already in the target format
     */
    public VideoFrame convertTo(int targetFormat) {
        if (format == targetFormat) {
            return this;
        }
        
        // In a real implementation, this would convert between formats
        // For example, MJPEG → YUV420, YUYV → YUV420, etc.
        // This would use something like MediaCodec or a native library like libyuv
        
        // For this demonstration, we'll return a copy of the frame with the format changed
        // Without actual conversion
        VideoFrame converted = new VideoFrame();
        converted.width = width;
        converted.height = height;
        converted.format = targetFormat;
        converted.timestamp = timestamp;
        
        // Just duplicate the data (in real implementation, it would be converted)
        converted.data = ByteBuffer.allocate(data.capacity());
        data.rewind();
        converted.data.put(data);
        data.rewind();
        converted.data.rewind();
        
        return converted;
    }
    
    /**
     * Releases resources associated with this frame.
     */
    public void release() {
        data = null;
    }
} 