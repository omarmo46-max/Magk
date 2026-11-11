package com.magk.app.sandbox.offload

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Review 2026-09-23 — guards eff4ac95f (T-android-a11y-screenshot-mount) for a
 * sub-agent / helper that shares its parent's workspace (T-p2-shared-workspace).
 *
 * For a helper, ExecutionCoordinator binds `/var/magk/attachments` to the
 * PARENT's `magk-sessions/<parent>/attachments` (fsSessionId), but exports
 * `MAGK_CHAT_SESSION_ID=<helper id>` (dispatchSessionId). The a11y handler
 * resolves the screenshot directory from that env id, so the PNG is written to
 * `magk-sessions/<helper>/attachments/screenshots/`, while the returned path
 * `/var/magk/attachments/screenshots/a11y_<ts>.png` is looked up by the
 * helper's shell (parent mount) and by ReadImageTool (called with fsSessionId =
 * parent). Neither finds it: the exact "File not found" eff4ac95f fixed for the
 * main chat comes back for every helper.
 *
 * Invariant: the id the handler resolves `/var/magk/attachments` with must be
 * the id whose attachments directory the guest has mounted.
 */
class Review0923A11yHelperSessionTest {

    private fun src(path: String): String {
        val f = File(path)
        assertTrue("missing ${f.absolutePath}", f.exists())
        return f.readText()
    }

    private val coord by lazy { src("src/main/java/com/magk/app/sandbox/ExecutionCoordinator.kt") }
    private val a11y by lazy { src("src/main/java/com/magk/app/sandbox/offload/AccessibilityOffloadHandler.kt") }
    private val vm by lazy { src("src/main/java/com/magk/app/ui/chat/ChatViewModel.kt") }

    @Test
    fun `premise - helpers read images and mount dirs by the parent's id`() {
        assertTrue(vm.contains("helperConfig?.parentSessionId ?: activeSessionId"))
        assertTrue(vm.contains("ReadImageTool.execute(argsJson, fsSessionId, context)"))
        assertTrue(coord.contains("buildSessionBindMounts(fsSessionId ?: sessionId)"))
    }

    @Test
    fun `BUG the screenshot directory is resolved with the mounted session id`() {
        // Currently: env carries the dispatch (helper) id, and the handler
        // resolves attachments with it. Either the coordinator must export the
        // fs session id (e.g. MAGK_FS_SESSION_ID) and the handler prefer it, or
        // the handler must map a helper id to its parent before resolving.
        val envIsDispatchId = coord.contains("sessionId = sessionId,") &&
            !coord.contains("MAGK_FS_SESSION_ID")
        val handlerKeysOnEnvId = a11y.contains("uiSub(args, request.sessionId)") &&
            a11y.contains("resolveSessionHostPath(it, VAR_MAGK_ATTACHMENTS, context)")
        assertFalse(
            "a helper's a11y screenshot lands in the helper's own attachments dir, " +
                "which neither its shell nor read_image can see",
            envIsDispatchId && handlerKeysOnEnvId,
        )
    }
}
