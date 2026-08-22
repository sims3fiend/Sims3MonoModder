#include "audit_api.h"
#include "hook_manager.h"
#include "mod_loader.h"
#include "conflict_detector.h"
#include "../core/addresses.h"
#include "../core/assembly_hooks.h"
#include "../core/mono_bridge.h"
#include "../core/mono_types.h"
#include "../util/logger.h"
#include <sstream>

namespace S3MM {

// Static member definitions
std::unordered_map<std::string, bool> AuditAPI::s_classMethodIndex;
size_t AuditAPI::s_cachedChainCount = 0;

// Managed type cache
bool AuditAPI::s_typesInitialized = false;
void* AuditAPI::s_hookInfoKlass = nullptr;
int32_t AuditAPI::s_hookInfoElemSize = 0;
int32_t AuditAPI::s_hiTargetOff = 0;
int32_t AuditAPI::s_hiModNameOff = 0;
int32_t AuditAPI::s_hiPriorityOff = 0;
int32_t AuditAPI::s_hiIsWatcherOff = 0;
int32_t AuditAPI::s_hiIsEntryOff = 0;

void* AuditAPI::s_replacedAsmKlass = nullptr;
int32_t AuditAPI::s_replacedAsmElemSize = 0;
int32_t AuditAPI::s_raAssemblyNameOff = 0;
int32_t AuditAPI::s_raSourcePackageOff = 0;

// MANAGED TYPE RESOLUTION - Resolve MonoClass* and field offsets from S3MM.Helper image, mono has a 4-byte MonoObject (just MonoVTable*), so field offsets include sizeof(MonoObject) = 4; subtract 4 for unboxed storage
static constexpr int32_t MONO_OBJECT_HEADER = 4;

bool AuditAPI::InitManagedTypes() {
    if (s_typesInitialized) return true;

    auto& bridge = MonoBridge::Get();

    // Find the S3MM.Helper image
    if (!bridge.AreImagesInitialized()) { bridge.InitializeAllImages(); }

    void* s3mmImage = nullptr;
    const auto& images = bridge.GetAllImages();
    for (void* image : images) {
        const char* name = MonoBridge::GetImageName(image);
        if (!name) continue;
        // Match "S3MM.Helper" or just "S3MM" assembly name
        if (strcmp(name, "S3MM.Helper") == 0 || strcmp(name, "S3MM") == 0) {
            s3mmImage = image;
            break;
        }
    }

    if (!s3mmImage) {
        LOG_ERROR("[AuditAPI] Failed to find S3MM.Helper image - managed types not available");
        return false;
    }

    // Resolve HookInfo
    s_hookInfoKlass = bridge.GetClassFromNameInImage(s3mmImage, "S3MM", "HookInfo");
    if (!s_hookInfoKlass) {
        LOG_ERROR("[AuditAPI] Failed to resolve S3MM.HookInfo class");
        return false;
    }

    // Element size = instance_size - sizeof(MonoObject)
    uint32_t instanceSize = *(uint32_t*)((uintptr_t)s_hookInfoKlass + Offsets::CLASS_INSTANCE_SIZE);
    s_hookInfoElemSize = static_cast<int32_t>(instanceSize - MONO_OBJECT_HEADER);

    // Resolve field offsets (adjusted for unboxed: field->offset - 8)
    auto resolveFieldOffset = [&](void* klass, const char* fieldName) -> int32_t {
        void* field = bridge.GetFieldFromName(klass, fieldName);
        if (!field) {
            LOG_ERROR("[AuditAPI] Failed to resolve field: " + std::string(fieldName));
            return -1;
        }
        int32_t rawOffset = *(int32_t*)((uintptr_t)field + Offsets::FIELD_OFFSET);
        return rawOffset - MONO_OBJECT_HEADER;
    };

    s_hiTargetOff = resolveFieldOffset(s_hookInfoKlass, "Target");
    s_hiModNameOff = resolveFieldOffset(s_hookInfoKlass, "ModName");
    s_hiPriorityOff = resolveFieldOffset(s_hookInfoKlass, "Priority");
    s_hiIsWatcherOff = resolveFieldOffset(s_hookInfoKlass, "IsWatcher");
    s_hiIsEntryOff = resolveFieldOffset(s_hookInfoKlass, "IsEntry");

    if (s_hiTargetOff < 0 || s_hiModNameOff < 0 || s_hiPriorityOff < 0 || s_hiIsWatcherOff < 0 || s_hiIsEntryOff < 0) {
        LOG_ERROR("[AuditAPI] HookInfo field resolution failed");
        return false;
    }

    LOG_DEBUG("[AuditAPI] HookInfo resolved: elemSize=" + std::to_string(s_hookInfoElemSize) + " Target@" + std::to_string(s_hiTargetOff) + " ModName@" + std::to_string(s_hiModNameOff) + " Priority@" +
              std::to_string(s_hiPriorityOff) + " IsWatcher@" + std::to_string(s_hiIsWatcherOff) + " IsEntry@" + std::to_string(s_hiIsEntryOff));

    // Resolve ReplacedAssembly
    s_replacedAsmKlass = bridge.GetClassFromNameInImage(s3mmImage, "S3MM", "ReplacedAssembly");
    if (!s_replacedAsmKlass) {
        LOG_ERROR("[AuditAPI] Failed to resolve S3MM.ReplacedAssembly class");
        return false;
    }

    instanceSize = *(uint32_t*)((uintptr_t)s_replacedAsmKlass + Offsets::CLASS_INSTANCE_SIZE);
    s_replacedAsmElemSize = static_cast<int32_t>(instanceSize - MONO_OBJECT_HEADER);

    s_raAssemblyNameOff = resolveFieldOffset(s_replacedAsmKlass, "AssemblyName");
    s_raSourcePackageOff = resolveFieldOffset(s_replacedAsmKlass, "SourcePackage");

    if (s_raAssemblyNameOff < 0 || s_raSourcePackageOff < 0) {
        LOG_ERROR("[AuditAPI] ReplacedAssembly field resolution failed");
        return false;
    }

    LOG_DEBUG("[AuditAPI] ReplacedAssembly resolved: elemSize=" + std::to_string(s_replacedAsmElemSize) + " AssemblyName@" + std::to_string(s_raAssemblyNameOff) + " SourcePackage@" + std::to_string(s_raSourcePackageOff));

    s_typesInitialized = true;
    LOG_INFO("[AuditAPI] Managed types initialized successfully");
    return true;
}

// WRITE HOOKINFO - Write a HookInfo element into a value type array
void AuditAPI::WriteHookInfo(void* array, int32_t index, void* targetStr, const char* modName, int32_t priority, bool isWatcher, bool isEntry) {
    auto& bridge = MonoBridge::Get();
    char* elem = (char*)bridge.GetArrayElementPtr(array, index, s_hookInfoElemSize);
    if (!elem) return;

    // Write reference fields (MonoString* pointers)
    *(void**)(elem + s_hiTargetOff) = targetStr;
    *(void**)(elem + s_hiModNameOff) = bridge.CreateString(modName);

    // Write value fields
    *(int32_t*)(elem + s_hiPriorityOff) = priority;

    // Bool in Mono sequential struct: 1 byte at the field offset. Zero the full int32 first to clear padding, then write the byte.
    *(int32_t*)(elem + s_hiIsWatcherOff) = 0;
    *(uint8_t*)(elem + s_hiIsWatcherOff) = isWatcher ? 1 : 0;
    *(int32_t*)(elem + s_hiIsEntryOff) = 0;
    *(uint8_t*)(elem + s_hiIsEntryOff) = isEntry ? 1 : 0;
}

// INDEX MANAGEMENT - Rebuild class:method lookup index
void AuditAPI::RebuildClassMethodIndex() {
    const auto& chains = HookManager::Get().GetAllChains();
    if (chains.size() == s_cachedChainCount) return;

    s_classMethodIndex.clear();
    s_classMethodIndex.reserve(chains.size());
    for (const auto& [methodPtr, chain] : chains) {
        // A chain with no live entries is not "hooked" e.g. one whose only hooks were rejected (ineligible typed) or removed on hot-reload. Indexing it would mis-report IsMethodHooked.
        if (chain.hooks.empty() && chain.watchers.empty()) continue;
        std::string key = chain.targetClass + ":" + chain.targetMethod;
        s_classMethodIndex[key] = true;
    }
    s_cachedChainCount = chains.size();
    LOG_DEBUG("[AuditAPI] Rebuilt classMethod index: " + std::to_string(s_cachedChainCount) + " entries");
}

// TIER 1 - Fast checks
int32_t __cdecl AuditAPI::NativeIsMethodHooked(void* classMethodStr) {
    if (!classMethodStr) return 0;
    std::string classMethod = MonoBridge::Get().ExtractString(classMethodStr);
    if (classMethod.empty()) return 0;

    RebuildClassMethodIndex();
    return s_classMethodIndex.count(classMethod) > 0 ? 1 : 0;
}

int32_t __cdecl AuditAPI::NativeIsAssemblyReplaced(void* assemblyNameStr) {
    if (!assemblyNameStr) return 0;
    std::string name = MonoBridge::Get().ExtractString(assemblyNameStr);
    if (name.empty()) return 0;

    return IsCoreAssemblyReplaced(name.c_str()) ? 1 : 0;
}

// Did "Class:Method" install on the zero-overhead baked chain? Source is HookChain.bakedActive (set in TryInstallBakedChain, cleared when a baked chain empties)
// A method on the boxing path - or a typed hook that was rejected and stripped - reports 0 here while IsMethodHooked may still report 1/0.
int32_t __cdecl AuditAPI::NativeIsMethodBaked(void* classMethodStr) {
    if (!classMethodStr) return 0;
    std::string classMethod = MonoBridge::Get().ExtractString(classMethodStr);
    if (classMethod.empty()) return 0;

    // Parse "Class:Method" (same split as NativeGetHookChain).
    size_t colonPos = classMethod.rfind(':');
    if (colonPos == std::string::npos || colonPos == 0) return 0;

    std::string targetClass = classMethod.substr(0, colonPos);
    std::string targetMethod = classMethod.substr(colonPos + 1);

    HookChain* chain = HookManager::Get().GetChainByName(targetClass, targetMethod);
    return (chain && chain->bakedActive) ? 1 : 0;
}

// Why did a typed hook here fail to bake? Source is HookChain.bakeRejectReason (set in DemoteIneligibleTypedHooks, cleared on a clean bake). null = baked / no typed hooks / not hooked
void* __cdecl AuditAPI::NativeGetBakeRejectReason(void* classMethodStr) {
    if (!classMethodStr) return nullptr;
    std::string classMethod = MonoBridge::Get().ExtractString(classMethodStr);
    if (classMethod.empty()) return nullptr;

    size_t colonPos = classMethod.rfind(':');
    if (colonPos == std::string::npos || colonPos == 0) return nullptr;

    std::string targetClass = classMethod.substr(0, colonPos);
    std::string targetMethod = classMethod.substr(colonPos + 1);

    HookChain* chain = HookManager::Get().GetChainByName(targetClass, targetMethod);
    if (!chain || chain->bakeRejectReason.empty()) return nullptr;
    return MonoBridge::Get().CreateString(chain->bakeRejectReason.c_str());
}

// TIER 2 - Bulk queries returning typed managed arrays
// Count only chains with LIVE entries - matches RebuildClassMethodIndex / IsMethodHooked, which skip chains emptied by rejection (ineligible typed) or hot-reload removal. Counting raw m_chains would over-report.
int32_t __cdecl AuditAPI::NativeGetHookedMethodCount() {
    int32_t count = 0;
    for (const auto& [methodPtr, chain] : HookManager::Get().GetAllChains()) {
        if (!chain.hooks.empty() || !chain.watchers.empty()) ++count;
    }
    return count;
}

void* __cdecl AuditAPI::NativeGetAllHooks() {
    if (!InitManagedTypes()) return nullptr;

    auto& bridge = MonoBridge::Get();
    const auto& chains = HookManager::Get().GetAllChains();

    // Count total entries
    int32_t totalEntries = 0;
    for (const auto& [methodPtr, chain] : chains) { totalEntries += static_cast<int32_t>(chain.watchers.size() + chain.hooks.size()); }
    if (totalEntries == 0) return nullptr;

    // Create HookInfo[] value type array
    void* array = bridge.CreateArray(s_hookInfoKlass, totalEntries);
    if (!array) {
        LOG_ERROR("[AuditAPI] Failed to create HookInfo[] array of size " + std::to_string(totalEntries));
        return nullptr;
    }

    int32_t idx = 0;
    for (const auto& [methodPtr, chain] : chains) {
        // Build target string once per chain, reuse for all entries
        std::string target = chain.targetClass + ":" + chain.targetMethod;
        void* targetStr = bridge.CreateString(target.c_str());

        for (const auto& w : chain.watchers) { WriteHookInfo(array, idx++, targetStr, w.modName.c_str(), w.priority, true, false); }
        for (const auto& h : chain.hooks) { WriteHookInfo(array, idx++, targetStr, h.modName.c_str(), h.priority, false, &h == &chain.hooks.front()); }
    }

    return array;
}

void* __cdecl AuditAPI::NativeGetHookChain(void* classMethodStr) {
    if (!classMethodStr) return nullptr;
    if (!InitManagedTypes()) return nullptr;

    auto& bridge = MonoBridge::Get();
    std::string classMethod = bridge.ExtractString(classMethodStr);
    if (classMethod.empty()) return nullptr;

    // Parse "Class:Method"
    size_t colonPos = classMethod.rfind(':');
    if (colonPos == std::string::npos || colonPos == 0) return nullptr;

    std::string targetClass = classMethod.substr(0, colonPos);
    std::string targetMethod = classMethod.substr(colonPos + 1);

    HookChain* chain = HookManager::Get().GetChainByName(targetClass, targetMethod);
    if (!chain) return nullptr;

    int32_t totalEntries = static_cast<int32_t>(chain->watchers.size() + chain->hooks.size());
    if (totalEntries == 0) return nullptr;

    void* array = bridge.CreateArray(s_hookInfoKlass, totalEntries);
    if (!array) return nullptr;

    void* targetStr = bridge.CreateString(classMethod.c_str());

    int32_t idx = 0;
    for (const auto& w : chain->watchers) { WriteHookInfo(array, idx++, targetStr, w.modName.c_str(), w.priority, true, false); }
    for (const auto& h : chain->hooks) { WriteHookInfo(array, idx++, targetStr, h.modName.c_str(), h.priority, false, &h == &chain->hooks.front()); }

    return array;
}

void* __cdecl AuditAPI::NativeGetReplacedAssemblies() {
    if (!InitManagedTypes()) return nullptr;

    auto& bridge = MonoBridge::Get();
    auto sources = GetAllAssemblyPackageSources();
    auto replaced = GetReplacedCoreAssemblies();

    if (replaced.empty()) return nullptr;

    void* array = bridge.CreateArray(s_replacedAsmKlass, static_cast<int32_t>(replaced.size()));
    if (!array) return nullptr;

    int32_t idx = 0;
    for (const auto& name : replaced) {
        char* elem = (char*)bridge.GetArrayElementPtr(array, idx, s_replacedAsmElemSize);
        if (!elem) {
            idx++;
            continue;
        }

        *(void**)(elem + s_raAssemblyNameOff) = bridge.CreateString(name.c_str());

        auto it = sources.find(name);
        const char* source = (it != sources.end()) ? it->second.c_str() : "";
        *(void**)(elem + s_raSourcePackageOff) = bridge.CreateString(source);

        idx++;
    }

    return array;
}

int32_t __cdecl AuditAPI::NativeGetLoadedModCount() {
    return static_cast<int32_t>(ModLoader::Get().GetLoadedMods().size());
}

void* __cdecl AuditAPI::NativeGetLoadedMods() {
    auto& bridge = MonoBridge::Get();
    const auto& mods = ModLoader::Get().GetLoadedMods();

    if (mods.empty()) return nullptr;

    // Create string[] using mono_defaults.string_class for the element type
    uintptr_t stringClassAddr = Addresses::Defaults.string_class;
    if (!stringClassAddr) {
        LOG_ERROR("[AuditAPI] mono_defaults.string_class address not resolved");
        return nullptr;
    }
    void* stringKlass = *(void**)stringClassAddr;
    if (!stringKlass) {
        LOG_ERROR("[AuditAPI] mono_defaults.string_class is null");
        return nullptr;
    }

    void* array = bridge.CreateArray(stringKlass, static_cast<int32_t>(mods.size()));
    if (!array) return nullptr;

    // string[] is a reference type array, element size = sizeof(ptr) = 4
    for (int32_t i = 0; i < static_cast<int32_t>(mods.size()); i++) {
        void* elemPtr = bridge.GetArrayElementPtr(array, i, sizeof(void*));
        if (!elemPtr) continue;
        *(void**)elemPtr = bridge.CreateString(mods[i].name.c_str());
    }

    return array;
}

// A mod's refused hook installs, from HookManager's per-mod refusal records. empty/null modName -> all mods
void* __cdecl AuditAPI::NativeGetRefusedHooks(void* modNameStr) {
    auto& bridge = MonoBridge::Get();
    std::string modName = modNameStr ? bridge.ExtractString(modNameStr) : "";

    std::vector<std::string> lines;
    for (const auto& [mod, stats] : HookManager::Get().GetModHookStats()) {
        if (!modName.empty() && mod != modName) continue;
        for (const auto& r : stats.refusals) { lines.push_back((modName.empty() ? mod + ": " : "") + r.target + " - " + r.reason); }
    }
    if (lines.empty()) return nullptr;

    // string[] via mono_defaults.string_class (same pattern as NativeGetLoadedMods)
    uintptr_t stringClassAddr = Addresses::Defaults.string_class;
    if (!stringClassAddr) {
        LOG_ERROR("[AuditAPI] mono_defaults.string_class address not resolved");
        return nullptr;
    }
    void* stringKlass = *(void**)stringClassAddr;
    if (!stringKlass) {
        LOG_ERROR("[AuditAPI] mono_defaults.string_class is null");
        return nullptr;
    }

    void* array = bridge.CreateArray(stringKlass, static_cast<int32_t>(lines.size()));
    if (!array) return nullptr;

    for (int32_t i = 0; i < static_cast<int32_t>(lines.size()); i++) {
        void* elemPtr = bridge.GetArrayElementPtr(array, i, sizeof(void*));
        if (!elemPtr) continue;
        *(void**)elemPtr = bridge.CreateString(lines[i].c_str());
    }

    return array;
}

} // namespace S3MM
