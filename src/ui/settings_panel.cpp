#include "settings_panel.h"
#include "themes.h"
#include "../util/config.h"
#include "../util/logger.h"
#include "../core/mono_bridge.h"
#include "../core/vanilla_extractor.h"
#include "imgui.h"

namespace S3MM {

void SettingsPanel::Render() {
    RenderTheme();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    RenderKeyBindings();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    RenderDiagnostics();
}

void SettingsPanel::RenderTheme() {
    ImGui::Text("Theme");
    ImGui::Separator();

    auto& config = Config::Get();
    std::string currentTheme = config.GetTheme();

    ImGui::SetNextItemWidth(200);
    auto& loader = ThemeLoader::Get();
    int currentIndex = loader.GetThemeIndex(currentTheme);
    const char* preview = (currentIndex >= 0) ? loader.GetThemeName(currentIndex) : currentTheme.c_str();
    if (ImGui::BeginCombo("##theme", preview)) {
        for (int i = 0; i < loader.GetThemeCount(); i++) {
            bool isSelected = (i == currentIndex);
            if (ImGui::Selectable(loader.GetThemeName(i), isSelected)) {
                config.SetTheme(loader.GetThemeName(i));
                loader.ApplyTheme(i);
            }
            if (isSelected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // Font size selector
    int fontSize = config.GetFontSize();
    struct FontOption {
        int size;
        const char* label;
    };
    static const FontOption fontOptions[] = {
        {11, "11px"},
        {12, "12px"},
        {13, "13px"},
        {14, "14px"},
        {15, "15px"},
        {16, "16px"},
        {17, "17px"},
        {18, "18px"},
        {20, "20px"},
        {22, "22px"},
    };
    static const int fontOptionCount = sizeof(fontOptions) / sizeof(fontOptions[0]);

    // Find current selection
    int fontIdx = 0;
    for (int i = 0; i < fontOptionCount; i++) {
        if (fontOptions[i].size == fontSize) {
            fontIdx = i;
            break;
        }
    }

    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##fontsize", fontOptions[fontIdx].label)) {
        for (int i = 0; i < fontOptionCount; i++) {
            bool isSelected = (i == fontIdx);
            if (ImGui::Selectable(fontOptions[i].label, isSelected)) {
                config.SetFontSize(fontOptions[i].size);
                ThemeLoader::ApplyFontScale();
            }
            if (isSelected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Font Size");

    ImGui::Spacing();
    ImGui::TextDisabled("Themes from github.com/Patitotective/ImThemes");
}

void SettingsPanel::RenderKeyBindings() {
    ImGui::Text("Key Bindings");
    ImGui::Separator();

    auto& config = Config::Get();
    UINT currentKey = config.GetToggleKey();
    std::string currentKeyName = Config::GetKeyName(currentKey);

    ImGui::Text("Toggle UI Key:");
    ImGui::SameLine();

    if (m_waitingForKey) {
        // Waiting for key press
        Colors::Text(Colors::Get().neutral, "Press any key...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { m_waitingForKey = false; }

        // Check for key press (common keys)
        int keyOptions[] = {VK_HOME, VK_INSERT, VK_END, VK_DELETE, VK_PRIOR, VK_NEXT, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, VK_PAUSE, VK_SCROLL, VK_OEM_3};

        for (int key : keyOptions) {
            if (GetAsyncKeyState(key) & 0x8000) {
                config.SetToggleKey(key);
                m_waitingForKey = false;
                LOG_INFO("[Settings] Toggle key changed to: " + Config::GetKeyName(key));
                break;
            }
        }
    } else {
        // Show current key with dropdown
        ImGui::SetNextItemWidth(150);

        int keyCount = 0;
        const Config::KeyOption* options = Config::GetKeyOptions(&keyCount);

        // Find current selection index
        int currentIndex = 0;
        for (int i = 0; i < keyCount; i++) {
            if (options[i].vkCode == currentKey) {
                currentIndex = i;
                break;
            }
        }

        if (ImGui::BeginCombo("##togglekey", currentKeyName.c_str())) {
            for (int i = 0; i < keyCount; i++) {
                bool isSelected = (i == currentIndex);
                if (ImGui::Selectable(options[i].name, isSelected)) { config.SetToggleKey(options[i].vkCode); }
                if (isSelected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Press Key...")) { m_waitingForKey = true; }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Note: S3SS uses INSERT by default.");
    ImGui::TextDisabled("Use a different key if running both.");
}

void SettingsPanel::RenderDiagnostics() {
    ImGui::Text("Tools");
    ImGui::Separator();

    if (ImGui::Button("Dump Icall Table")) {
        MonoBridge::Get().DumpIcallTable();
        LOG_INFO("[Settings] Icall dump requested - check S3MM.log");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(check S3MM.log)");

    ImGui::Spacing();

    if (ImGui::Button("Extract Vanilla Libs")) {
        LOG_INFO("[Settings] Starting vanilla library extraction...");
        int count = ExtractVanillaLibraries();
        LOG_INFO("[Settings] Extracted " + std::to_string(count) + " assemblies - check S3MM/Libs/");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(extracts DLLs to S3MM/Libs/)");
}

} // namespace S3MM
