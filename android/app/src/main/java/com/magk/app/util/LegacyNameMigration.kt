package com.magk.app.util

import android.app.Application
import android.app.NotificationManager
import android.content.Context
import android.os.Build
import android.util.Log
import java.io.File

/**
 * One-time move of an existing install's data from the old `minis…` names to
 * the `magk…` names this build uses.
 *
 * The rename changed every on-disk name (database `minis.db`, the
 * `minis-sessions` / `minis-global` folders, the `minis_*` preference files,
 * `/var/minis` inside the rootfs, notification channel ids). Without this, an
 * app updated in place would look for `magk…`, find nothing, and start empty -
 * chats, memory, skills and API keys would appear lost.
 *
 * Two stages, both idempotent and never overwriting anything that exists:
 *  - [runPlain] from attachBaseContext, before any provider or singleton can
 *    open a database or preference file: renames plain files and folders.
 *  - [runEncrypted] from onCreate: encrypted preference stores bind their
 *    contents to the FILE NAME, so they cannot simply be renamed; they are
 *    opened under the old name and copied into the new one.
 */
object LegacyNameMigration {
    private const val TAG = "LegacyNameMigration"
    private const val OLD = "minis"
    private const val NEW = "magk"
    private const val PLAIN_MARKER = ".legacy-names-plain-v1"
    private const val ENCRYPTED_MARKER = ".legacy-names-encrypted-v1"
    private const val KEYSET_MARK = "__androidx_security_crypto_encrypted_prefs_key_keyset__"

    private fun newName(old: String) = NEW + old.removePrefix(OLD)

    private fun isMainProcess(context: Context): Boolean =
        Build.VERSION.SDK_INT < 28 || Application.getProcessName() == context.packageName

    /** Stage 1: files, folders, plain preference files, datastore, channels. */
    fun runPlain(context: Context) {
        if (!isMainProcess(context)) return
        val marker = File(context.filesDir, PLAIN_MARKER)
        if (marker.exists()) return
        runCatching { renamePrefixed(context.filesDir) }.onFailure { Log.w(TAG, "files: ${it.message}") }
        runCatching { renamePrefixed(context.getDatabasePath("x").parentFile) }.onFailure { Log.w(TAG, "databases: ${it.message}") }
        runCatching { renamePrefixed(File(context.filesDir, "datastore")) }.onFailure { Log.w(TAG, "datastore: ${it.message}") }
        runCatching {
            File(context.applicationInfo.dataDir, "shared_prefs").listFiles()
                ?.filter { it.name.startsWith(OLD) && it.name.endsWith(".xml") && !isEncryptedStore(it) }
                ?.forEach { move(it, File(it.parentFile, newName(it.name))) }
        }.onFailure { Log.w(TAG, "prefs: ${it.message}") }
        runCatching { migrateRootfs(File(context.filesDir, "alpine-rootfs")) }.onFailure { Log.w(TAG, "rootfs: ${it.message}") }
        runCatching { dropOldChannels(context) }.onFailure { Log.w(TAG, "channels: ${it.message}") }
        runCatching { marker.createNewFile() }
    }

    /** Stage 2: encrypted preference stores (needs a real application context). */
    fun runEncrypted(context: Context) {
        if (!isMainProcess(context)) return
        val marker = File(context.filesDir, ENCRYPTED_MARKER)
        if (marker.exists()) return
        val prefsDir = File(context.applicationInfo.dataDir, "shared_prefs")
        prefsDir.listFiles()
            ?.filter { it.name.startsWith(OLD) && it.name.endsWith(".xml") && isEncryptedStore(it) }
            ?.forEach { oldFile ->
                val oldBase = oldFile.name.removeSuffix(".xml")
                val newBase = newName(oldBase)
                if (File(prefsDir, "$newBase.xml").exists()) return@forEach
                runCatching {
                    val old = EncryptedPrefsFactory.safeCreate(context, oldBase)
                    val entries = old.all
                    if (entries.isNotEmpty()) {
                        val edit = EncryptedPrefsFactory.safeCreate(context, newBase).edit()
                        for ((k, v) in entries) when (v) {
                            is String -> edit.putString(k, v)
                            is Int -> edit.putInt(k, v)
                            is Long -> edit.putLong(k, v)
                            is Float -> edit.putFloat(k, v)
                            is Boolean -> edit.putBoolean(k, v)
                            is Set<*> -> edit.putStringSet(k, v.filterIsInstance<String>().toSet())
                        }
                        edit.commit()
                        context.deleteSharedPreferences(oldBase)
                        Log.i(TAG, "copied encrypted store $oldBase -> $newBase (${entries.size} entries)")
                    }
                }.onFailure { Log.w(TAG, "encrypted store $oldBase: ${it.message}") }
            }
        runCatching { marker.createNewFile() }
    }

    private fun isEncryptedStore(f: File): Boolean =
        runCatching { f.readText().contains(KEYSET_MARK) }.getOrDefault(false)

    private fun renamePrefixed(dir: File?) {
        dir?.listFiles()?.filter { it.name.startsWith(OLD) }?.forEach {
            move(it, File(it.parentFile, newName(it.name)))
        }
    }

    private fun move(from: File, to: File) {
        if (to.exists()) return // never overwrite data the new build already has
        if (from.renameTo(to)) Log.i(TAG, "moved ${from.name} -> ${to.name}")
        else Log.w(TAG, "could not move ${from.name}")
    }

    /** `/var/minis` -> `/var/magk`; stale stubs/profile are rewritten by the overlay at boot. */
    private fun migrateRootfs(root: File) {
        if (!root.isDirectory) return
        move(File(root, "var/$OLD"), File(root, "var/$NEW"))
        File(root, "usr/local/bin").listFiles()?.filter { it.name.startsWith("$OLD-") }?.forEach { it.delete() }
        File(root, "etc/profile.d/$OLD.sh").delete()
    }

    private fun dropOldChannels(context: Context) {
        val nm = context.getSystemService(NotificationManager::class.java) ?: return
        nm.notificationChannels.filter { it.id.startsWith(OLD) }.forEach { nm.deleteNotificationChannel(it.id) }
        if (Build.VERSION.SDK_INT >= 28) {
            nm.notificationChannelGroups.filter { it.id.startsWith(OLD) }
                .forEach { nm.deleteNotificationChannelGroup(it.id) }
        }
    }
}
