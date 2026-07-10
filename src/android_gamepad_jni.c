// Android gamepad DETECTION glue (labelle-assembler#248, live deltas #258).
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
// ── Three discovery paths, one presence set ────────────────────────────────
// Live connect/disconnect deltas after launch are delivered by whichever of
// these paths is available:
//
//   1. Startup enumeration — `getInputDeviceIds()` at init. Always runs. Sees
//      only what is already connected.
//   2. Java InputDeviceListener (#258) — the in-APK `LabelleInputDeviceListener`
//      shim, registered via `registerInputDeviceListener`. Delivers deltas the
//      instant they happen. Requires the APK to ship the class (classes.dex +
//      `android:hasCode="true"`), which needs a labelle-cli packaging step that
//      does not exist yet — so this path is attempted and silently skipped when
//      the class is absent.
//   3. Polling re-enumeration (#258) — `labelle_android_gamepad_poll()`, called
//      each frame (or throttled) from the Zig `pollEvents`. Diffs the current
//      device set against what we last reported. Works TODAY in a code-free APK
//      with no Java, at poll cadence. `labelle_android_gamepad_listener_active()`
//      lets the caller skip/throttle polling when path 2 is live.
//
// All three funnel through a shared `g_known_ids` set so a device is reported
// `connected` / `disconnected` exactly once regardless of which path observed
// the change.
//
// Everything is wrapped in `#ifdef __ANDROID__` so the translation unit is an
// empty object on every other target (it is always added to the C sources, but
// only emits code for Android).
//
// ON-DEVICE STATUS: this compiles/cross-compiles for the Android NDK target.
// The JNI logic itself — RegisterNatives binding, registerInputDeviceListener
// attachment, and the poll diff against real hotplug — can only be validated on
// a real device / emulator (see the PR checklist and labelle-engine#261).

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
static jobject g_handler = NULL;         // global ref to the main-Looper Handler
static int g_listener_active = 0;        // 1 once registerInputDeviceListener won

// Cached classes/methods resolved once in init.
static jclass g_input_device_cls = NULL; // android.view.InputDevice
static jmethodID g_mid_get_device = NULL;  // InputDevice.getDevice(int)
static jmethodID g_mid_get_name = NULL;     // InputDevice.getName()
static jmethodID g_mid_get_descriptor = NULL; // InputDevice.getDescriptor()
static jmethodID g_mid_get_sources = NULL;     // InputDevice.getSources()

// ── Presence set shared by all three discovery paths ────────────────────────
// The gamepad device ids we have already reported as connected. Startup
// enumeration, the Java listener, and the polling fallback all reconcile against
// this so no `connected`/`disconnected` is emitted twice for one id. Android
// exposes only a handful of input devices at once, so a small fixed array is
// ample (kept larger than MAX_DEVICES to tolerate transient churn).
#define LBL_MAX_TRACKED 32
static jint g_known_ids[LBL_MAX_TRACKED];
static int g_known_count = 0;

static int lbl_is_known(jint id) {
    for (int i = 0; i < g_known_count; i++) {
        if (g_known_ids[i] == id) return 1;
    }
    return 0;
}

static void lbl_add_known(jint id) {
    if (lbl_is_known(id)) return;
    if (g_known_count < LBL_MAX_TRACKED) {
        g_known_ids[g_known_count++] = id;
    }
}

static void lbl_remove_known(jint id) {
    for (int i = 0; i < g_known_count; i++) {
        if (g_known_ids[i] == id) {
            g_known_ids[i] = g_known_ids[g_known_count - 1];
            g_known_count--;
            return;
        }
    }
}

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

