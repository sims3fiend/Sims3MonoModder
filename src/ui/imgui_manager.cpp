#include "imgui_manager.h"
#include "themes.h"
#include "../util/logger.h"
#include "../util/config.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

#include <detours/detours.h>
#include <atomic>

// Forward declare the Win32 message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace S3MM {

// Static instance pointer for hooks
static ImGuiManager* g_instance = nullptr;
static std::atomic<bool> g_inEndScene(false);
static std::atomic<bool> g_imguiInitializing(false);
static std::atomic<bool> g_imguiInitialized(false);

ImGuiManager& ImGuiManager::Get() {
    static ImGuiManager instance;
    return instance;
}

bool ImGuiManager::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[ImGuiManager] Starting D3D9 hook initialization");
    g_instance = this;

    try {
        // Create D3D9 interface
        IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
        if (!pD3D) {
            LOG_ERROR("[ImGuiManager] Failed to create D3D9 interface");
            return false;
        }

        LOG_INFO("[ImGuiManager] Created D3D9 interface");

        // Create a minimal hidden window
        WNDCLASS wc = {};
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"S3MM_DummyWndClass";

        if (!RegisterClass(&wc)) {
            DWORD regErr = GetLastError();
            if (regErr != ERROR_CLASS_ALREADY_EXISTS) {
                LOG_ERROR("[ImGuiManager] RegisterClass failed");
                pD3D->Release();
                return false;
            }
        }

        HWND dummyWnd = CreateWindowEx(0, wc.lpszClassName, L"S3MM Dummy", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, wc.hInstance, nullptr);

        D3DPRESENT_PARAMETERS d3dpp = {};
        d3dpp.Windowed = TRUE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.hDeviceWindow = dummyWnd;

        IDirect3DDevice9* pDevice = nullptr;
        // Thank u previous me in S3SS xoxo
        // We only need the vtable (implemented by the d3d9 runtime, identical across device types), and a HAL device drags the whole GPU user-mode driver into the process just to be thrown away :D LOVE THAT YAAAAY
        HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, d3dpp.hDeviceWindow, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDevice);
        if (FAILED(hr) || !pDevice) {
            // Some setups might reject NULLREF so fall back to HAL rather than losing the overlay entirely
            LOG_WARN("[Init] NULLREF device creation failed, falling back to HAL");
            hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d3dpp.hDeviceWindow, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDevice);
        }

        if (FAILED(hr) || !pDevice) {
            LOG_ERROR("[ImGuiManager] Failed to create D3D device");
            if (dummyWnd) DestroyWindow(dummyWnd);
            pD3D->Release();
            return false;
        }

        LOG_INFO("[ImGuiManager] Created D3D device");

        // Get vtable
        void** vTable = *reinterpret_cast<void***>(pDevice);
        m_originalEndScene = reinterpret_cast<EndScene_t>(vTable[42]);
        m_originalReset = reinterpret_cast<Reset_t>(vTable[16]);

        // Cleanup dummy objects
        pDevice->Release();
        pD3D->Release();
        if (dummyWnd) DestroyWindow(dummyWnd);

        LOG_INFO("[ImGuiManager] Starting Detours transaction");

        // Install hooks
        if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR || DetourAttach(&(PVOID&)m_originalEndScene, HookedEndScene) != NO_ERROR ||
            DetourAttach(&(PVOID&)m_originalReset, HookedReset) != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {

            LOG_ERROR("[ImGuiManager] Failed to attach D3D hooks");
            return false;
        }

        m_initialized = true;
        LOG_INFO("[ImGuiManager] D3D9 hook initialization completed");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("[ImGuiManager] Exception: " + std::string(e.what()));
        return false;
    } catch (...) {
        LOG_ERROR("[ImGuiManager] Unknown exception during initialization");
        return false;
    }
}

void ImGuiManager::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("[ImGuiManager] Shutting down...");

    // Restore window proc
    if (m_hwnd && m_originalWndProc) {
        LOG_INFO("[ImGuiManager] Restoring original window procedure");
        SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_originalWndProc);
    }

    // Remove hooks
    if (m_originalEndScene || m_originalReset) {
        LOG_INFO("[ImGuiManager] Detaching D3D hooks");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        if (m_originalEndScene) DetourDetach(&(PVOID&)m_originalEndScene, HookedEndScene);
        if (m_originalReset) DetourDetach(&(PVOID&)m_originalReset, HookedReset);
        DetourTransactionCommit();
    }

    // Shutdown ImGui
    if (m_imguiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_imguiInitialized = false;
    }

    m_initialized = false;
    g_instance = nullptr;
    g_imguiInitialized.store(false);
    LOG_INFO("[ImGuiManager] Shutdown complete");
}

