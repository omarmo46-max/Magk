package com.magk.app.sandbox

import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit

/**
 * Which optional options the bundled PRoot understands.
 *
 * Magk was designed around a patched PRoot that adds `--native-offload` and
 * `--fake-netlink`. A PRoot without them rejects unknown options with
 * "unknown option", so passing them unconditionally kept every shell from
 * starting. The launchers now ask here first and only pass what the binary
 * lists in `--help`.
 *
 * deps/build_proot.sh applies deps/proot-patches, which adds `--fake-netlink`
 * (rtnetlink answered inside PRoot), so the bundled binary supports it.
 * Without `--native-offload` the `magk-*` tools still work: the handler stubs
 * run the guest-side `magk-offload` client, which talks to the same
 * [NativeOffloadServer] over its abstract socket (see HandlerStub).
 */
internal object ProotCapabilities {
    const val NATIVE_OFFLOAD = "--native-offload"
    const val FAKE_NETLINK = "--fake-netlink"

    private val helpCache = ConcurrentHashMap<String, String>()

    /** True when [proot]'s `--help` output mentions [flag]. Asked once per binary. */
    fun supports(proot: File, flag: String): Boolean =
        helpText(proot).contains(flag)

    private fun helpText(proot: File): String =
        helpCache.getOrPut(proot.absolutePath) {
            runCatching {
                val p = ProcessBuilder(proot.absolutePath, "--help").redirectErrorStream(true).start()
                val text = p.inputStream.bufferedReader().readText()
                p.waitFor(3, TimeUnit.SECONDS)
                text
            }.getOrDefault("")
        }
}
