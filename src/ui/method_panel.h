#pragma once
#include "../core/mono_types.h"
#include <string>
#include <vector>

namespace S3MM {

// METHOD PANEL - Display and invoke methods on selected class
class MethodPanel {
  public:
    void Render();

    // Set the class to display methods for
    void SetClass(void* klass, const std::string& className);

    // Navigation - filter to show a specific method
    void NavigateToMethod(const std::string& methodName);

    // Get the MonoImage for the current class (for XREF lookups)
    void* GetCurrentImage() const;

    // Get current class info for context menu
    const std::string& GetClassName() const { return m_className; }
    void* GetClassHandle() const { return m_klass; }

  private:
    void RenderMethodList();
    void RenderMethodDetails();
    void RenderParamInputs();
    void InvokeSelectedMethod();

    // Current class
    void* m_klass = nullptr;
    std::string m_className;
    std::vector<MethodInfo> m_methods;

    // Selection
    size_t m_selectedIndex = SIZE_MAX;
    char m_filterText[256] = "";
    bool m_showStaticOnly = false;
    bool m_showInherited = false;

    // Parameter inputs
    static constexpr int MAX_PARAMS = 8;
    int m_paramInts[MAX_PARAMS] = {};
    int64_t m_paramInt64s[MAX_PARAMS] = {};
    float m_paramFloats[MAX_PARAMS] = {};   // For R4 (float)
    double m_paramDoubles[MAX_PARAMS] = {}; // For R8 (double)
    bool m_paramBools[MAX_PARAMS] = {};
    char m_paramStrings[MAX_PARAMS][256] = {};
    char m_paramArrays[MAX_PARAMS][512] = {}; // Comma-separated array input

    // Enum parameter cache
    struct EnumCache {
        bool isEnum = false;
        std::vector<EnumValue> values;
        int selectedIndex = 0;
    };
    EnumCache m_enumCache[MAX_PARAMS];
    size_t m_lastCachedMethodIndex = SIZE_MAX;

    // Array parameter cache (stores element type for each array param)
    struct ArrayCache {
        bool isArray = false;
        MonoTypeCode elementType = MonoTypeCode::Void;
        void* elementClass = nullptr;
    };
    ArrayCache m_arrayCache[MAX_PARAMS];

    void CacheEnumParams();
    void CacheArrayParams();
    void* CreateArrayFromInput(int paramIndex, const ArrayCache& cache);

    // Last result
    bool m_hasResult = false;
    bool m_resultSuccess = false;
    std::string m_resultText;
    std::string m_resultError;

    // Return value details (for extended display)
    MonoTypeCode m_returnType = MonoTypeCode::Void;
    void* m_returnObject = nullptr;            // For object/array returns - can be explored
    void* m_returnTypeClass = nullptr;         // Class of the return type (for enums)
    std::vector<EnumValue> m_returnEnumValues; // Cached enum values if return is enum
    int32_t m_returnArrayLength = 0;           // For array returns
    MonoTypeCode m_returnArrayElementType = MonoTypeCode::Void;

    // Helper to format return value
    std::string FormatReturnValue(MonoInvokeState* state, MonoTypeCode retType);
    std::string ExtractExceptionMessage(void* exception);
};

} // namespace S3MM
