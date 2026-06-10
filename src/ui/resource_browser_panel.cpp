#include "resource_browser_panel.h"
#include "themes.h"
#include "../util/logger.h"
#include "../util/config_paths.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace S3MM {

// HELPERS

static std::string ToHexStr(uint32_t val) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << val;
    return ss.str();
}

// DATA LOADING

void ResourceBrowserPanel::LoadDatabases() {
    m_loading = true;
    m_databases = ResourceSystem::GetRegisteredDatabases();
    m_loading = false;
    m_loaded = true;

    // Invalidate TGI cache (db indices changed)
    m_dbPackageKeys.clear();
    m_dbDDFKeys.clear();
    m_dbKeysLoaded.clear();
    m_loadedDbIdx = SIZE_MAX;
    m_tgiDisplayStrings.clear();
    m_tgiDisplayStringsLower.clear();
    m_filteredTgiIndices.clear();
    m_selectedTgiIdx = -1;

    // Pre-compute display strings
    m_dbDisplayNames.clear();
    m_dbDisplayNamesLower.clear();
    m_dbDisplayNames.reserve(m_databases.size());
    m_dbDisplayNamesLower.reserve(m_databases.size());

    for (const auto& db : m_databases) {
        std::string display = db.filename;
        if (db.typeId == DB_TYPE_DIRECTORY_FILES) { display += "  (DDF)"; }
        display += "  [pri: " + std::to_string(db.priority) + "]";
        m_dbDisplayNames.push_back(display);

        std::string lower = display;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        m_dbDisplayNamesLower.push_back(lower);
    }

    LOG_INFO("[ResourceBrowser] Loaded " + std::to_string(m_databases.size()) + " databases");
}

// RENDERING

void ResourceBrowserPanel::Render() {
    // Toolbar
    if (ImGui::Button("Refresh")) { LoadDatabases(); }
    if (m_loading) {
        ImGui::SameLine();
        Colors::Text(Colors::Get().neutral, "Loading...");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu databases)", m_databases.size());

    ImGui::Separator();

    // Search filter
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##resfilter", "Filter packages...", m_searchFilter, sizeof(m_searchFilter))) {
        m_cachedFilterLower = m_searchFilter;
        std::transform(m_cachedFilterLower.begin(), m_cachedFilterLower.end(), m_cachedFilterLower.begin(), ::tolower);
    }

    // Split view: database list (top) + detail pane (bottom)
    float availHeight = ImGui::GetContentRegionAvail().y;

    if (m_showDetails && m_selectedDbIdx < m_databases.size()) {
        float listHeight = availHeight * m_splitRatio;

        // Database list
        ImGui::BeginChild("DbList", ImVec2(-1, listHeight), true);
        RenderDatabaseList();
        ImGui::EndChild();

        // Splitter (simple draggable)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.6f, 1.0f));
        ImGui::Button("##splitter", ImVec2(-1, 4));
        if (ImGui::IsItemActive()) {
            float delta = ImGui::GetIO().MouseDelta.y / availHeight;
            m_splitRatio = std::clamp(m_splitRatio + delta, 0.2f, 0.9f);
        }
        ImGui::PopStyleColor(2);

        // Detail pane
        ImGui::BeginChild("DbDetail", ImVec2(-1, -1), true);
        RenderDetailPane();
        ImGui::EndChild();
    } else {
        // No selection, full height list
        ImGui::BeginChild("DbList", ImVec2(-1, -1), true);
        RenderDatabaseList();
        ImGui::EndChild();
    }
}

void ResourceBrowserPanel::RenderDatabaseList() {
    if (!m_loaded) {
        ImGui::TextDisabled("Click 'Refresh' to load databases.");
        return;
    }

    if (m_databases.empty()) {
        Colors::Text(Colors::Get().negative, "No databases found.");
        return;
    }

    for (size_t i = 0; i < m_databases.size(); i++) {
        // Filter check
        if (!m_cachedFilterLower.empty()) {
            if (m_dbDisplayNamesLower[i].find(m_cachedFilterLower) == std::string::npos) {
                // Also check full path
                std::string pathLower = m_databases[i].path;
                std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
                if (pathLower.find(m_cachedFilterLower) == std::string::npos) { continue; }
            }
        }

        RenderDatabaseNode(i);
    }
}

