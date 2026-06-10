#include "mono_class.h"
#include "../util/logger.h"
#include <cstring>
#include <algorithm>
#include <map>

namespace S3MM {

// MONO CLASS WRAPPER - Core class metadata access
MonoClass::MonoClass(void* klass) : m_klass(klass) {}

std::string MonoClass::Name() const {
    if (!IsValid()) return "";
    const char* name = MonoBridge::GetClassName(m_klass);
    return name ? name : "";
}

std::string MonoClass::Namespace() const {
    if (!IsValid()) return "";
    const char* ns = MonoBridge::GetClassNamespace(m_klass);
    return ns ? ns : "";
}

std::string MonoClass::FullName() const {
    if (!IsValid()) return "";

    // For nested classes, build the full path: Namespace.Parent+Nested+SubNested
    if (IsNested()) {
        // Walk up the declaring class chain to build nested path
        std::vector<std::string> names;
        names.push_back(Name());

        MonoClass current = DeclaringClass();
        while (current.IsValid()) {
            names.push_back(current.Name());
            if (!current.IsNested()) {
                // Reached the top-level class, get its namespace
                std::string ns = current.Namespace();
                if (!ns.empty()) {
                    // Build: Namespace.TopClass+Nested+SubNested
                    std::string result = ns + ".";
                    for (int i = (int)names.size() - 1; i >= 0; i--) {
                        result += names[i];
                        if (i > 0) result += "+";
                    }
                    return result;
                }
                break;
            }
            current = current.DeclaringClass();
        }

        // No namespace case - just join with +
        std::string result;
        for (int i = (int)names.size() - 1; i >= 0; i--) {
            result += names[i];
            if (i > 0) result += "+";
        }
        return result;
    }

    // Non-nested class - simple namespace.name
    std::string ns = Namespace();
    std::string name = Name();
    if (ns.empty()) return name;
    return ns + "." + name;
}

bool MonoClass::IsEnum() const {
    return IsValid() && MonoBridge::IsEnumClass(m_klass);
}

bool MonoClass::IsNested() const {
    if (!IsValid()) return false;
    void* declaring = MonoBridge::GetDeclaringClass(m_klass);
    return declaring && IsValidPointer(declaring);
}

MonoClass MonoClass::Parent() const {
    if (!IsValid()) return MonoClass();
    return MonoClass(MonoBridge::GetParentClass(m_klass));
}

MonoClass MonoClass::DeclaringClass() const {
    if (!IsValid()) return MonoClass();
    return MonoClass(MonoBridge::GetDeclaringClass(m_klass));
}

void* MonoClass::GetVTable() const {
    if (!IsValid()) return nullptr;
    return MonoBridge::Get().GetClassVTable(m_klass);
}

// METHOD ENUMERATION - Iterate and collect method metadata
static void EnumerateMethodsForClass(void* klass, void* ownerKlass, bool includeSpecial, std::vector<MethodInfo>& outMethods) {
    if (!klass || !IsValidPointer(klass)) return;

    auto& bridge = MonoBridge::Get();
    bool isInherited = (klass != ownerKlass);
    const char* declaringClassName = isInherited ? MonoBridge::GetClassName(klass) : nullptr;

    int iter = 0;
    void* method = nullptr;

    while ((method = bridge.GetNextMethod(klass, &iter)) != nullptr) {
        if (!IsValidPointer(method)) continue;

        const char* name = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);
        if (!name || !IsValidPointer((void*)name)) continue;

        // Skip special methods unless requested
        if (!includeSpecial) {
            if (name[0] == '.' || name[0] == '<' || strncmp(name, "get_", 4) == 0 || strncmp(name, "set_", 4) == 0 || strncmp(name, "add_", 4) == 0 || strncmp(name, "remove_", 7) == 0) { continue; }
        }

        MethodInfo info;
        info.handle = method;
        info.name = name;
        info.flags = *(uint32_t*)((uintptr_t)method + Offsets::METHOD_FLAGS);
        info.isStatic = (info.flags & MethodFlags::STATIC) != 0;
        info.isInherited = isInherited;
        info.declaringClass = klass;
        info.declaringClassName = (declaringClassName && isInherited) ? declaringClassName : "";

        // Get signature
        info.signature = bridge.GetMethodSignature(method);
        info.returnType = MonoTypeCode::Void;

        if (info.signature && IsValidPointer(info.signature)) {
            uint16_t paramCount = *(uint16_t*)((uintptr_t)info.signature + Offsets::SIG_PARAM_COUNT);

            // Return type
            void* retType = *(void**)((uintptr_t)info.signature + Offsets::SIG_RET_TYPE);
            if (retType && IsValidPointer(retType)) { info.returnType = (MonoTypeCode)(*(uint8_t*)((uintptr_t)retType + Offsets::TYPE_CODE)); }

            // Parameter types
            for (uint16_t i = 0; i < paramCount; i++) {
                void* paramType = *(void**)((uintptr_t)info.signature + Offsets::SIG_PARAMS + i * sizeof(void*));
                if (paramType && IsValidPointer(paramType)) {
                    info.paramTypes.push_back((MonoTypeCode)(*(uint8_t*)((uintptr_t)paramType + Offsets::TYPE_CODE)));
                } else {
                    info.paramTypes.push_back(MonoTypeCode::Void);
                }
            }
        }

        outMethods.push_back(std::move(info));
    }
}

