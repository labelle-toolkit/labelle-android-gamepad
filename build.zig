const std = @import("std");

/// Shared Android gamepad source (labelle-assembler#310 Stage 4 relocation).
///
/// Exposes ONE `android_gamepad` module — the per-device button/axis STATE
/// machine (`android_gamepad_state.zig`, #250) — plus the backend-agnostic
/// InputManager JNI DETECTION glue (`src/android_gamepad_jni.c`, #248). BOTH
/// the sokol and bgfx Android backends depend on this package (each via
/// `.path = "../android_gamepad"` in its `build.zig.zon`), so the gamepad
/// detection/state code lives in exactly one place.
///
/// ## What's shared and why it's backend-agnostic
///
///   * `android_gamepad_state.zig` — pure Zig: the Android-keycode → canonical
///     `GamepadButton`/`GamepadAxis` mapping, the per-device-name axis-routing
///     quirk table, and the mutex-guarded per-device state table. It `@export`s
///     `labelle_android_gamepad_state_added/_removed` (C ABI) for the JNI glue
///     and imports NOTHING from a specific backend. Host-runnable.
///   * `android_gamepad_jni.c` — pure C: walks Android's InputManager via JNI
///     from an `ANativeActivity*` and calls core's exported
///     `labelle_android_on_device_added/_removed` + the state module's
///     `labelle_android_gamepad_state_added/_removed`. It takes the activity
///     pointer as a `const void*`, so it does not care WHICH backend produced
///     it (sokol's `sapp_android_get_native_activity()` or the bgfx shell's
///     stored `app.activity`).
///
/// ## Consumption model
///
/// The C glue needs the NDK sysroot (`jni.h`, `android/native_activity.h`),
/// which is resolved per-target by the CONSUMING backend's build.zig. So this
/// package does NOT compile the C file itself — it exposes the `.c` source path
/// (via `dep.path("src/android_gamepad_jni.c")`) for the consumer to
/// `addCSourceFile` into its own (NDK-sysroot-wired) `input` module, alongside
/// `addImport("android_gamepad", dep.module("android_gamepad"))`. The C TU is
/// `#ifdef __ANDROID__`-gated, so it emits an empty object off Android.
///
/// ## Live hotplug (labelle-assembler#258)
///
/// The C glue exposes, beyond `_init`/`_shutdown`, two extra C-ABI entry points
/// the consumer wires into its `pollEvents`:
///
///   * `void labelle_android_gamepad_poll(void)` — re-enumerates the input
///     devices and emits connect/disconnect deltas (the code-free-APK fallback;
///     works today with no Java).
///   * `int labelle_android_gamepad_listener_active(void)` — 1 when the Java
///     `LabelleInputDeviceListener` shim is attached (deltas arrive live), so
///     the caller can skip/throttle polling.
///
/// The shim lives at `java/com/labelle/LabelleInputDeviceListener.java`
/// (`dep.path("java")`). Loading it needs the APK to ship a `classes.dex` and
/// flip `android:hasCode="true"` — a labelle-cli packaging step that does not
/// exist yet, so until it lands the poll path is the live delta source.
///
/// Standalone (`cd backends/android_gamepad && zig build test`): the host test
/// runs the pure mapping/quirk/state unit tests in `android_gamepad_state.zig`
/// (no JNI, no NDK).
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // ── The shared module consumed by the sokol + bgfx Android backends ──
    // Root is the pure-Zig state machine. The JNI C glue is NOT compiled in
    // here (it needs the consumer's NDK sysroot); the consumer pulls the .c
    // in via `dep.path("src/android_gamepad_jni.c")`. The Android-only
    // `@export`/`extern` symbols in the state module are gated behind
    // `is_android`, so a host/desktop import references none of them.
    _ = b.addModule("android_gamepad", .{
        .root_source_file = b.path("src/android_gamepad_state.zig"),
        .target = target,
        .optimize = optimize,
    });

    // ── Host tests ──────────────────────────────────────────────────────
    // The mapping table, quirk routing, and per-device state machine are pure
    // Zig — host-runnable with no JNI/NDK deps. Pinned to the host so a
    // cross-compiled `zig build test -Dtarget=...` of a consumer never tries
    // to execute a foreign binary.
    const host_target = b.resolveTargetQuery(.{});
    const test_step = b.step("test", "Run android_gamepad unit tests");
    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/android_gamepad_state.zig"),
            .target = host_target,
            .optimize = optimize,
        }),
    });
    test_step.dependOn(&b.addRunArtifact(tests).step);
}
