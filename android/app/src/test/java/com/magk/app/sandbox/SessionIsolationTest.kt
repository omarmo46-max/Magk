package com.magk.app.sandbox

import java.io.File
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * [T-android-session-private-mounts] Per-session `/var/magk` directories go
 * only into the owning shell's argv; the global mount table never holds them.
 *
 * The regression this guards: every shell build wrote its session's dirs into
 * the process-global PRootKernel.bindMounts, so the last session to run a
 * command decided what `/var/magk/workspace` meant for the terminal and for
 * every path lookup without a session.
 */
class SessionIsolationTest {

    private val filesDir: File = Files.createTempDirectory("magk-files").toFile()

    @After
    fun cleanUp() {
        filesDir.deleteRecursively()
        for (sub in SessionMounts.SESSION_SUBDIRS) PRootKernel.bindMounts.remove("/var/magk/$sub")
    }

    // ── SessionMounts ─────────────────────────────────────────────────────

    @Test
    fun `a session gets its own four dirs plus the four global ones, all created`() {
        val built = SessionMounts.build(filesDir, "A")
        assertEquals(emptyList<String>(), built.skipped)
        for (sub in SessionMounts.SESSION_SUBDIRS) {
            val host = File(built.mounts.getValue("/var/magk/$sub"))
            assertEquals(File(filesDir, "magk-sessions/A/$sub").absolutePath, host.absolutePath)
            assertTrue("$sub must exist before it is bound", host.isDirectory)
        }
        for (sub in SessionMounts.GLOBAL_SUBDIRS) {
            assertEquals(File(filesDir, "magk-global/$sub").absolutePath, built.mounts["/var/magk/$sub"])
        }
    }

    @Test
    fun `two sessions never share a per-session dir, and do share the global ones`() {
        val a = SessionMounts.build(filesDir, "A").mounts
        val b = SessionMounts.build(filesDir, "B").mounts
        for (sub in SessionMounts.SESSION_SUBDIRS) {
            assertNotEquals("/var/magk/$sub", a["/var/magk/$sub"], b["/var/magk/$sub"])
        }
        for (sub in SessionMounts.GLOBAL_SUBDIRS) {
            assertEquals(a["/var/magk/$sub"], b["/var/magk/$sub"])
        }
    }

    @Test
    fun `building mounts leaves the global table without per-session entries`() {
        SessionMounts.build(filesDir, "A")
        SessionMounts.build(filesDir, "B")
        assertTrue(PRootKernel.bindMounts.keys.none { PRootKernel.isPerSessionPath(it) })
    }

    @Test
    fun `a shell with no session gets the global dirs only`() {
        val built = SessionMounts.build(filesDir, null)
        assertTrue(built.mounts.keys.none { PRootKernel.isPerSessionPath(it) })
        assertEquals(SessionMounts.GLOBAL_SUBDIRS.map { "/var/magk/$it" }, built.mounts.keys.toList())
    }

    @Test
    fun `a dir that cannot be created is skipped and reported, not bound`() {
        // A regular file where the directory should be makes mkdirs fail.
        File(filesDir, "magk-sessions/A").mkdirs()
        File(filesDir, "magk-sessions/A/workspace").writeText("not a directory")
        val built = SessionMounts.build(filesDir, "A")
        assertEquals(listOf("/var/magk/workspace"), built.skipped)
        assertFalse(built.mounts.containsKey("/var/magk/workspace"))
        assertTrue("the others are still bound", built.mounts.containsKey("/var/magk/attachments"))
    }

    @Test
    fun `external mounts pass through unchanged`() {
        val ext = File(filesDir, "sd/photos").apply { mkdirs() }
        val built = SessionMounts.build(filesDir, "A", listOf("/var/magk/mounts/photos" to ext.absolutePath))
        assertEquals(ext.absolutePath, built.mounts["/var/magk/mounts/photos"])
    }

    @Test
    fun `argv pairs are -b host colon guest in bind order`() {
        val args = SessionMounts.toProotArgs(linkedMapOf("/var/magk/workspace" to "/h/ws", "/var/magk/shared" to "/h/sh"))
        assertEquals(listOf("-b", "/h/ws:/var/magk/workspace", "-b", "/h/sh:/var/magk/shared"), args)
    }

    // ── PRootKernel global table ──────────────────────────────────────────

    @Test
    fun `the global table refuses per-session mounts and accepts global ones`() {
        PRootKernel.addBindMount("/var/magk/workspace", "/h/ws")
        PRootKernel.addBindMount("/var/magk/attachments/sub", "/h/at")
        assertNull(PRootKernel.bindMounts["/var/magk/workspace"])
        assertNull(PRootKernel.bindMounts["/var/magk/attachments/sub"])
        PRootKernel.addBindMount("/var/magk/test-global-probe", "/h/g")
        assertEquals("/h/g", PRootKernel.bindMounts.remove("/var/magk/test-global-probe"))
    }

    @Test
    fun `per-session path classification`() {
        for (p in listOf("/var/magk/workspace", "/var/magk/attachments/a.png", "/var/magk/offloads/x", "/var/magk/browser/s.jpg")) {
            assertTrue(p, PRootKernel.isPerSessionPath(p))
        }
        for (p in listOf("/var/magk/shared/a", "/var/magk/memory", "/var/magk/mounts/x", "/var/magk/workspace2", "/root/workspace")) {
            assertFalse(p, PRootKernel.isPerSessionPath(p))
        }
    }

    @Test
    fun `a lookup with no session gets the legacy most-recent-session answer, explicitly`() {
        PRootKernel.noteLegacySession(filesDir, "B")
        val before = PRootKernel.legacyResolveCount
        val f = PRootKernel.resolveHostPath("/var/magk/attachments/cat.png")
        assertEquals(File(filesDir, "magk-sessions/B/attachments/cat.png").absolutePath, f?.absolutePath)
        assertEquals(before + 1, PRootKernel.legacyResolveCount)
        // A session-aware lookup is unaffected by the hint.
        val scoped = SessionMounts.sessionDir(filesDir, "A", "attachments")
        assertEquals(File(filesDir, "magk-sessions/A/attachments").absolutePath, scoped.absolutePath)
    }

    // ── Wiring ────────────────────────────────────────────────────────────

    private fun src(path: String) = File("src/main/java/com/magk/app/$path").readText()

    @Test
    fun `the coordinator builds private mounts and no longer writes the global table`() {
        val coord = src("sandbox/ExecutionCoordinator.kt")
        assertFalse(coord.contains("PRootKernel.addBindMount("))
        assertTrue(coord.contains("SessionMounts.forContext(appContext, sessionId)"))
        // Both shell types take their -b argv from the private map only.
        assertTrue(src("sandbox/PersistentShell.kt").contains("for ((linuxPath, hostPath) in sessionBindMounts)"))
        assertTrue(src("sandbox/FreshProcessShell.kt").contains("for ((linuxPath, hostPath) in sessionBindMounts)"))
    }

    @Test
    fun `the terminal mounts its chat's dirs and says which files it sees`() {
        val term = src("sandbox/TerminalSession.kt")
        assertTrue(term.contains("buildInteractiveCommand(sessionId)"))
        assertTrue(term.contains("_outputBytes.emit(banner(sessionId).toByteArray())"))
        assertTrue(term.contains("Global terminal — not tied to any chat"))
    }
}