std::vector<MethodInfo> MonoClass::GetMethods(bool includeSpecial, bool includeInherited) const {
    std::vector<MethodInfo> methods;
    if (!IsValid()) return methods;

    if (includeInherited) {
        // Walk up the inheritance chain, collecting methods from each class
        std::vector<void*> classChain;
        void* current = m_klass;

        while (current && IsValidPointer(current)) {
            classChain.push_back(current);
            current = MonoBridge::GetParentClass(current);
        }

        for (void* klass : classChain) { EnumerateMethodsForClass(klass, m_klass, includeSpecial, methods); }
    } else {
        EnumerateMethodsForClass(m_klass, m_klass, includeSpecial, methods);
    }

    return methods;
}

// PROPERTY ENUMERATION - Build properties from getter/setter pairs
std::vector<PropertyInfo> MonoClass::GetProperties() const {
    std::vector<PropertyInfo> properties;
    if (!IsValid()) return properties;

    auto& bridge = MonoBridge::Get();

    // First pass: collect all getter and setter methods
    struct MethodPair {
        void* getter = nullptr;
        void* setter = nullptr;
        void* getterSig = nullptr;
        void* setterSig = nullptr;
        uint32_t getterFlags = 0;
        uint32_t setterFlags = 0;
    };
    std::map<std::string, MethodPair> propMethods;

    int iter = 0;
    void* method = nullptr;

    while ((method = bridge.GetNextMethod(m_klass, &iter)) != nullptr) {
        if (!IsValidPointer(method)) continue;

        const char* name = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);
        if (!name || !IsValidPointer((void*)name)) continue;

        uint32_t flags = *(uint32_t*)((uintptr_t)method + Offsets::METHOD_FLAGS);

        // Check for getter (get_PropertyName)
        if (strncmp(name, "get_", 4) == 0) {
            std::string propName = name + 4;
            propMethods[propName].getter = method;
            propMethods[propName].getterFlags = flags;
            propMethods[propName].getterSig = bridge.GetMethodSignature(method);
        }
        // Check for setter (set_PropertyName)
        else if (strncmp(name, "set_", 4) == 0) {
            std::string propName = name + 4;
            propMethods[propName].setter = method;
            propMethods[propName].setterFlags = flags;
            propMethods[propName].setterSig = bridge.GetMethodSignature(method);
        }
    }

    // Second pass: build PropertyInfo from collected methods
    for (auto& [propName, pair] : propMethods) {
        PropertyInfo info;
        info.name = propName;
        info.nameLower = propName;
        std::transform(info.nameLower.begin(), info.nameLower.end(), info.nameLower.begin(), ::tolower);
        info.canRead = pair.getter != nullptr;
        info.canWrite = pair.setter != nullptr;
        info.getterMethod = pair.getter;
        info.setterMethod = pair.setter;
        info.getterSignature = pair.getterSig;
        info.setterSignature = pair.setterSig;
        info.isEnum = false;
        info.valueTypeClass = nullptr;

        // Determine if static (both getter and setter must agree, prefer getter)
        if (pair.getter) {
            info.isStatic = (pair.getterFlags & MethodFlags::STATIC) != 0;
        } else if (pair.setter) {
            info.isStatic = (pair.setterFlags & MethodFlags::STATIC) != 0;
        } else {
            info.isStatic = false;
        }

        // Determine property type from getter return type or setter param type
        info.type = MonoTypeCode::Object; // Default

        if (pair.getter && pair.getterSig && IsValidPointer(pair.getterSig)) {
            void* retType = *(void**)((uintptr_t)pair.getterSig + Offsets::SIG_RET_TYPE);
            if (retType && IsValidPointer(retType)) {
                info.type = (MonoTypeCode)(*(uint8_t*)((uintptr_t)retType + Offsets::TYPE_CODE));

                // Check for enum or struct
                if (info.type == MonoTypeCode::ValueType) {
                    void* typeKlass = *(void**)retType; // data.klass at offset 0
                    if (typeKlass && IsValidPointer(typeKlass)) {
                        if (MonoBridge::IsEnumClass(typeKlass)) {
                            info.isEnum = true;
                            MonoClass enumClass(typeKlass);
                            info.enumValues = enumClass.GetEnumValues();
                        } else {
                            info.valueTypeClass = typeKlass;
                        }
                    }
                }
            }
        } else if (pair.setter && pair.setterSig && IsValidPointer(pair.setterSig)) {
            // Setter has 1 parameter - the value to set
            uint16_t paramCount = *(uint16_t*)((uintptr_t)pair.setterSig + Offsets::SIG_PARAM_COUNT);
            if (paramCount >= 1) {
                void* paramType = *(void**)((uintptr_t)pair.setterSig + Offsets::SIG_PARAMS);
                if (paramType && IsValidPointer(paramType)) {
                    info.type = (MonoTypeCode)(*(uint8_t*)((uintptr_t)paramType + Offsets::TYPE_CODE));

                    // Check for enum or struct
                    if (info.type == MonoTypeCode::ValueType) {
                        void* typeKlass = *(void**)paramType;
                        if (typeKlass && IsValidPointer(typeKlass)) {
                            if (MonoBridge::IsEnumClass(typeKlass)) {
                                info.isEnum = true;
                                MonoClass enumClass(typeKlass);
                                info.enumValues = enumClass.GetEnumValues();
                            } else {
                                info.valueTypeClass = typeKlass;
                            }
                        }
                    }
                }
            }
        }

        properties.push_back(std::move(info));
    }

    // Sort by name for consistent display
    std::sort(properties.begin(), properties.end(), [](const PropertyInfo& a, const PropertyInfo& b) { return a.name < b.name; });

    return properties;
}