void ResourceBrowserPanel::RenderDatabaseNode(size_t dbIdx) {
    const auto& db = m_databases[dbIdx];
    bool isSelected = (dbIdx == m_selectedDbIdx);

    // Color by type (theme-adaptive)
    const auto& tc = Colors::Get();
    ImVec4 color;
    if (db.typeId == DB_TYPE_DIRECTORY_FILES) {
        color = tc.positive; // Green for DDF
    } else if (db.typeId == DB_TYPE_PACKED_FILE) {
        color = tc.accent; // Blue for PackedFile (mods)
    } else if (db.typeId == DB_TYPE_PACKED_STREAM) {
        color = tc.neutral; // Amber for game data packages
    } else {
        color = Colors::Desaturate(tc.accent); // Gray for unknown
    }

    ImGui::PushStyleColor(ImGuiCol_Text, color);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isSelected) { flags |= ImGuiTreeNodeFlags_Selected; }

    // Use priority badge + filename
    std::string label = m_dbDisplayNames[dbIdx] + "##db" + std::to_string(dbIdx);
    ImGui::TreeNodeEx(label.c_str(), flags);

    if (ImGui::IsItemClicked()) {
        m_selectedDbIdx = dbIdx;
        m_selectionChanged = true;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Copy Path")) { ImGui::SetClipboardText(db.path.c_str()); }
        if (ImGui::MenuItem("Copy Pointer")) {
            std::ostringstream ss;
            ss << "0x" << std::hex << (uintptr_t)db.ptr;
            ImGui::SetClipboardText(ss.str().c_str());
        }
        ImGui::EndPopup();
    }

    // Tooltip with full path
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Path: %s", db.path.c_str());
        ImGui::Text("Type: %s", db.typeName.c_str());
        ImGui::Text("Priority: %d", db.priority);
        ImGui::EndTooltip();
    }

    ImGui::PopStyleColor();
}

