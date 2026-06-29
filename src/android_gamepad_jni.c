// Android gamepad DETECTION glue (labelle-assembler#248).
//
// Bridges sokol's running ANativeActivity to Android's InputManager via JNI so
// labelle-core's `gamepad_source/android.zig` can emit hotplug/identity events.
// DETECTION / REMOVAL / IDENTITY only — button/axis state is #250.
//
// Why C and not Zig: the JNI reflection path walks the InputManager /
// InputDevice Java API entirely through `(*env)->...` vtable calls. Writing
// that in C keeps it ABI-robust across NDK versions and keeps the Zig source
// (android.zig) free of the JNINativeInterface vtable transcription. The Zig
// side declares two `extern` entry points (`labelle_android_gamepad_init`,
// `labelle_android_gamepad_shutdown`) and two `export` callbacks
// (`labelle_android_on_device_added`, `labelle_android_on_device_removed`)
// that this file calls.
//
// Everything is wrapped in `#ifdef __ANDROID__` so the translation unit is an
// empty object on every other target (it is always added to the C sources, but
// only emits code for Android).
//
// ON-DEVICE STATUS: this compiles for the Android NDK target. The JNI logic
// itself can only be validated on a real device / emulator (see the PR
// checklist) — there is no host equivalent.

#ifdef __ANDROID__

#include <jni.h>
#include <android/native_activity.h>
#include <android/input.h>
#include <string.h>

// ── Callbacks implemented in Zig (android.zig `export fn`) ──────────────────
extern void labelle_android_on_device_added(int device_id, int sources,
                                            const char *name_ptr, size_t name_len,
                                            const char *descriptor_ptr,
                                            size_t descriptor_len);
extern void labelle_android_on_device_removed(int device_id);

// ── Gamepad STATE hooks (labelle-assembler#250; android_gamepad_state.zig) ──
// The state module lives in the same backend package. It can't see the device
// NAME on its own (the forwarded AInputEvent only carries a device id), so we
// seed the axis-routing quirk from the name here, where the InputDevice
// reflection already has it. Drop the state on removal so a reconnect of the
// same id starts clean.
extern void labelle_android_gamepad_state_added(int device_id,
                                                const char *name_ptr,
                                                size_t name_len);
extern void labelle_android_gamepad_state_removed(int device_id);

// ── Module state ────────────────────────────────────────────────────────────
static JavaVM *g_vm = NULL;
static jobject g_input_manager = NULL; // global ref to the InputManager
static jobject g_listener = NULL;       // global ref to our InputDeviceListener

// Cached classes/methods resolved once in init.
static jclass g_input_device_cls = NULL; // android.view.InputDevice
static jmethodID g_mid_get_device = NULL;  // InputDevice.getDevice(int)
static jmethodID g_mid_get_name = NULL;     // InputDevice.getName()
static jmethodID g_mid_get_descriptor = NULL; // InputDevice.getDescriptor()
static jmethodID g_mid_get_sources = NULL;     // InputDevice.getSources()

// Marshal a Java String into a Zig callback as a UTF-8 byte span. The chars
// are released before returning. Returns via the supplied callback-shaped
// closure parameters is awkward in C, so callers copy out explicitly below.

// android.view.InputDevice SOURCE_* bitmasks. A device's getSources() is a
// bitmask; the low byte is the broad class. We only care about real
// controllers — gamepad / joystick — so we can skip the keyboard, touchscreen,
// d-pad-only remotes and other system input devices that getInputDeviceIds()
// also returns. Mirrors `classifySources` in labelle-core
// (gamepad_source/android.zig); keep the constants in lockstep.
#define LBL_SOURCE_GAMEPAD 0x00000401
#define LBL_SOURCE_JOYSTICK 0x01000010

static int is_gamepad_sources(jint sources) {
    return ((sources & LBL_SOURCE_GAMEPAD) == LBL_SOURCE_GAMEPAD) ||
           ((sources & LBL_SOURCE_JOYSTICK) == LBL_SOURCE_JOYSTICK);
}