// FIELD ENUMERATION - Iterate class fields with type resolution

// Helper: enumerate fields for a single class (no inheritance)
void MonoClass::EnumerateFieldsForClass(void* klass, bool wantStatic, std::vector<FieldInfo>& outFields) const {
    if (!klass || !IsValidPointer(klass)) return;

    auto& bridge = MonoBridge::Get();
    bridge.SetupFields(klass);

    uint32_t fieldCount = *(uint32_t*)((uintptr_t)klass + Offsets::CLASS_FIELD_COUNT);
    void* fieldsPtr = *(void**)((uintptr_t)klass + Offsets::CLASS_FIELDS);

    if (!fieldsPtr || !IsValidPointer(fieldsPtr) || fieldCount == 0 || fieldCount > 500) { return; }

    // Get declaring class name for inherited field annotation
    const char* declaringClassName = MonoBridge::GetClassName(klass);
    bool isInherited = (klass != m_klass);

    for (uint32_t i = 0; i < fieldCount; i++) {
        uintptr_t fieldAddr = (uintptr_t)fieldsPtr + i * Offsets::FIELD_SIZE;

        void* fieldType = *(void**)fieldAddr;
        if (!fieldType || !IsValidPointer(fieldType)) continue;

        const char* fieldName = *(const char**)(fieldAddr + Offsets::FIELD_NAME);
        if (!fieldName || !IsValidPointer((void*)fieldName)) continue;

        // Get type attributes
        uint16_t typeAttrs = *(uint16_t*)((uintptr_t)fieldType + Offsets::TYPE_ATTRS);
        bool isStatic = (typeAttrs & FieldFlags::STATIC) != 0;
        bool isLiteral = (typeAttrs & FieldFlags::LITERAL) != 0;

        // Filter by static/instance
        if (isStatic != wantStatic) continue;

        MonoTypeCode typeCode = (MonoTypeCode)(*(uint8_t*)((uintptr_t)fieldType + Offsets::TYPE_CODE));

        FieldInfo info;
        info.handle = (void*)fieldAddr;
        info.name = fieldName;
        // Pre-compute lowercase name for filtering
        info.nameLower = fieldName;
        std::transform(info.nameLower.begin(), info.nameLower.end(), info.nameLower.begin(), ::tolower);
        info.type = typeCode;
        info.isStatic = isStatic;
        info.isLiteral = isLiteral;
        info.isEnum = false;
        info.isInherited = isInherited;
        info.declaringClass = klass;
        info.declaringClassName = (declaringClassName && isInherited) ? declaringClassName : "";

        // Check if it's an enum or struct type
        if (typeCode == MonoTypeCode::ValueType) {
            void* fieldKlass = *(void**)fieldType; // data.klass at offset 0
            if (fieldKlass && IsValidPointer(fieldKlass)) {
                if (MonoBridge::IsEnumClass(fieldKlass)) {
                    info.isEnum = true;
                    // Get enum values
                    MonoClass enumClass(fieldKlass);
                    info.enumValues = enumClass.GetEnumValues();
                    // Pre-compute enum labels for combo boxes
                    info.enumLabels.reserve(info.enumValues.size());
                    for (const auto& ev : info.enumValues) { info.enumLabels.push_back(ev.name + " (" + std::to_string(ev.value) + ")"); }
                } else {
                    // It's a struct - store the class for field enumeration
                    info.valueTypeClass = fieldKlass;
                }
            }
        }

        // Extract array element type info
        if (typeCode == MonoTypeCode::SzArray) {
            // For arrays, data.klass at offset 0 is the array class (T[])
            void* arrayKlass = *(void**)fieldType;
            if (arrayKlass && IsValidPointer(arrayKlass)) {
                info.arrayElementClass = arrayKlass; // Store the array class for element size lookup
                // Element type code is at CLASS_ELEMENT_TYPE in the array class
                info.arrayElementType = (MonoTypeCode)(*(uint8_t*)((uintptr_t)arrayKlass + Offsets::CLASS_ELEMENT_TYPE));
            }
        }

        outFields.push_back(std::move(info));
    }
}

