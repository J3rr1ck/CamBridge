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
     * Gets the data as a byte array for direct access by JNI code.
     * This is more efficient for JNI since ByteBuffer requires extra handling.
     * 
     * @return The frame data as a byte array
     */
    public byte[] getDataArray() {
        if (data == null) {
            return new byte[0];
        }
        
        data.rewind();
        if (data.hasArray() && data.arrayOffset() == 0 && data.array().length == data.remaining()) {
            // Use the existing array if possible
            return data.array();
        } else {
            // Copy to a new array
            byte[] array = new byte[data.remaining()];
            data.get(array);
            data.rewind();
            return array;
        }
    }
    
    /**
     * Creates a VideoFrame from raw components.
     * 
     * @param dataArray The frame data as a byte array
     * @param width The frame width
     * @param height The frame height
     * @param format The frame format (one of the FORMAT_* constants)
     * @return A new VideoFrame with the specified parameters
     */
    public static VideoFrame fromRawComponents(byte[] dataArray, int width, int height, int format) {
        VideoFrame frame = new VideoFrame();
        frame.width = width;
        frame.height = height;
        frame.format = format;
        frame.data = ByteBuffer.wrap(dataArray);
        return frame;
    }
    
    /**
     * Releases resources associated with this frame.
     */
    public void release() {
        data = null;
    }
} 