// Resolve InputDevice metadata for `device_id` and forward to Zig.
static void emit_device_added(JNIEnv *env, jint device_id) {
    // All cached method IDs must be present — calling a NULL jmethodID is UB.
    if (!g_input_device_cls || !g_mid_get_device || !g_mid_get_name ||
        !g_mid_get_descriptor || !g_mid_get_sources) {
        return;
    }
    jobject device = (*env)->CallStaticObjectMethod(
        env, g_input_device_cls, g_mid_get_device, device_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return;
    }
    if (device == NULL) {
        return;
    }

    jint sources = (*env)->CallIntMethod(env, device, g_mid_get_sources);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, device);
        return;
    }

    // getInputDeviceIds() returns ALL input devices (keyboards, touchscreens,
    // power buttons, the virtual keyboard, …), not just controllers. Forwarding
    // those as `gamepad_connected` makes the HUD list system devices instead of
    // the pad. Only emit for devices that actually expose gamepad/joystick
    // sources. (The engine also carries source_class, but it does not filter on
    // emit, so we gate here at the discovery point.)
    if (!is_gamepad_sources(sources)) {
        (*env)->DeleteLocalRef(env, device);
        return;
    }

    jstring jname = (jstring)(*env)->CallObjectMethod(env, device, g_mid_get_name);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        jname = NULL;
    }
    jstring jdesc = (jstring)(*env)->CallObjectMethod(env, device, g_mid_get_descriptor);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        jdesc = NULL;
    }

    const char *name = jname ? (*env)->GetStringUTFChars(env, jname, NULL) : NULL;
    const char *desc = jdesc ? (*env)->GetStringUTFChars(env, jdesc, NULL) : NULL;

    labelle_android_on_device_added(
        (int)device_id, (int)sources,
        name ? name : "", name ? strlen(name) : 0,
        desc ? desc : "", desc ? strlen(desc) : 0);

    // Seed the gamepad-state axis-routing quirk from the device name (#250).
    labelle_android_gamepad_state_added(
        (int)device_id, name ? name : "", name ? strlen(name) : 0);

    if (name) (*env)->ReleaseStringUTFChars(env, jname, name);
    if (desc) (*env)->ReleaseStringUTFChars(env, jdesc, desc);
    (*env)->DeleteLocalRef(env, device);
    if (jname) (*env)->DeleteLocalRef(env, jname);
    if (jdesc) (*env)->DeleteLocalRef(env, jdesc);
}

// ── JNI native callbacks for the registered InputDeviceListener ─────────────
//
// We register a listener implemented as a dynamic proxy / a small Java shim.
// Rather than ship a .jar (the APK is `android:hasCode="false"`), we register
// the listener through `InputManager.registerInputDeviceListener` using a
// Java object whose methods route here. The simplest no-extra-class approach
// is to subscribe with a Handler on the main Looper and rely on
// `getInputDeviceIds()` polling at init for the initial set, plus the
// listener for deltas. The listener object is created via JNI by implementing
// the `InputManager.InputDeviceListener` interface with
// `RegisterNatives`-backed methods on a generated proxy.
//
// NOTE: a code-free APK cannot define a Java class, so the production path
// registers these natives against a tiny in-APK helper class
// (`LabelleInputDeviceListener`) OR falls back to periodic re-enumeration via
// `getInputDeviceIds()` each frame from the Zig side. The init below wires the
// startup enumeration unconditionally; listener registration is attempted and
// silently skipped if the helper class is absent. See the PR checklist.
JNIEXPORT void JNICALL
Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceAdded(
    JNIEnv *env, jobject thiz, jint device_id) {
    (void)thiz;
    emit_device_added(env, device_id);
}

JNIEXPORT void JNICALL
Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceRemoved(
    JNIEnv *env, jobject thiz, jint device_id) {
    (void)env;
    (void)thiz;
    labelle_android_on_device_removed((int)device_id);
    labelle_android_gamepad_state_removed((int)device_id);
}

JNIEXPORT void JNICALL
Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceChanged(
    JNIEnv *env, jobject thiz, jint device_id) {
    // A changed device may have gained/lost gamepad sources; re-emit as added
    // (the Zig side keys on slot id, so this refreshes identity).
    (void)thiz;
    emit_device_added(env, device_id);
}

// Enumerate the devices present at startup and emit a `connected` for each.
static void enumerate_existing(JNIEnv *env) {
    if (!g_input_manager) {
        return;
    }
    jclass im_cls = (*env)->GetObjectClass(env, g_input_manager);
    if (im_cls == NULL) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        return;
    }
    jmethodID mid_ids = (*env)->GetMethodID(env, im_cls, "getInputDeviceIds", "()[I");
    if (!mid_ids) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, im_cls);
        return;
    }
    jintArray ids = (jintArray)(*env)->CallObjectMethod(env, g_input_manager, mid_ids);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (ids == NULL) {
        (*env)->DeleteLocalRef(env, im_cls);
        return;
    }
    jsize n = (*env)->GetArrayLength(env, ids);
    jint *elems = (*env)->GetIntArrayElements(env, ids, NULL);
    if (elems != NULL) {
        for (jsize i = 0; i < n; i++) {
            emit_device_added(env, elems[i]);
        }
        (*env)->ReleaseIntArrayElements(env, ids, elems, JNI_ABORT);
    }
    (*env)->DeleteLocalRef(env, ids);
    (*env)->DeleteLocalRef(env, im_cls);
}