std::vector<FieldInfo> MonoClass::EnumerateFields(bool wantStatic, bool includeInherited) const {
    std::vector<FieldInfo> fields;
    if (!IsValid()) return fields;

    if (includeInherited) {
        // Walk up the inheritance chain, collecting fields from each class
        // We go from most-derived to base, so inherited fields appear after declared fields
        std::vector<void*> classChain;
        void* current = m_klass;

        while (current && IsValidPointer(current)) {
            classChain.push_back(current);
            current = MonoBridge::GetParentClass(current);
        }

        // Enumerate fields for each class in the chain
        for (void* klass : classChain) { EnumerateFieldsForClass(klass, wantStatic, fields); }
    } else {
        // Just enumerate fields for this class
        EnumerateFieldsForClass(m_klass, wantStatic, fields);
    }

    return fields;
}

std::vector<FieldInfo> MonoClass::GetStaticFields(bool includeInherited) const {
    return EnumerateFields(true, includeInherited);
}

std::vector<FieldInfo> MonoClass::GetInstanceFields(bool includeInherited) const {
    return EnumerateFields(false, includeInherited);
}

std::vector<FieldInfo> MonoClass::GetAllFields(bool includeInherited) const {
    auto staticFields = GetStaticFields(includeInherited);
    auto instanceFields = GetInstanceFields(includeInherited);
    staticFields.insert(staticFields.end(), instanceFields.begin(), instanceFields.end());
    return staticFields;
}