// Resolve InputDevice metadata for `device_id`; if it is a gamepad we have not
// reported yet, emit a `connected` and seed its state. Returns 1 if the device
// exposes gamepad/joystick sources (whether or not it was newly added — an
// already-known gamepad also returns 1), 0 if it is not a controller or could
// not be resolved. Idempotent: calling it repeatedly for a live pad emits at
// most one add.
static int probe_and_emit_add(JNIEnv *env, jint device_id) {
    // All cached method IDs must be present — calling a NULL jmethodID is UB.
    if (!g_input_device_cls || !g_mid_get_device || !g_mid_get_name ||
        !g_mid_get_descriptor || !g_mid_get_sources) {
        return 0;
    }
    jobject device = (*env)->CallStaticObjectMethod(
        env, g_input_device_cls, g_mid_get_device, device_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    if (device == NULL) {
        return 0;
    }

    jint sources = (*env)->CallIntMethod(env, device, g_mid_get_sources);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, device);
        return 0;
    }

    // getInputDeviceIds() returns ALL input devices (keyboards, touchscreens,
    // power buttons, the virtual keyboard, …), not just controllers. Forwarding
    // those as `gamepad_connected` makes the HUD list system devices instead of
    // the pad. Only emit for devices that actually expose gamepad/joystick
    // sources. (The engine also carries source_class, but it does not filter on
    // emit, so we gate here at the discovery point.)
    if (!is_gamepad_sources(sources)) {
        (*env)->DeleteLocalRef(env, device);
        return 0;
    }

    // Already reported — nothing to emit, but it IS a gamepad, so report that so
    // the "changed" path does not mistake it for a removal.
    if (lbl_is_known(device_id)) {
        (*env)->DeleteLocalRef(env, device);
        return 1;
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

    lbl_add_known(device_id);

    if (name) (*env)->ReleaseStringUTFChars(env, jname, name);
    if (desc) (*env)->ReleaseStringUTFChars(env, jdesc, desc);
    (*env)->DeleteLocalRef(env, device);
    if (jname) (*env)->DeleteLocalRef(env, jname);
    if (jdesc) (*env)->DeleteLocalRef(env, jdesc);
    return 1;
}

// Emit a `disconnected` for `device_id` if we had reported it. No JNI metadata
// is needed (the id is enough), so this is safe from any thread/path.
static void emit_removed(jint device_id) {
    if (!lbl_is_known(device_id)) {
        return;
    }
    lbl_remove_known(device_id);
    labelle_android_on_device_removed((int)device_id);
    labelle_android_gamepad_state_removed((int)device_id);
}

// ── JNI native callbacks for the registered InputDeviceListener (#258) ──────
//
// Bound to `LabelleInputDeviceListener`'s three `native` methods via
// RegisterNatives in register_listener(). They run on the main Looper (the
// Handler we register with), NOT sokol's render thread. Each just reconciles
// the one id through the shared presence set.
JNIEXPORT void JNICALL
Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceAdded(
    JNIEnv *env, jobject thiz, jint device_id) {
    (void)thiz;
    probe_and_emit_add(env, device_id);
}

JNIEXPORT void JNICALL
Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceRemoved(
    JNIEnv *env, jobject thiz, jint device_id) {
    (void)env;
    (void)thiz;
    emit_removed(device_id);
}

JNIEXPORT void JNICALL
Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceChanged(
    JNIEnv *env, jobject thiz, jint device_id) {
    // A changed device may have gained OR lost gamepad sources. Reconcile: if it
    // is a gamepad, ensure it is reported (probe_and_emit_add adds it if new); if
    // it is no longer a gamepad but we had reported it, treat the change as a
    // removal.
    (void)thiz;
    if (!probe_and_emit_add(env, device_id)) {
        emit_removed(device_id);
    }
}

// Obtain a JNIEnv valid on the CURRENT thread. sokol runs init_cb / the frame
// loop on its own render thread, where the activity's stored env is INVALID —
// using it makes every JNI call silently fail (labelle-engine#261). Attach to
// the VM to get a thread-correct env.
static JNIEnv *lbl_get_env(void) {
    if (g_vm == NULL) {
        return NULL;
    }
    JNIEnv *env = NULL;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        (*g_vm)->AttachCurrentThread(g_vm, &env, NULL);
    }
    return env;
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
            probe_and_emit_add(env, elems[i]);
        }
        (*env)->ReleaseIntArrayElements(env, ids, elems, JNI_ABORT);
    }
    (*env)->DeleteLocalRef(env, ids);
    (*env)->DeleteLocalRef(env, im_cls);
}

// Cache the InputDevice static/instance methods used by probe_and_emit_add.
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
    // probe_and_emit_add still guards against any NULL id before use.
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

