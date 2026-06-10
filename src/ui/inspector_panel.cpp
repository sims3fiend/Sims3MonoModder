#include "inspector_panel.h"
#include "class_browser_panel.h"
#include "method_panel.h"
#include "field_panel.h"
#include "property_panel.h"
#include "singleton_panel.h"
#include "../util/logger.h"
#include "imgui.h"
#include <algorithm>

namespace S3MM {

void InspectorPanel::Initialize(ClassBrowserPanel* classBrowser, MethodPanel* methods, FieldPanel* fields, PropertyPanel* properties, SingletonPanel* singletons) {
    m_classBrowser = classBrowser;
    m_methods = methods;
    m_fields = fields;
    m_properties = properties;
    m_singletons = singletons;
}

void InspectorPanel::Render() {
    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (m_bottomUndocked) {
        // Only top half when undocked
        RenderTopHalf();
    } else {
        // Split layout: top + splitter + bottom
        float topHeight = avail.y * m_splitRatio;

        ImGui::BeginChild("InspectorTop", ImVec2(-1, topHeight), true);
        RenderTopHalf();
        ImGui::EndChild();

        RenderSplitter(avail.y);

        ImGui::BeginChild("InspectorBottom", ImVec2(-1, -1), true);
        RenderBottomHalf();
        ImGui::EndChild();
    }
}

void InspectorPanel::RenderTopHalf() {
    if (m_classBrowser) { m_classBrowser->Render(); }
}

void InspectorPanel::RenderBottomHalf() {
    if (!m_hasClassSelected) {
        ImGui::TextDisabled("Select a class to inspect");
        return;
    }

    // Undock/Dock button
    if (m_bottomUndocked) {
        if (ImGui::SmallButton("Dock")) { m_bottomUndocked = false; }
    } else {
        if (ImGui::SmallButton("Undock")) { m_bottomUndocked = true; }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_selectedClassName.c_str());

    ImGui::Separator();

    // Tabbed detail panels
    RenderBottomTabs();
}

void InspectorPanel::RenderBottomTabs() {
    if (ImGui::BeginTabBar("InspectorDetailTabs")) {
        if (ImGui::BeginTabItem("Methods")) {
            if (m_methods) { m_methods->Render(); }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Fields")) {
            if (m_fields) { m_fields->Render(); }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Properties")) {
            if (m_properties) { m_properties->Render(); }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Instances")) {
            if (m_singletons) { m_singletons->Render(); }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void InspectorPanel::RenderSplitter(float availHeight) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.5f, 0.6f, 1.0f));

    ImGui::Button("##inspector_splitter", ImVec2(-1, 6));
    if (ImGui::IsItemActive()) {
        float delta = ImGui::GetIO().MouseDelta.y / availHeight;
        m_splitRatio = std::clamp(m_splitRatio + delta, 0.15f, 0.85f);
    }
    if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); }

    ImGui::PopStyleColor(3);
}

void InspectorPanel::OnClassSelected(void* klass, const std::string& className) {
    m_hasClassSelected = (klass != nullptr);
    m_selectedClassName = className;
}

} // namespace S3MM