// ENUM VALUES - Extract named constants from enum classes
std::vector<EnumValue> MonoClass::GetEnumValues() const {
    std::vector<EnumValue> values;
    if (!IsValid() || !IsEnum()) return values;

    auto& bridge = MonoBridge::Get();
    bridge.SetupFields(m_klass);

    uint32_t fieldCount = *(uint32_t*)((uintptr_t)m_klass + Offsets::CLASS_FIELD_COUNT);
    void* fieldsPtr = *(void**)((uintptr_t)m_klass + Offsets::CLASS_FIELDS);

    if (!fieldsPtr || !IsValidPointer(fieldsPtr) || fieldCount == 0 || fieldCount > 1000) { return values; }

    void* vtable = GetVTable();

    for (uint32_t i = 0; i < fieldCount; i++) {
        uintptr_t fieldAddr = (uintptr_t)fieldsPtr + i * Offsets::FIELD_SIZE;

        const char* fieldName = *(const char**)(fieldAddr + Offsets::FIELD_NAME);
        if (!fieldName || !IsValidPointer((void*)fieldName)) continue;

        // Skip the "value__" field
        if (strcmp(fieldName, "value__") == 0) continue;

        // Use 8-byte buffer to safely handle any enum underlying type
        int64_t value = 0;
        if (vtable) { bridge.GetStaticFieldValue(vtable, (void*)fieldAddr, &value); }

        values.push_back({std::string(fieldName), (int32_t)value});
    }

    return values;
}

// SINGLETON DETECTION - Find self-referential static fields
bool MonoClass::HasSingletonField(std::string* outFieldName) const {
    if (!IsValid()) return false;

    auto& bridge = MonoBridge::Get();
    bridge.SetupFields(m_klass);

    uint32_t fieldCount = *(uint32_t*)((uintptr_t)m_klass + Offsets::CLASS_FIELD_COUNT);
    void* fieldsPtr = *(void**)((uintptr_t)m_klass + Offsets::CLASS_FIELDS);

    if (!fieldsPtr || !IsValidPointer(fieldsPtr) || fieldCount == 0 || fieldCount > 500) { return false; }

    for (uint32_t i = 0; i < fieldCount; i++) {
        uintptr_t fieldAddr = (uintptr_t)fieldsPtr + i * Offsets::FIELD_SIZE;

        void* fieldType = *(void**)fieldAddr;
        if (!fieldType || !IsValidPointer(fieldType)) continue;

        const char* fieldName = *(const char**)(fieldAddr + Offsets::FIELD_NAME);
        if (!fieldName || !IsValidPointer((void*)fieldName)) continue;

        // Check attributes - must be static
        uint16_t typeAttrs = *(uint16_t*)((uintptr_t)fieldType + Offsets::TYPE_ATTRS);
        bool isStatic = (typeAttrs & FieldFlags::STATIC) != 0;
        if (!isStatic) continue;

        // Must be Class type
        MonoTypeCode typeCode = (MonoTypeCode)(*(uint8_t*)((uintptr_t)fieldType + Offsets::TYPE_CODE));
        if (typeCode != MonoTypeCode::Class) continue;

        // Get the class of the field type
        void* fieldKlass = *(void**)fieldType; // data.klass at offset 0
        if (!fieldKlass || !IsValidPointer(fieldKlass)) continue;

        // Check if field type == this class (self-referential = singleton pattern)
        bool isSingleton = false;

        if (fieldKlass == m_klass) {
            isSingleton = true;
        } else {
            // Also check naming patterns
            if (strcmp(fieldName, "sInstance") == 0 || strcmp(fieldName, "sSingleton") == 0 || strcmp(fieldName, "gSingleton") == 0 || strcmp(fieldName, "Instance") == 0 || strcmp(fieldName, "Singleton") == 0 ||
                strcmp(fieldName, "s_Instance") == 0 || strcmp(fieldName, "s_instance") == 0 || strcmp(fieldName, "gInstance") == 0 || strcmp(fieldName, "kInstance") == 0) {
                isSingleton = true;
            }
        }

        if (isSingleton) {
            if (outFieldName) *outFieldName = fieldName;
            return true;
        }
    }

    return false;
}

