#pragma once
#include "mod_compiler.h"
#include <vector>
#include <string>

namespace S3MM {

// LOADED MOD - A mod loaded into the Mono runtime
struct LoadedMod {
    std::string name;
    void* monoAssembly = nullptr; // MonoAssembly*
    void* monoImage = nullptr;    // MonoImage*

    // OnLoad lifecycle callbacks (MonoMethod* pointers)
    // Called after hooks are registered and applied
    std::vector<void*> onLoadMethods;

    // OnUnload lifecycle callbacks (MonoMethod* pointers)
    // Called before hooks are removed during hot-reload
    std::vector<void*> onUnloadMethods;

    // Source info for hot-reload recompilation
    ModInfo sourceInfo;

    // Hot-reload version counter (for unique filenames)
    int hotReloadVersion = 0;

    // S3MM API version this mod was compiled against, -1 = unknown/pre-compiled without metadata
    int apiVersion = -1;
};

// MOD LOADER - Loads compiled mod DLLs into the Mono runtime and registers hooks via HookManager
class ModLoader {
  public:
    static ModLoader& Get();

    // Initialize loader, must be called after Mono runtime is ready
    bool Initialize();

    // Load a single compiled mod
    bool LoadMod(const ModInfo& mod);

    // Load all compiled mods
    int LoadAllMods(const std::vector<ModInfo>& mods);

    // Scan a loaded mod for hooks and lifecycle callbacks
    void ScanForReplacements(LoadedMod& mod);

    // Hot-reload a mod: recompile, reload assembly, update patches
    bool HotReloadMod(const std::string& modName);

    // Fire OnLoad callbacks for a mod (after hooks are registered and applied)
    void FireOnLoadCallbacks(const std::string& modName);

    // Fire OnUnload callbacks for a mod (before removing hooks during hot-reload)
    void FireOnUnloadCallbacks(const std::string& modName);

    // Get loaded mods
    const std::vector<LoadedMod>& GetLoadedMods() const { return m_loadedMods; }

    // Resolve the logical mod name for a given MonoImage (matches a loaded mod's current image).
    // Returns empty string if none matches. Used so dynamically-registered (RegisterByPointer) hooks attribute to the stable mod name rather than the hot-reload-patched assembly name
    std::string GetModNameForImage(void* image) const;

    // Load a standalone assembly (e.g., S3MM.Helper.dll)
    void* LoadAssemblyFromPath(const std::filesystem::path& dllPath);

    // Load a standalone assembly from a raw memory buffer (e.g., embedded resource)
    // Takes ownership semantics: buffer can be freed after call (Mono copies internally)
    void* LoadAssemblyFromMemory(const char* data, uint32_t dataSize, const char* name);

  private:
    ModLoader() = default;
    ~ModLoader() = default;
    ModLoader(const ModLoader&) = delete;
    ModLoader& operator=(const ModLoader&) = delete;

    // Find Mono functions for assembly loading
    bool FindMonoFunctions();

    // Result of loading an assembly
    struct LoadResult {
        void* assembly = nullptr;
        void* image = nullptr; // The ACTUAL image we created (not from GetImageFromAssembly!)
    };

    // Load assembly using Mono API - returns both assembly and the actual image we created
    LoadResult LoadAssemblyEx(const std::filesystem::path& dllPath);

    // Load a pre-versioned DLL for hot-reload via memory-based loading
    LoadResult LoadImageForHotReload(const std::filesystem::path& dllPath);

    // Get image from assembly (WARNING: may return cached/old image!)
    void* GetImageFromAssembly(void* assembly);

    // Check for Hook_/Watch_ naming and register with HookManager
    bool CheckAndRegisterHook(void* method, const std::string& modName);

    // Check for [S3MM.Hook], [S3MM.Watcher] attributes and register hooks
    bool CheckAndRegisterHookFromAttribute(void* method, const std::string& modName);

    // Initialize S3MM attribute class cache (called once when first mod with S3MM.Helper is loaded)
    void InitializeAttributeClasses();

    // Scan an image for hooks and register them (for hot-reload). Returns number of hooks registered.
    int ScanImageForHooks(void* image, const std::string& modName);

    bool m_initialized = false;
    bool m_canLoadAssemblies = false;
    std::vector<LoadedMod> m_loadedMods;

    // S3MM.Helper image (stored when loaded)
    void* m_s3mmHelperImage = nullptr;

    // Cached S3MM attribute classes (for attribute-based hook detection)
    bool m_attributeClassesInitialized = false;
    void* m_hookAttrClass = nullptr;     // S3MM.HookAttribute
    void* m_watcherAttrClass = nullptr;  // S3MM.WatcherAttribute
    void* m_onLoadAttrClass = nullptr;   // S3MM.OnLoadAttribute
    void* m_onUnloadAttrClass = nullptr; // S3MM.OnUnloadAttribute

    // Check embedded S3MM API version in a loaded mod's image. Returns the version number, or -1 if no metadata found.
    int CheckApiVersion(void* image, const std::string& modName);

    // Find a method by name in the new image by iterating MethodDef tokens directly. This avoids triggering TypeRef resolution which can crash on unregistered images.
    void* FindMethodByNameInImage(void* image, const std::string& methodName);

    // Mono function pointers
    // mono_assembly_open_full takes 3 params: filename, status_out, refonly (0=normal, 1=reflection only)
    using mono_assembly_open_t = void*(__cdecl*)(const char* filename, int* status, int refonly);
    using mono_assembly_get_image_t = void*(__cdecl*)(void* assembly);
    using mono_image_get_table_rows_t = int(__cdecl*)(void* image, int tableId);
    using mono_class_get_t = void*(__cdecl*)(void* image, uint32_t typeToken);

    mono_assembly_open_t m_assemblyOpen = nullptr;
    mono_assembly_get_image_t m_assemblyGetImage = nullptr;
};

} // namespace S3MM
