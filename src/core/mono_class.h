#pragma once
#include "mono_types.h"
#include "mono_bridge.h"
#include <functional>

namespace S3MM {

// MONO CLASS WRAPPER - Higher-level class reflection API
class MonoClass {
  public:
    MonoClass() = default;
    explicit MonoClass(void* klass);

    // Validity
    bool IsValid() const { return m_klass != nullptr && IsValidPointer(m_klass); }
    void* Handle() const { return m_klass; }

    // Metadata
    std::string Name() const;
    std::string Namespace() const;
    std::string FullName() const;
    bool IsEnum() const;
    bool IsNested() const;

    // Parent/hierarchy
    MonoClass Parent() const;
    MonoClass DeclaringClass() const;

    // Method enumeration (includeInherited walks up the parent class chain)
    std::vector<MethodInfo> GetMethods(bool includeSpecial = false, bool includeInherited = false) const;

    // Property enumeration (finds matching get_/set_ pairs)
    std::vector<PropertyInfo> GetProperties() const;

    // Field enumeration (includeInherited walks up the parent class chain)
    std::vector<FieldInfo> GetStaticFields(bool includeInherited = false) const;
    std::vector<FieldInfo> GetInstanceFields(bool includeInherited = false) const;
    std::vector<FieldInfo> GetAllFields(bool includeInherited = false) const;

    // Enum values (if IsEnum())
    std::vector<EnumValue> GetEnumValues() const;

    // VTable (for static field access)
    void* GetVTable() const;

    // Singleton detection - looks for static self-referential fields
    bool HasSingletonField(std::string* outFieldName = nullptr) const;
    void* GetSingletonInstance() const;

    // Static class lookup
    static MonoClass Find(const std::string& nameSpace, const std::string& className);

  private:
    void* m_klass = nullptr;

    // Helper to enumerate fields with a filter
    std::vector<FieldInfo> EnumerateFields(bool wantStatic, bool includeInherited) const;

    // Helper to enumerate fields for a single class (no inheritance)
    void EnumerateFieldsForClass(void* klass, bool wantStatic, std::vector<FieldInfo>& outFields) const;
};

// CLASS BROWSER - Enumerate classes across all loaded assemblies
class ClassBrowser {
  public:
    using ClassCallback = std::function<bool(const MonoClass& klass)>; // return false to stop

    // Enumerate all classes matching a namespace prefix
    static void EnumerateByNamespace(const std::string& namespacePrefix, ClassCallback callback);

    // Get all classes in a namespace (from all assemblies)
    static std::vector<MonoClass> GetClassesInNamespace(const std::string& namespacePrefix, int maxClasses = 500);

    // Get all classes from a specific assembly image
    static std::vector<MonoClass> GetClassesFromImage(void* image, const std::string& namespacePrefix = "", int maxClasses = 50000);
};

} // namespace S3MM
