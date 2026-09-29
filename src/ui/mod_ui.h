#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace S3MM {

// Event types (must match S3MM.UIInternal managed dispatcher)
enum class UIEventType : uint32_t {
    Click = 1,
    TextCommit = 2,
    BoolToggle = 3,
    WindowHidden = 4,    // user clicked the X: the slot is kept, so the window can be shown again
    WindowShown = 5,     // native re-showed a hidden window (mods panel "UI" button)
    Hover = 6,           // intPayload: 1 = hover enter, 0 = leave (edge-triggered only)
    ValueChanged = 7,    // intPayload: SliderInt = the int; SliderFloat = raw float bits (managed decodes with BitConverter). Fires on release/commit, NOT per frame
    WindowDestroyed = 8, // slot dropped (hot-reload force-close): terminal, the managed side must unroot the window
};

// MOD UI MANAGER - Mod-facing ImGui bridge (the managed S3MM.UI / S3MM.Threading API)
// Mods submit per-window widget command buffers via icall, Render() replays the latest buffer into ImGui on the render thread
// Input events queue here and are dispatched to managed handlers on the sim thread by PumpSimThread (per-tick, from the ProcessTasks detour)
// Each shared structure has its own mutex/atomic.
class ModUIManager {
  public:
    static ModUIManager& Get();

    // Register the S3MM.UI / S3MM.Threading icalls. Call after MonoBridge::Initialize.
    void RegisterInternalCalls();

    // Render toasts (always) + mod windows (only when overlayVisible). Render thread only.
    void Render(bool overlayVisible);

    // Cheap check for ImGuiManager: any toasts worth drawing a frame for?
    bool HasToasts();

    // Native-side toast (same queue the S3MM.UI.Notify icall feeds). severity 0=Info 1=Warning 2=Error; durationMs <= 0 = default. Any thread.
    void PushToast(std::string text, int32_t severity, int32_t durationMs);

    // Per-tick pump, sim thread only (called from the ProcessTasks detour after DrainSimTasks): drains the UI event queue into the managed dispatcher and runs posted managed actions
    void PumpSimThread();

    // Hot-reload backstop: drop every window owned by this assembly (queues WindowDestroyed events so the managed registry unroots them too)
    void CloseWindowsForMod(const std::string& modName);

    // Mods panel integration: windows (visible or hidden) owned by a mod, and show + focus all of them (hidden ones get a WindowShown event so the managed side re-registers their handlers)
    int CountWindowsForMod(const std::string& modName);
    void OpenWindowsForMod(const std::string& modName);

    // icall implementations (managed signatures in S3MM.Helper/S3MM.cs)
    static void __cdecl NativeNotify(void* textStr, int32_t severity, int32_t durationMs);
    static void __cdecl NativeSubmitWindow(uint64_t handle, void* titleStr, void* bufferArr);
    static void __cdecl NativeCloseWindow(uint64_t handle);
    static void __cdecl NativeSetWindowOpen(uint64_t handle, int32_t open);
    static void __cdecl NativeWakeSimThread();

  private:
    ModUIManager() = default;
    ~ModUIManager() = default;

    // Toasts (notification queue)
    struct Toast {
        std::string text;
        int32_t severity;
        uint64_t expireTick; // GetTickCount64
    };
    std::mutex m_toastMutex;
    std::deque<Toast> m_toasts;

    // Toast area layout (persisted via Config::SetToastLayout): top-left as display fractions (<0 = default top-right), bg alpha
    bool m_toastLayoutLoaded = false;
    float m_toastPosX = -1.0f;
    float m_toastPosY = -1.0f;
    float m_toastAlpha = 0.65f;
    bool m_toastPosForce = false;    // apply the stored position on the next frame even while the overlay is up (reset/first show)
    bool m_toastLayoutDirty = false; // write-back pending (deferred until the drag ends)

    // Window slots (managed-submitted command buffers)
    struct WidgetEditState {
        // InputText: native owns the in-progress edit between frames; lastSynced = the submitted value we last copied in (external-change detect)
        std::string lastSynced;
        std::string editBuf;
        bool wasActive = false;
        // Checkbox: optimistic local value until the next submit confirms it (anti-flicker)
        bool hasPending = false;
        bool pendingValue = false;
        // Sliders: native owns the value while dragging (localBits), then holds the committed value (pendingBits) until a submit confirms, stale periodic Update() between commit and dispatch can't snap the knob back. Bits = int32 or float
        uint32_t localBits = 0;
        uint32_t pendingBits = 0;
        bool hasPendingBits = false;
        bool sliderActive = false;
        // Hover events: last reported IsItemHovered state, for edge-triggered enter/leave (only for widgets that opted in)
        bool hovered = false;
        // WindowSlot::gen of the last replay that referenced this widget. Entries left behind by a Clear()/rebuild (fresh handles each time) fall behind and get pruned at the end of that replay. 0 = never seen.
        uint32_t seenGen = 0;
    };
    struct WindowSlot {
        uint64_t handle = 0;
        std::string title;
        std::string modName;
        bool open = true;            // user X flips this + sends WindowHidden; SetWindowOpen / OpenWindowsForMod re-show
        uint32_t gen = 1;            // bumped per submit; stamped into WidgetEditState so stale entries are identifiable (never 0)
        bool focusRequested = false; // one-shot SetNextWindowFocus on the next replay
        std::vector<uint8_t> buffer;
        std::unordered_map<uint32_t, WidgetEditState> widgetState;
    };
    std::mutex m_windowsMutex;
    std::unordered_map<uint64_t, WindowSlot> m_windows;

    // Events (render thread -> sim thread)
    struct UIEvent {
        uint64_t window;
        uint32_t widget;
        UIEventType type;
        int32_t intPayload;
        std::string strPayload;
    };
    std::mutex m_eventMutex;
    std::deque<UIEvent> m_events;
    std::atomic<int> m_eventCount{0}; // cheap empty check for the per-tick pump

    void PushEvent(uint64_t window, uint32_t widget, UIEventType type, int32_t intPayload, const std::string& strPayload);

    // Managed dispatcher (resolved lazily once S3MM.Helper is loaded)
    void* m_dispatchMethod = nullptr;     // S3MM.UIInternal::DispatchEvent
    void* m_drainActionsMethod = nullptr; // S3MM.UIInternal::DrainPostedActions
    int m_resolveCooldown = 0;            // tick counter between resolve attempts
    int m_resolveAttempts = 0;            // throttles the "cannot resolve" warning
    bool TryResolveDispatcher();          // sim thread only, throttled

    // Managed-posted actions (S3MM.Threading.Post): flag wakes the pump; the managed side owns the queue itself (keeps delegates GC-rooted)
    std::atomic<bool> m_actionsPending{false};

    void DispatchEvent(const UIEvent& ev); // sim thread only
    void DrainPostedActions();             // sim thread only

    // Rendering helpers (render thread only)
    void RenderToasts(bool overlayVisible); // real toasts always; with the overlay up also a draggable placeholder + right-click opacity menu
    void LoadToastLayoutLocked();
    void RenderToastContextMenuLocked();
    void RenderWindowsLocked(); // called with m_windowsMutex held
    void ReplayWindow(WindowSlot& slot);
};

} // namespace S3MM