// Build a Handler bound to the main Looper, so registerInputDeviceListener
// delivers callbacks on the main thread (the render thread has no Looper).
// Returns a LOCAL ref (caller promotes/deletes), or NULL on failure — in which
// case the caller passes NULL to registerInputDeviceListener (Android then uses
// the calling thread's Looper, which may fail; the poll path remains as backup).
static jobject make_main_handler(JNIEnv *env) {
    jclass looper_cls = (*env)->FindClass(env, "android/os/Looper");
    if (looper_cls == NULL) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    jmethodID mid_main = (*env)->GetStaticMethodID(
        env, looper_cls, "getMainLooper", "()Landroid/os/Looper;");
    jobject looper = mid_main
        ? (*env)->CallStaticObjectMethod(env, looper_cls, mid_main)
        : NULL;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        looper = NULL;
    }
    (*env)->DeleteLocalRef(env, looper_cls);
    if (looper == NULL) {
        return NULL;
    }

    jclass handler_cls = (*env)->FindClass(env, "android/os/Handler");
    if (handler_cls == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, looper);
        return NULL;
    }
    jmethodID ctor = (*env)->GetMethodID(
        env, handler_cls, "<init>", "(Landroid/os/Looper;)V");
    jobject handler = ctor
        ? (*env)->NewObject(env, handler_cls, ctor, looper)
        : NULL;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        handler = NULL;
    }
    (*env)->DeleteLocalRef(env, handler_cls);
    (*env)->DeleteLocalRef(env, looper);
    return handler;
}

// Register the in-APK LabelleInputDeviceListener shim (#258). No-op (leaving the
// poll fallback as the only live path) when the class is absent — which is the
// case until labelle-cli ships the classes.dex packaging step. Sets
// g_listener_active on success.
static void register_listener(JNIEnv *env) {
    jclass listener_cls = (*env)->FindClass(
        env, "com/labelle/LabelleInputDeviceListener");
    if (listener_cls == NULL) {
        (*env)->ExceptionClear(env);
        return; // no helper class — startup enumeration + polling only
    }

    static const JNINativeMethod methods[] = {
        {"nativeOnDeviceAdded", "(I)V",
         (void *)Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceAdded},
        {"nativeOnDeviceRemoved", "(I)V",
         (void *)Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceRemoved},
        {"nativeOnDeviceChanged", "(I)V",
         (void *)Java_com_labelle_LabelleInputDeviceListener_nativeOnDeviceChanged},
    };
    if ((*env)->RegisterNatives(env, listener_cls, methods, 3) < 0) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        (*env)->DeleteLocalRef(env, listener_cls);
        return;
    }

    jmethodID ctor = (*env)->GetMethodID(env, listener_cls, "<init>", "()V");
    if (!ctor) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, listener_cls);
        return;
    }
    jobject listener = (*env)->NewObject(env, listener_cls, ctor);
    if (listener == NULL || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, listener_cls);
        return;
    }
    g_listener = (*env)->NewGlobalRef(env, listener);
    (*env)->DeleteLocalRef(env, listener);

    jobject handler = make_main_handler(env); // may be NULL

    jclass im_cls = (*env)->GetObjectClass(env, g_input_manager);
    jmethodID mid_reg = im_cls
        ? (*env)->GetMethodID(
              env, im_cls, "registerInputDeviceListener",
              "(Landroid/hardware/input/InputManager$InputDeviceListener;"
              "Landroid/os/Handler;)V")
        : NULL;
    if (mid_reg) {
        (*env)->CallVoidMethod(env, g_input_manager, mid_reg, g_listener, handler);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        } else {
            g_listener_active = 1;
        }
    } else if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }

    if (handler) {
        g_handler = (*env)->NewGlobalRef(env, handler);
        (*env)->DeleteLocalRef(env, handler);
    }
    if (im_cls) {
        (*env)->DeleteLocalRef(env, im_cls);
    }
    (*env)->DeleteLocalRef(env, listener_cls);
}

void labelle_android_gamepad_init(const void *activity_ptr) {
    if (activity_ptr == NULL) {
        return;
    }
    const ANativeActivity *activity = (const ANativeActivity *)activity_ptr;
    g_vm = activity->vm;

    // Attach the current thread to the VM to obtain a JNIEnv valid HERE (see
    // lbl_get_env / labelle-engine#261); fall back to the stored env only if
    // attach somehow fails (harmless on a single-threaded config).
    JNIEnv *env = lbl_get_env();
    if (env == NULL) {
        env = activity->env; // last-ditch fallback
    }
    if (env == NULL) {
        return;
    }

    cache_input_device_methods(env);
    acquire_input_manager(env, activity->clazz);
    enumerate_existing(env);

    // Attach the Java listener for live deltas when the shim class is present;
    // otherwise the polling fallback (labelle_android_gamepad_poll) carries them.
    register_listener(env);
}

