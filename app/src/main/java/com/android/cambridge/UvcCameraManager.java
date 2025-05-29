package com.android.cambridge;

import android.content.Context;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.hardware.usb.UsbRequest;
import android.util.Log;
import android.util.Size;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Consumer;

/**
 * Manages UVC camera connections and video streaming.
 */
public class UvcCameraManager {
    private static final String TAG = "UvcCameraManager";
    private static final int USB_CLASS_VIDEO = 0x0E;
    private static final int USB_VIDEO_INTERFACE_SUBCLASS_CONTROL = 0x01;
    private static final int USB_VIDEO_INTERFACE_SUBCLASS_STREAMING = 0x02;

    // UVC Constants
    private static final int UVC_PROCESSING_UNIT_ID = 0x02;
    private static final int UVC_BRIGHTNESS_CONTROL_SELECTOR = 0x02; // PU_BRIGHTNESS_CONTROL
    private static final int UVC_SET_CUR = 0x01;
    private static final int UVC_GET_CUR = 0x81;
    private static final int UVC_CONTROL_REQUEST_TYPE_SET = UsbConstants.USB_TYPE_CLASS | UsbConstants.USB_DIR_OUT | 0x01; // 0x21
    private static final int UVC_CONTROL_REQUEST_TYPE_GET = UsbConstants.USB_TYPE_CLASS | UsbConstants.USB_DIR_IN | 0x01; // 0xA1
    
    // Buffer size for reading video frames (adjust based on expected frame size)
    private static final int BUFFER_SIZE = 1024 * 1024; // 1MB
    
    private final Context mContext;
    private final UsbManager mUsbManager;
    private final ExecutorService mExecutor;
    
    // Maps device name to active connection
    private final Map<String, UsbConnection> mActiveConnections = new HashMap<>();
    
    /**
     * Container for USB device connection information
     */
    private static class UsbConnection {
        final UsbDevice device;
        final UsbDeviceConnection connection;
        final UsbInterface controlInterface;
        final UsbInterface streamingInterface;
        final UsbEndpoint videoEndpoint;
        Consumer<VideoFrame> frameCallback;
        boolean streaming = false;
        Thread streamingThread;
        
        UsbConnection(UsbDevice device, UsbDeviceConnection connection, 
                     UsbInterface controlInterface, UsbInterface streamingInterface,
                     UsbEndpoint videoEndpoint) {
            this.device = device;
            this.connection = connection;
            this.controlInterface = controlInterface;
            this.streamingInterface = streamingInterface;
            this.videoEndpoint = videoEndpoint;
        }
    }
    
    public UvcCameraManager(Context context) {
        mContext = context;
        mUsbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        mExecutor = Executors.newCachedThreadPool();
    }
    
