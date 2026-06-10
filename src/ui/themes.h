#pragma once
#include "imgui.h"
#include <string>
#include <vector>

namespace S3MM {

// Font scaling, default font is baked at 13px * kFontOversample.
// FontGlobalScale = 1/kFontOversample at baseline (displays at native 13px).
// Themes/user settings multiply on top of that base scale.
constexpr float kFontOversample = 3.0f; // Atlas oversample factor (13*3 = 39px baked)
constexpr float kMinScale = 0.7f;       // Floor multiplier (never below ~9px visual)
constexpr float kMaxScale = 2.0f;       // Ceiling multiplier (never above ~26px visual)

// THEME DATA - Loaded from themes.toml at runtime
struct ThemeData {
    std::string name;
    ImGuiStyle style; // Fully populated style including colors
};

// THEME LOADER - Parses themes.toml and applies themes to ImGui
class ThemeLoader {
  public:
    static ThemeLoader& Get();

    // Load themes from embedded resource, falls back to built-in defaults.
    // Call once during init.
    bool Load();

    // Apply theme by index or name, then derive adaptive palette + font scale
    void ApplyTheme(int index);
    void ApplyTheme(const std::string& name);

    // Recompute FontGlobalScale from current style + config fontSize setting.
    // Call after changing font size in settings without switching themes.
    static void ApplyFontScale();

    // Accessors
    int GetThemeCount() const;
    const char* GetThemeName(int index) const;
    int GetThemeIndex(const std::string& name) const;
    bool IsLoaded() const { return !m_themes.empty(); }

  private:
    ThemeLoader() = default;
    std::vector<ThemeData> m_themes;

    // Parsing helpers
    static ImVec4 ParseRgba(const std::string& str);
    static int ColorNameToIndex(const std::string& name);

    // Parse TOML from string content
    bool LoadFromString(const std::string& content, const std::string& sourceName);

    // Load from embedded DLL resource
    bool LoadFromResource();

    // Populate with built-in ImGui themes as last-resort fallback
    void LoadBuiltinFallback();

    // Derive adaptive Colors palette from current WindowBg
    static void DeriveColorPalette();
};

// COLOR SYSTEM - Derives tinted colors from the active theme's text color.
// Only 4 base colors (accent, positive, negative, neutral) are stored.
// Everything else is derived at the call site via Dim/Brighten/Tint.
// Colors auto-adapt to light/dark themes since they inherit text luminance.
struct Colors {
    // Base palette, tinted from theme text color
    ImVec4 accent;   // Blue-ish:  info, links, static members, type refs
    ImVec4 positive; // Green-ish: success, ready, added, editable
    ImVec4 negative; // Red-ish:   errors, removed, invalid
    ImVec4 neutral;  // Amber-ish: warnings, loading, modified, type names

    static Colors& Get();

    // HSV-based derivation utilities
    static ImVec4 Dim(ImVec4 c, float amount = 0.3f);
    static ImVec4 Brighten(ImVec4 c, float amount = 0.2f);
    static ImVec4 Desaturate(ImVec4 c, float amount = 0.4f);
    static ImVec4 WithAlpha(ImVec4 c, float a);
    static ImVec4 Tint(ImVec4 base, float hue, float saturation = 0.5f);

    // Variadic text helper, replaces all the per-color TextFoo() wrappers
    static void Text(const ImVec4& col, const char* fmt, ...);
};

} // namespace S3MM
