package com.magk.app.ui.chat

// [T-android-split-chat] Small UI-state toggle methods extracted from
// ChatViewModel as extension functions (verbatim): tool-detail sheet,
// browser sheet, memory sheet, attachment list. The 4 backing state fields
// were flipped private->internal. No logic change.

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.compose.foundation.lazy.LazyListState
import com.magk.app.agent.Level
import com.magk.app.agent.ToolLoopDetector
import com.magk.app.browser.BrowserActionInput
import com.magk.app.browser.BrowserTabPool
import com.magk.app.data.db.MessageEntity
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Compress
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Lightbulb
import androidx.compose.material.icons.filled.Psychology
import androidx.compose.material.icons.outlined.Build
import androidx.compose.material.icons.outlined.Extension
import com.magk.app.data.BPETokenizer
import com.magk.app.data.ContextOffload
import com.magk.app.data.ContextPolicy
import com.magk.app.logging.AppLogger
import com.magk.app.data.FileMentionIndex
import com.magk.app.data.db.CompactMarkerEntity
import com.magk.app.data.model.AgentContentPart
import com.magk.app.data.model.AgentToolDefinition
import com.magk.app.data.model.LLMMessage
import com.magk.app.data.model.LLMModel
import com.magk.app.data.model.LLMStreamChunk
import com.magk.app.data.model.LLMUsage
import com.magk.app.data.model.ModelGroup
import com.magk.app.data.model.ThinkingLevel
import com.magk.app.R
import com.magk.app.data.repository.ChatRepository
import com.magk.app.data.repository.MemoryRepository
import com.magk.app.data.repository.ProviderRepository
import com.magk.app.provider.ImageBudget
import com.magk.app.provider.LLMProvider
import com.magk.app.provider.ProviderFactory
import com.magk.app.sandbox.ExecutionCoordinator
import com.magk.app.terminal.MagkOpenUrlBroker
import com.magk.app.terminal.MagkUrlMarker
import com.magk.app.tools.AgentTools
import com.magk.app.tools.FileEditTool
import com.magk.app.tools.FileReadTool
import com.magk.app.tools.FileWriteTool
import com.magk.app.tools.MemoryTools
import com.magk.app.tools.ReadImageTool
import com.magk.app.tools.ToolExecutionResult
import com.magk.app.offload.OffloadPermissionManager
import com.magk.app.service.SessionActivityTracker
import com.magk.app.service.SessionConcurrencyManager
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.yield
import org.json.JSONObject
import java.io.ByteArrayOutputStream

internal fun ChatViewModel.openToolDetail(toolBlockId: String) {
    _selectedToolDetailId.value = toolBlockId
}

internal fun ChatViewModel.closeToolDetail() {
    _selectedToolDetailId.value = null
}

internal fun ChatViewModel.toggleBrowserSheet() {
    val opening = !_showBrowserSheet.value
    if (opening) browserTabPool.ensureTabForUI()
    _showBrowserSheet.value = opening
}

internal fun ChatViewModel.dismissBrowserSheet() {
    _showBrowserSheet.value = false
}

/**
 * Open the session browser sheet, focused on the tab whose URL matches
 * [url]. If no pool tab currently has that URL, a new tab is created and
 * loaded. Used by the tool-call preview's globe button so the agent's
 * existing browser_use page is reused when available instead of spawning
 * a duplicate tab.
 */
internal fun ChatViewModel.openBrowserSheetForUrl(url: String) {
    if (url.isBlank()) {
        browserTabPool.ensureTabForUI()
    } else {
        browserTabPool.selectOrCreateTabForURL(url)
    }
    _showBrowserSheet.value = true
}

internal fun ChatViewModel.toggleMemorySheet() {
    _showMemorySheet.value = !_showMemorySheet.value
}

internal fun ChatViewModel.dismissMemorySheet() {
    _showMemorySheet.value = false
}

internal fun ChatViewModel.addAttachment(attachment: InputAttachment) {
    _attachments.value = _attachments.value + attachment
}

internal fun ChatViewModel.removeAttachment(id: String) {
    _attachments.value = _attachments.value.filter { it.id != id }
}

internal fun ChatViewModel.clearAttachments() {
    _attachments.value = emptyList()
}
