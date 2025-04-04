package com.android.cambridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.UserHandle;
import android.util.Log;

import java.util.HashMap;

/**
 * System service that listens for UVC camera connections and manages
 * the bridge to the Android Camera HAL.
 */
public class CamBridgeService extends Service {
    private static final String TAG = "CamBridgeService";
    private static final String NOTIFICATION_CHANNEL_ID = "com.android.cambridge.channel";
    private static final int NOTIFICATION_ID = 1;
    
    private UsbManager mUsbManager;
    private UvcCameraManager mUvcCameraManager;
    private VirtualCameraManager mVirtualCameraManager;
    private Handler mHandler;
    private boolean mIsRunning = false;
    
    // BroadcastReceiver for USB events
    private final BroadcastReceiver mUsbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (action == null) return;
            
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (device == null) return;
            
            switch (action) {
                case UsbManager.ACTION_USB_DEVICE_ATTACHED:
                    Log.i(TAG, "USB device attached: " + device.getDeviceName());
                    handleUsbDeviceAttached(device);
                    break;
                case UsbManager.ACTION_USB_DEVICE_DETACHED:
                    Log.i(TAG, "USB device detached: " + device.getDeviceName());
                    handleUsbDeviceDetached(device);
                    break;
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "Service onCreate");
        
        // Initialize managers
        mUsbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        mUvcCameraManager = new UvcCameraManager(this);
        mVirtualCameraManager = new VirtualCameraManager(this);
        mHandler = new Handler(Looper.getMainLooper());
        
        // Register for USB events
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        registerReceiver(mUsbReceiver, filter);
        
        // Check for already connected devices
        checkForExistingUsbCameras();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.i(TAG, "Service onStartCommand");
        
        if (!mIsRunning) {
            mIsRunning = true;
            createNotificationChannel();
            startForeground(NOTIFICATION_ID, createNotification());
        }
        
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "Service onDestroy");
        unregisterReceiver(mUsbReceiver);
        mUvcCameraManager.closeAllCameras();
        mVirtualCameraManager.stopVirtualCamera();
        super.onDestroy();
    }

    /**
     * Checks for existing UVC cameras that may already be connected
     */
    private void checkForExistingUsbCameras() {
        HashMap<String, UsbDevice> deviceList = mUsbManager.getDeviceList();
        Log.i(TAG, "Found " + deviceList.size() + " USB devices");
        
        for (UsbDevice device : deviceList.values()) {
            if (UvcCameraManager.isUvcCamera(device)) {
                Log.i(TAG, "Found existing UVC camera: " + device.getDeviceName());
                handleUsbDeviceAttached(device);
            }
        }
    }

    /**
     * Handles the attachment of a USB device
     */
    private void handleUsbDeviceAttached(UsbDevice device) {
        if (!UvcCameraManager.isUvcCamera(device)) {
            Log.d(TAG, "Not a UVC camera, ignoring: " + device.getDeviceName());
            return;
        }
        
        mHandler.post(() -> {
            if (mUvcCameraManager.openCamera(device)) {
                // Successfully opened the camera, now start streaming
                startBridging(device);
            }
        });
    }

    /**
     * Handles the detachment of a USB device
     */
    private void handleUsbDeviceDetached(UsbDevice device) {
        if (!UvcCameraManager.isUvcCamera(device)) {
            return;
        }
        
        mHandler.post(() -> {
            mUvcCameraManager.closeCamera(device);
            mVirtualCameraManager.stopVirtualCamera();
        });
    }

    /**
     * Starts the bridging between UVC camera and virtual camera
     */
    private void startBridging(UsbDevice device) {
        // Initialize the virtual camera with the UVC camera's capabilities
        UvcCameraInfo cameraInfo = mUvcCameraManager.getCameraInfo(device);
        if (cameraInfo == null) {
            Log.e(TAG, "Failed to get camera info");
            return;
        }
        
        // Register the virtual camera with the system
        if (mVirtualCameraManager.registerVirtualCamera(cameraInfo)) {
            // Start the data flow from UVC to virtual camera
            mUvcCameraManager.setFrameCallback(device, frame -> {
                // Process and forward frame to virtual camera
                mVirtualCameraManager.pushFrame(frame);
            });
            
            // Start streaming from the UVC camera
            mUvcCameraManager.startStreaming(device);
        }
    }

    /**
     * Creates notification channel for foreground service
     */
    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                "UVC Camera Bridge",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Handles USB camera connections");
        
        NotificationManager notificationManager = getSystemService(NotificationManager.class);
        if (notificationManager != null) {
            notificationManager.createNotificationChannel(channel);
        }
    }

    /**
     * Creates notification for foreground service
     */
    private Notification createNotification() {
        return new Notification.Builder(this, NOTIFICATION_CHANNEL_ID)
                .setContentTitle(getString(R.string.notification_title))
                .setContentText(getString(R.string.notification_message))
                .setSmallIcon(android.R.drawable.ic_menu_camera)
                .build();
    }
} 