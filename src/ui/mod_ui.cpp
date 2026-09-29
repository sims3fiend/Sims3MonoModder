#include "mod_ui.h"
#include "../core/mono_bridge.h"
#include "../modding/hook_manager.h"
#include "../modding/mod_loader.h"
#include "../util/config.h"
#include "../util/logger.h"
#include "imgui.h"
#include <cfloat>
#include <cstring>

namespace S3MM {

// PROTOCOL - shared with the managed serializer in S3MM.Helper (S3MM.UI.Window.Serialize)
// Layout (little-endian): u32 magic | u32 reserved | u32 opCount | u32 blobBytes | opCount * OpRecord(32B) | string blob
// Strings are null-terminated UTF-8 in the blob, referenced by offset; kStrNone = unused slot
// No separate protocol version: a breaking wire change is an API change, so it bumps ModCompiler::API_VERSION (which also re-extracts S3MM.Helper)
static constexpr uint32_t kMagic = 0x31553353; // "S3U1"
static constexpr uint32_t kStrNone = 0xFFFFFFFF;
static constexpr size_t kHeaderBytes = 16;
static constexpr size_t kOpRecordBytes = 32;
static constexpr uint32_t kMaxOps = 4096;
static constexpr uint32_t kMaxBlobBytes = 1 << 20;
static constexpr uint32_t kMaxBufferBytes = kHeaderBytes + kMaxOps * kOpRecordBytes + kMaxBlobBytes;
static constexpr size_t kMaxEvents = 256;   // drop-oldest past this, with a warn
static constexpr size_t kMaxToasts = 16;    // drop-oldest past this
static constexpr size_t kEditBufSize = 512; // fixed InputText edit buffers

// Op codes (must match S3MM.UI.Window managed serializer). Additive: unknown ops are skipped, so new ops don't bump API_VERSION
enum class UIOp : uint32_t {
    Label = 3,     // strA = text, flags = LabelStyle (bit3 colored: reserved = RGBA8 packed, IM_COL32 layout)
    Button = 4,    // strA = label, widget -> Click
    Separator = 5, // strA = optional label (SeparatorText), kStrNone = plain line
    SameLine = 6,
    Selectable = 7, // strA = label, flags bit0 = selected, widget -> Click
    InputText = 8,  // strA = label, strB = value, widget -> TextCommit
    Checkbox = 9,   // strA = label, flags bit0 = checked, widget -> BoolToggle
    Spacing = 10,
    Tooltip = 11,          // strA = text; attaches to the PREVIOUS item (SetItemTooltip)
    ContextMenuBegin = 12, // opens a right-click popup bound to the PREVIOUS item's ID
    ContextMenuItem = 13,  // strA = label, widget -> Click (only rendered while the popup is open)
    ContextMenuEnd = 14,
    SliderFloat = 15, // strA = label, strB = printf format (kStrNone = "%.3f"), fA = min, fB = max, reserved = value bits, flags bit0 = logarithmic; widget -> ValueChanged(float bits)
    SliderInt = 16,   // strA = label, strB = printf format (kStrNone = "%d"), fA/fB slots = raw int32 min/max, reserved = int32 value; widget -> ValueChanged(int)
    ProgressBar = 17, // fA = fraction 0..1, strA = optional overlay text (kStrNone = default percentage)
    RadioButton = 18, // strA = label, flags bit0 = active, widget -> Click
    Indent = 19,
    Unindent = 20,
    ItemWidth = 21,     // fA = width px for the NEXT item (SetNextItemWidth); <= 0 = ImGui default
    BeginDisabled = 22, // flags bit0 = disabled; everything until EndDisabled is greyed + non-interactive when set
    EndDisabled = 23,
};

// Label flags (UIOp::Label): combinable style bits (Bullet | Wrapped is a wrapped bullet), color as an extra bit.
// Disabled is applied as the greyed text color, so it composes with Wrapped/Bullet; an explicit color overrides it.
enum LabelStyle : uint32_t {
    LabelStyle_Normal = 0,
    LabelStyle_Disabled = 1,
    LabelStyle_Wrapped = 2,
    LabelStyle_Bullet = 4,
    LabelStyle_StyleMask = 7,
    LabelStyle_Colored = 1u << 3, // reserved = RGBA8
};

// Flag bits shared by every item op, above the per-op bits (must match S3MM.UI.WidgetFlagWantsHover)
enum WidgetFlag : uint32_t {
    WidgetFlag_WantsHover = 1u << 16, // the mod attached a Hover() handler; without it we neither track nor allocate state
};

struct OpRecord {
    uint32_t op;
    uint32_t widget;
    uint32_t flags;
    uint32_t strA;
    uint32_t strB;
    float fA;
    float fB;
    uint32_t reserved;
};
static_assert(sizeof(OpRecord) == kOpRecordBytes, "OpRecord layout drifted vs managed serializer");

ModUIManager& ModUIManager::Get() {
    static ModUIManager instance;
    return instance;
}

// ICALL REGISTRATION - mirrors the managed externs in S3MM.UI / S3MM.Threading
void ModUIManager::RegisterInternalCalls() {
    MonoBridge::Get().AddInternalCall("S3MM.UI::Notify", (void*)&ModUIManager::NativeNotify);
    MonoBridge::Get().AddInternalCall("S3MM.UI::SubmitWindow", (void*)&ModUIManager::NativeSubmitWindow);
    MonoBridge::Get().AddInternalCall("S3MM.UI::CloseWindow", (void*)&ModUIManager::NativeCloseWindow);
    MonoBridge::Get().AddInternalCall("S3MM.UI::SetWindowOpen", (void*)&ModUIManager::NativeSetWindowOpen);
    MonoBridge::Get().AddInternalCall("S3MM.Threading::WakeSimThread", (void*)&ModUIManager::NativeWakeSimThread);
    LOG_INFO("[ModUI] Internal calls registered");
}

// ICALLS - managed S3MM.UI / S3MM.Threading entry points

void __cdecl ModUIManager::NativeNotify(void* textStr, int32_t severity, int32_t durationMs) {
    if (!textStr) return;
    Get().PushToast(MonoBridge::Get().ExtractString(textStr), severity, durationMs);
}

void ModUIManager::PushToast(std::string text, int32_t severity, int32_t durationMs) {
    if (durationMs <= 0) durationMs = 4000;
    if (severity < 0 || severity > 2) severity = 0;

    std::lock_guard<std::mutex> lock(m_toastMutex);
    if (m_toasts.size() >= kMaxToasts) { m_toasts.pop_front(); }
    Toast t;
    t.text = std::move(text);
    t.severity = severity;
    t.expireTick = GetTickCount64() + (uint64_t)durationMs;
    m_toasts.push_back(std::move(t));
}

void __cdecl ModUIManager::NativeSubmitWindow(uint64_t handle, void* titleStr, void* bufferArr) {
    if (!bufferArr) return;
    std::string title = titleStr ? MonoBridge::Get().ExtractString(titleStr, 256) : "Window";
    // Tag windows with their owning assembly so hot-reload can drop them
    std::string modName = ResolveModAssemblyFromFrames();
    if (modName.empty()) { modName = "Mod"; }

    MonoBridge& bridge = MonoBridge::Get();
    int32_t len = bridge.GetArrayLength(bufferArr);
    if (len < (int32_t)kHeaderBytes || (uint32_t)len > kMaxBufferBytes) {
        LOG_WARN("[ModUI] SubmitWindow: bad buffer length " + std::to_string(len) + " from " + modName);
        return;
    }
    void* data = bridge.GetArrayElementPtr(bufferArr, 0, 1);
    if (!data) return;

    std::vector<uint8_t> copy((uint8_t*)data, (uint8_t*)data + len);

    auto& self = Get();
    std::lock_guard<std::mutex> lock(self.m_windowsMutex);
    auto& slot = self.m_windows[handle];
    bool isNew = slot.buffer.empty() && slot.title.empty();
    slot.handle = handle;
    slot.title = std::move(title);
    slot.modName = std::move(modName);
    slot.buffer = std::move(copy);
    if (++slot.gen == 0) { slot.gen = 1; } // 0 means "never seen" in WidgetEditState, so skip it on wraparound
    if (isNew) { slot.open = true; }       // resubmits never change open state; X-close / SetWindowOpen own it
}

void __cdecl ModUIManager::NativeCloseWindow(uint64_t handle) {
    auto& self = Get();
    std::lock_guard<std::mutex> lock(self.m_windowsMutex);
    self.m_windows.erase(handle);
}

void __cdecl ModUIManager::NativeSetWindowOpen(uint64_t handle, int32_t open) {
    auto& self = Get();
    std::lock_guard<std::mutex> lock(self.m_windowsMutex);
    auto it = self.m_windows.find(handle);
    if (it != self.m_windows.end()) { it->second.open = open != 0; }
}

void __cdecl ModUIManager::NativeWakeSimThread() {
    Get().m_actionsPending.store(true, std::memory_order_release);
}

// EVENTS - render thread -> sim thread queue, plus mod lifecycle (hot-reload close, mods panel reopen)

void ModUIManager::PushEvent(uint64_t window, uint32_t widget, UIEventType type, int32_t intPayload, const std::string& strPayload) {
    std::lock_guard<std::mutex> lock(m_eventMutex);
    if (m_events.size() >= kMaxEvents) {
        m_events.pop_front();
        LOG_WARN("[ModUI] Event queue full - dropping oldest event");
    }
    UIEvent ev;
    ev.window = window;
    ev.widget = widget;
    ev.type = type;
    ev.intPayload = intPayload;
    ev.strPayload = strPayload;
    m_events.push_back(std::move(ev));
    m_eventCount.fetch_add(1, std::memory_order_release);
}

void ModUIManager::CloseWindowsForMod(const std::string& modName) {
    std::vector<uint64_t> closed;
    {
        std::lock_guard<std::mutex> lock(m_windowsMutex);
        for (auto it = m_windows.begin(); it != m_windows.end();) {
            if (it->second.modName == modName) {
                closed.push_back(it->first);
                it = m_windows.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Tell the managed registry these windows are gone (it lives in S3MM.Helper, which survives hot-reload - the delegates behind them don't)
    for (uint64_t h : closed) { PushEvent(h, 0, UIEventType::WindowDestroyed, 0, ""); }
    if (!closed.empty()) { LOG_INFO("[ModUI] Closed " + std::to_string(closed.size()) + " window(s) for unloaded mod " + modName); }
}

int ModUIManager::CountWindowsForMod(const std::string& modName) {
    std::lock_guard<std::mutex> lock(m_windowsMutex);
    int n = 0;
    for (auto& kv : m_windows) {
        if (kv.second.modName == modName) n++;
    }
    return n;
}

void ModUIManager::OpenWindowsForMod(const std::string& modName) {
    std::vector<uint64_t> reopened;
    {
        std::lock_guard<std::mutex> lock(m_windowsMutex);
        for (auto& kv : m_windows) {
            WindowSlot& slot = kv.second;
            if (slot.modName != modName) continue;
            if (!slot.open) {
                slot.open = true;
                reopened.push_back(slot.handle);
            }
            slot.focusRequested = true;
        }
    }
    // The managed registry parked these as hidden; tell it to take them back so their handlers fire again
    for (uint64_t h : reopened) { PushEvent(h, 0, UIEventType::WindowShown, 0, ""); }
}

// SIM-THREAD PUMP - resolve the managed dispatcher, then drain events and posted actions each tick

bool ModUIManager::TryResolveDispatcher() {
    if (m_resolveCooldown > 0) {
        m_resolveCooldown--;
        return false;
    }
    m_resolveCooldown = 60; // retry about once every 2s of ticks until S3MM.Helper shows up
    m_resolveAttempts++;

    if (!MonoBridge::Get().IsRuntimeReady()) return false;

    // UIInternal lives in S3MM.Helper, which S3MM loads itself, it is NOT in the ScriptHost's mImages rbtree, so the game's FindClass (SPEC_GetClassFromAllImages / 0x00D79CA0) can never see it.
    // Resolve off the stored helper image like persist_bridge/audit_api do, GetClassByName stays as fallback
    void* klass = nullptr;
    void* helperImage = ModLoader::Get().GetHelperImage();
    if (helperImage) { klass = MonoBridge::Get().GetClassFromNameInImage(helperImage, "S3MM", "UIInternal"); }
    if (!klass) { klass = MonoBridge::Get().GetClassByName("S3MM", "UIInternal"); }
    if (!klass) {
        // Warn on the first miss and then about once a minute, never silently
        if (m_resolveAttempts == 1 || (m_resolveAttempts % 30) == 0) {
            LOG_WARN("[ModUI] Cannot resolve S3MM.UIInternal (helper image " + std::string(helperImage ? "present" : "missing") + ") - UI events are queued but not dispatched (attempt " +
                     std::to_string(m_resolveAttempts) + ")");
        }
        return false;
    }

    int iter = 0;
    void* method = nullptr;
    while ((method = MonoBridge::Get().GetNextMethod(klass, &iter)) != nullptr) {
        std::string fullName = MonoBridge::GetMethodFullName(method);
        // Suffix match on "Namespace.Class::Method" (GetMethodFullName format) lengths derived from the literals
        static constexpr char kDispatch[] = "::DispatchEvent";
        static constexpr char kDrain[] = "::DrainPostedActions";
        constexpr size_t kDispatchLen = sizeof(kDispatch) - 1;
        constexpr size_t kDrainLen = sizeof(kDrain) - 1;
        if (fullName.size() >= kDispatchLen && fullName.compare(fullName.size() - kDispatchLen, kDispatchLen, kDispatch) == 0) {
            m_dispatchMethod = method;
        } else if (fullName.size() >= kDrainLen && fullName.compare(fullName.size() - kDrainLen, kDrainLen, kDrain) == 0) {
            m_drainActionsMethod = method;
        }
    }

    if (m_dispatchMethod && m_drainActionsMethod) {
        LOG_INFO("[ModUI] Managed dispatcher resolved (UIInternal @ " + std::to_string((uintptr_t)klass) + ")");
        return true;
    }
    // Partial match means a stale S3MM.Helper, clear so we keep retrying instead of half-working (warn on the first miss, then about once a minute)
    if (m_resolveAttempts == 1 || (m_resolveAttempts % 30) == 0) {
        LOG_WARN("[ModUI] S3MM.UIInternal found but DispatchEvent=" + std::string(m_dispatchMethod ? "ok" : "missing") + " DrainPostedActions=" + std::string(m_drainActionsMethod ? "ok" : "missing") +
                 " - stale S3MM.Helper.dll? (attempt " + std::to_string(m_resolveAttempts) + ")");
    }
    m_dispatchMethod = nullptr;
    m_drainActionsMethod = nullptr;
    return false;
}

void ModUIManager::DispatchEvent(const UIEvent& ev) {
    if (!m_dispatchMethod) return;

    uint64_t wh = ev.window;
    uint32_t widget = ev.widget;
    uint32_t type = (uint32_t)ev.type; // marshalled as uint to the managed dispatcher
    int32_t ip = ev.intPayload;
    void* monoStr = MonoBridge::Get().CreateString(ev.strPayload.c_str());

    // Interp invoke convention (see CacheAPI::NativeInvoke): reference types as the object pointer itself, value types as a pointer to the raw value
    void* params[5] = {&wh, &widget, &type, &ip, monoStr};
    MonoInvokeState state = {};
    if (!MonoBridge::Get().InvokeMethod(m_dispatchMethod, nullptr, params, &state)) { LOG_WARN("[ModUI] DispatchEvent invoke failed (managed exception), window=" + std::to_string(wh) + " type=" + std::to_string(type)); }
}

void ModUIManager::DrainPostedActions() {
    if (!m_drainActionsMethod) return;
    MonoInvokeState state = {};
    if (!MonoBridge::Get().InvokeMethod(m_drainActionsMethod, nullptr, nullptr, &state)) { LOG_WARN("[ModUI] DrainPostedActions invoke failed (managed exception)"); }
}

void ModUIManager::PumpSimThread() {
    // Fast path: nothing to do (this runs every tick)
    bool wantActions = m_actionsPending.load(std::memory_order_acquire);
    if (m_eventCount.load(std::memory_order_acquire) == 0 && !wantActions) return;

    if (!m_dispatchMethod && !TryResolveDispatcher()) return;

    if (wantActions && m_actionsPending.exchange(false, std::memory_order_acq_rel)) { DrainPostedActions(); }

    if (m_eventCount.load(std::memory_order_acquire) > 0) {
        std::deque<UIEvent> batch;
        {
            std::lock_guard<std::mutex> lock(m_eventMutex);
            batch.swap(m_events);
            m_eventCount.store(0, std::memory_order_release);
        }
        for (const UIEvent& ev : batch) { DispatchEvent(ev); }
    }
}

// RENDERING - toasts and window replay, render thread only

bool ModUIManager::HasToasts() {
    std::lock_guard<std::mutex> lock(m_toastMutex);
    return !m_toasts.empty();
}

void ModUIManager::Render(bool overlayVisible) {
    RenderToasts(overlayVisible);
    if (!overlayVisible) return;

    std::lock_guard<std::mutex> lock(m_windowsMutex);
    RenderWindowsLocked();
}

static constexpr float kToastWidth = 300.0f; // fixed width, auto height (text wraps) so a right-side anchor never grows off-screen
static constexpr float kToastMargin = 10.0f;

void ModUIManager::LoadToastLayoutLocked() {
    if (m_toastLayoutLoaded) return;
    Config::Get().GetToastLayout(m_toastPosX, m_toastPosY, m_toastAlpha);
    m_toastLayoutLoaded = true;
    m_toastPosForce = true;
}

// Right-click menu on the toast area (overlay visible only): opacity slider, position reset
void ModUIManager::RenderToastContextMenuLocked() {
    if (!ImGui::BeginPopupContextWindow("##s3mm_toast_ctx")) return;

    ImGui::TextDisabled("Notifications");
    ImGui::Separator();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Opacity", &m_toastAlpha, 0.1f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) { m_toastLayoutDirty = true; }
    if (ImGui::MenuItem("Reset position")) {
        m_toastPosX = -1.0f;
        m_toastPosY = -1.0f;
        m_toastPosForce = true;
        m_toastLayoutDirty = true;
    }
    ImGui::EndPopup();
}

void ModUIManager::RenderToasts(bool overlayVisible) {
    std::lock_guard<std::mutex> lock(m_toastMutex);

    uint64_t now = GetTickCount64();
    for (auto it = m_toasts.begin(); it != m_toasts.end();) {
        if (it->expireTick <= now) {
            it = m_toasts.erase(it);
        } else {
            ++it;
        }
    }
    // With the overlay up, an empty area still draws a placeholder so the user can see, drag, and right-click it
    bool placeholder = overlayVisible && m_toasts.empty();
    if (m_toasts.empty() && !placeholder) return;

    LoadToastLayoutLocked();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos;
    if (m_toastPosX >= 0.0f && m_toastPosY >= 0.0f) {
        pos = ImVec2(m_toastPosX * io.DisplaySize.x, m_toastPosY * io.DisplaySize.y);
    } else {
        pos = ImVec2(io.DisplaySize.x - kToastWidth - kToastMargin, kToastMargin); // default: top-right
    }
    // Keep it on screen across resolution changes
    float maxX = io.DisplaySize.x - kToastWidth;
    float maxY = io.DisplaySize.y - 40.0f;
    pos.x = (maxX > 0.0f) ? ((pos.x < 0.0f) ? 0.0f : (pos.x > maxX ? maxX : pos.x)) : 0.0f;
    pos.y = (maxY > 0.0f) ? ((pos.y < 0.0f) ? 0.0f : (pos.y > maxY ? maxY : pos.y)) : 0.0f;

    // Hidden overlay: pinned + click-through. Visible overlay: draggable (body drag, no title bar) and right-clickable
    ImGuiCond posCond = (!overlayVisible || m_toastPosForce) ? ImGuiCond_Always : ImGuiCond_Appearing;
    m_toastPosForce = false;
    ImGui::SetNextWindowPos(pos, posCond);
    ImGui::SetNextWindowSize(ImVec2(kToastWidth, 0.0f)); // 0 = auto-fit height
    ImGui::SetNextWindowBgAlpha(m_toastAlpha);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    if (!overlayVisible) { flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs; }

    if (ImGui::Begin("##s3mm_toasts", nullptr, flags)) {
        ImGui::PushTextWrapPos(0.0f);
        if (placeholder) {
            ImGui::TextDisabled("Notifications appear here");
            ImGui::TextDisabled("Drag to move, right-click for opacity");
        } else {
            static const ImVec4 kSeverityColors[3] = {
                ImVec4(0.90f, 0.90f, 0.90f, 1.0f), // Info
                ImVec4(1.00f, 0.85f, 0.30f, 1.0f), // Warning
                ImVec4(1.00f, 0.40f, 0.40f, 1.0f), // Error
            };
            int idx = 0;
            for (const Toast& t : m_toasts) {
                int sev = (t.severity >= 0 && t.severity <= 2) ? t.severity : 0;
                ImGui::PushID(idx++);
                ImGui::TextColored(kSeverityColors[sev], "%s", t.text.c_str());
                ImGui::PopID();
            }
        }
        ImGui::PopTextWrapPos();

        if (overlayVisible) {
            RenderToastContextMenuLocked();

            // Drag write-back: store as display fractions; defer the file write until the mouse is released
            ImVec2 cur = ImGui::GetWindowPos();
            if ((cur.x != pos.x || cur.y != pos.y) && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
                m_toastPosX = cur.x / io.DisplaySize.x;
                m_toastPosY = cur.y / io.DisplaySize.y;
                m_toastLayoutDirty = true;
            }
        }
    }
    ImGui::End();

    if (m_toastLayoutDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        Config::Get().SetToastLayout(m_toastPosX, m_toastPosY, m_toastAlpha);
        m_toastLayoutDirty = false;
    }
}

void ModUIManager::RenderWindowsLocked() {
    for (auto& kv : m_windows) {
        WindowSlot& slot = kv.second;
        if (!slot.open || slot.buffer.empty()) continue;
        ReplayWindow(slot);
    }
}

// Safe blob string fetch: bounded scan for the NUL, nullptr on anything fishy stinkyyy
static const char* BlobString(const uint8_t* blob, uint32_t blobBytes, uint32_t offset) {
    if (offset == kStrNone) return nullptr;
    if (offset >= blobBytes) return nullptr;
    const char* s = (const char*)(blob + offset);
    if (!memchr(s, '\0', blobBytes - offset)) return nullptr;
    return s;
}

void ModUIManager::ReplayWindow(WindowSlot& slot) {
    const uint8_t* buf = slot.buffer.data();
    size_t size = slot.buffer.size();
    if (size < kHeaderBytes) return;

    uint32_t magic, opCount, blobBytes;
    memcpy(&magic, buf, 4);
    memcpy(&opCount, buf + 8, 4);
    memcpy(&blobBytes, buf + 12, 4);

    if (magic != kMagic) {
        LOG_WARN("[ModUI] Window " + std::to_string(slot.handle) + ": bad magic - is S3MM.Helper out of date?");
        return;
    }
    if (opCount > kMaxOps || blobBytes > kMaxBlobBytes) return;
    if (size < kHeaderBytes + (size_t)opCount * kOpRecordBytes + blobBytes) return;

    const OpRecord* ops = (const OpRecord*)(buf + kHeaderBytes);
    const uint8_t* blob = buf + kHeaderBytes + (size_t)opCount * kOpRecordBytes;

    // Stable unique window identity: title + handle, clean title bar text
    std::string windowTitle = slot.title + "##" + std::to_string(slot.handle);
    ImGui::SetNextWindowSize(ImVec2(320, 240), ImGuiCond_FirstUseEver);
    if (slot.focusRequested) {
        ImGui::SetNextWindowFocus();
        slot.focusRequested = false;
    }

    bool open = true;
    if (ImGui::Begin(windowTitle.c_str(), &open)) {
        ImGui::PushID((int)(slot.handle & 0x7FFFFFFF)); // cross-window ID isolation

        // Mark a widget's state as referenced by this replay and count it on the FIRST stamp only, an op that touches its state twice (slider body + trackHover) must not inflate the live count, or the prune below will never fire
        size_t touched = 0;
        auto stamp = [&slot, &touched](WidgetEditState& st) -> WidgetEditState& {
            if (st.seenGen != slot.gen) {
                st.seenGen = slot.gen;
                touched++;
            }
            return st;
        };

        // Edge-triggered hover events (UIEventType::Hover): fire only on state change, never per frame, and only for widgets whose mod attached a handler
        // otherwise we would queue events nobody reads and allocate WidgetEditState for every label the mouse crosses
        auto trackHover = [this, &slot, &stamp](uint32_t widget, uint32_t flags) {
            if (!(flags & WidgetFlag_WantsHover)) return;
            bool h = ImGui::IsItemHovered();
            bool& prev = stamp(slot.widgetState[widget]).hovered;
            if (h != prev) {
                prev = h;
                PushEvent(slot.handle, widget, UIEventType::Hover, h ? 1 : 0, "");
            }
        };
        bool ctxOpen = false;  // a ContextMenuBegin popup is open; ContextMenuItem ops render into it
        int disabledDepth = 0; // open BeginDisabled scopes; balanced at window end so a mod that forgets EndDisabled can't trip ImGui's assert

        // Slider value round-trip (shared by SliderFloat/SliderInt, value carried as raw bits): returns the bits to display this frame
        // While the knob is held native owns the value; after release the committed value is held until a submit echoes it
        auto sliderDisplayBits = [](WidgetEditState& st, uint32_t submittedBits) -> uint32_t {
            if (st.hasPendingBits && st.pendingBits == submittedBits) { st.hasPendingBits = false; }
            if (!st.sliderActive) { st.localBits = st.hasPendingBits ? st.pendingBits : submittedBits; }
            return st.localBits;
        };
        auto sliderCommit = [this, &slot](WidgetEditState& st, uint32_t widget) {
            st.sliderActive = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                st.hasPendingBits = true;
                st.pendingBits = st.localBits;
                int32_t payload;
                memcpy(&payload, &st.localBits, 4);
                PushEvent(slot.handle, widget, UIEventType::ValueChanged, payload, "");
            }
        };

        for (uint32_t i = 0; i < opCount; i++) {
            const OpRecord& op = ops[i];
            const char* strA = BlobString(blob, blobBytes, op.strA);
            const char* strB = BlobString(blob, blobBytes, op.strB);

            switch ((UIOp)op.op) {
            case UIOp::Label:
                if (strA) {
                    uint32_t style = op.flags & LabelStyle_StyleMask;
                    // Color and Disabled are both just a text color, so they share one push slot; an explicit color wins
                    bool pushedColor = true;
                    if (op.flags & LabelStyle_Colored) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(op.reserved));
                    } else if (style & LabelStyle_Disabled) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    } else {
                        pushedColor = false;
                    }
                    if (style & LabelStyle_Bullet) {
                        ImGui::Bullet();
                        ImGui::SameLine();
                    }
                    if (style & LabelStyle_Wrapped) {
                        ImGui::TextWrapped("%s", strA);
                    } else {
                        ImGui::TextUnformatted(strA);
                    }
                    if (pushedColor) { ImGui::PopStyleColor(); }
                    trackHover(op.widget, op.flags);
                }
                break;

            case UIOp::SliderFloat:
                if (strA) {
                    ImGui::PushID((int)op.widget);
                    WidgetEditState& st = stamp(slot.widgetState[op.widget]);
                    uint32_t bits = sliderDisplayBits(st, op.reserved);
                    float v;
                    memcpy(&v, &bits, 4);
                    ImGuiSliderFlags sf = ImGuiSliderFlags_AlwaysClamp | ((op.flags & 1) ? ImGuiSliderFlags_Logarithmic : 0);
                    if (ImGui::SliderFloat(strA, &v, op.fA, op.fB, strB ? strB : "%.3f", sf)) { memcpy(&st.localBits, &v, 4); }
                    sliderCommit(st, op.widget);
                    trackHover(op.widget, op.flags);
                    ImGui::PopID();
                }
                break;

            case UIOp::SliderInt:
                if (strA) {
                    ImGui::PushID((int)op.widget);
                    WidgetEditState& st = stamp(slot.widgetState[op.widget]);
                    uint32_t bits = sliderDisplayBits(st, op.reserved);
                    int32_t v, vmin, vmax;
                    memcpy(&v, &bits, 4);
                    memcpy(&vmin, &op.fA, 4); // int32 min/max travel in the float slots as raw bits (exact, no float rounding)
                    memcpy(&vmax, &op.fB, 4);
                    if (ImGui::SliderInt(strA, &v, vmin, vmax, strB ? strB : "%d", ImGuiSliderFlags_AlwaysClamp)) { memcpy(&st.localBits, &v, 4); }
                    sliderCommit(st, op.widget);
                    trackHover(op.widget, op.flags);
                    ImGui::PopID();
                }
                break;

            case UIOp::ProgressBar: {
                float frac = op.fA;
                if (!(frac >= 0.0f)) frac = 0.0f; // also catches NaN
                if (frac > 1.0f) frac = 1.0f;
                ImGui::PushID((int)op.widget);
                // Size 0 = CalcItemWidth(): the same default width as sliders/text fields, and SetNextItemWidth (UIOp::ItemWidth) applies.
                // (-FLT_MIN would fill the row and silently ignore ItemWidth.) null overlay = ImGui's default "xx%"
                ImGui::ProgressBar(frac, ImVec2(0.0f, 0.0f), strA);
                trackHover(op.widget, op.flags);
                ImGui::PopID();
                break;
            }

            case UIOp::RadioButton:
                if (strA) {
                    ImGui::PushID((int)op.widget);
                    if (ImGui::RadioButton(strA, (op.flags & 1) != 0)) { PushEvent(slot.handle, op.widget, UIEventType::Click, 0, ""); }
                    trackHover(op.widget, op.flags);
                    ImGui::PopID();
                }
                break;

            case UIOp::Indent:
                ImGui::Indent();
                break;

            case UIOp::Unindent:
                ImGui::Unindent();
                break;

            case UIOp::ItemWidth:
                if (op.fA > 0.0f) ImGui::SetNextItemWidth(op.fA);
                break;

            case UIOp::BeginDisabled:
                ImGui::BeginDisabled((op.flags & 1) != 0);
                disabledDepth++;
                break;

            case UIOp::EndDisabled:
                if (disabledDepth > 0) {
                    ImGui::EndDisabled();
                    disabledDepth--;
                }
                break;

            case UIOp::Button:
                if (strA) {
                    ImGui::PushID((int)op.widget);
                    if (ImGui::Button(strA)) { PushEvent(slot.handle, op.widget, UIEventType::Click, 0, ""); }
                    trackHover(op.widget, op.flags);
                    ImGui::PopID();
                }
                break;

            case UIOp::Separator:
                if (strA) {
                    ImGui::SeparatorText(strA);
                } else {
                    ImGui::Separator();
                }
                break;

            case UIOp::SameLine:
                ImGui::SameLine();
                break;

            case UIOp::Spacing:
                ImGui::Spacing();
                break;

            case UIOp::Selectable:
                if (strA) {
                    ImGui::PushID((int)op.widget);
                    if (ImGui::Selectable(strA, (op.flags & 1) != 0)) { PushEvent(slot.handle, op.widget, UIEventType::Click, 0, ""); }
                    trackHover(op.widget, op.flags);
                    ImGui::PopID();
                }
                break;

            case UIOp::Checkbox:
                if (strA) {
                    ImGui::PushID((int)op.widget);
                    WidgetEditState& st = stamp(slot.widgetState[op.widget]);
                    bool submitted = (op.flags & 1) != 0;
                    // Drop the optimistic value once the managed side confirms it in a submit
                    if (st.hasPending && st.pendingValue == submitted) { st.hasPending = false; }
                    bool v = st.hasPending ? st.pendingValue : submitted;
                    if (ImGui::Checkbox(strA, &v)) {
                        st.hasPending = true;
                        st.pendingValue = v;
                        PushEvent(slot.handle, op.widget, UIEventType::BoolToggle, v ? 1 : 0, "");
                    }
                    trackHover(op.widget, op.flags);
                    ImGui::PopID();
                }
                break;

            case UIOp::InputText:
                if (strA) {
                    ImGui::PushID((int)op.widget);
                    WidgetEditState& st = stamp(slot.widgetState[op.widget]);
                    std::string value = strB ? strB : "";

                    // Managed resubmitted a different value and the user isn't mid-edit: resync the native edit buffer
                    if (value != st.lastSynced && !st.wasActive) {
                        st.editBuf = value;
                        st.lastSynced = value;
                    }

                    char editBuf[kEditBufSize];
                    strncpy_s(editBuf, st.editBuf.c_str(), _TRUNCATE);

                    bool enter = ImGui::InputText(strA, editBuf, sizeof(editBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                    bool committed = enter || ImGui::IsItemDeactivatedAfterEdit();
                    st.wasActive = ImGui::IsItemActive();

                    if (committed) {
                        st.editBuf = editBuf;
                        st.lastSynced = st.editBuf; // treat our commit as the current value until managed confirms
                        PushEvent(slot.handle, op.widget, UIEventType::TextCommit, 0, st.editBuf);
                    } else {
                        st.editBuf = editBuf;
                    }
                    trackHover(op.widget, op.flags);
                    ImGui::PopID();
                }
                break;

            case UIOp::Tooltip:
                // Attaches to the previous item
                if (strA) ImGui::SetItemTooltip("%s", strA);
                break;

            case UIOp::ContextMenuBegin:
                // Null str_id binds the popup to the previous item's ID - unique per widget for free
                ctxOpen = ImGui::BeginPopupContextItem();
                break;

            case UIOp::ContextMenuItem:
                if (ctxOpen && strA) {
                    ImGui::PushID((int)op.widget);
                    if (ImGui::MenuItem(strA)) { PushEvent(slot.handle, op.widget, UIEventType::Click, 0, ""); }
                    ImGui::PopID();
                }
                break;

            case UIOp::ContextMenuEnd:
                if (ctxOpen) {
                    ImGui::EndPopup();
                    ctxOpen = false;
                }
                break;

            default:
                break; // unknown op: forward-compat, skip
            }
        }

        // Balance anything the buffer left open (missing EndContextMenu / EndDisabled) ImGui asserts on unbalanced scopes at End()
        if (ctxOpen) { ImGui::EndPopup(); }
        while (disabledDepth-- > 0) { ImGui::EndDisabled(); }

        // PRUNE STALE WIDGET STATE - a Clear()/rebuild hands every widget a fresh handle, the slider/checkbox/text state from each past rebuild would otherwise sit in the map for the life of the process
        // touched is exactly the live set, so this (should) iterate only when something is genuinely stale and costs one compare per frame otherwise
        // Must stay inside the Begin() body: a collapsed window skips op loop, and pruning on touched == 0 there would throw away the state of every live widget.
        if (slot.widgetState.size() > touched) {
            for (auto it = slot.widgetState.begin(); it != slot.widgetState.end();) {
                if (it->second.seenGen != slot.gen) {
                    it = slot.widgetState.erase(it);
                } else {
                    ++it;
                }
            }
        }

        ImGui::PopID();
    }
    ImGui::End();

    if (!open) {
        slot.open = false;
        PushEvent(slot.handle, 0, UIEventType::WindowHidden, 0, ""); // the slot stays; managed parks the window as hidden
    }
}

} // namespace S3MM
