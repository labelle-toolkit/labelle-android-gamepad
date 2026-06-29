# labelle-android-gamepad

Shared Android gamepad source for [labelle](https://github.com/labelle-toolkit)
backends — the per-device state machine (#250) + InputManager JNI detection glue
(#248). Extracted from the assembler's in-tree `backends/android_gamepad/` so
out-of-tree backends (e.g. `labelle-bgfx`) can depend on it as a versioned
package instead of a vendored copy (labelle-assembler#386). Off Android nothing
here is referenced. No dependencies.