    /**
     * Checks if a USB device is a UVC camera.
     */
    public static boolean isUvcCamera(UsbDevice device) {
        if (device == null) return false;
        
        // Check interfaces for video class
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface intf = device.getInterface(i);
            if (intf.getInterfaceClass() == USB_CLASS_VIDEO) {
                return true;
            }
        }
        return false;
    }
    
    /**
     * Opens a UVC camera device.
     *
     * @param device The UVC camera device to open
     * @return true if successful
     */
    public boolean openCamera(UsbDevice device) {
        if (device == null) return false;
        
        String deviceKey = device.getDeviceName();
        if (mActiveConnections.containsKey(deviceKey)) {
            Log.i(TAG, "Camera already opened: " + deviceKey);
            return true;
        }
        
        // Find control and streaming interfaces
        UsbInterface controlInterface = null;
        UsbInterface streamingInterface = null;
        
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface intf = device.getInterface(i);
            if (intf.getInterfaceClass() == USB_CLASS_VIDEO) {
                if (intf.getInterfaceSubclass() == USB_VIDEO_INTERFACE_SUBCLASS_CONTROL) {
                    controlInterface = intf;
                } else if (intf.getInterfaceSubclass() == USB_VIDEO_INTERFACE_SUBCLASS_STREAMING) {
                    streamingInterface = intf;
                }
            }
        }
        
        if (streamingInterface == null) {
            Log.e(TAG, "No video streaming interface found");
            return false;
        }
        
        // Find the video streaming endpoint (BULK or ISO)
        UsbEndpoint videoEndpoint = null;
        for (int i = 0; i < streamingInterface.getEndpointCount(); i++) {
            UsbEndpoint endpoint = streamingInterface.getEndpoint(i);
            // We're looking for an IN endpoint for video data
            if (endpoint.getDirection() == UsbConstants.USB_DIR_IN && 
                   (endpoint.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK || 
                    endpoint.getType() == UsbConstants.USB_ENDPOINT_XFER_ISOC)) {
                videoEndpoint = endpoint;
                break;
            }
        }
        
        if (videoEndpoint == null) {
            Log.e(TAG, "No suitable video endpoint found");
            return false;
        }
        
        // Open a connection to the device
        UsbDeviceConnection connection = mUsbManager.openDevice(device);
        if (connection == null) {
            Log.e(TAG, "Failed to open device connection");
            return false;
        }
        
        // Claim the interfaces
        if (controlInterface != null) {
            boolean claimControl = connection.claimInterface(controlInterface, true);
            if (!claimControl) {
                Log.w(TAG, "Failed to claim control interface");
                // We can continue without control interface in some cases
            }
        }
        
        boolean claimStreaming = connection.claimInterface(streamingInterface, true);
        if (!claimStreaming) {
            Log.e(TAG, "Failed to claim streaming interface");
            connection.close();
            return false;
        }
        
        // Create and store the connection
        UsbConnection usbConnection = new UsbConnection(
                device, connection, controlInterface, streamingInterface, videoEndpoint);
        mActiveConnections.put(deviceKey, usbConnection);
        
        // Configure the camera with default settings
        configureCamera(usbConnection);
        
        Log.i(TAG, "Successfully opened camera: " + deviceKey);
        return true;
    }
    
    /**
     * Closes a UVC camera device.
     *
     * @param device The UVC camera device to close
     */
    public void closeCamera(UsbDevice device) {
        if (device == null) return;
        
        String deviceKey = device.getDeviceName();
        UsbConnection conn = mActiveConnections.get(deviceKey);
        if (conn == null) return;
        
        // Stop streaming if active
        if (conn.streaming) {
            stopStreaming(device);
        }
        
        // Release interfaces and close connection
        if (conn.streamingInterface != null) {
            conn.connection.releaseInterface(conn.streamingInterface);
        }
        
        if (conn.controlInterface != null) {
            conn.connection.releaseInterface(conn.controlInterface);
        }
        
        conn.connection.close();
        mActiveConnections.remove(deviceKey);
        Log.i(TAG, "Closed camera: " + deviceKey);
    }
    
    /**
     * Closes all open cameras.
     */
    public void closeAllCameras() {
        List<UsbDevice> devices = new ArrayList<>(mActiveConnections.size());
        for (UsbConnection conn : mActiveConnections.values()) {
            devices.add(conn.device);
        }
        
        for (UsbDevice device : devices) {
            closeCamera(device);
        }
    }
    
    /**
     * Sets a callback to receive video frames from the camera.
     *
     * @param device The UVC camera device
     * @param callback The callback to receive frames
     */
    public void setFrameCallback(UsbDevice device, Consumer<VideoFrame> callback) {
        if (device == null) return;
        
        String deviceKey = device.getDeviceName();
        UsbConnection conn = mActiveConnections.get(deviceKey);
        if (conn != null) {
            conn.frameCallback = callback;
        }
    }
    
    /**
     * Starts video streaming from the camera.
     *
     * @param device The UVC camera device
     * @return true if streaming started successfully
     */
    public boolean startStreaming(UsbDevice device) {
        if (device == null) return false;
        
        String deviceKey = device.getDeviceName();
        UsbConnection conn = mActiveConnections.get(deviceKey);
        if (conn == null) {
            Log.e(TAG, "Camera not open: " + deviceKey);
            return false;
        }
        
        if (conn.streaming) {
            Log.i(TAG, "Camera already streaming: " + deviceKey);
            return true;
        }
        
        // Start streaming thread
        conn.streaming = true;
        conn.streamingThread = new Thread(() -> streamVideoData(conn));
        conn.streamingThread.start();
        
        Log.i(TAG, "Started streaming from camera: " + deviceKey);
        return true;
    }
    
    /**
     * Stops video streaming from the camera.
     *
     * @param device The UVC camera device
     */
    public void stopStreaming(UsbDevice device) {
        if (device == null) return;
        
        String deviceKey = device.getDeviceName();
        UsbConnection conn = mActiveConnections.get(deviceKey);
        if (conn == null || !conn.streaming) return;
        
        // Signal thread to stop and wait for it
        conn.streaming = false;
        if (conn.streamingThread != null) {
            try {
                conn.streamingThread.join(1000);
            } catch (InterruptedException e) {
                Log.w(TAG, "Interrupted while stopping streaming thread", e);
            }
            conn.streamingThread = null;
        }
        
        Log.i(TAG, "Stopped streaming from camera: " + deviceKey);
    }
    
    /**
     * Gets information about the camera's capabilities.
     *
     * @param device The UVC camera device
     * @return UvcCameraInfo object containing the camera's capabilities
     */
    public UvcCameraInfo getCameraInfo(UsbDevice device) {
        if (device == null) return null;
        
        String deviceKey = device.getDeviceName();
        UsbConnection conn = mActiveConnections.get(deviceKey);
        if (conn == null) return null;
        
        // Query camera capabilities using UVC control requests
        // This is simplified - in a real implementation, you would parse
        // format descriptors from the device
        
        UvcCameraInfo info = new UvcCameraInfo();
        info.deviceId = deviceKey;
        info.productName = device.getProductName();
        
        // For demonstration, we'll add some common resolutions
        // In a real implementation, query these from the device
        info.supportedResolutions = new ArrayList<>();
        info.supportedResolutions.add(new Size(640, 480));
        info.supportedResolutions.add(new Size(1280, 720));
        info.supportedResolutions.add(new Size(1920, 1080));
        
        // Similarly for frame rates
        info.supportedFrameRates = new ArrayList<>();
        info.supportedFrameRates.add(15.0f);
        info.supportedFrameRates.add(30.0f);
        
        return info;
    }
    
    /**
     * Thread function to read video data from the USB device
     */
    private void streamVideoData(UsbConnection conn) {
        if (conn == null || conn.videoEndpoint == null) return;
        
        UsbDeviceConnection connection = conn.connection;
        UsbEndpoint endpoint = conn.videoEndpoint;
        ByteBuffer buffer = ByteBuffer.allocate(BUFFER_SIZE);
        
        while (conn.streaming) {
            buffer.clear();
            
            // Read data from the endpoint
            int bytesRead = connection.bulkTransfer(endpoint, buffer.array(), 
                    buffer.capacity(), 5000);
            
            if (bytesRead > 0) {
                // We have data, create a video frame and deliver it
                buffer.limit(bytesRead);
                deliverFrame(conn, buffer);
            } else if (bytesRead == 0) {
                // Timeout, no data available
                try {
                    Thread.sleep(5);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            } else {
                // Error
                Log.e(TAG, "Error reading from device: " + bytesRead);
                break;
            }
        }
        
        Log.i(TAG, "Streaming thread exiting");
    }
    
    /**
     * Delivers a video frame to the registered callback
     */
    private void deliverFrame(UsbConnection conn, ByteBuffer data) {
        if (conn.frameCallback == null) return;
        
        // In a real implementation, you would need to parse the UVC payload
        // headers and extract the actual video frame data
        
        // For demonstration, we'll create a simple frame
        // Assume the data is already in the right format (likely not true in practice)
        VideoFrame frame = new VideoFrame();
        frame.width = 640;  // These should be determined from the UVC headers
        frame.height = 480;
        frame.format = VideoFrame.FORMAT_MJPEG;  // Or YUYV, etc.
        
        // Copy the data
        frame.data = ByteBuffer.allocate(data.limit());
        data.rewind();
        frame.data.put(data);
        frame.data.rewind();
        
        // Call the frame callback
        try {
            conn.frameCallback.accept(frame);
        } catch (Exception e) {
            Log.e(TAG, "Error in frame callback", e);
        }
    }
    
    /**
     * Configures the camera with default settings
     */
    private void configureCamera(UsbConnection conn) {
        if (conn == null || conn.controlInterface == null) return;
        
        // In a full implementation, you would:
        // 1. Query the device for its format descriptors
        // 2. Select an appropriate format (resolution, frame rate, etc.)
        // 3. Send UVC control requests to configure the device
        
        // This is a placeholder for the actual configuration code
        Log.i(TAG, "Camera configured with default settings");
    }

    /**
     * Gets the current brightness value from the camera.
     *
     * @param deviceName The name of the UVC camera device
     * @return The current brightness value, or -1 if an error occurred
     */
    public int getBrightness(String deviceName) {
        UsbConnection conn = mActiveConnections.get(deviceName);
        if (conn == null) {
            Log.e(TAG, "getBrightness: Camera not found for device " + deviceName);
            return -1;
        }
        if (conn.controlInterface == null) {
            Log.e(TAG, "getBrightness: No control interface found for device " + deviceName);
            return -1;
        }

        byte[] data = new byte[2];
        int wIndex = (UVC_PROCESSING_UNIT_ID << 8) | conn.controlInterface.getId();
        int wValue = (UVC_BRIGHTNESS_CONTROL_SELECTOR << 8);

        int bytesTransferred = conn.connection.controlTransfer(
                UVC_CONTROL_REQUEST_TYPE_GET, UVC_GET_CUR, wValue, wIndex, data, data.length, 1000);

        if (bytesTransferred >= 0) {
            // Convert little-endian bytes to integer
            int brightness = (data[1] << 8) | (data[0] & 0xFF);
            Log.i(TAG, "getBrightness: Device " + deviceName + ", Value: " + brightness + 
                         ", Raw: " + data[0] + " " + data[1]);
            return brightness;
        } else {
            Log.e(TAG, "getBrightness: Control transfer failed for device " + deviceName + 
                         ", Error: " + bytesTransferred);
            return -1;
        }
    }

    /**
     * Sets the brightness value for the camera.
     *
     * @param deviceName The name of the UVC camera device
     * @param brightnessValue The brightness value to set
     * @return true if successful, false otherwise
     */
    public boolean setBrightness(String deviceName, int brightnessValue) {
        UsbConnection conn = mActiveConnections.get(deviceName);
        if (conn == null) {
            Log.e(TAG, "setBrightness: Camera not found for device " + deviceName);
            return false;
        }
        if (conn.controlInterface == null) {
            Log.e(TAG, "setBrightness: No control interface found for device " + deviceName);
            return false;
        }

        byte[] data = new byte[2];
        data[0] = (byte) (brightnessValue & 0xFF);          // Low byte
        data[1] = (byte) ((brightnessValue >> 8) & 0xFF); // High byte

        int wIndex = (UVC_PROCESSING_UNIT_ID << 8) | conn.controlInterface.getId();
        int wValue = (UVC_BRIGHTNESS_CONTROL_SELECTOR << 8);

        Log.i(TAG, "setBrightness: Device " + deviceName + ", Value: " + brightnessValue + 
                     ", Raw: " + data[0] + " " + data[1]);

        int bytesTransferred = conn.connection.controlTransfer(
                UVC_CONTROL_REQUEST_TYPE_SET, UVC_SET_CUR, wValue, wIndex, data, data.length, 1000);

        if (bytesTransferred >= 0) {
            Log.i(TAG, "setBrightness: Success for device " + deviceName);
            return true;
        } else {
            Log.e(TAG, "setBrightness: Control transfer failed for device " + deviceName + 
                         ", Error: " + bytesTransferred);
            return false;
        }
    }
}