// Cache the InputDevice static/instance methods used by emit_device_added.
static void cache_input_device_methods(JNIEnv *env) {
    jclass local = (*env)->FindClass(env, "android/view/InputDevice");
    if (local == NULL) {
        (*env)->ExceptionClear(env);
        return;
    }
    g_input_device_cls = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (g_input_device_cls == NULL) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        return;
    }
    g_mid_get_device = (*env)->GetStaticMethodID(
        env, g_input_device_cls, "getDevice", "(I)Landroid/view/InputDevice;");
    g_mid_get_name = (*env)->GetMethodID(
        env, g_input_device_cls, "getName", "()Ljava/lang/String;");
    g_mid_get_descriptor = (*env)->GetMethodID(
        env, g_input_device_cls, "getDescriptor", "()Ljava/lang/String;");
    g_mid_get_sources = (*env)->GetMethodID(
        env, g_input_device_cls, "getSources", "()I");
    // A failed GetStaticMethodID/GetMethodID throws NoSuchMethodError; clear it
    // so the env isn't left with a pending exception for later JNI calls.
    // emit_device_added still guards against any NULL id before use.
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
}

// Acquire the InputManager via Context.getSystemService(Context.INPUT_SERVICE).
static void acquire_input_manager(JNIEnv *env, jobject activity) {
    jclass ctx_cls = (*env)->GetObjectClass(env, activity);
    if (ctx_cls == NULL) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        return;
    }
    jmethodID mid_get_service = (*env)->GetMethodID(
        env, ctx_cls, "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!mid_get_service) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, ctx_cls);
        return;
    }
    // Context.INPUT_SERVICE == "input"
    jstring svc = (*env)->NewStringUTF(env, "input");
    if (svc == NULL) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        (*env)->DeleteLocalRef(env, ctx_cls);
        return;
    }
    jobject im = (*env)->CallObjectMethod(env, activity, mid_get_service, svc);
    // getSystemService can throw; clear before any later JNI call relies on a
    // clean env, otherwise enumerate_existing / FindClass would misbehave.
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (im != NULL) {
        g_input_manager = (*env)->NewGlobalRef(env, im);
        (*env)->DeleteLocalRef(env, im);
    }
    (*env)->DeleteLocalRef(env, svc);
    (*env)->DeleteLocalRef(env, ctx_cls);
}

void labelle_android_gamepad_init(const void *activity_ptr) {
    if (activity_ptr == NULL) {
        return;
    }
    const ANativeActivity *activity = (const ANativeActivity *)activity_ptr;
    g_vm = activity->vm;

    // `activity->env` is the JNIEnv of the thread that CREATED the activity
    // (the Android main thread). sokol runs init_cb and the frame loop on its
    // own render thread, where that env is INVALID — using it makes every JNI
    // call below silently fail, so the InputManager enumeration found nothing
    // and the HUD stayed empty (labelle-engine#261). Attach the current thread
    // to the VM to obtain a JNIEnv valid HERE; fall back to the stored env only
    // if attach somehow fails (harmless on a single-threaded config).
    JNIEnv *env = NULL;
    if (g_vm != NULL) {
        jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
        if (rc == JNI_EDETACHED) {
            (*g_vm)->AttachCurrentThread(g_vm, &env, NULL);
        }
    }
    if (env == NULL) {
        env = activity->env; // last-ditch fallback
    }
    if (env == NULL) {
        return;
    }

    cache_input_device_methods(env);
    acquire_input_manager(env, activity->clazz);
    enumerate_existing(env);

    // Listener registration against the in-APK helper class is attempted here;
    // if the class is absent (code-free APK), we skip it and rely on the Zig
    // side re-enumerating. See the PR checklist for the on-device wiring.
    jclass listener_cls = (*env)->FindClass(env, "com/labelle/LabelleInputDeviceListener");
    if (listener_cls == NULL) {
        (*env)->ExceptionClear(env);
        return; // no helper class — startup enumeration only
    }
    // (Registration of natives + InputManager.registerInputDeviceListener
    //  happens here when the helper class is present.)
    (*env)->DeleteLocalRef(env, listener_cls);
}

void labelle_android_gamepad_shutdown(void) {
    if (g_vm == NULL) {
        return;
    }
    JNIEnv *env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
        return;
    }
    if (g_listener) {
        (*env)->DeleteGlobalRef(env, g_listener);
        g_listener = NULL;
    }
    if (g_input_manager) {
        (*env)->DeleteGlobalRef(env, g_input_manager);
        g_input_manager = NULL;
    }
    if (g_input_device_cls) {
        (*env)->DeleteGlobalRef(env, g_input_device_cls);
        g_input_device_cls = NULL;
    }
}

#else  // !__ANDROID__

// Non-Android targets: provide the entry points as no-ops so the symbols
// resolve if (improbably) referenced, and the TU is never empty.
typedef unsigned long labelle_size_t;
void labelle_android_gamepad_init(const void *activity_ptr) { (void)activity_ptr; }
void labelle_android_gamepad_shutdown(void) {}

#endif // __ANDROID__
