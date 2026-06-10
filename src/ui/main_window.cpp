#include "main_window.h"
#include "class_browser_panel.h"
#include "method_panel.h"
#include "field_panel.h"
#include "property_panel.h"
#include "singleton_panel.h"
#include "settings_panel.h"
#include "object_explorer_panel.h"
#include "profiler_panel.h"
#include "mods_panel.h"
#include "xref_panel.h"
#include "inspector_panel.h"
#include "resource_browser_panel.h"
#include "../core/mono_bridge.h"
#include "../core/mono_profiler.h"
#include "../core/script_host_hook.h"
#include "../modding/mod_loader.h"
#include "../util/logger.h"
#include "../util/config.h"

#include "imgui.h"

namespace S3MM {

MainWindow& MainWindow::Get() {
    static MainWindow instance;
    return instance;
}

void MainWindow::Initialize() {
    LOG_INFO("[MainWindow] Initializing...");

    m_classBrowser = std::make_unique<ClassBrowserPanel>();
    m_methodPanel = std::make_unique<MethodPanel>();
    m_fieldPanel = std::make_unique<FieldPanel>();
    m_propertyPanel = std::make_unique<PropertyPanel>();
    m_singletonPanel = std::make_unique<SingletonPanel>();
    m_settingsPanel = std::make_unique<SettingsPanel>();
    // Object explorers created on demand via ExploreObject()
    m_profilerPanel = std::make_unique<ProfilerPanel>();
    m_modsPanel = std::make_unique<ModsPanel>();
    m_xrefPanel = std::make_unique<XRefPanel>();
    m_inspectorPanel = std::make_unique<InspectorPanel>();
    m_resourceBrowser = std::make_unique<ResourceBrowserPanel>();
    // Wire settings panel into Mods Hub
    m_modsPanel->SetSettingsPanel(m_settingsPanel.get());

    // Initialize inspector with all sub-panels
    m_inspectorPanel->Initialize(m_classBrowser.get(), m_methodPanel.get(), m_fieldPanel.get(), m_propertyPanel.get(), m_singletonPanel.get());

    // Initialize the profiler (hooks Mono interpreter)
    MonoProfiler::Get().Initialize();

    LOG_INFO("[MainWindow] Initialized");
}

void MainWindow::Shutdown() {
    // Shutdown hooks first
    MonoProfiler::Get().Shutdown();

    m_classBrowser.reset();
    m_methodPanel.reset();
    m_fieldPanel.reset();
    m_propertyPanel.reset();
    m_singletonPanel.reset();
    m_settingsPanel.reset();
    m_objectExplorers.clear();
    m_profilerPanel.reset();
    m_modsPanel.reset();
    m_xrefPanel.reset();
    m_inspectorPanel.reset();
    m_resourceBrowser.reset();
}

void MainWindow::ExploreObject(void* object, const std::string& label) {
    if (!object) return;

    auto explorer = std::make_unique<ObjectExplorerPanel>();
    explorer->SetWindowId(m_nextExplorerId++);
    explorer->ExploreObject(object, label);
    m_objectExplorers.push_back(std::move(explorer));
}

void MainWindow::NavigateToHook(const std::string& className, const std::string& methodName) {
    if (!m_classBrowser || !m_methodPanel) return;

    // Navigate class browser to the target class
    if (m_classBrowser->NavigateToClass(className)) {
        // Get the class and set it on method panel
        const MonoClass* klass = m_classBrowser->GetSelectedClass();
        if (klass && klass->IsValid()) {
            m_methodPanel->SetClass(klass->Handle(), klass->FullName());
            m_methodPanel->NavigateToMethod(methodName);

            m_showInspector = true;
        }
    }

    LOG_INFO("[MainWindow] NavigateToHook: " + className + "::" + methodName);
}

void MainWindow::BrowseAssembly(void* image) {
    if (!m_classBrowser) return;

    m_classBrowser->SelectAssembly(image);
    m_showInspector = true;

    const char* name = MonoBridge::GetImageName(image);
    LOG_INFO("[MainWindow] BrowseAssembly: " + std::string(name ? name : "???"));
}

void MainWindow::Render() {
    bool monoReady = MonoBridge::Get().IsRuntimeReady();

    // Handle inter-panel communication
    if (monoReady) {
        // Check if classes were just loaded - clear the flag
        if (m_classBrowser && m_classBrowser->ClassesJustLoaded()) { m_classBrowser->ClearClassesJustLoaded(); }

        // Handle class selection change - route to all panels + inspector
        if (m_classBrowser && m_classBrowser->HasSelectionChanged()) {
            const MonoClass* klass = m_classBrowser->GetSelectedClass();
            if (klass && klass->IsValid()) {
                if (m_methodPanel) m_methodPanel->SetClass(klass->Handle(), klass->FullName());
                if (m_fieldPanel) m_fieldPanel->SetClass(klass->Handle(), klass->FullName());
                if (m_propertyPanel) m_propertyPanel->SetClass(klass->Handle(), klass->FullName());
                if (m_singletonPanel) m_singletonPanel->SetClass(klass->Handle(), klass->FullName());

                if (m_inspectorPanel) { m_inspectorPanel->OnClassSelected(klass->Handle(), klass->FullName()); }
            }
            m_classBrowser->ClearSelectionChanged();
        }
    }

    // MODS HUB - Primary window, always visible
    if (m_showMods && m_modsPanel) {
        ImGui::SetNextWindowSize(ImVec2(420, 550), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);

        ImGuiWindowFlags modsFlags = ImGuiWindowFlags_MenuBar;
        if (ImGui::Begin("S3MM - Mods Hub", nullptr, modsFlags)) {
            // Menu bar
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("View")) {
                    ImGui::MenuItem("Inspector", nullptr, &m_showInspector, monoReady);
                    ImGui::MenuItem("Profiler", nullptr, &m_showProfiler);
                    ImGui::MenuItem("Resource Browser", nullptr, &m_showResourceBrowser);
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            m_modsPanel->Render();
        }
        ImGui::End();
    }

    // INSPECTOR - Developer tool for class/method/field browsing
    if (m_showInspector && m_inspectorPanel && monoReady) {
        ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(490, 50), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("S3MM - Inspector", &m_showInspector)) { m_inspectorPanel->Render(); }
        ImGui::End();

        // Render undocked bottom half if needed
        if (m_inspectorPanel->IsBottomUndocked()) {
            ImGui::SetNextWindowSize(ImVec2(550, 450), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(910, 50), ImGuiCond_FirstUseEver);

            bool showDetails = true;
            if (ImGui::Begin("Inspector Details", &showDetails)) { m_inspectorPanel->RenderBottomHalf(); }
            ImGui::End();

            if (!showDetails) { m_inspectorPanel->SetBottomDocked(); }
        }
    }