void* MonoClass::GetSingletonInstance() const {
    if (!IsValid()) return nullptr;

    auto& bridge = MonoBridge::Get();
    bridge.SetupFields(m_klass);

    uint32_t fieldCount = *(uint32_t*)((uintptr_t)m_klass + Offsets::CLASS_FIELD_COUNT);
    void* fieldsPtr = *(void**)((uintptr_t)m_klass + Offsets::CLASS_FIELDS);

    if (!fieldsPtr || !IsValidPointer(fieldsPtr) || fieldCount == 0 || fieldCount > 500) { return nullptr; }

    void* vtable = GetVTable();
    if (!vtable) return nullptr;

    for (uint32_t i = 0; i < fieldCount; i++) {
        uintptr_t fieldAddr = (uintptr_t)fieldsPtr + i * Offsets::FIELD_SIZE;

        void* fieldType = *(void**)fieldAddr;
        if (!fieldType || !IsValidPointer(fieldType)) continue;

        const char* fieldName = *(const char**)(fieldAddr + Offsets::FIELD_NAME);
        if (!fieldName || !IsValidPointer((void*)fieldName)) continue;

        // Check attributes - must be static
        uint16_t typeAttrs = *(uint16_t*)((uintptr_t)fieldType + Offsets::TYPE_ATTRS);
        bool isStatic = (typeAttrs & FieldFlags::STATIC) != 0;
        if (!isStatic) continue;

        // Must be Class type
        MonoTypeCode typeCode = (MonoTypeCode)(*(uint8_t*)((uintptr_t)fieldType + Offsets::TYPE_CODE));
        if (typeCode != MonoTypeCode::Class) continue;

        // Get the class of the field type
        void* fieldKlass = *(void**)fieldType;

        // Check for singleton pattern
        bool isSingleton = (fieldKlass == m_klass) || strcmp(fieldName, "sInstance") == 0 || strcmp(fieldName, "sSingleton") == 0 || strcmp(fieldName, "gSingleton") == 0 || strcmp(fieldName, "Instance") == 0 ||
                           strcmp(fieldName, "Singleton") == 0 || strcmp(fieldName, "s_Instance") == 0 || strcmp(fieldName, "s_instance") == 0 || strcmp(fieldName, "gInstance") == 0 || strcmp(fieldName, "kInstance") == 0;

        if (isSingleton) {
            void* instancePtr = nullptr;
            bridge.GetStaticFieldValue(vtable, (void*)fieldAddr, &instancePtr);
            return instancePtr;
        }
    }

    return nullptr;
}

// STATIC LOOKUP - Find class by namespace and name
MonoClass MonoClass::Find(const std::string& nameSpace, const std::string& className) {
    void* klass = MonoBridge::Get().GetClassByName(nameSpace.c_str(), className.c_str());
    return MonoClass(klass);
}

