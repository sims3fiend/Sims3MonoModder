#pragma once
#include <Windows.h>
#include <d3d9.h>
#include <functional>

namespace S3MM {

// IMGUI MANAGER - Handles D3D9 hooking and ImGui lifecycle
class ImGuiManager {
  public:
    using RenderCallback = std::function<void()>;

    static ImGuiManager& Get();

    // Initialize the D3D9 hook and ImGui
    bool Initialize();
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    bool IsVisible() const { return m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }
    void ToggleVisible() { m_visible = !m_visible; }

    // Register a callback to be called during rendering
    void SetRenderCallback(RenderCallback callback) { m_renderCallback = callback; }

    // Get the D3D device (for custom rendering)
    LPDIRECT3DDEVICE9 GetDevice() const { return m_device; }

  private:
    ImGuiManager() = default;
    ~ImGuiManager() = default;

    // D3D9 hook callbacks
    static HRESULT __stdcall HookedEndScene(LPDIRECT3DDEVICE9 pDevice);
    static HRESULT __stdcall HookedReset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* params);
    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void InitImGui(LPDIRECT3DDEVICE9 pDevice);
    void RenderFrame();

    // State
    bool m_initialized = false;
    bool m_visible = false;
    bool m_imguiInitialized = false;

    // D3D9
    LPDIRECT3DDEVICE9 m_device = nullptr;
    HWND m_hwnd = nullptr;
    WNDPROC m_originalWndProc = nullptr;

    // Original function pointers
    using EndScene_t = HRESULT(__stdcall*)(LPDIRECT3DDEVICE9);
    using Reset_t = HRESULT(__stdcall*)(LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS*);
    EndScene_t m_originalEndScene = nullptr;
    Reset_t m_originalReset = nullptr;

    // Render callback
    RenderCallback m_renderCallback = nullptr;
};

} // namespace S3MM