void ResourceBrowserPanel::RenderDetailPane() {
    if (m_selectedDbIdx >= m_databases.size()) {
        ImGui::TextDisabled("Select a database to view details.");
        return;
    }

    const auto& db = m_databases[m_selectedDbIdx];

    ImGui::TextColored(Colors::Get().neutral, "Database Details");
    ImGui::Separator();

    ImGui::Text("Filename:  %s", db.filename.c_str());
    ImGui::Text("Full Path: %s", db.path.c_str());
    ImGui::Text("Type:      %s", db.typeName.c_str());
    ImGui::Text("Type ID:   %s", ToHexStr(db.typeId).c_str());
    ImGui::Text("Priority:  %d", db.priority);
    ImGui::Text("Pointer:   0x%08X", (uintptr_t)db.ptr);

    ImGui::Separator();

    // Highlight mod packages
    bool isMod = (db.path.find("\\Mods\\") != std::string::npos || db.path.find("/Mods/") != std::string::npos);
    if (isMod) { Colors::Text(Colors::Get().positive, "[MOD PACKAGE]"); }

    bool isVanilla = (db.path.find("\\GameData\\") != std::string::npos || db.path.find("/GameData/") != std::string::npos);
    if (isVanilla) { ImGui::TextDisabled("[VANILLA]"); }

    // TGI ENUMERATION
    ImGui::Separator();

    // Auto-load TGIs when a new database is selected
    if (m_loadedDbIdx != m_selectedDbIdx) { LoadTGIsForDatabase(m_selectedDbIdx); }

    bool isDDF = (db.typeId == DB_TYPE_DIRECTORY_FILES);
    size_t totalKeys = isDDF ? (m_selectedDbIdx < m_dbDDFKeys.size() ? m_dbDDFKeys[m_selectedDbIdx].size() : 0) : (m_selectedDbIdx < m_dbPackageKeys.size() ? m_dbPackageKeys[m_selectedDbIdx].size() : 0);

    // TGI filter
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##tgifilter", "Filter TGIs (type name, hex...)", m_tgiFilter, sizeof(m_tgiFilter))) {
        m_cachedTgiFilterLower = m_tgiFilter;
        std::transform(m_cachedTgiFilterLower.begin(), m_cachedTgiFilterLower.end(), m_cachedTgiFilterLower.begin(), ::tolower);
        ApplyTGIFilter();
    }

    ImGui::Text("Showing %zu / %zu resources", m_filteredTgiIndices.size(), totalKeys);

    // Extract status message
    if (m_extractStatusTimer > 0.0f) {
        const auto& sc = Colors::Get();
        ImVec4 statusColor = (m_extractStatus.find("Failed") != std::string::npos) ? sc.negative : sc.positive;
        ImGui::TextColored(statusColor, "%s", m_extractStatus.c_str());
        m_extractStatusTimer -= ImGui::GetIO().DeltaTime;
    }

    // TGI table
    int colCount = isDDF ? 4 : (m_nameMap.empty() ? 6 : 7);
    if (totalKeys > 0 && ImGui::BeginTable("TGITable", colCount, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_None, 3.0f);
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_None, 2.0f);
        ImGui::TableSetupColumn("Instance", ImGuiTableColumnFlags_None, 3.0f);
        if (!isDDF) {
            if (!m_nameMap.empty()) { ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 3.0f); }
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_None, 1.5f);
            ImGui::TableSetupColumn("Mem", ImGuiTableColumnFlags_None, 1.5f);
            ImGui::TableSetupColumn("C", ImGuiTableColumnFlags_None, 0.4f);
        } else {
            ImGui::TableSetupColumn("Type Name", ImGuiTableColumnFlags_None, 2.0f);
        }
        ImGui::TableHeadersRow();

        // ImGui clipper for large lists
        ImGuiListClipper clipper;
        clipper.Begin((int)m_filteredTgiIndices.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                size_t origIdx = m_filteredTgiIndices[row];
                ImGui::TableNextRow();

                if (!isDDF && origIdx < m_dbPackageKeys[m_selectedDbIdx].size()) {
                    const auto& pki = m_dbPackageKeys[m_selectedDbIdx][origIdx];
                    const auto& k = pki.key;

                    // Invisible row-spanning selectable for context menu
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(row);
                    ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 0));
                    ImGui::OpenPopupOnItemClick("tgi_ctx", ImGuiPopupFlags_MouseButtonRight);

                    if (ImGui::BeginPopup("tgi_ctx")) {
                        if (ImGui::MenuItem("Copy TGI")) { ImGui::SetClipboardText(k.Format().c_str()); }
                        if (ImGui::MenuItem("Copy Instance ID")) {
                            char buf[20];
                            snprintf(buf, sizeof(buf), "0x%08X%08X", k.instance_hi, k.instance_lo);
                            ImGui::SetClipboardText(buf);
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Extract Resource")) {
                            // Build output path: S3MM/Extracted/<package_stem>/S3_T_G_I[_name].ext
                            std::string stem = db.filename;
                            size_t dotPos = stem.rfind('.');
                            if (dotPos != std::string::npos) stem = stem.substr(0, dotPos);

                            char tgiPart[64];
                            snprintf(tgiPart, sizeof(tgiPart), "S3_%08X_%08X_%08X%08X", k.type, k.group, k.instance_hi, k.instance_lo);
                            std::string filename = tgiPart;

                            auto nameIt2 = m_nameMap.find(k.GetInstance());
                            if (nameIt2 != m_nameMap.end() && !nameIt2->second.empty()) { filename += "_" + nameIt2->second; }
                            filename += ResourceSystem::GetTypeExtension(k.type);

                            std::string outPath = ConfigPaths::WideToUtf8(ConfigPaths::GetS3MMDirectory()) + "Extracted\\" + stem + "\\" + filename;

                            if (ResourceSystem::ExtractResourceToFile(db.path, pki, outPath)) {
                                m_extractStatus = "Extracted: " + filename;
                            } else {
                                m_extractStatus = "Failed: " + filename;
                            }
                            m_extractStatusTimer = 4.0f;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::PopID();

                    // Type column - show name if known
                    std::string typeName = ResourceSystem::GetTypeName(k.type);
                    if (!typeName.empty()) {
                        ImGui::Text("%s", typeName.c_str());
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("%s", ToHexStr(k.type).c_str());
                            ImGui::EndTooltip();
                        }
                    } else {
                        ImGui::TextDisabled("%s", ToHexStr(k.type).c_str());
                    }

                    // Group
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", ToHexStr(k.group).c_str());

                    // Instance
                    ImGui::TableSetColumnIndex(2);
                    char instBuf[20];
                    snprintf(instBuf, sizeof(instBuf), "%08X%08X", k.instance_hi, k.instance_lo);
                    ImGui::Text("0x%s", instBuf);

                    int col = 3;

                    // Name column (only if Name Map is available)
                    if (!m_nameMap.empty()) {
                        ImGui::TableSetColumnIndex(col++);
                        auto nameIt = m_nameMap.find(k.GetInstance());
                        if (nameIt != m_nameMap.end()) {
                            ImGui::Text("%s", nameIt->second.c_str());
                        } else {
                            ImGui::TextDisabled("--");
                        }
                    }

                    // Size
                    ImGui::TableSetColumnIndex(col++);
                    if (pki.fileSize < 1024)
                        ImGui::Text("%u B", pki.fileSize);
                    else if (pki.fileSize < 1024 * 1024)
                        ImGui::Text("%.1f KB", pki.fileSize / 1024.0f);
                    else
                        ImGui::Text("%.1f MB", pki.fileSize / (1024.0f * 1024.0f));

                    // Mem size
                    ImGui::TableSetColumnIndex(col++);
                    if (pki.memSize < 1024)
                        ImGui::Text("%u B", pki.memSize);
                    else if (pki.memSize < 1024 * 1024)
                        ImGui::Text("%.1f KB", pki.memSize / 1024.0f);
                    else
                        ImGui::Text("%.1f MB", pki.memSize / (1024.0f * 1024.0f));

                    // Compressed flag
                    ImGui::TableSetColumnIndex(col++);
                    if (pki.compressed) { Colors::Text(Colors::Get().neutral, "Y"); }

                } else if (isDDF && origIdx < m_dbDDFKeys[m_selectedDbIdx].size()) {
                    const auto& k = m_dbDDFKeys[m_selectedDbIdx][origIdx];

                    // Type
                    ImGui::TableSetColumnIndex(0);
                    std::string typeName = ResourceSystem::GetTypeName(k.type);
                    if (!typeName.empty()) {
                        ImGui::Text("%s", typeName.c_str());
                    } else {
                        ImGui::Text("%s", ToHexStr(k.type).c_str());
                    }

                    // Group
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", ToHexStr(k.group).c_str());

                    // Instance
                    ImGui::TableSetColumnIndex(2);
                    char instBuf[20];
                    snprintf(instBuf, sizeof(instBuf), "%08X%08X", k.instance_hi, k.instance_lo);
                    ImGui::Text("0x%s", instBuf);

                    // Type name again in last column for DDF
                    ImGui::TableSetColumnIndex(3);
                    if (!typeName.empty()) {
                        ImGui::TextDisabled("%s", ToHexStr(k.type).c_str());
                    } else {
                        ImGui::TextDisabled("--");
                    }
                }
            }
        }
        ImGui::EndTable();
    } else if (totalKeys == 0 && m_loadedDbIdx == m_selectedDbIdx) {
        ImGui::TextDisabled("No resources found in this database.");
    }
}

