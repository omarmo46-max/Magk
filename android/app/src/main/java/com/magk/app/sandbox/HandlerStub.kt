package com.magk.app.sandbox

/**
 * [T-native-offload-bind] The `/usr/local/bin/<handler>` stub every offload CLI
 * resolves to on PATH.
 *
 * On the normal path the stub never runs: proot's native_offload extension
 * intercepts the execve and serves the command from the app. It runs only
 * when that did not happen - the app could not bind its offload socket (the
 * launcher then leaves `--native-offload` off), or the offload call failed.
 * It used to be `exit 0`, so a broken offload looked like a successful
 * command with no output; now it says what is wrong and exits non-zero.
 */
internal object HandlerStub {
    /** What every earlier build wrote; upgraded in place on boot. */
    const val LEGACY_CONTENT = "#!/bin/sh\nexit 0\n"

    /** sysexits EX_UNAVAILABLE. */
    const val EXIT_UNAVAILABLE = 69

    /** Guest path of the offload client (installed by PRootKernel from libmagk_offload.so). */
    const val CLIENT_GUEST_PATH = "/usr/local/libexec/magk-offload"

    /** Native library the client ships as (jniLibs/arm64-v8a). */
    const val CLIENT_LIB_NAME = "libmagk_offload.so"

    /**
     * The stub that was written before the guest-side client existed: it only
     * reported the CLI unavailable. Upgraded in place, like [LEGACY_CONTENT].
     */
    fun previousContent(name: String): String =
        "#!/bin/sh\n" +
            "echo \"$name: unavailable - Magk could not start its native offload service. " +
            "Restart the Magk app and try again.\" >&2\n" +
            "exit $EXIT_UNAVAILABLE\n"

    /**
     * With a patched proot (`--native-offload`) the execve of this file is
     * intercepted and the stub never runs. With a stock proot it does run, and
     * hands the call to the guest-side client, which talks to the app's
     * NativeOffloadServer itself and prints the handler's output.
     */
    fun content(name: String): String =
        "#!/bin/sh\n" +
            "if [ -x $CLIENT_GUEST_PATH ]; then\n" +
            "    exec $CLIENT_GUEST_PATH \"$name\" \"\$@\"\n" +
            "fi\n" +
            "echo \"$name: unavailable - the Magk offload client is missing from this build. " +
            "Update or reinstall Magk.\" >&2\n" +
            "exit $EXIT_UNAVAILABLE\n"
}
