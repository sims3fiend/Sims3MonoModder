#pragma once
#include "../core/il_xref.h"
#include <string>
#include <vector>
#include <map>
#include <future>
#include <chrono>

namespace S3MM {

// XREF PANEL - IL References popup window showing callers/callees
class XRefPanel {
  public:
    void Render(); // Called from MainWindow if visible

    // Trigger searches (called from MethodPanel context menu)
    void FindCallers(void* image, const std::string& methodFullName);
    void FindCallees(void* image, void* methodHandle, const std::string& methodFullName);

    bool IsVisible() const { return m_visible; }
    void Show() { m_visible = true; }
    void Hide() { m_visible = false; }

  private:
    bool m_visible = false;

    // Current query state
    enum class Mode { None, Callers, Callees };
    Mode m_mode = Mode::None;
    std::string m_targetMethod;
    void* m_image = nullptr;
    void* m_methodHandle = nullptr;

    // Async scanning state
    bool m_isScanning = false;
    bool m_searchAllAssemblies = false; // True when doing cross-assembly search
    std::future<std::vector<XRefEntry>> m_scanFuture;
    std::chrono::steady_clock::time_point m_scanStartTime;

    // Results (computed on query, cached until next query)
    std::vector<XRefEntry> m_results;
    bool m_resultsComputed = false;

    // Grouped results for display

    struct ClassGroup {
        std::vector<XRefEntry> entries;
    };
    struct NamespaceGroup {
        std::map<std::string, ClassGroup> classes;
    };
    std::map<std::string, NamespaceGroup> m_groupedResults;

    void StartAsyncScan();
    void CheckScanComplete();
    void GroupResults();
    void RenderResults();
    void RenderScanningIndicator();

    // Get display name for opcode
    static const char* GetOpcodeName(uint8_t opcode);
};

} // namespace S3MM
