package com.android.cambridge;

import android.util.Size;
import java.util.List;

/**
 * Stores information about a UVC camera's capabilities.
 */
public class UvcCameraInfo {
    // Camera identification
    public String deviceId;
    public String productName;
    
    // Camera capabilities
    public List<Size> supportedResolutions;
    public List<Float> supportedFrameRates;
    
    // Current settings
    public Size currentResolution;
    public float currentFrameRate;
    
    /**
     * Returns the best available resolution (highest).
     * 
     * @return The highest resolution supported by the camera
     */
    public Size getBestResolution() {
        if (supportedResolutions == null || supportedResolutions.isEmpty()) {
            return new Size(640, 480); // Default fallback
        }
        
        Size best = supportedResolutions.get(0);
        for (Size size : supportedResolutions) {
            if (size.getWidth() * size.getHeight() > best.getWidth() * best.getHeight()) {
                best = size;
            }
        }
        
        return best;
    }
    
    /**
     * Returns the best available frame rate (highest).
     * 
     * @return The highest frame rate supported by the camera
     */
    public float getBestFrameRate() {
        if (supportedFrameRates == null || supportedFrameRates.isEmpty()) {
            return 30.0f; // Default fallback
        }
        
        float best = supportedFrameRates.get(0);
        for (float rate : supportedFrameRates) {
            if (rate > best) {
                best = rate;
            }
        }
        
        return best;
    }
    
    /**
     * Gets a unique camera ID for use with the Camera HAL.
     * 
     * @return A unique identifier for this camera
     */
    public String getHalCameraId() {
        // Create a unique ID for the Camera HAL
        // Format: uvc_<device_id_hash>
        return "uvc_" + Integer.toHexString(deviceId.hashCode());
    }
} 