// TGI LOADING AND FILTERING

void ResourceBrowserPanel::LoadTGIsForDatabase(size_t dbIdx) {
    if (dbIdx >= m_databases.size()) return;

    const auto& db = m_databases[dbIdx];

    // Ensure vectors are large enough
    if (m_dbPackageKeys.size() <= dbIdx) m_dbPackageKeys.resize(dbIdx + 1);
    if (m_dbDDFKeys.size() <= dbIdx) m_dbDDFKeys.resize(dbIdx + 1);
    if (m_dbKeysLoaded.size() <= dbIdx) m_dbKeysLoaded.resize(dbIdx + 1, false);

    // Load if not cached
    if (!m_dbKeysLoaded[dbIdx]) {
        if (db.typeId == DB_TYPE_DIRECTORY_FILES) {
            m_dbDDFKeys[dbIdx] = ResourceSystem::EnumerateDDFKeys(db.path);
        } else {
            m_dbPackageKeys[dbIdx] = ResourceSystem::EnumeratePackageKeys(db.path);
        }
        m_dbKeysLoaded[dbIdx] = true;
    }

    // Load Name Map for package databases
    m_nameMap.clear();
    if (db.typeId != DB_TYPE_DIRECTORY_FILES) { m_nameMap = ResourceSystem::ParseNameMap(db.path); }

    m_loadedDbIdx = dbIdx;
    m_selectedTgiIdx = -1;

    // Build display strings and apply filter
    BuildTGIDisplayStrings();
    ApplyTGIFilter();
}

