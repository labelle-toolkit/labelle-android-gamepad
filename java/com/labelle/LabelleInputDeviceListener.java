// Live controller hotplug shim (labelle-assembler#258).
//
// A code-free APK (`android:hasCode="false"`, android.app.NativeActivity) gets
// its controllers enumerated ONCE at startup via InputManager.getInputDeviceIds()
// from the JNI glue (android_gamepad_jni.c, #248). But it cannot register an
// InputManager.InputDeviceListener to receive live connect/disconnect *deltas*,
// because you cannot instantiate a Java listener object from pure JNI without a
// class to instantiate.
//
// This tiny class IS that object. Its three interface methods simply forward the
// Android device id to native callbacks that android_gamepad_jni.c binds with
// RegisterNatives; the native side resolves the InputDevice metadata and drives
// labelle-core's `gamepad_connected` / `gamepad_disconnected` events. The class
// carries no game logic and holds no state.
//
// PACKAGING: shipping this requires the APK to contain a classes.dex and flip
// `android:hasCode="true"` in AndroidManifest.xml — a labelle-cli packaging
// step (javac -> d8) that does not exist yet (see the #258 PR body). Until that
// lands, android_gamepad_jni.c FindClass("com/labelle/LabelleInputDeviceListener")
// returns null and the backend falls back to the polling re-enumeration path
// (labelle_android_gamepad_poll), which delivers the same deltas at poll cadence
// with no Java at all.
package com.labelle;

import android.hardware.input.InputManager;

public final class LabelleInputDeviceListener
        implements InputManager.InputDeviceListener {

    @Override
    public void onInputDeviceAdded(int deviceId) {
        nativeOnDeviceAdded(deviceId);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        nativeOnDeviceRemoved(deviceId);
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        nativeOnDeviceChanged(deviceId);
    }

    // Bound at runtime by android_gamepad_jni.c via RegisterNatives. Instance
    // (non-static) natives, so the JNI entry points receive (JNIEnv*, jobject).
    private native void nativeOnDeviceAdded(int deviceId);

    private native void nativeOnDeviceRemoved(int deviceId);

    private native void nativeOnDeviceChanged(int deviceId);
}
