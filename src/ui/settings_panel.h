#pragma once

namespace S3MM {

// SETTINGS PANEL - UI for configuring S3MM options
class SettingsPanel {
  public:
    void Render();

  private:
    void RenderTheme();
    void RenderKeyBindings();
    void RenderDiagnostics();

    // Key rebind state
    bool m_waitingForKey = false;
};

} // namespace S3MM