void ImGuiManager::InitImGui(LPDIRECT3DDEVICE9 pDevice) {
    LOG_INFO("[ImGuiManager] Initializing ImGui context");

    m_device = pDevice;

    // Get the correct window handle
    D3DDEVICE_CREATION_PARAMETERS params;
    if (FAILED(pDevice->GetCreationParameters(&params))) {
        LOG_ERROR("[ImGuiManager] Failed to get device creation parameters");
        g_imguiInitializing.store(false);
        return;
    }

    HWND gameWindow = params.hFocusWindow;

    // Fallback via swap chain if needed
    if (!gameWindow) {
        IDirect3DSwapChain9* pSwapChain = nullptr;
        if (SUCCEEDED(pDevice->GetSwapChain(0, &pSwapChain)) && pSwapChain) {
            D3DPRESENT_PARAMETERS pp = {};
            if (SUCCEEDED(pSwapChain->GetPresentParameters(&pp)) && pp.hDeviceWindow) { gameWindow = pp.hDeviceWindow; }
            pSwapChain->Release();
        }
    }

    if (!gameWindow) {
        LOG_ERROR("[ImGuiManager] No valid window handle found");
        g_imguiInitializing.store(false);
        return;
    }

    m_hwnd = gameWindow;

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Hook window procedure
    SetLastError(0);
    m_originalWndProc = (WNDPROC)SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    DWORD wndProcErr = GetLastError();
    if (!m_originalWndProc && wndProcErr != ERROR_SUCCESS) { LOG_ERROR("[ImGuiManager] SetWindowLongPtr failed"); }
    LOG_INFO("[ImGuiManager] Window procedure hooked");

    // Oversample the default font (ProggyClean) for crisp downscaling.
    // Bake at 13 * OVERSAMPLE, then divide FontGlobalScale by OVERSAMPLE
    // so it displays at native size but has headroom to scale up for spacious themes.
    {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * kFontOversample;
        io.Fonts->AddFontDefault(&cfg);
        io.FontGlobalScale = 1.0f / kFontOversample;
    }

    // Load themes from TOML and apply saved theme
    auto& themeLoader = ThemeLoader::Get();
    themeLoader.Load();
    std::string savedTheme = Config::Get().GetTheme();
    themeLoader.ApplyTheme(savedTheme);
    LOG_INFO("[ImGuiManager] Applied theme: " + savedTheme);

    ImGui_ImplWin32_Init(gameWindow);
    ImGui_ImplDX9_Init(pDevice);

    m_imguiInitialized = true;
    g_imguiInitialized.store(true);
    g_imguiInitializing.store(false);
    LOG_INFO("[ImGuiManager] ImGui initialized");
}

void ImGuiManager::RenderFrame() {
    if (!m_imguiInitialized || !m_visible) return;

    try {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Call registered render callback
        if (m_renderCallback) { m_renderCallback(); }

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    } catch (const std::exception& e) { LOG_ERROR("[ImGuiManager] Render exception: " + std::string(e.what())); } catch (...) {
        LOG_ERROR("[ImGuiManager] Unknown render exception");
    }
}

// HOOKED FUNCTIONS - D3D9 and WndProc hook implementations
HRESULT __stdcall ImGuiManager::HookedEndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (!pDevice || !g_instance) { return g_instance ? g_instance->m_originalEndScene(pDevice) : D3D_OK; }

    // Prevent re-entrancy
    if (g_inEndScene.exchange(true)) { return g_instance->m_originalEndScene(pDevice); }

    // Check device state
    HRESULT coop = pDevice->TestCooperativeLevel();
    if (FAILED(coop) && coop != D3DERR_DEVICENOTRESET) {
        g_inEndScene.store(false);
        return g_instance->m_originalEndScene(pDevice);
    }

    // Initialize ImGui on first valid EndScene
    if (!g_imguiInitialized.load()) {
        bool expected = false;
        if (g_imguiInitializing.compare_exchange_strong(expected, true)) { g_instance->InitImGui(pDevice); }
    }

    // Render
    if (g_imguiInitialized.load()) { g_instance->RenderFrame(); }

    HRESULT result = g_instance->m_originalEndScene(pDevice);
    g_inEndScene.store(false);
    return result;
}

HRESULT __stdcall ImGuiManager::HookedReset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* params) {
    if (g_imguiInitialized.load()) { ImGui_ImplDX9_InvalidateDeviceObjects(); }

    HRESULT hr = g_instance->m_originalReset(pDevice, params);

    if (SUCCEEDED(hr) && g_imguiInitialized.load()) { ImGui_ImplDX9_CreateDeviceObjects(); }

    return hr;
}

LRESULT CALLBACK ImGuiManager::HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Pass events to ImGui
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) { return 0; }

    // Toggle visibility with configurable key (default: HOME)
    if (uMsg == WM_KEYDOWN && wParam == Config::Get().GetToggleKey()) {
        if (g_instance) {
            g_instance->ToggleVisible();
            return 0;
        }
    }

    // Block input when UI is visible
    if (g_instance && g_instance->m_visible) {
        switch (uMsg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEMOVE:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
            return 0;
        }
    }

    // Call original
    if (g_instance && g_instance->m_originalWndProc) { return CallWindowProc(g_instance->m_originalWndProc, hWnd, uMsg, wParam, lParam); }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

} // namespace S3MM
