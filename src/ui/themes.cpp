#include "themes.h"
#include "../resources/resource.h"
#include "../util/config.h"
#include "../util/logger.h"
#include "imgui.h"
#include <toml++/toml.hpp>
#include <cstdarg>
#include <cstdio>
#include <sstream>
#include <unordered_map>
#include <Windows.h>

namespace S3MM {

// COLOR NAME -> ImGuiCol MAPPING
static const std::unordered_map<std::string, int>& GetColorMap() {
    static const std::unordered_map<std::string, int> map = {
        {"Text", ImGuiCol_Text},
        {"TextDisabled", ImGuiCol_TextDisabled},
        {"WindowBg", ImGuiCol_WindowBg},
        {"ChildBg", ImGuiCol_ChildBg},
        {"PopupBg", ImGuiCol_PopupBg},
        {"Border", ImGuiCol_Border},
        {"BorderShadow", ImGuiCol_BorderShadow},
        {"FrameBg", ImGuiCol_FrameBg},
        {"FrameBgHovered", ImGuiCol_FrameBgHovered},
        {"FrameBgActive", ImGuiCol_FrameBgActive},
        {"TitleBg", ImGuiCol_TitleBg},
        {"TitleBgActive", ImGuiCol_TitleBgActive},
        {"TitleBgCollapsed", ImGuiCol_TitleBgCollapsed},
        {"MenuBarBg", ImGuiCol_MenuBarBg},
        {"ScrollbarBg", ImGuiCol_ScrollbarBg},
        {"ScrollbarGrab", ImGuiCol_ScrollbarGrab},
        {"ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered},
        {"ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive},
        {"CheckMark", ImGuiCol_CheckMark},
        {"SliderGrab", ImGuiCol_SliderGrab},
        {"SliderGrabActive", ImGuiCol_SliderGrabActive},
        {"Button", ImGuiCol_Button},
        {"ButtonHovered", ImGuiCol_ButtonHovered},
        {"ButtonActive", ImGuiCol_ButtonActive},
        {"Header", ImGuiCol_Header},
        {"HeaderHovered", ImGuiCol_HeaderHovered},
        {"HeaderActive", ImGuiCol_HeaderActive},
        {"Separator", ImGuiCol_Separator},
        {"SeparatorHovered", ImGuiCol_SeparatorHovered},
        {"SeparatorActive", ImGuiCol_SeparatorActive},
        {"ResizeGrip", ImGuiCol_ResizeGrip},
        {"ResizeGripHovered", ImGuiCol_ResizeGripHovered},
        {"ResizeGripActive", ImGuiCol_ResizeGripActive},
        {"Tab", ImGuiCol_Tab},
        {"TabHovered", ImGuiCol_TabHovered},
        {"TabActive", ImGuiCol_TabActive},
        {"TabUnfocused", ImGuiCol_TabUnfocused},
        {"TabUnfocusedActive", ImGuiCol_TabUnfocusedActive},
        {"PlotLines", ImGuiCol_PlotLines},
        {"PlotLinesHovered", ImGuiCol_PlotLinesHovered},
        {"PlotHistogram", ImGuiCol_PlotHistogram},
        {"PlotHistogramHovered", ImGuiCol_PlotHistogramHovered},
        {"TableHeaderBg", ImGuiCol_TableHeaderBg},
        {"TableBorderStrong", ImGuiCol_TableBorderStrong},
        {"TableBorderLight", ImGuiCol_TableBorderLight},
        {"TableRowBg", ImGuiCol_TableRowBg},
        {"TableRowBgAlt", ImGuiCol_TableRowBgAlt},
        {"TextSelectedBg", ImGuiCol_TextSelectedBg},
        {"DragDropTarget", ImGuiCol_DragDropTarget},
        {"NavHighlight", ImGuiCol_NavHighlight},
        {"NavWindowingHighlight", ImGuiCol_NavWindowingHighlight},
        {"NavWindowingDimBg", ImGuiCol_NavWindowingDimBg},
        {"ModalWindowDimBg", ImGuiCol_ModalWindowDimBg},
    };
    return map;
}

int ThemeLoader::ColorNameToIndex(const std::string& name) {
    auto& map = GetColorMap();
    auto it = map.find(name);
    return (it != map.end()) ? it->second : -1;

} // RGBA STRING PARSER
ImVec4 ThemeLoader::ParseRgba(const std::string& str) {
    float r = 0, g = 0, b = 0, a = 1.0f;
    // Parse all components as floats: handles both 0-255 int and 0.0-1.0 float formats
    if (sscanf(str.c_str(), "rgba(%f, %f, %f, %f)", &r, &g, &b, &a) >= 3 ||
        sscanf(str.c_str(), "rgba(%f,%f,%f,%f)", &r, &g, &b, &a) >= 3) {
        // If any RGB channel exceeds 1.0, treat as 0-255 range
        if (r > 1.0f || g > 1.0f || b > 1.0f) { r /= 255.0f; g /= 255.0f; b /= 255.0f; }
        return ImVec4(r, g, b, a);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Fallback white
}

// THEME LOADER
ThemeLoader& ThemeLoader::Get() {
    static ThemeLoader instance;
    return instance;
}

bool ThemeLoader::Load() {
    // Load from embedded resource
    LOG_INFO("[ThemeLoader] Using embedded themes resource");
    if (LoadFromResource()) return true;

    // Last resort
    LoadBuiltinFallback();
    return false;
}

bool ThemeLoader::LoadFromResource() {
    // Get our DLL's HMODULE
    HMODULE hModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ThemeLoader::Get, &hModule);
    if (!hModule) {
        LOG_ERROR("[ThemeLoader] Failed to get DLL module handle");
        return false;
    }

    HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(IDR_THEMES_TOML), RT_RCDATA);
    if (!hRes) {
        LOG_ERROR("[ThemeLoader] Embedded themes resource not found");
        return false;
    }

    HGLOBAL hData = LoadResource(hModule, hRes);
    DWORD size = SizeofResource(hModule, hRes);
    if (!hData || size == 0) {
        LOG_ERROR("[ThemeLoader] Failed to load embedded themes resource");
        return false;
    }

    const char* data = static_cast<const char*>(LockResource(hData));
    std::string content(data, size);
    return LoadFromString(content, "embedded");
}

bool ThemeLoader::LoadFromString(const std::string& content, const std::string& sourceName) {
    m_themes.clear();

    try {
        toml::table root = toml::parse(content, sourceName);
        auto* themes = root.get_as<toml::array>("themes");
        if (!themes) {
            LOG_ERROR("[ThemeLoader] No [[themes]] array in " + sourceName);
            return false;
        }

        m_themes.reserve(themes->size());

        for (auto& entry : *themes) {
            auto* table = entry.as_table();
            if (!table) continue;

            ThemeData td;

            // Name
            if (auto* n = table->get_as<std::string>("name")) {
                td.name = n->get();
            } else {
                td.name = "Unnamed";
            }

            // Start with ImGui defaults
            ImGui::StyleColorsDark();
            td.style = ImGui::GetStyle();

            // Parse style section
            auto* styleTable = table->get_as<toml::table>("style");
            if (styleTable) {
                ImGuiStyle& s = td.style;

                // Float fields
                auto readFloat = [&](const char* key, float& dst) {
                    if (auto* v = styleTable->get_as<double>(key)) dst = (float)v->get();
                };

                readFloat("alpha", s.Alpha);
                readFloat("disabledAlpha", s.DisabledAlpha);
                readFloat("windowRounding", s.WindowRounding);
                readFloat("windowBorderSize", s.WindowBorderSize);
                readFloat("childRounding", s.ChildRounding);
                readFloat("childBorderSize", s.ChildBorderSize);
                readFloat("popupRounding", s.PopupRounding);
                readFloat("popupBorderSize", s.PopupBorderSize);
                readFloat("frameRounding", s.FrameRounding);
                readFloat("frameBorderSize", s.FrameBorderSize);
                readFloat("indentSpacing", s.IndentSpacing);
                readFloat("columnsMinSpacing", s.ColumnsMinSpacing);
                readFloat("scrollbarSize", s.ScrollbarSize);
                readFloat("scrollbarRounding", s.ScrollbarRounding);
                readFloat("grabMinSize", s.GrabMinSize);
                readFloat("grabRounding", s.GrabRounding);
                readFloat("tabRounding", s.TabRounding);
                readFloat("tabBorderSize", s.TabBorderSize);
                readFloat("tabMinWidthForCloseButton", s.TabMinWidthForCloseButton);

                // ImVec2 fields
                auto readVec2 = [&](const char* key, ImVec2& dst) {
                    if (auto* arr = styleTable->get_as<toml::array>(key)) {
                        if (arr->size() >= 2) {
                            dst.x = (float)arr->get_as<double>(0)->get();
                            dst.y = (float)arr->get_as<double>(1)->get();
                        }
                    }
                };

                readVec2("windowPadding", s.WindowPadding);
                readVec2("windowMinSize", s.WindowMinSize);
                readVec2("windowTitleAlign", s.WindowTitleAlign);
                readVec2("framePadding", s.FramePadding);
                readVec2("itemSpacing", s.ItemSpacing);
                readVec2("itemInnerSpacing", s.ItemInnerSpacing);
                readVec2("cellPadding", s.CellPadding);
                readVec2("buttonTextAlign", s.ButtonTextAlign);
                readVec2("selectableTextAlign", s.SelectableTextAlign);

                // Direction fields (string -> ImGuiDir)
                auto readDir = [&](const char* key, ImGuiDir& dst) {
                    if (auto* v = styleTable->get_as<std::string>(key)) {
                        if (v->get() == "Left")
                            dst = ImGuiDir_Left;
                        else if (v->get() == "Right")
                            dst = ImGuiDir_Right;
                        else if (v->get() == "Up")
                            dst = ImGuiDir_Up;
                        else if (v->get() == "Down")
                            dst = ImGuiDir_Down;
                        else
                            dst = ImGuiDir_None;
                    }
                };

                readDir("windowMenuButtonPosition", s.WindowMenuButtonPosition);
                readDir("colorButtonPosition", s.ColorButtonPosition);

                // Colors section
                auto* colorTable = styleTable->get_as<toml::table>("colors");
                if (colorTable) {
                    for (auto& [key, val] : *colorTable) {
                        if (auto* str = val.as_string()) {
                            int idx = ColorNameToIndex(std::string(key));
                            if (idx >= 0) { s.Colors[idx] = ParseRgba(str->get()); }
                        }
                    }
                }
            }

            m_themes.push_back(std::move(td));
        }

        LOG_INFO("[ThemeLoader] Loaded " + std::to_string(m_themes.size()) + " themes from " + sourceName);
        return true;

    } catch (const toml::parse_error& err) {
        LOG_ERROR("[ThemeLoader] Parse error in " + sourceName + ": " + std::string(err.what()));
        return false;

    } catch (const std::exception& err) {
        LOG_ERROR("[ThemeLoader] Error loading " + sourceName + ": " + std::string(err.what()));
        return false;
    }
}

void ThemeLoader::LoadBuiltinFallback() {
    m_themes.clear();

    // Dark
    {
        ThemeData td;
        td.name = "Dark";
        ImGui::StyleColorsDark();
        td.style = ImGui::GetStyle();
        m_themes.push_back(std::move(td));
    }
    // Light
    {
        ThemeData td;
        td.name = "Light";
        ImGui::StyleColorsLight();
        td.style = ImGui::GetStyle();
        m_themes.push_back(std::move(td));
    }
    // Classic
    {
        ThemeData td;
        td.name = "Classic";
        ImGui::StyleColorsClassic();
        td.style = ImGui::GetStyle();
        m_themes.push_back(std::move(td));
    }

    LOG_INFO("[ThemeLoader] Using 3 built-in fallback themes");
}

void ThemeLoader::ApplyTheme(int index) {
    if (index < 0 || index >= (int)m_themes.size()) {
        LOG_ERROR("[ThemeLoader] Theme index " + std::to_string(index) + " out of range");
        if (!m_themes.empty())
            index = 0;
        else
            return;
    }

    ImGui::GetStyle() = m_themes[index].style;
    ApplyFontScale();
    DeriveColorPalette();
}

int ThemeLoader::GetThemeCount() const {
    return (int)m_themes.size();
}

const char* ThemeLoader::GetThemeName(int index) const {
    if (index < 0 || index >= (int)m_themes.size()) return "Unknown";
    return m_themes[index].name.c_str();
}

int ThemeLoader::GetThemeIndex(const std::string& name) const {
    for (int i = 0; i < (int)m_themes.size(); i++) {
        if (m_themes[i].name == name) return i;
    }
    return -1;
}

void ThemeLoader::ApplyTheme(const std::string& name) {
    int idx = GetThemeIndex(name);
    if (idx < 0) {
        LOG_ERROR("[ThemeLoader] Theme not found: " + name + ", falling back to first");
        idx = 0;
    }
    ApplyTheme(idx);
}

// FONT SCALING
void ThemeLoader::ApplyFontScale() {
    float baseScale = 1.0f / kFontOversample; // Native 13px display size
    int userSize = Config::Get().GetFontSize();
    float multiplier;

    if (userSize > 0) {
        // Explicit: user wants N px, native is 13px
        multiplier = static_cast<float>(userSize) / 13.0f;
    } else {
        // Native 13px display size, theme doesn't influence font
        multiplier = 1.0f;
    }

    multiplier = (multiplier < kMinScale) ? kMinScale : (multiplier > kMaxScale) ? kMaxScale : multiplier;
    ImGui::GetIO().FontGlobalScale = baseScale * multiplier;
}

// ADAPTIVE COLOR PALETTE
static Colors s_colors;

void ThemeLoader::DeriveColorPalette() {
    // Derive palette from theme background brightness.
    // Dark bg -> bright tinted colors, light bg -> medium saturated colors.
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    float bgH, bgS, bgV;
    ImGui::ColorConvertRGBtoHSV(bg.x, bg.y, bg.z, bgH, bgS, bgV);
    float v = bgV < 0.5f ? 0.9f : 0.55f;
    float s = bgV < 0.5f ? 0.55f : 0.7f;

    auto& c = s_colors;
    auto T = [v, s](float hue) {
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, s, v, r, g, b);
        return ImVec4(r, g, b, 1.0f);
    };
    c.accent = T(0.58f);   // Blue
    c.positive = T(0.35f); // Green
    c.negative = T(0.0f);  // Red
    c.neutral = T(0.12f);  // Amber
}

// COLOR SYSTEM
Colors& Colors::Get() {
    return s_colors;
}

void Colors::Text(const ImVec4& col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::TextColoredV(col, fmt, args);
    va_end(args);
}

ImVec4 Colors::Tint(ImVec4 base, float hue, float saturation) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(base.x, base.y, base.z, h, s, v);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, saturation, v, r, g, b);
    return ImVec4(r, g, b, base.w);
}

ImVec4 Colors::Dim(ImVec4 c, float amount) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    v = (v - amount > 0.0f) ? v - amount : 0.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImVec4(r, g, b, c.w);
}

ImVec4 Colors::Brighten(ImVec4 c, float amount) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    v = (v + amount < 1.0f) ? v + amount : 1.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImVec4(r, g, b, c.w);
}

ImVec4 Colors::Desaturate(ImVec4 c, float amount) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    s = (s - amount > 0.0f) ? s - amount : 0.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImVec4(r, g, b, c.w);
}

ImVec4 Colors::WithAlpha(ImVec4 c, float a) {
    return ImVec4(c.x, c.y, c.z, a);
}

} // namespace S3MM