    // PROFILER - Shows method call stats and allocations
    if (m_showProfiler && m_profilerPanel) {
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Profiler", &m_showProfiler)) { m_profilerPanel->Render(); }
        ImGui::End();
    }

    // IL REFERENCES - Shows callers/callees cross-references
    if (m_xrefPanel && m_xrefPanel->IsVisible()) {
        m_xrefPanel->Render();
        m_showXRef = m_xrefPanel->IsVisible(); // Sync visibility
    }

    // RESOURCE BROWSER - Package/database viewer
    if (m_showResourceBrowser && m_resourceBrowser) {
        ImGui::SetNextWindowSize(ImVec2(550, 500), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(150, 100), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Resource Browser (WIP)", &m_showResourceBrowser)) { m_resourceBrowser->Render(); }
        ImGui::End();
    }

    // OBJECT EXPLORERS - Each explore spawns an independent window
    for (auto it = m_objectExplorers.begin(); it != m_objectExplorers.end();) {
        auto& explorer = *it;
        bool open = explorer->IsOpen();

        ImGui::SetNextWindowSize(ImVec2(450, 500), ImGuiCond_FirstUseEver);

        std::string title = explorer->GetWindowTitle();
        if (ImGui::Begin(title.c_str(), &open)) { explorer->Render(); }
        ImGui::End();

        explorer->SetOpen(open);
        if (!open) {
            it = m_objectExplorers.erase(it);
        } else {
            ++it;
        }
    }
}

void MainWindow::OpenProfilerForMethod(const std::string& methodFullName) {
    if (m_profilerPanel) {
        m_profilerPanel->SetFilter(methodFullName);
        m_showProfiler = true;
    }
}

void MainWindow::OpenProfilerForClass(const std::string& classFullName) {
    if (m_profilerPanel) {
        m_profilerPanel->SetFilter(classFullName);
        m_showProfiler = true;
    }
}

} // namespace S3MM
