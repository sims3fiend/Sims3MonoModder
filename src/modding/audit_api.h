#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace S3MM {

// AUDIT API - Read-only introspection of hook chains, assembly conflicts, and loaded mods. Consumers: error trap mods (stack trace scanning), diagnostic tools.
// Thread safety: ICALLs run on game thread. HookManager modified only during ProcessMods. ConflictDetector uses its own mutexes. Tier 2 methods return managed arrays natively.
class AuditAPI {
  public:
    // TIER 1 - Fast checks for ErrorTrap stack trace scanning

    // Returns 1 if any hook chain targets "Class:Method", else 0. O(1) via cached index.
    static int32_t __cdecl NativeIsMethodHooked(void* classMethodStr);

    // Returns 1 if a core assembly was loaded from .package, else 0
    static int32_t __cdecl NativeIsAssemblyReplaced(void* assemblyNameStr);

    // TIER 2 - Bulk queries returning typed managed arrays

    // Total number of hook chains (not individual hooks)
    static int32_t __cdecl NativeGetHookedMethodCount();

    // Returns HookInfo[] with all chains' entries, or nullptr if empty
    static void* __cdecl NativeGetAllHooks();

    // Returns HookInfo[] for a single "Class:Method", or nullptr if not found
    static void* __cdecl NativeGetHookChain(void* classMethodStr);

    // Returns ReplacedAssembly[] of replaced core assemblies, or nullptr if empty
    static void* __cdecl NativeGetReplacedAssemblies();

    // Total S3MM mods loaded
    static int32_t __cdecl NativeGetLoadedModCount();

    // Returns string[] of loaded mod names, or nullptr if empty
    static void* __cdecl NativeGetLoadedMods();

  private:
    // Lazily-built index for O(1) IsMethodHooked lookup
    static void RebuildClassMethodIndex();
    static std::unordered_map<std::string, bool> s_classMethodIndex;
    static size_t s_cachedChainCount;

    // Cached managed type handles, resolved lazily on first T2 call
    static bool s_typesInitialized;
    static bool InitManagedTypes(); // Returns false on failure

    // HookInfo struct: { string Target, string ModName, int Priority, bool IsWatcher }
    static void* s_hookInfoKlass;      // MonoClass* for S3MM.HookInfo
    static int32_t s_hookInfoElemSize; // Unboxed size (instance_size - 8)
    static int32_t s_hiTargetOff;      // field->offset - 8 (adjusted for inline value type)
    static int32_t s_hiModNameOff;
    static int32_t s_hiPriorityOff;
    static int32_t s_hiIsWatcherOff;

    // ReplacedAssembly struct: { string AssemblyName, string SourcePackage }
    static void* s_replacedAsmKlass;
    static int32_t s_replacedAsmElemSize;
    static int32_t s_raAssemblyNameOff;
    static int32_t s_raSourcePackageOff;

    // Helper: write a HookInfo element into array at index
    static void WriteHookInfo(void* array, int32_t index, void* targetStr, const char* modName, int32_t priority, bool isWatcher);
};

} // namespace S3MM