// CLASS BROWSER - Enumerate classes across loaded images
void ClassBrowser::EnumerateByNamespace(const std::string& namespacePrefix, ClassCallback callback) {
    auto& bridge = MonoBridge::Get();
    if (!bridge.IsRuntimeReady()) return;

    // Initialize all images if not done yet
    if (!bridge.AreImagesInitialized()) { bridge.InitializeAllImages(); }

    size_t prefixLen = namespacePrefix.length();

    // Enumerate all cached images (discovered via seed classes)
    for (void* image : bridge.GetAllImages()) {
        if (!image || !IsValidPointer(image)) continue;

        int typeDefCount = bridge.GetTypeDefCount(image);
        if (typeDefCount <= 0 || typeDefCount > 50000) continue;

        for (int i = 1; i <= typeDefCount; i++) {
            void* klass = bridge.GetClassFromToken(image, TOKEN_TYPE_DEF | i);
            if (!klass || !IsValidPointer(klass)) continue;

            MonoClass mc(klass);

            // Check namespace prefix (nested classes inherit parent's namespace)
            std::string ns = mc.Namespace();
            if (ns.compare(0, prefixLen, namespacePrefix) != 0) continue;

            // Skip generic/compiler-generated
            std::string name = mc.Name();
            if (name.empty() || name[0] == '<' || name.find('`') != std::string::npos) continue;

            // Callback
            if (!callback(mc)) return;
        }
    }
}

std::vector<MonoClass> ClassBrowser::GetClassesInNamespace(const std::string& namespacePrefix, int maxClasses) {
    std::vector<MonoClass> classes;
    classes.reserve(maxClasses);

    EnumerateByNamespace(namespacePrefix, [&](const MonoClass& klass) {
        classes.push_back(klass);
        return (int)classes.size() < maxClasses;
    });

    return classes;
}

std::vector<MonoClass> ClassBrowser::GetClassesFromImage(void* image, const std::string& namespacePrefix, int maxClasses) {
    std::vector<MonoClass> classes;
    if (!image || !IsValidPointer(image)) {
        LOG_WARN("[ClassBrowser] GetClassesFromImage: invalid image pointer");
        return classes;
    }

    auto& bridge = MonoBridge::Get();
    if (!bridge.IsRuntimeReady()) {
        LOG_WARN("[ClassBrowser] GetClassesFromImage: runtime not ready (g_ScriptVM null)");
        return classes;
    }
    if (!bridge.IsInitialized()) {
        LOG_WARN("[ClassBrowser] GetClassesFromImage: MonoBridge not initialized (function pointers null)");
        return classes;
    }

    size_t prefixLen = namespacePrefix.length();
    int typeDefCount = bridge.GetTypeDefCount(image);
    if (typeDefCount <= 0 || typeDefCount > 50000) {
        LOG_WARN("[ClassBrowser] GetClassesFromImage: bad typeDefCount=" + std::to_string(typeDefCount));
        return classes;
    }

    LOG_DEBUG("[ClassBrowser] GetClassesFromImage: typeDefCount=" + std::to_string(typeDefCount));
    classes.reserve(typeDefCount < maxClasses ? typeDefCount : maxClasses);

    int nullCount = 0;
    for (int i = 1; i <= typeDefCount && (int)classes.size() < maxClasses; i++) {
        void* klass = bridge.GetClassFromToken(image, TOKEN_TYPE_DEF | i);
        if (!klass || !IsValidPointer(klass)) {
            nullCount++;
            continue;
        }

        MonoClass mc(klass);

        // Check namespace prefix if specified
        if (!namespacePrefix.empty()) {
            std::string ns = mc.Namespace();
            if (ns.compare(0, prefixLen, namespacePrefix) != 0) continue;
        }

        // Skip generic/compiler-generated
        std::string name = mc.Name();
        if (name.empty() || name[0] == '<' || name.find('`') != std::string::npos) continue;

        classes.push_back(mc);
    }

    if (nullCount > 0) { LOG_WARN("[ClassBrowser] GetClassesFromImage: " + std::to_string(nullCount) + "/" + std::to_string(typeDefCount) + " classes returned null from GetClassFromToken"); }

    return classes;
}

} // namespace S3MM
