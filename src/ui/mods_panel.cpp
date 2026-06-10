#include "mods_panel.h"
#include "settings_panel.h"
#include "main_window.h"
#include "../modding/mod_manager.h"
#include "../modding/assembly_diff.h"
#include "../modding/conflict_detector.h"
#include "../core/mono_bridge.h"
#include "../core/assembly_hooks.h"
#include "../core/script_host_hook.h"
#include "../util/logger.h"
#include "../util/config.h"
#include "themes.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace S3MM {

void ModsPanel::Render() {
    // TAB BAR - Mods, Logs, Settings
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    float statusBarHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    float tabContentHeight = availSize.y - statusBarHeight;

    if (ImGui::BeginTabBar("ModsHubTabs")) {
        if (ImGui::BeginTabItem("Mods")) {
            ImGui::BeginChild("ModsTabContent", ImVec2(-1, tabContentHeight - ImGui::GetFrameHeightWithSpacing()), false);
            RenderModsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Logs")) {
            ImGui::BeginChild("LogsTabContent", ImVec2(-1, tabContentHeight - ImGui::GetFrameHeightWithSpacing()), false);
            RenderLogsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Settings")) {
            ImGui::BeginChild("SettingsTabContent", ImVec2(-1, tabContentHeight - ImGui::GetFrameHeightWithSpacing()), false);
            RenderSettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // STATUS BAR
    RenderStatusBar();

    // OVERRIDES WINDOW - separate window
    if (m_showOverridesWindow) { RenderOverridesWindow(); }
}

void ModsPanel::RenderModsTab() {
    RenderModsSection();
}

void ModsPanel::RenderLogsTab() {
    RenderLogsSection();
}

void ModsPanel::RenderSettingsTab() {
    // Delegate to settings panel if available
    if (m_settingsPanel) { m_settingsPanel->Render(); }
}

void ModsPanel::RenderStatusBar() {
    ImGui::Separator();

    auto phase = ScriptHostHook::Get().GetPhase();
    std::string toggleKeyName = Config::GetKeyName(Config::Get().GetToggleKey());

    if (phase == InitPhase::Ready) {
        Colors::Text(Colors::Get().positive, "Ready");
    } else {
        // Spinner: rotate through |/-\ ~4 fps
        const char* spinner = "|/-\\";
        int idx = (int)(ImGui::GetTime() * 4.0f) % 4;
        Colors::Text(Colors::Get().neutral, "%c Waiting for startup...", spinner[idx]);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("| %s to toggle", toggleKeyName.c_str());
}

void ModsPanel::RenderModsSection() {
    // Tab bar for S3MM Mods vs Detected Mods
    if (ImGui::BeginTabBar("ModsTabs")) {
        // S3MM MODS TAB
        if (ImGui::BeginTabItem("S3MM Mods")) {
            // Action buttons
            if (ImGui::Button("Scan for New Mods")) { ScanForNewMods(); }

            ImGui::Separator();

            // Summary stats
            auto& manager = ModManager::Get();
            const auto& status = manager.GetStatus();
            const auto& loadedMods = manager.GetLoader().GetLoadedMods();
            const auto& chains = manager.GetHookManager().GetAllChains();

            // Count total hooks
            int totalHooks = 0;
            for (const auto& [ptr, chain] : chains) { totalHooks += (int)chain.hooks.size(); }

            ImGui::Text("%d mods loaded | %d hooks active", (int)loadedMods.size(), totalHooks);

            if (!status.errors.empty()) {
                ImGui::SameLine();
                Colors::Text(Colors::Get().negative, "| %d errors", (int)status.errors.size());
            }

            ImGui::Separator();

            // Mods list
            ImGui::BeginChild("S3MMModsList", ImVec2(-1, -1), false);

            if (loadedMods.empty()) {
                ImGui::TextDisabled("No mods loaded");
                ImGui::TextDisabled("Place mods in S3MM/Mods/ folder");
            } else {
                for (const auto& mod : loadedMods) { RenderModEntry(mod); }
            }

            // Show source mods that failed to compile (diagnostics go to S3MM.log)
            {
                const auto& discovered = manager.GetDiscoveredMods();
                bool anyFailed = false;
                for (const auto& m : discovered) {
                    if (m.hasSourceFiles && !m.compiledSuccessfully) {
                        anyFailed = true;
                        break;
                    }
                }
                if (anyFailed) {
                    ImGui::Separator();
                    for (const auto& m : discovered) {
                        if (!m.hasSourceFiles || m.compiledSuccessfully) continue;
                        Colors::Text(Colors::Get().negative, "%s - Failed to compile (check log)", m.name.c_str());
                    }
                }
            }

            // Show errors if any
            if (!status.errors.empty()) {
                ImGui::Separator();
                Colors::Text(Colors::Get().negative, "Errors:");
                for (const auto& error : status.errors) { ImGui::TextWrapped("  %s", error.c_str()); }
            }

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // EXTERNAL MODS TAB
        if (ImGui::BeginTabItem("External Mods")) {
            RenderDetectedMods();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void ModsPanel::RenderModEntry(const LoadedMod& mod) {
    // Count hooks for this mod
    int hookCount = 0;
    const auto& chains = ModManager::Get().GetHookManager().GetAllChains();
    for (const auto& [ptr, chain] : chains) {
        for (const auto& hook : chain.hooks) {
            if (hook.modName == mod.name) hookCount++;
        }
    }

    // Tree node for mod
    bool isExpanded = m_expandedMods.count(mod.name) > 0;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
    if (hookCount == 0) { flags |= ImGuiTreeNodeFlags_Leaf; }

    // Mod name with hook count
    std::string label = mod.name + " (" + std::to_string(hookCount) + " hooks)";

    bool nodeOpen = ImGui::TreeNodeEx(mod.name.c_str(), flags, "%s", label.c_str());

    // Track expansion state
    if (nodeOpen != isExpanded) {
        if (nodeOpen) {
            m_expandedMods.insert(mod.name);
        } else {
            m_expandedMods.erase(mod.name);
        }
    }

    // Right-click context menu
    if (ImGui::BeginPopupContextItem()) {
        if (mod.monoImage && ImGui::MenuItem("Browse in Class Browser")) { MainWindow::Get().BrowseAssembly(mod.monoImage); }
        if (ImGui::MenuItem("Hot-Reload")) {
            LOG_INFO("[ModsPanel] Hot-reloading: " + mod.name);
            ModLoader::Get().HotReloadMod(mod.name);
        }
        ImGui::EndPopup();
    }

    // Show hooks if expanded
    if (nodeOpen) {
        RenderHooksForMod(mod.name);
        ImGui::TreePop();
    }
}

void ModsPanel::RenderHooksForMod(const std::string& modName) {
    const auto& chains = ModManager::Get().GetHookManager().GetAllChains();

    // Collect all hooks for this mod
    struct HookDisplay {
        std::string type;
        std::string targetClass;
        std::string targetMethod;
        std::string displayTarget;
        int priority;
    };
    std::vector<HookDisplay> hooks;

    for (const auto& [ptr, chain] : chains) {
        for (const auto& hook : chain.hooks) {
            if (hook.modName == modName) {
                std::string typeName;
                switch (hook.type) {
                case HookType::Watcher:
                    typeName = "Watch";
                    break;
                default:
                    typeName = "Hook";
                    break;
                }
                hooks.push_back({typeName, chain.targetClass, chain.targetMethod, chain.targetClass + "." + chain.targetMethod, hook.priority});
            }
        }
    }

    // Render hooks
    int hookIdx = 0;
    for (const auto& hook : hooks) {
        // Color by type (theme-adaptive)
        const auto& hc = Colors::Get();
        ImVec4 color;
        if (hook.type == "Watch") {
            color = hc.accent; // Blue for watch
        } else if (hook.type == "Replace") {
            color = hc.negative; // Red for replace
        } else {
            color = hc.neutral; // Amber for other
        }

        ImGui::TextColored(color, "  %s:", hook.type.c_str());
        ImGui::SameLine();

        // Make the target clickable
        ImGui::PushID(hookIdx++);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::Get().accent);
        if (ImGui::SmallButton(hook.displayTarget.c_str())) { MainWindow::Get().NavigateToHook(hook.targetClass, hook.targetMethod); }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click to navigate to method"); }
        ImGui::PopID();

        if (hook.priority != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("[pri:%d]", hook.priority);
        }
    }

    if (hooks.empty()) { ImGui::TextDisabled("  (no hooks)"); }
}

void ModsPanel::RenderLogsSection() {
    float availWidth = ImGui::GetContentRegionAvail().x;

    // Row 1: Title and actions - align Clear to right edge
    float clearWidth = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2;
    float checkboxWidth = ImGui::CalcTextSize("Auto-scroll").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;

    ImGui::Text("Logs");
    ImGui::SameLine(availWidth - clearWidth - checkboxWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { ModLog::Get().Clear(); }

    // Row 2: Filters - use available width
    float sourceWidth = availWidth * 0.35f;
    float textWidth = availWidth - sourceWidth - ImGui::GetStyle().ItemSpacing.x;

    ImGui::SetNextItemWidth(sourceWidth);
    if (ImGui::BeginCombo("##source", m_sourceFilter.empty() ? "All" : m_sourceFilter.c_str())) {
        if (ImGui::Selectable("All", m_sourceFilter.empty())) { m_sourceFilter.clear(); }
        for (const auto& source : m_knownSources) {
            if (ImGui::Selectable(source.c_str(), m_sourceFilter == source)) { m_sourceFilter = source; }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(textWidth);
    ImGui::InputTextWithHint("##logfilter", "Filter...", m_logFilter, sizeof(m_logFilter));

    ImGui::Separator();

    // Log entries
    ImGui::BeginChild("LogEntries", ImVec2(-1, -1), false);

    // Refresh cache if needed
    size_t currentVersion = ModLog::Get().GetVersion();
    if (currentVersion != m_lastLogVersion) {
        m_cachedLogs = ModLog::Get().GetEntries();
        m_lastLogVersion = currentVersion;

        // Update known sources
        std::set<std::string> sources;
        for (const auto& entry : m_cachedLogs) { sources.insert(entry.source); }
        m_knownSources.assign(sources.begin(), sources.end());
    }

    // Filter text (lowercase for comparison)
    std::string filterLower = m_logFilter;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    // Render entries
    for (const auto& entry : m_cachedLogs) {
        // Apply source filter
        if (!m_sourceFilter.empty() && entry.source != m_sourceFilter) { continue; }

        // Apply text filter
        if (!filterLower.empty()) {
            std::string msgLower = entry.message;
            std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);
            if (msgLower.find(filterLower) == std::string::npos) { continue; }
        }

        // Format timestamp
        auto time = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(entry.timestamp.time_since_epoch()) % 1000;
        std::tm tm_buf;
        localtime_s(&tm_buf, &time);

        std::ostringstream timeStr;
        timeStr << std::put_time(&tm_buf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();

        // Color based on source (hash to color)
        size_t hash = std::hash<std::string>{}(entry.source);
        float hue = (hash % 360) / 360.0f;
        ImVec4 sourceColor = Colors::Tint(Colors::Get().accent, hue);

        // Severity badge + message tint, driven by the entry's level (not a text prefix)
        const char* levelTag = "INFO ";
        ImVec4 levelColor = Colors::Get().accent;
        ImVec4 msgColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        switch (entry.level) {
        case LogLevel::Debug:
            levelTag = "DEBUG";
            levelColor = Colors::Dim(msgColor);
            msgColor = levelColor;
            break;
        case LogLevel::Info:
            levelTag = "INFO ";
            levelColor = Colors::Get().accent;
            break;
        case LogLevel::Warn:
            levelTag = "WARN ";
            levelColor = Colors::Get().neutral;
            msgColor = levelColor;
            break;
        case LogLevel::Error:
            levelTag = "ERROR";
            levelColor = Colors::Get().negative;
            msgColor = levelColor;
            break;
        }

        // Render: "HH:MM:SS.mmm LEVEL [Source] Message"
        ImGui::TextDisabled("%s", timeStr.str().c_str());
        ImGui::SameLine();
        ImGui::TextColored(levelColor, "%s", levelTag);
        ImGui::SameLine();
        ImGui::TextColored(sourceColor, "[%s]", entry.source.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, msgColor);
        ImGui::TextWrapped("%s", entry.message.c_str());
        ImGui::PopStyleColor();
    }

    // Auto-scroll
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10) { ImGui::SetScrollHereY(1.0f); }

    ImGui::EndChild();
}

void ModsPanel::ScanForNewMods() {
    LOG_INFO("[ModsPanel] Scanning for new mods...");
    ModLog::Get().Log("[System]", "Scanning for new mods...");

    auto& compiler = ModManager::Get().GetCompiler();
    auto mods = compiler.DiscoverMods();

    const auto& loadedMods = ModLoader::Get().GetLoadedMods();

    // Compile on the UI thread (csc.exe is slow; must not block the sim frame). Collect the mods
    // to load - pointers into `mods`, which outlives the blocking RunOnSimThread call below.
    std::vector<ModInfo*> toLoad;
    for (auto& mod : mods) {
        bool alreadyLoaded = false;
        for (const auto& loaded : loadedMods) {
            if (loaded.name == mod.name) {
                alreadyLoaded = true;
                break;
            }
        }
        if (alreadyLoaded) continue;

        if (mod.hasSourceFiles && mod.needsRecompile) {
            if (compiler.CompileMod(mod)) {
                ModLog::Get().Log("[System]", "Compiled: " + mod.name);
            } else {
                ModLog::Get().Log("[System]", "Failed to compile: " + mod.name);
                continue;
            }
        }
        toLoad.push_back(&mod);
    }

    std::vector<std::string> newlyLoaded;

    // Load + hook + OnLoad on the sim thread. LoadMod runs the attribute scan, which constructs attribute instances in the interpreter (CustomAttrsGetAttr),
    // OnLoad runs managed code. Both must be serialized with the game's interpreter or they race it (see RunOnSimThread).
    if (!toLoad.empty()) {
        MonoBridge::Get().RunOnSimThread([&]() {
            for (ModInfo* mod : toLoad) {
                if (ModLoader::Get().LoadMod(*mod)) {
                    ModLog::Get().Log("[System]", "Loaded new mod: " + mod->name);
                    newlyLoaded.push_back(mod->name);
                }
            }

            if (newlyLoaded.empty()) return;

            // Apply hooks, then run the OnLoad lifecycle, mirroring ProcessMods. 
            // Without firing OnLoad here, a mod brought in via this path gets its hooks installed but never runs OnLoad
            auto& hookManager = ModManager::Get().GetHookManager();
            int hooks = hookManager.ApplyHooks();
            ModLog::Get().Log("[System]", "Applied " + std::to_string(hooks) + " new hooks");

            for (const auto& name : newlyLoaded) { ModLoader::Get().FireOnLoadCallbacks(name); }

            // OnLoad callbacks may register new hooks via Hook.RegisterByPointer():
            //  - ReinstallDispatchStubs: existing chains where hooks were added
            //  - ApplyHooks: brand-new chains created during OnLoad
            int postOnLoadReinstalled = hookManager.ReinstallDispatchStubs();
            int postOnLoadApplied = hookManager.ApplyHooks();
            if (postOnLoadReinstalled > 0 || postOnLoadApplied > 0) {
                ModLog::Get().Log("[System]", "Post-OnLoad: reinstalled " + std::to_string(postOnLoadReinstalled) + " stubs, applied " + std::to_string(postOnLoadApplied) + " new chains");
            }
        });
    }

    ModLog::Get().Log("[System]", "Scan complete. " + std::to_string(newlyLoaded.size()) + " new mods loaded.");
}

void ModsPanel::RefreshDetectedMods() {
    m_detectedMods.clear();

    auto& bridge = MonoBridge::Get();
    if (!bridge.IsRuntimeReady()) return;

    if (!bridge.AreImagesInitialized()) { bridge.InitializeAllImages(); }

    // Get assembly -> package mappings from our hooks
    auto assemblyPackages = GetAllAssemblyPackageSources();

    // Build reverse map: package path -> list of assemblies it provides
    std::map<std::string, std::vector<std::string>> packageAssemblies;
    for (const auto& [asmName, pkgPath] : assemblyPackages) { packageAssemblies[pkgPath].push_back(asmName); }

    // Core assemblies we care about
    static const char* kCoreAssemblies[] = {"ScriptCore", "SimIFace", "mscorlib", nullptr};

    // Build map: package path -> core assemblies it overrides
    std::map<std::string, std::vector<std::string>> packageCoreOverrides;
    for (const auto& [pkgPath, assemblies] : packageAssemblies) {
        // Only track mods (packages in Mods folder)
        if (pkgPath.find("\\Mods\\") == std::string::npos && pkgPath.find("/Mods/") == std::string::npos) { continue; }

        for (const auto& asmName : assemblies) {
            for (const char** core = kCoreAssemblies; *core; core++) {
                if (asmName == *core) {
                    packageCoreOverrides[pkgPath].push_back(asmName);
                    break;
                }
            }
        }
    }

    // Filter out known game/system assemblies only
    static const char* kGameAssemblies[] = {
        "mscorlib", "System", "System.Xml", "System.Core", "SimIFace", "ScriptCore", "UI", "Sims3GameplayObjects", "Sims3GameplaySystems", "Sims3StoreObjects", "Sims3Metadata", "S3MM.Helper", nullptr};

    // Collect S3MM mod images so we don't list them as external
    std::set<void*> s3mmImages;
    for (const auto& mod : ModLoader::Get().GetLoadedMods()) {
        if (mod.monoImage) s3mmImages.insert(mod.monoImage);
    }

    for (void* image : bridge.GetAllImages()) {
        if (!image) continue;

        const char* name = MonoBridge::GetImageName(image);
        if (!name) continue;

        // Skip S3MM mod images
        if (s3mmImages.count(image)) continue;

        // Check if it's a known game assembly
        bool isGameAssembly = false;
        for (const char** ga = kGameAssemblies; *ga; ga++) {
            if (strcmp(name, *ga) == 0) {
                isGameAssembly = true;
                break;
            }
        }

        if (!isGameAssembly) {
            DetectedMod mod;
            mod.name = name;
            mod.typeCount = bridge.GetTypeDefCount(image);
            mod.image = image;

            // Look up package source for this assembly
            auto it = assemblyPackages.find(name);
            if (it != assemblyPackages.end()) {
                mod.packagePath = it->second;

                // Check if this package also provides core overrides
                auto coreIt = packageCoreOverrides.find(mod.packagePath);
                if (coreIt != packageCoreOverrides.end()) { mod.overriddenCoreAssemblies = coreIt->second; }
            }

            m_detectedMods.push_back(mod);
        }
    }

    m_detectedModsLoaded = true;
    LOG_INFO("[ModsPanel] Detected " + std::to_string(m_detectedMods.size()) + " external mods");
}

void ModsPanel::RenderDetectedMods() {
    if (!m_detectedModsLoaded) { RefreshDetectedMods(); }

    // Header with count
    ImGui::Text("%d external mods detected", (int)m_detectedMods.size());
    ImGui::Separator();

    // CORE ASSEMBLY OVERRIDE WARNINGS
    auto conflicts = GetDetectedConflicts();
    if (!conflicts.empty()) {
        Colors::Text(Colors::Get().neutral, "Core Assembly Overrides (%d):", (int)conflicts.size());

        for (const auto& conflict : conflicts) {
            // Extract filename from the package path stored at detection time
            std::string sourcePackage = "<unknown>";
            std::string fullPackagePath = conflict.sourcePackage;
            if (!fullPackagePath.empty()) {
                size_t lastSlash = fullPackagePath.find_last_of("\\/");
                sourcePackage = (lastSlash != std::string::npos) ? fullPackagePath.substr(lastSlash + 1) : fullPackagePath;
            }

            std::string label = conflict.name + " (v" + conflict.existingVersion + ") <- " + sourcePackage;
            ImGui::BulletText("%s", label.c_str());

            // Right-click to view diff
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Right-click to view changes"); }
            if (ImGui::BeginPopupContextItem(("##core_override_" + conflict.name).c_str())) {
                if (ImGui::MenuItem("View Changes")) {
                    m_showOverridesWindow = true;
                    m_overridesModName = sourcePackage;
                    m_overridesPackagePath = fullPackagePath;
                    m_overridesCoreAssemblies = {conflict.name};
                    m_cachedDiffs.clear();
                    LOG_INFO("[ModsPanel] View Changes requested for core override: " + conflict.name);
                }
                if (ImGui::MenuItem("Log Diff to S3MM.log")) {
                    LOG_INFO("[ModsPanel] Computing diff for " + conflict.name + "...");
                    AssemblyDiffResult diff = ComputeAssemblyDiff(conflict.name.c_str());
                    if (diff.valid) {
                        LOG_INFO("[CoreOverride] " + conflict.name + " replaced by " + sourcePackage);
                        LOG_INFO("[CoreOverride]   Modified: " + std::to_string(diff.modifiedMethods.size()) + " | Added: " + std::to_string(diff.addedMethods.size()) +
                                 " | Removed: " + std::to_string(diff.removedMethods.size()));
                        for (const auto& m : diff.modifiedMethods) LOG_INFO("[CoreOverride]   [M] " + m);
                        for (const auto& a : diff.addedMethods) LOG_INFO("[CoreOverride]   [+] " + a);
                        for (const auto& r : diff.removedMethods) LOG_INFO("[CoreOverride]   [-] " + r);
                    } else {
                        LOG_WARN("[CoreOverride] Diff failed: " + diff.error);
                    }
                }
                ImGui::EndPopup();
            }
        }
        ImGui::Separator();
    }

    // Mods list
    ImGui::BeginChild("DetectedModsList", ImVec2(-1, -1), false);

    if (m_detectedMods.empty()) {
        ImGui::TextDisabled("No external mods detected");
        ImGui::TextDisabled("(.package mods like NRaas will appear here)");
    } else {
        for (const auto& mod : m_detectedMods) {
            bool hasOverrides = !mod.overriddenCoreAssemblies.empty();

            // Mod entry with type count
            std::string label = mod.name + " (" + std::to_string(mod.typeCount) + " types)";

            // Orange color for mods that override core assemblies
            if (hasOverrides) { ImGui::PushStyleColor(ImGuiCol_Text, Colors::Get().neutral); }

            bool clicked = ImGui::Selectable(label.c_str(), false);

            if (hasOverrides) { ImGui::PopStyleColor(); }

            // Left-click: browse assembly
            if (clicked) { MainWindow::Get().BrowseAssembly(mod.image); }

            // Tooltip showing overrides and package path
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                if (hasOverrides) {
                    Colors::Text(Colors::Get().neutral, "Overrides core assemblies:");
                    for (const auto& core : mod.overriddenCoreAssemblies) { ImGui::BulletText("%s", core.c_str()); }
                    ImGui::Separator();
                }
                if (!mod.packagePath.empty()) {
                    // Show just filename in tooltip
                    size_t lastSlash = mod.packagePath.find_last_of("\\/");
                    std::string filename = (lastSlash != std::string::npos) ? mod.packagePath.substr(lastSlash + 1) : mod.packagePath;
                    ImGui::TextDisabled("Source: %s", filename.c_str());
                }
                ImGui::TextDisabled("Click to browse, right-click for options");
                ImGui::EndTooltip();
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem(mod.name.c_str())) {
                if (ImGui::MenuItem("Open in Class Browser")) { MainWindow::Get().BrowseAssembly(mod.image); }

                if (hasOverrides) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("View Overrides...")) {
                        // Open overrides window
                        m_showOverridesWindow = true;
                        m_overridesModName = mod.name;
                        m_overridesPackagePath = mod.packagePath;
                        m_overridesCoreAssemblies = mod.overriddenCoreAssemblies;
                        // Clear cached diffs to recompute
                        m_cachedDiffs.clear();
                        LOG_INFO("[ModsPanel] View Overrides requested for: " + mod.name);
                    }

                    // Show which cores are overridden
                    ImGui::Separator();
                    ImGui::TextDisabled("Overrides:");
                    for (const auto& core : mod.overriddenCoreAssemblies) {
                        ImGui::MenuItem(core.c_str(), nullptr, false, false); // Disabled, just info
                    }
                }

                ImGui::EndPopup();
            }
        }
    }

    ImGui::EndChild();
}

void ModsPanel::RenderOverridesWindow() {
    // Extract just filename for title
    size_t lastSlash = m_overridesPackagePath.find_last_of("\\/");
    std::string filename = (lastSlash != std::string::npos) ? m_overridesPackagePath.substr(lastSlash + 1) : m_overridesPackagePath;

    std::string windowTitle = "Overrides: " + filename + "###OverridesWindow";

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(windowTitle.c_str(), &m_showOverridesWindow)) {
        // Header
        Colors::Text(Colors::Get().neutral, "Core Assembly Overrides");
        ImGui::TextDisabled("Mod: %s", m_overridesModName.c_str());
        ImGui::TextDisabled("Package: %s", filename.c_str());
        ImGui::Separator();

        // For each overridden core assembly
        for (const auto& coreAsm : m_overridesCoreAssemblies) {
            // Compute diff if not cached
            if (!m_cachedDiffs[coreAsm].computed) {
                // Use the proper diff logic with PE-based parsing
                AssemblyDiffResult asmDiff = ComputeAssemblyDiff(coreAsm.c_str());

                CachedDiff& result = m_cachedDiffs[coreAsm];
                result.modifiedMethods = asmDiff.modifiedMethods;
                result.addedMethods = asmDiff.addedMethods;
                result.removedMethods = asmDiff.removedMethods;
                result.computed = true;

                if (!asmDiff.valid && !asmDiff.error.empty()) {
                    LOG_WARN("[ModsPanel] Diff error for " + coreAsm + ": " + asmDiff.error);
                } else {
                    LOG_INFO("[ModsPanel] Computed diff for " + coreAsm + ": " + std::to_string(result.modifiedMethods.size()) + " modified, " + std::to_string(result.addedMethods.size()) + " added, " +
                             std::to_string(result.removedMethods.size()) + " removed");
                }
            }

            const CachedDiff& diff = m_cachedDiffs[coreAsm];

            // Collapsing header for this assembly
            int totalChanges = (int)(diff.modifiedMethods.size() + diff.addedMethods.size() + diff.removedMethods.size());
            std::string header = coreAsm + " (" + std::to_string(totalChanges) + " changes)";

            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent();

                // Modified methods (yellow)
                if (!diff.modifiedMethods.empty()) {
                    Colors::Text(Colors::Get().neutral, "Modified (%d):", (int)diff.modifiedMethods.size());
                    for (const auto& method : diff.modifiedMethods) { ImGui::BulletText("%s", method.c_str()); }
                }

                // Added methods (green)
                if (!diff.addedMethods.empty()) {
                    Colors::Text(Colors::Get().positive, "Added (%d):", (int)diff.addedMethods.size());
                    for (const auto& method : diff.addedMethods) { ImGui::BulletText("%s", method.c_str()); }
                }

                // Removed methods (red)
                if (!diff.removedMethods.empty()) {
                    Colors::Text(Colors::Get().negative, "Removed (%d):", (int)diff.removedMethods.size());
                    for (const auto& method : diff.removedMethods) { ImGui::BulletText("%s", method.c_str()); }
                }

                if (totalChanges == 0) { ImGui::TextDisabled("No IL differences detected"); }

                ImGui::Unindent();
            }
        }

        if (m_overridesCoreAssemblies.empty()) { ImGui::TextDisabled("No core assemblies overridden"); }
    }
    ImGui::End();
}

} // namespace S3MM