// Polling re-enumeration fallback (#258). Re-reads getInputDeviceIds() and
// reconciles it against the reported set, emitting connect/disconnect deltas.
// Idempotent and cheap enough to call each frame; the caller may throttle it
// (or skip it when labelle_android_gamepad_listener_active() is true). Safe to
// call before init (no-op) and from the render thread (attaches its own env).
void labelle_android_gamepad_poll(void) {
    if (g_input_manager == NULL) {
        return;
    }
    JNIEnv *env = lbl_get_env();
    if (env == NULL) {
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
        (*env)->DeleteLocalRef(env, im_cls);
        return;
    }
    if (ids == NULL) {
        (*env)->DeleteLocalRef(env, im_cls);
        return;
    }
    jsize n = (*env)->GetArrayLength(env, ids);
    jint *elems = (*env)->GetIntArrayElements(env, ids, NULL);
    if (elems == NULL) {
        (*env)->DeleteLocalRef(env, ids);
        (*env)->DeleteLocalRef(env, im_cls);
        return;
    }

    // Removal sweep: any known id absent from the fresh enumeration is gone. A
    // disconnected controller drops out of getInputDeviceIds(). Collect first,
    // then emit — emit_removed() mutates g_known_ids, so we must not iterate it
    // while it changes.
    jint to_remove[LBL_MAX_TRACKED];
    int rm = 0;
    for (int i = 0; i < g_known_count; i++) {
        jint kid = g_known_ids[i];
        int still = 0;
        for (jsize j = 0; j < n; j++) {
            if (elems[j] == kid) {
                still = 1;
                break;
            }
        }
        if (!still && rm < LBL_MAX_TRACKED) {
            to_remove[rm++] = kid;
        }
    }
    for (int i = 0; i < rm; i++) {
        emit_removed(to_remove[i]);
    }

    // Addition sweep: probe every current id; probe_and_emit_add() no-ops for
    // non-gamepads and already-known pads, so only genuine new pads emit.
    for (jsize j = 0; j < n; j++) {
        probe_and_emit_add(env, elems[j]);
    }

    (*env)->ReleaseIntArrayElements(env, ids, elems, JNI_ABORT);
    (*env)->DeleteLocalRef(env, ids);
    (*env)->DeleteLocalRef(env, im_cls);
}

// 1 when the Java InputDeviceListener is attached and delivering live deltas, so
// the caller can skip or throttle the polling fallback. 0 otherwise (poll each
// frame to catch hotplug).
int labelle_android_gamepad_listener_active(void) {
    return g_listener_active;
}

void labelle_android_gamepad_shutdown(void) {
    if (g_vm == NULL) {
        return;
    }
    JNIEnv *env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
        return;
    }
    if (g_listener_active && g_input_manager && g_listener) {
        jclass im_cls = (*env)->GetObjectClass(env, g_input_manager);
        jmethodID mid_unreg = im_cls
            ? (*env)->GetMethodID(
                  env, im_cls, "unregisterInputDeviceListener",
                  "(Landroid/hardware/input/InputManager$InputDeviceListener;)V")
            : NULL;
        if (mid_unreg) {
            (*env)->CallVoidMethod(env, g_input_manager, mid_unreg, g_listener);
        }
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        if (im_cls) {
            (*env)->DeleteLocalRef(env, im_cls);
        }
        g_listener_active = 0;
    }
    if (g_handler) {
        (*env)->DeleteGlobalRef(env, g_handler);
        g_handler = NULL;
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
    g_known_count = 0;
}

#else  // !__ANDROID__

// Non-Android targets: provide the entry points as no-ops so the symbols
// resolve if (improbably) referenced, and the TU is never empty.
typedef unsigned long labelle_size_t;
void labelle_android_gamepad_init(const void *activity_ptr) { (void)activity_ptr; }
void labelle_android_gamepad_poll(void) {}
int labelle_android_gamepad_listener_active(void) { return 0; }
void labelle_android_gamepad_shutdown(void) {}

#endif // __ANDROID__