void ResourceBrowserPanel::BuildTGIDisplayStrings() {
    m_tgiDisplayStrings.clear();
    m_tgiDisplayStringsLower.clear();

    if (m_loadedDbIdx >= m_databases.size()) return;

    const auto& db = m_databases[m_loadedDbIdx];
    bool isDDF = (db.typeId == DB_TYPE_DIRECTORY_FILES);

    if (isDDF) {
        if (m_loadedDbIdx >= m_dbDDFKeys.size()) return;
        const auto& keys = m_dbDDFKeys[m_loadedDbIdx];
        m_tgiDisplayStrings.reserve(keys.size());
        m_tgiDisplayStringsLower.reserve(keys.size());

        for (const auto& k : keys) {
            std::string typeName = ResourceSystem::GetTypeName(k.type);
            std::string display = typeName.empty() ? ToHexStr(k.type) : typeName;
            display += " " + ToHexStr(k.group) + " ";
            char buf[20];
            snprintf(buf, sizeof(buf), "%08X%08X", k.instance_hi, k.instance_lo);
            display += buf;

            m_tgiDisplayStrings.push_back(display);
            std::string lower = display;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            m_tgiDisplayStringsLower.push_back(lower);
        }
    } else {
        if (m_loadedDbIdx >= m_dbPackageKeys.size()) return;
        const auto& keys = m_dbPackageKeys[m_loadedDbIdx];
        m_tgiDisplayStrings.reserve(keys.size());
        m_tgiDisplayStringsLower.reserve(keys.size());

        for (const auto& pki : keys) {
            const auto& k = pki.key;
            std::string typeName = ResourceSystem::GetTypeName(k.type);
            std::string display = typeName.empty() ? ToHexStr(k.type) : typeName;
            display += " " + ToHexStr(k.group) + " ";
            char buf[20];
            snprintf(buf, sizeof(buf), "%08X%08X", k.instance_hi, k.instance_lo);
            display += buf;

            // Include resource name from Name Map for filtering
            auto nameIt = m_nameMap.find(k.GetInstance());
            if (nameIt != m_nameMap.end()) { display += " " + nameIt->second; }

            m_tgiDisplayStrings.push_back(display);
            std::string lower = display;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            m_tgiDisplayStringsLower.push_back(lower);
        }
    }
}

void ResourceBrowserPanel::ApplyTGIFilter() {
    m_filteredTgiIndices.clear();

    for (size_t i = 0; i < m_tgiDisplayStringsLower.size(); i++) {
        if (m_cachedTgiFilterLower.empty() || m_tgiDisplayStringsLower[i].find(m_cachedTgiFilterLower) != std::string::npos) { m_filteredTgiIndices.push_back(i); }
    }
}

} // namespace S3MM
