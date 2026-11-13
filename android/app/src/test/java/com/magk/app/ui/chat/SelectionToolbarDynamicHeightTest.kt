package com.magk.app.ui.chat

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * [T-android-toolbar-dynamic-height] Source guards for the text-selection bars
 * (the MagkTextKit bar and the SelectionContainer bar).
 *
 * Both used to be a fixed `height(40.dp)`. Each label needs 20sp of line height
 * plus 20dp of padding, so with a larger system font size - or a taller script
 * such as Arabic - the labels no longer fitted and the Row's clip cut the bottom
 * off every word. The bar now wraps its content (never below 40dp), and the
 * dividers are sized from the label line height instead of filling a height that
 * no longer exists (fillMaxHeight would stretch them to the popup window).
 */
class SelectionToolbarDynamicHeightTest {

    private val main = listOf(File("src/main"), File("app/src/main")).first { it.isDirectory }
    private fun src(name: String) = File(main, "java/com/magk/app/ui/chat/$name").readText()

    private val bars = listOf("MagkTextKitGesture.kt", "MagkMarkdownTextToolbar.kt")

    @Test fun `the bar wraps its content with a 40dp minimum, not a fixed height`() {
        for (name in bars) {
            val s = src(name)
            assertTrue("$name sizes the bar with heightIn(min = 40.dp)", s.contains(".heightIn(min = 40.dp)"))
            assertFalse("$name must not pin the bar to height(40.dp)", s.contains(".height(40.dp)"))
        }
    }

    @Test fun `dividers follow the label line height instead of filling the bar`() {
        for (name in bars) {
            val s = src(name)
            assertFalse("$name dividers must not use fillMaxHeight()", s.contains(".fillMaxHeight()"))
            assertTrue("$name sizes the divider from labelLarge.lineHeight",
                s.contains("MaterialTheme.typography.labelLarge.lineHeight"))
        }
    }

    @Test fun `labels stay on a single line so the bar height is predictable`() {
        for (name in bars) {
            val s = src(name)
            assertTrue("$name keeps maxLines = 1", s.contains("maxLines = 1,\n        softWrap = false,"))
        }
    }
}
