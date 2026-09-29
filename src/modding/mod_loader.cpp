#include "mod_loader.h"
#include "persist_bridge.h"
#include "mod_compiler.h"
#include "hook_manager.h"
#include "../core/mono_bridge.h"
#include "../core/addresses.h"
#include "../core/vanilla_extractor.h"
#include "../util/logger.h"
#include "../core/exception_helper.h"
#include "../ui/mod_ui.h"
#include <fstream>
#include <cstring>
#include <cstdlib>

namespace S3MM {

// Mono table IDs
constexpr int MONO_TABLE_TYPEDEF = 0x02;
constexpr int MONO_TABLE_METHODDEF = 0x06;

// mono_loader_get_last_error returns a pointer to a MonoLoaderError struct or null
using mono_loader_get_last_error_t = void*(__cdecl*)();
using mono_loader_clear_error_t = void(__cdecl*)();

// mono_image_open_from_data_full - loads a MonoImage from memory buffer (data, data_len, need_copy, status_out, refonly)
using mono_image_open_from_data_t = void*(__cdecl*)(char* data, uint32_t data_len, int need_copy, int* status, int refonly);

// mono_assembly_load_from_full - registers a MonoImage as an assembly (image, fname, status_out, refonly)
using mono_assembly_load_from_t = void*(__cdecl*)(void* image, const char* fname, int* status, int refonly);

// mono_get_method - gets a MonoMethod* from image + token (image, token, context)
using mono_get_method_t = void*(__cdecl*)(void* image, uint32_t token, void* context);

// mono_image_get_table_rows - gets number of rows in a metadata table (image, tableId)
using mono_image_get_table_rows_t = int(__cdecl*)(void* image, int tableId);

static mono_loader_get_last_error_t s_loaderGetError = nullptr;
static mono_loader_clear_error_t s_loaderClearError = nullptr;
static mono_image_open_from_data_t s_imageOpenFromData = nullptr;
static mono_assembly_load_from_t s_assemblyLoadFrom = nullptr;
static mono_get_method_t s_getMethod = nullptr;

// Counter for unique assembly names across hot-reload calls
static int s_hotReloadCounter = 0;

// Patch the internal CLI assembly name in-place within the #Strings heap to make it unique for hot-reload.
// Prevents mono_assembly_load_from_full from returning the cached (old) assembly on name collision.
static bool PatchAssemblyNameForHotReload(char* fileData, uint32_t fileSize, int version) {
    PEMetadata meta;
    if (!InitPEMetadata(&meta, (const uint8_t*)fileData, fileSize)) return false;
    if (meta.tableRows[0x20] == 0) return false; // No Assembly table

    int strSize = (meta.heapSizes & 1) ? 4 : 2;
    int blobSize = (meta.heapSizes & 4) ? 4 : 2;

    // Assembly table (0x20) row layout:
    //   HashAlgId(4) + Major(2) + Minor(2) + Build(2) + Rev(2) + Flags(4) + PubKey(blob) + Name(str) + Culture(str)
    // Name column offset within row = 16 + blobSize
    const uint8_t* assemblyRow = meta.tableData + meta.tableOffsets[0x20];
    uint32_t nameIndex;
    if (strSize == 2)
        nameIndex = *(const uint16_t*)(assemblyRow + 16 + blobSize);
    else
        nameIndex = *(const uint32_t*)(assemblyRow + 16 + blobSize);

    // Compute file offset of the name string
    ptrdiff_t stringsHeapFileOffset = meta.stringsHeap - (const uint8_t*)fileData;
    char* namePtr = fileData + stringsHeapFileOffset + nameIndex;
    size_t nameLen = strlen(namePtr);

    if (nameLen < 2) return false; // Name too short to safely patch

    // Replace last character with version-dependent char (0-9, a-z = 36 unique values)
    // Same-length replacement ensures no corruption of adjacent #Strings entries
    int v = version % 36;
    char suffix = (v < 10) ? (char)('0' + v) : (char)('a' + v - 10);
    namePtr[nameLen - 1] = suffix;

    return true;
}

ModLoader& ModLoader::Get() {
    static ModLoader instance;
    return instance;
}

bool ModLoader::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[ModLoader] Initializing...");

    // Wait for Mono runtime to be ready
    if (!MonoBridge::Get().IsRuntimeReady()) {
        LOG_ERROR("[ModLoader] Mono runtime not ready");
        return false;
    }

    // Set up TLS for this thread
    MonoBridge::Get().SetupTLS();

    // Initialize function pointers from resolved addresses
    s_loaderGetError = (mono_loader_get_last_error_t)Addresses::Assemblies.mono_loader_get_last_error;
    s_loaderClearError = (mono_loader_clear_error_t)Addresses::Assemblies.mono_loader_clear_error;
    s_imageOpenFromData = (mono_image_open_from_data_t)Addresses::Assemblies.mono_image_open_from_data_full;
    s_assemblyLoadFrom = (mono_assembly_load_from_t)Addresses::Assemblies.mono_assembly_load_from_full;
    s_getMethod = (mono_get_method_t)Addresses::Methods.mono_get_method;

    // Find Mono functions for assembly loading
    if (!FindMonoFunctions()) {
        LOG_WARN("[ModLoader] Could not find assembly loading functions");
        LOG_WARN("[ModLoader] Mod loading will use alternative method");
        m_canLoadAssemblies = false;
    } else {
        m_canLoadAssemblies = true;
    }

    // Clean up any leftover hot-reload files from previous sessions
    std::filesystem::path hotReloadDir = ModCompiler::Get().GetModsPath() / "HotReload";
    if (std::filesystem::exists(hotReloadDir)) {
        std::error_code ec;
        std::filesystem::remove_all(hotReloadDir, ec);
        if (!ec) { LOG_DEBUG("[ModLoader] Cleaned up HotReload folder"); }
    }

    m_initialized = true;
    LOG_INFO("[ModLoader] Initialized");
    return true;
}

bool ModLoader::FindMonoFunctions() {
    // Pull the already-resolved addresses from the Addresses pipeline (pattern scan + call-site extraction, with fallbacks).
    // mono_assembly_open loads a .dll/.exe assembly; mono_assembly_get_image gets MonoImage* from MonoAssembly*.

    m_assemblyOpen = (mono_assembly_open_t)Addresses::Assemblies.mono_assembly_open_full;
    m_assemblyGetImage = (mono_assembly_get_image_t)Addresses::Assemblies.mono_assembly_get_image;

    // Basic validation - check if addresses look like code
    if (!IsValidPointer((void*)m_assemblyOpen) || !IsValidPointer((void*)m_assemblyGetImage)) {
        LOG_WARN("[ModLoader] Assembly function addresses appear invalid");
        m_assemblyOpen = nullptr;
        m_assemblyGetImage = nullptr;
        return false;
    }

    LOG_INFO("[ModLoader] Found mono_assembly_open at " + std::to_string((uintptr_t)m_assemblyOpen));

    return true;
}

ModLoader::LoadResult ModLoader::LoadAssemblyEx(const std::filesystem::path& dllPath) {
    LoadResult result;

    // Ensure TLS is set up
    MonoBridge::Get().SetupTLS();

    // Check if file exists first
    if (!std::filesystem::exists(dllPath)) {
        LOG_ERROR("[ModLoader] Assembly file does not exist: " + dllPath.string());
        return result;
    }

    // Get absolute path for display
    std::string pathStr = std::filesystem::absolute(dllPath).string();

    // Log file info
    auto fileSize = std::filesystem::file_size(dllPath);
    LOG_DEBUG("[ModLoader] Loading assembly: " + pathStr + " (size: " + std::to_string(fileSize) + " bytes)");

    // MEMORY-BASED LOADING - Read DLL into memory via mono_image_open_from_data_full, bypassing the game's file loader

    // Read file into memory
    HANDLE hFile = CreateFileA(pathStr.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_ERROR("[ModLoader] Failed to open file: " + pathStr + " (error: " + std::to_string(GetLastError()) + ")");
        return result;
    }

    DWORD fileSizeWin = GetFileSize(hFile, nullptr);
    if (fileSizeWin == INVALID_FILE_SIZE || fileSizeWin == 0) {
        CloseHandle(hFile);
        LOG_ERROR("[ModLoader] Invalid file size for: " + pathStr);
        return result;
    }

    // Allocate buffer for file data
    char* fileData = (char*)malloc(fileSizeWin);
    if (!fileData) {
        CloseHandle(hFile);
        LOG_ERROR("[ModLoader] Failed to allocate memory for file");
        return result;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(hFile, fileData, fileSizeWin, &bytesRead, nullptr) || bytesRead != fileSizeWin) {
        free(fileData);
        CloseHandle(hFile);
        LOG_ERROR("[ModLoader] Failed to read file");
        return result;
    }
    CloseHandle(hFile);

    LOG_DEBUG("[ModLoader] Read " + std::to_string(bytesRead) + " bytes into memory");

    // Create MonoImage from memory buffer
    int status = 0;
    void* image = s_imageOpenFromData(fileData, fileSizeWin, 1, &status, 0); // need_copy=1, refonly=0

    // We can free our buffer since need_copy=1 means Mono copied it
    free(fileData);

    if (!image || status != 0) {
        std::string statusName;
        switch (status) {
        case 0:
            statusName = "OK";
            break;
        case 1:
            statusName = "ERRNO";
            break;
        case 2:
            statusName = "MISSING_ASSEMBLYREF";
            break;
        case 3:
            statusName = "IMAGE_INVALID";
            break;
        default:
            statusName = "UNKNOWN";
            break;
        }
        LOG_ERROR("[ModLoader] mono_image_open_from_data_full failed (status=" + std::to_string(status) + " = " + statusName + ")");
        return result;
    }

    LOG_DEBUG("[ModLoader] MonoImage created successfully @ " + std::to_string((uintptr_t)image));

    // Store the image we actually created - this is critical for hot-reload!
    result.image = image;

    // Register the image as an assembly
    status = 0;
    void* assembly = s_assemblyLoadFrom(image, pathStr.c_str(), &status, 0); // refonly=0

    if (!assembly || status != 0) {
        std::string statusName;
        switch (status) {
        case 0:
            statusName = "OK";
            break;
        case 1:
            statusName = "ERRNO";
            break;
        case 2:
            statusName = "MISSING_ASSEMBLYREF";
            break;
        case 3:
            statusName = "IMAGE_INVALID";
            break;
        default:
            statusName = "UNKNOWN";
            break;
        }
        LOG_ERROR("[ModLoader] mono_assembly_load_from_full failed (status=" + std::to_string(status) + " = " + statusName + ")");

        // Try to get more error info from Mono's loader
        void* loaderError = s_loaderGetError();
        if (loaderError) {
            int exType = *(int*)loaderError;
            const char* msg = *(const char**)((uintptr_t)loaderError + 8);
            LOG_ERROR("[ModLoader] Mono loader error: type=" + std::to_string(exType) + ", msg=" + (msg ? msg : "(null)"));
            s_loaderClearError();
        }

        return result; // Still return - we have the image even if assembly registration failed
    }

    result.assembly = assembly;
    LOG_INFO("[ModLoader] Assembly loaded successfully: " + pathStr);
    return result;
}

ModLoader::LoadResult ModLoader::LoadImageForHotReload(const std::filesystem::path& dllPath) {
    // Hot-reload uses memory-based loading to bypass EA's file loader.

    LoadResult result;

    // Ensure TLS is set up (critical for Mono operations)
    MonoBridge::Get().SetupTLS();

    LOG_DEBUG("[ModLoader] Loading versioned DLL: " + dllPath.string());

    // Read file into memory (same approach as LoadAssemblyEx)
    std::string pathStr = dllPath.string();
    HANDLE hFile = CreateFileA(pathStr.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_ERROR("[ModLoader] Failed to open file: " + pathStr + " (error: " + std::to_string(GetLastError()) + ")");
        return result;
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        LOG_ERROR("[ModLoader] Invalid file size for: " + pathStr);
        return result;
    }

    char* fileData = (char*)malloc(fileSize);
    if (!fileData) {
        CloseHandle(hFile);
        LOG_ERROR("[ModLoader] Failed to allocate memory for file");
        return result;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(hFile, fileData, fileSize, &bytesRead, nullptr) || bytesRead != fileSize) {
        free(fileData);
        CloseHandle(hFile);
        LOG_ERROR("[ModLoader] Failed to read file");
        return result;
    }
    CloseHandle(hFile);

    // Patch internal assembly name to prevent Mono name collision on hot-reload.
    // Without this, mono_assembly_load_from_full finds the original by name, closes our new image, and returns the cached one (missing TypeRef cache, crash).
    s_hotReloadCounter++;
    if (PatchAssemblyNameForHotReload(fileData, fileSize, s_hotReloadCounter)) {
        LOG_DEBUG("[ModLoader] Patched assembly name for hot-reload (counter=" + std::to_string(s_hotReloadCounter) + ")");
    } else {
        LOG_WARN("[ModLoader] Failed to patch assembly name - collision may occur");
    }

    // Create MonoImage from memory buffer
    int status = 0;
    void* image = s_imageOpenFromData(fileData, fileSize, 1, &status, 0); // need_copy=1, refonly=0
    free(fileData);

    if (!image || status != 0) {
        LOG_ERROR("[ModLoader] mono_image_open_from_data failed for hot-reload (status=" + std::to_string(status) + ")");
        return result;
    }

    // Register as assembly (using versioned path as the "name" so Mono sees it as unique)
    status = 0;
    void* assembly = s_assemblyLoadFrom(image, pathStr.c_str(), &status, 0);

    if (!assembly || status != 0) {
        LOG_ERROR("[ModLoader] mono_assembly_load_from failed for hot-reload (status=" + std::to_string(status) + ")");
        return result;
    }

    // Verify Mono registered our new image (should always succeed after name patching)
    void* registeredImage = m_assemblyGetImage(assembly);
    if (registeredImage != image) {
        LOG_ERROR("[ModLoader] UNEXPECTED: Assembly name collision despite patching!");
        LOG_ERROR("[ModLoader] Our image: " + std::to_string((uintptr_t)image) + ", registered: " + std::to_string((uintptr_t)registeredImage));
        // Use registered image, ours was closed by Mono on collision
        result.image = registeredImage;
    } else {
        result.image = image;
    }

    result.assembly = assembly;
    LOG_DEBUG("[ModLoader] Hot-reload loaded via memory: " + pathStr);

    return result;
}

void* ModLoader::LoadAssemblyFromPath(const std::filesystem::path& dllPath) {
    if (!m_canLoadAssemblies) {
        LOG_ERROR("[ModLoader] Cannot load assemblies - Mono functions not found");
        return nullptr;
    }

    auto result = LoadAssemblyEx(dllPath);

    // If this is S3MM.Helper.dll, store its image for attribute class lookup
    std::string filename = dllPath.filename().string();
    if (filename == "S3MM.Helper.dll" && result.image) {
        m_s3mmHelperImage = result.image;
        LOG_INFO("[ModLoader] Stored S3MM.Helper image @ " + std::to_string((uintptr_t)m_s3mmHelperImage));
    }

    return result.assembly;
}

void* ModLoader::LoadAssemblyFromMemory(const char* data, uint32_t dataSize, const char* name) {
    if (!m_canLoadAssemblies) {
        LOG_ERROR("[ModLoader] Cannot load assemblies - Mono functions not found");
        return nullptr;
    }

    MonoBridge::Get().SetupTLS();

    LOG_DEBUG("[ModLoader] Loading assembly from memory: " + std::string(name) + " (" + std::to_string(dataSize) + " bytes)");

    int status = 0;
    void* image = s_imageOpenFromData((char*)data, dataSize, 1, &status, 0); // need_copy=1
    if (!image || status != 0) {
        LOG_ERROR("[ModLoader] mono_image_open_from_data_full failed for embedded " + std::string(name) + " (status=" + std::to_string(status) + ")");
        return nullptr;
    }

    status = 0;
    void* assembly = s_assemblyLoadFrom(image, name, &status, 0);
    if (!assembly || status != 0) {
        LOG_ERROR("[ModLoader] mono_assembly_load_from_full failed for embedded " + std::string(name) + " (status=" + std::to_string(status) + ")");
        return nullptr;
    }

    // Store S3MM.Helper image if that's what we loaded
    if (strcmp(name, "S3MM.Helper.dll") == 0) {
        m_s3mmHelperImage = image;
        LOG_INFO("[ModLoader] Stored embedded S3MM.Helper image @ " + std::to_string((uintptr_t)image));
    }

    LOG_INFO("[ModLoader] Loaded assembly from memory: " + std::string(name));
    return assembly;
}

void* ModLoader::GetImageFromAssembly(void* assembly) {
    if (!assembly || !m_assemblyGetImage) return nullptr;
    void* image = m_assemblyGetImage(assembly);
    LOG_DEBUG("[ModLoader] GetImageFromAssembly: assembly @ " + std::to_string((uintptr_t)assembly) + " -> image @ " + std::to_string((uintptr_t)image));
    return image;
}

std::string ModLoader::GetModNameForImage(void* image) const {
    if (!image) return "";
    for (const auto& mod : m_loadedMods) {
        if (mod.monoImage == image) return mod.name;
    }
    return "";
}

bool ModLoader::LoadMod(const ModInfo& mod) {
    if (!mod.compiledSuccessfully) {
        LOG_WARN("[ModLoader] Skipping mod '" + mod.name + "' - not compiled");
        return false;
    }

    if (!std::filesystem::exists(mod.compiledDll)) {
        LOG_ERROR("[ModLoader] DLL not found: " + mod.compiledDll.string());
        return false;
    }

    LOG_INFO("[ModLoader] Loading mod: " + mod.name);

    LoadedMod loadedMod;
    loadedMod.name = mod.name;
    loadedMod.sourceInfo = mod; // Store for hot-reload

    if (m_canLoadAssemblies) {
        // Load via Mono API
        loadedMod.monoAssembly = LoadAssemblyEx(mod.compiledDll).assembly;
        if (!loadedMod.monoAssembly) {
            LOG_ERROR("[ModLoader] Failed to load mod assembly: " + mod.name);
            return false;
        }

        loadedMod.monoImage = GetImageFromAssembly(loadedMod.monoAssembly);
        if (!loadedMod.monoImage) {
            LOG_ERROR("[ModLoader] Failed to get image from assembly: " + mod.name);
            return false;
        }

        // Scan for replacements
        ScanForReplacements(loadedMod);

        // Check embedded API version
        loadedMod.apiVersion = CheckApiVersion(loadedMod.monoImage, mod.name);
        if (loadedMod.apiVersion == -1) {
            LOG_WARN("[ModLoader] '" + mod.name + "' has no API version tag - unknown compatibility");
        } else if (loadedMod.apiVersion != ModCompiler::API_VERSION) {
            LOG_WARN("[ModLoader] '" + mod.name + "' compiled against API v" + std::to_string(loadedMod.apiVersion) + ", current is v" + std::to_string(ModCompiler::API_VERSION) + " - may have issues");
        } else {
            LOG_DEBUG("[ModLoader] '" + mod.name + "' API version OK (v" + std::to_string(loadedMod.apiVersion) + ")");
        }
    } else {
        // Alternative: just record that we tried to load this mod. The user will need to use .package files for now.
        LOG_WARN("[ModLoader] Assembly loading not available - mod '" + mod.name + "' cannot be loaded at runtime");
        LOG_WARN("[ModLoader] Consider packaging as .package or finding assembly_open address");
        return false;
    }

    m_loadedMods.push_back(loadedMod);
    LOG_INFO("[ModLoader] Loaded mod: " + mod.name);

    return true;
}

int ModLoader::LoadAllMods(const std::vector<ModInfo>& mods) {
    int loaded = 0;

    for (const auto& mod : mods) {
        if (LoadMod(mod)) { loaded++; }
    }

    LOG_INFO("[ModLoader] Loaded " + std::to_string(loaded) + "/" + std::to_string(mods.size()) + " mods");

    return loaded;
}

void ModLoader::ScanForReplacements(LoadedMod& mod) {
    if (!mod.monoImage) return;

    LOG_DEBUG("[ModLoader] Scanning mod '" + mod.name + "' for replacements...");

    // Try to initialize attribute classes if not done yet by looking for S3MM namespace in this mod's image.
    if (!m_attributeClassesInitialized) { InitializeAttributeClasses(); }

    // Get number of types in the assembly
    int typeCount = MonoBridge::Get().GetTypeDefCount(mod.monoImage);
    int hooksFromNaming = 0;
    int hooksFromAttrs = 0;
    int onLoadCount = 0;
    int onUnloadCount = 0;

    for (int i = 1; i <= typeCount; i++) {
        uint32_t token = 0x02000000 | i; // TypeDef token
        void* klass = MonoBridge::Get().GetClassFromToken(mod.monoImage, token);
        if (!klass) continue;

        const char* className = MonoBridge::Get().GetClassName(klass);
        const char* classNs = MonoBridge::Get().GetClassNamespace(klass);
        if (!className) continue;

        std::string fullClassName = classNs ? (std::string(classNs) + "." + className) : className;

        // Enumerate methods in this class
        int iter = 0;
        void* method;
        while ((method = MonoBridge::Get().GetNextMethod(klass, &iter)) != nullptr) {
            // Check for Hook_/Watch_ patterns (naming convention)
            if (CheckAndRegisterHook(method, mod.name)) {
                hooksFromNaming++;
            }
            // Check for [S3MM.Hook], [S3MM.Watcher] attributes
            else if (CheckAndRegisterHookFromAttribute(method, mod.name)) {
                hooksFromAttrs++;
            }

            // Check for OnLoad/OnUnload methods (naming convention or attributes)
            const char* mName = *(const char**)((uintptr_t)method + 0x18);
            if (mName) {
                auto* layout = (MonoMethodLayout*)method;
                bool isStatic = (layout->flagsPacked & MethodFlags::STATIC) != 0;

                // OnLoad
                bool isOnLoad = false;
                if (isStatic && strcmp(mName, "OnLoad") == 0) { isOnLoad = true; }
                if (!isOnLoad && isStatic && m_onLoadAttrClass) {
                    void* attrInfo = MonoBridge::Get().GetCustomAttrsFromMethod(method);
                    if (attrInfo) {
                        if (MonoBridge::Get().CustomAttrsHasAttr(attrInfo, m_onLoadAttrClass)) { isOnLoad = true; }
                        MonoBridge::Get().CustomAttrsFree(attrInfo);
                    }
                }
                if (isOnLoad) {
                    mod.onLoadMethods.push_back(method);
                    onLoadCount++;
                    LOG_INFO("[ModLoader] Found OnLoad: " + fullClassName + "::" + mName);
                }

                // OnUnload
                bool isOnUnload = false;
                if (isStatic && strcmp(mName, "OnUnload") == 0) { isOnUnload = true; }
                if (!isOnUnload && isStatic && m_onUnloadAttrClass) {
                    void* attrInfo = MonoBridge::Get().GetCustomAttrsFromMethod(method);
                    if (attrInfo) {
                        if (MonoBridge::Get().CustomAttrsHasAttr(attrInfo, m_onUnloadAttrClass)) { isOnUnload = true; }
                        MonoBridge::Get().CustomAttrsFree(attrInfo);
                    }
                }
                if (isOnUnload) {
                    mod.onUnloadMethods.push_back(method);
                    onUnloadCount++;
                    LOG_INFO("[ModLoader] Found OnUnload: " + fullClassName + "::" + mName);
                }
            }
        }
    }

    LOG_INFO("[ModLoader] Found " + std::to_string(hooksFromNaming) + " hooks (naming), " + std::to_string(hooksFromAttrs) + " hooks (attr), " + std::to_string(onLoadCount) + " OnLoad, " + std::to_string(onUnloadCount) +
             " OnUnload in " + mod.name);
}

int ModLoader::CheckApiVersion(void* image, const std::string& modName) {
    if (!image) return -1;

    auto& bridge = MonoBridge::Get();

    // Look for compiler-injected S3MM_CompilerMeta class (global namespace)
    void* metaClass = bridge.GetClassFromNameInImage(image, "", "S3MM_CompilerMeta");
    if (!metaClass) {
        LOG_DEBUG("[ModLoader] No S3MM_CompilerMeta in '" + modName + "' - pre-compiled or external DLL");
        return -1;
    }

    // Version is encoded in a method name "ApiVersion_<N>" (see ModCompiler) and read from metadata.
    const char* kPrefix = "ApiVersion_";
    const size_t kPrefixLen = 11; // strlen("ApiVersion_")
    int iter = 0;
    void* method = nullptr;
    while ((method = bridge.GetNextMethod(metaClass, &iter)) != nullptr) {
        const char* mName = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);
        if (mName && strncmp(mName, kPrefix, kPrefixLen) == 0) { return atoi(mName + kPrefixLen); }
    }

    LOG_WARN("[ModLoader] S3MM_CompilerMeta found but no ApiVersion_<N> method in '" + modName + "'");
    return -1;
}

// Check if method has Hook_/Watch_ naming convention and register with HookManager.
// Supports priority via HookN_ where N is an integer (higher = runs first).
// e.g. Hook_Class_Method (priority 0), Hook100_Class_Method (priority 100), Watch_Class_Method (watcher)
bool ModLoader::CheckAndRegisterHook(void* method, const std::string& modName) {
    const char* methodName = *(const char**)((uintptr_t)method + 0x18);
    if (!methodName) return false;

    std::string name(methodName);
    HookType hookType;
    size_t keywordLen;
    int priority = 0;

    // Check for Hook or Watch, with optional priority number (format: Hook[N]_Class_Method or Watch_Class_Method).
    if (name.rfind("Hook", 0) == 0) {
        hookType = HookType::Hook;
        keywordLen = 4; // "Hook"
    } else if (name.rfind("Watch", 0) == 0) {
        hookType = HookType::Watcher;
        keywordLen = 5; // "Watch"
    } else {
        return false; // Not a hook method
    }

    // Check if there's a priority number after Hook/Watch
    size_t underscorePos = name.find('_', keywordLen);
    if (underscorePos == std::string::npos) return false;

    // Extract potential priority number between keyword and first underscore
    std::string priorityStr = name.substr(keywordLen, underscorePos - keywordLen);
    if (!priorityStr.empty()) {
        // Try to parse as integer
        try {
            priority = std::stoi(priorityStr);
        } catch (...) {
            // Not a number, invalid format
            return false;
        }
    }

    // Skip past the underscore after priority
    size_t classStart = underscorePos + 1;

    // Parse [Priority_]Namespace_Class_Method by finding last underscore for method name.
    size_t lastUnderscore = name.rfind('_');
    if (lastUnderscore == std::string::npos || lastUnderscore < classStart) return false;

    std::string targetMethod = name.substr(lastUnderscore + 1);
    std::string classPath = name.substr(classStart, lastUnderscore - classStart);

    // Convert underscores to dots for namespace
    for (char& c : classPath) {
        if (c == '_') c = '.';
    }

    const char* hookTypeName = hookType == HookType::Hook ? "hook" : "watcher";
    LOG_INFO("[ModLoader] Found " + std::string(hookTypeName) + ": " + classPath + "::" + targetMethod + " (priority=" + std::to_string(priority) + ")");

    // Register with HookManager
    return HookManager::Get().RegisterHook(classPath, targetMethod, method, hookType, modName, priority);
}

// Initialize S3MM attribute class cache
void ModLoader::InitializeAttributeClasses() {
    if (m_attributeClassesInitialized) return;

    LOG_INFO("[ModLoader] Initializing S3MM attribute classes...");

    // Use stored S3MM.Helper image
    void* helperImage = m_s3mmHelperImage;
    if (!helperImage) {
        LOG_WARN("[ModLoader] S3MM.Helper image not available - attribute hooks disabled");
        m_attributeClassesInitialized = true;
        return;
    }

    LOG_INFO("[ModLoader] Using S3MM.Helper image @ " + std::to_string((uintptr_t)helperImage));

    // Find attribute classes by iterating TypeDefs in S3MM.Helper
    int typeCount = MonoBridge::Get().GetTypeDefCount(helperImage);
    LOG_DEBUG("[ModLoader] S3MM.Helper has " + std::to_string(typeCount) + " types");

    for (int i = 1; i <= typeCount; i++) {
        uint32_t token = 0x02000000 | i;
        void* klass = MonoBridge::Get().GetClassFromToken(helperImage, token);
        if (!klass) continue;

        const char* className = MonoBridge::Get().GetClassName(klass);
        const char* classNs = MonoBridge::Get().GetClassNamespace(klass);
        if (!className) continue;

        // Check for S3MM namespace
        if (!classNs || strcmp(classNs, "S3MM") != 0) continue;

        LOG_DEBUG("[ModLoader] Found S3MM class: " + std::string(className));

        if (strcmp(className, "HookAttribute") == 0) {
            m_hookAttrClass = klass;
        } else if (strcmp(className, "WatcherAttribute") == 0) {
            m_watcherAttrClass = klass;
        } else if (strcmp(className, "OnLoadAttribute") == 0) {
            m_onLoadAttrClass = klass;
        } else if (strcmp(className, "OnUnloadAttribute") == 0) {
            m_onUnloadAttrClass = klass;
        }
    }

    LOG_INFO("[ModLoader] HookAttribute: " + std::to_string((uintptr_t)m_hookAttrClass));
    LOG_INFO("[ModLoader] WatcherAttribute: " + std::to_string((uintptr_t)m_watcherAttrClass));
    LOG_INFO("[ModLoader] OnLoadAttribute: " + std::to_string((uintptr_t)m_onLoadAttrClass));
    LOG_INFO("[ModLoader] OnUnloadAttribute: " + std::to_string((uintptr_t)m_onUnloadAttrClass));

    m_attributeClassesInitialized = true;

    if (m_hookAttrClass && m_watcherAttrClass) {
        LOG_INFO("[ModLoader] S3MM attribute classes initialized successfully");
    } else {
        LOG_WARN("[ModLoader] Some S3MM attribute classes not found - attribute-based hooks may not work");
    }
}

// Check for [S3MM.Hook], [S3MM.Watcher] attributes and register hooks
bool ModLoader::CheckAndRegisterHookFromAttribute(void* method, const std::string& modName) {
    if (!method || !m_attributeClassesInitialized) return false;

    // Get method name for logging
    const char* methodName = *(const char**)((uintptr_t)method + 0x18);

    // Get custom attributes from the method
    void* attrInfo = MonoBridge::Get().GetCustomAttrsFromMethod(method);
    LOG_DEBUG("[ModLoader] CheckAttr: method=" + std::string(methodName ? methodName : "?") + " attrInfo=" + std::to_string((uintptr_t)attrInfo));
    if (!attrInfo) return false;

    bool registered = false;
    HookType hookType = HookType::Hook;
    void* attrClass = nullptr;

    // Log attribute class state
    LOG_DEBUG("[ModLoader] CheckAttr: attrClasses hook=" + std::to_string((uintptr_t)m_hookAttrClass) + " watcher=" + std::to_string((uintptr_t)m_watcherAttrClass));

    // Check for each attribute type
    bool hasHook = m_hookAttrClass && MonoBridge::Get().CustomAttrsHasAttr(attrInfo, m_hookAttrClass);
    bool hasWatcher = m_watcherAttrClass && MonoBridge::Get().CustomAttrsHasAttr(attrInfo, m_watcherAttrClass);

    LOG_DEBUG("[ModLoader] CheckAttr: hasHook=" + std::to_string(hasHook) + " hasWatcher=" + std::to_string(hasWatcher));

    if (hasHook) {
        hookType = HookType::Hook;
        attrClass = m_hookAttrClass;
    } else if (hasWatcher) {
        hookType = HookType::Watcher;
        attrClass = m_watcherAttrClass;
    }

    if (attrClass) {
        // Get the attribute instance
        void* attrObj = MonoBridge::Get().CustomAttrsGetAttr(attrInfo, attrClass);
        LOG_DEBUG("[ModLoader] CheckAttr: attrObj=" + std::to_string((uintptr_t)attrObj));

        if (attrObj) {
            // Read attribute fields
            void* targetClassField = MonoBridge::Get().GetFieldFromName(attrClass, "_targetClass");
            void* targetMethodField = MonoBridge::Get().GetFieldFromName(attrClass, "_targetMethod");
            void* priorityField = MonoBridge::Get().GetFieldFromName(attrClass, "_priority");
            void* signatureField = MonoBridge::Get().GetFieldFromName(attrClass, "_signature");

            LOG_DEBUG("[ModLoader] CheckAttr: fields - targetClass=" + std::to_string((uintptr_t)targetClassField) + " targetMethod=" + std::to_string((uintptr_t)targetMethodField) +
                      " priority=" + std::to_string((uintptr_t)priorityField) + " signature=" + std::to_string((uintptr_t)signatureField));

            if (targetClassField && targetMethodField) {
                void* targetClassStr = nullptr;
                void* targetMethodStr = nullptr;
                void* signatureStr = nullptr;
                int priority = 0;

                MonoBridge::Get().GetInstanceFieldValue(attrObj, targetClassField, &targetClassStr);
                MonoBridge::Get().GetInstanceFieldValue(attrObj, targetMethodField, &targetMethodStr);
                if (priorityField) { MonoBridge::Get().GetInstanceFieldValue(attrObj, priorityField, &priority); }
                if (signatureField) { MonoBridge::Get().GetInstanceFieldValue(attrObj, signatureField, &signatureStr); }

                if (targetClassStr && targetMethodStr) {
                    std::string targetClass = MonoBridge::Get().ExtractString(targetClassStr);
                    std::string targetMethod = MonoBridge::Get().ExtractString(targetMethodStr);
                    std::string signature = signatureStr ? MonoBridge::Get().ExtractString(signatureStr) : "";

                    const char* hookTypeName = hookType == HookType::Hook ? "hook" : "watcher";
                    std::string sigDisplay = signature.empty() ? "" : signature;
                    LOG_INFO("[ModLoader] Found " + std::string(hookTypeName) + " (attr): " + targetClass + ":" + targetMethod + sigDisplay + " (priority=" + std::to_string(priority) + ")");

                    // Baking is inferred from the handler IL at registration (HookUsesChain) - no Typed flag.
                    registered = HookManager::Get().RegisterHook(targetClass, targetMethod, method, hookType, modName, priority, signature);
                }
            }
        }
    }

    // Free the attr info if not cached
    MonoBridge::Get().CustomAttrsFree(attrInfo);

    return registered;
}

// Scan an image for hooks and register them (used for hot-reload)
int ModLoader::ScanImageForHooks(void* image, const std::string& modName) {
    if (!image) return 0;

    LOG_DEBUG("[ModLoader] Scanning image for hooks (mod: " + modName + ")");

    // Try to initialize attribute classes if not done yet
    if (!m_attributeClassesInitialized) { InitializeAttributeClasses(); }

    int hooksRegistered = 0;
    int onLoadCount = 0;
    int onUnloadCount = 0;

    // Find the LoadedMod for storing lifecycle methods
    auto modIt = std::find_if(m_loadedMods.begin(), m_loadedMods.end(), [&modName](const LoadedMod& m) { return m.name == modName; });

    // Get number of types in the image
    int typeCount = MonoBridge::Get().GetTypeDefCount(image);

    for (int i = 1; i <= typeCount; i++) {
        uint32_t token = 0x02000000 | i; // TypeDef token
        void* klass = MonoBridge::Get().GetClassFromToken(image, token);
        if (!klass) continue;

        const char* className = MonoBridge::Get().GetClassName(klass);
        const char* classNs = MonoBridge::Get().GetClassNamespace(klass);
        std::string fullClassName;
        if (className) { fullClassName = classNs ? (std::string(classNs) + "." + className) : className; }

        // Enumerate methods in this class
        int iter = 0;
        void* method;
        while ((method = MonoBridge::Get().GetNextMethod(klass, &iter)) != nullptr) {
            // Check naming convention first
            if (CheckAndRegisterHook(method, modName)) {
                hooksRegistered++;
            }
            // Then check attributes
            else if (CheckAndRegisterHookFromAttribute(method, modName)) {
                hooksRegistered++;
            }

            // Check for OnLoad/OnUnload methods (naming convention or attributes)
            if (modIt != m_loadedMods.end()) {
                const char* mName = *(const char**)((uintptr_t)method + 0x18);
                if (mName) {
                    auto* layout = (MonoMethodLayout*)method;
                    bool isStatic = (layout->flagsPacked & MethodFlags::STATIC) != 0;

                    // OnLoad
                    bool isOnLoad = false;
                    if (isStatic && strcmp(mName, "OnLoad") == 0) { isOnLoad = true; }
                    if (!isOnLoad && isStatic && m_onLoadAttrClass) {
                        void* attrInfo = MonoBridge::Get().GetCustomAttrsFromMethod(method);
                        if (attrInfo) {
                            if (MonoBridge::Get().CustomAttrsHasAttr(attrInfo, m_onLoadAttrClass)) { isOnLoad = true; }
                            MonoBridge::Get().CustomAttrsFree(attrInfo);
                        }
                    }
                    if (isOnLoad) {
                        modIt->onLoadMethods.push_back(method);
                        onLoadCount++;
                        LOG_INFO("[ModLoader] Found OnLoad: " + fullClassName + "::" + mName);
                    }

                    // OnUnload
                    bool isOnUnload = false;
                    if (isStatic && strcmp(mName, "OnUnload") == 0) { isOnUnload = true; }
                    if (!isOnUnload && isStatic && m_onUnloadAttrClass) {
                        void* attrInfo = MonoBridge::Get().GetCustomAttrsFromMethod(method);
                        if (attrInfo) {
                            if (MonoBridge::Get().CustomAttrsHasAttr(attrInfo, m_onUnloadAttrClass)) { isOnUnload = true; }
                            MonoBridge::Get().CustomAttrsFree(attrInfo);
                        }
                    }
                    if (isOnUnload) {
                        modIt->onUnloadMethods.push_back(method);
                        onUnloadCount++;
                        LOG_INFO("[ModLoader] Found OnUnload: " + fullClassName + "::" + mName);
                    }
                }
            }
        }
    }

    LOG_INFO("[ModLoader] Registered " + std::to_string(hooksRegistered) + " hooks, " + std::to_string(onLoadCount) + " OnLoad, " + std::to_string(onUnloadCount) + " OnUnload from image for mod: " + modName);
    return hooksRegistered;
}

void* ModLoader::FindMethodByNameInImage(void* image, const std::string& methodName) {
    // Iterate MethodDef tokens (0x06xxxxxx) directly via mono_get_method
    // This avoids triggering TypeRef resolution, which crashes on not-yet-registered images
    if (!image || methodName.empty()) return nullptr;

    // No cheap way to read the MethodDef row count here, so scan a fixed upper bound
    // The consecutive-null break below tolerates sparse token ranges
    constexpr int MAX_METHOD_SCAN = 100; // Most mods have far fewer methods

    LOG_DEBUG("[ModLoader] FindMethodByNameInImage: searching for '" + methodName + "'");

    int consecutiveNulls = 0;

    for (int i = 1; i <= MAX_METHOD_SCAN; i++) {
        uint32_t token = 0x06000000 | i; // MethodDef token

        // Use mono_get_method to get the MonoMethod* for this token
        void* method = s_getMethod(image, token, nullptr);

        if (!method) {
            // Track consecutive nulls - break after 10 in a row (sparse token range tolerance)
            consecutiveNulls++;
            if (consecutiveNulls > 10) break;
            continue;
        }

        consecutiveNulls = 0; // Reset on successful lookup

        // Check the method name
        const char* name = *(const char**)((uintptr_t)method + 0x18);
        if (name && methodName == name) {
            LOG_DEBUG("[ModLoader] Found method at token " + std::to_string(token) + " @ " + std::to_string((uintptr_t)method));
            return method;
        }
    }

    LOG_WARN("[ModLoader] Method not found in image: " + methodName);
    return nullptr;
}

void ModLoader::FireOnLoadCallbacks(const std::string& modName) {
    auto modIt = std::find_if(m_loadedMods.begin(), m_loadedMods.end(), [&modName](const LoadedMod& m) { return m.name == modName; });

    if (modIt == m_loadedMods.end()) return;

    LoadedMod& mod = *modIt;
    if (mod.onLoadMethods.empty()) return;

    LOG_INFO("[ModLoader] Firing " + std::to_string(mod.onLoadMethods.size()) + " OnLoad callback(s) for mod: " + modName);

    for (void* method : mod.onLoadMethods) {
        const char* mName = *(const char**)((uintptr_t)method + 0x18);
        auto* layout = (MonoMethodLayout*)method;
        std::string fullName;

        // Build method name for logging
        if (layout->klass) {
            const char* className = MonoBridge::Get().GetClassName(layout->klass);
            const char* classNs = MonoBridge::Get().GetClassNamespace(layout->klass);
            if (classNs && classNs[0]) {
                fullName = std::string(classNs) + "." + (className ? className : "?");
            } else {
                fullName = className ? className : "?";
            }
        }
        fullName += "::" + std::string(mName ? mName : "?");

        // Invoke: static, no params, void return. Marshalled to the sim thread - this may be called from the UI thread (Scan/Hot-Reload)
        // and the interpreter isn't reentrant across threads; cold-start firing (StartupComplete) is already on-thread so it runs inline :DDDD
        MonoInvokeState state = {};
        bool ok = MonoBridge::Get().InvokeMethodOnSimThread(method, nullptr, nullptr, &state);

        if (ok) {
            LOG_INFO("[ModLoader] OnLoad OK: " + fullName);
        } else {
            std::string excMsg = "unknown error";
            if (state.pThrowExc) { excMsg = ExtractExceptionInfo(state.pThrowExc); }
            LOG_ERROR("[ModLoader] OnLoad FAILED: " + fullName + " - " + excMsg);
        }
    }
}

void ModLoader::FireOnUnloadCallbacks(const std::string& modName) {
    auto modIt = std::find_if(m_loadedMods.begin(), m_loadedMods.end(), [&modName](const LoadedMod& m) { return m.name == modName; });

    if (modIt == m_loadedMods.end()) return;

    // Drop any UI windows the mod still has open
    ModUIManager::Get().CloseWindowsForMod(modName);

    LoadedMod& mod = *modIt;
    if (mod.onUnloadMethods.empty()) return;

    LOG_INFO("[ModLoader] Firing " + std::to_string(mod.onUnloadMethods.size()) + " OnUnload callback(s) for mod: " + modName);

    for (void* method : mod.onUnloadMethods) {
        const char* mName = *(const char**)((uintptr_t)method + 0x18);
        auto* layout = (MonoMethodLayout*)method;
        std::string fullName;

        // Build method name for logging
        if (layout->klass) {
            const char* className = MonoBridge::Get().GetClassName(layout->klass);
            const char* classNs = MonoBridge::Get().GetClassNamespace(layout->klass);
            if (classNs && classNs[0]) {
                fullName = std::string(classNs) + "." + (className ? className : "?");
            } else {
                fullName = className ? className : "?";
            }
        }
        fullName += "::" + std::string(mName ? mName : "?");

        // Invoke: static, no params, void return. Marshalled to the sim thread - this may be called from the UI thread (Scan/Hot-Reload) cold-start firing (StartupComplete) is already on-thread so it runs inline.
        MonoInvokeState state = {};
        bool ok = MonoBridge::Get().InvokeMethodOnSimThread(method, nullptr, nullptr, &state);

        if (ok) {
            LOG_INFO("[ModLoader] OnUnload OK: " + fullName);
        } else {
            LOG_ERROR("[ModLoader] OnUnload FAILED: " + fullName);
        }
    }

    // Clear, will be re-populated from new image scan
    mod.onUnloadMethods.clear();
}

bool ModLoader::HotReloadMod(const std::string& modName) {
    LOG_INFO("[ModLoader] Hot-reloading mod: " + modName);

    // Find the loaded mod
    auto modIt = std::find_if(m_loadedMods.begin(), m_loadedMods.end(), [&modName](const LoadedMod& m) { return m.name == modName; });

    if (modIt == m_loadedMods.end()) {
        LOG_ERROR("[ModLoader] Mod not found for hot-reload: " + modName);
        return false;
    }

    LoadedMod& mod = *modIt;

    // Increment version FIRST so we know the output filename
    mod.hotReloadVersion++;
    LOG_INFO("[ModLoader] Hot-reload version: " + std::to_string(mod.hotReloadVersion));

    // Build versioned output path in HotReload subfolder. CRITICAL: the internal assembly name will be "modname_vN" so Mono treats it as NEW.
    std::filesystem::path hotReloadDir = mod.sourceInfo.compiledDll.parent_path() / "HotReload";
    std::filesystem::create_directories(hotReloadDir);

    std::filesystem::path versionedDll = hotReloadDir / (mod.sourceInfo.compiledDll.stem().string() + "_v" + std::to_string(mod.hotReloadVersion) + ".dll");

    // Compile to the versioned path.
    if (mod.sourceInfo.hasSourceFiles) {
        if (!ModCompiler::Get().CompileModTo(mod.sourceInfo, versionedDll)) {
            LOG_ERROR("[ModLoader] Failed to recompile mod: " + modName);
            mod.hotReloadVersion--; // Rollback on failure
            return false;
        }
    } else {
        // No source - copy existing DLL to versioned path (won't help with caching but at least consistent)
        std::error_code ec;
        std::filesystem::copy_file(mod.sourceInfo.compiledDll, versionedDll, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("[ModLoader] Failed to copy DLL: " + ec.message());
            mod.hotReloadVersion--;
            return false;
        }
    }

    // Everything below runs managed code: image registration, attribute .ctor construction during the hook scan (CustomAttrsGetAttr), MINT transform during patching, and OnLoad.
    // The Mono interpreter is not reentrant across threads, and hot-reload is triggered from the UI thread, so this must run on the sim thread.
    // Otherwise attribute construction races the game thread's interpreter and CustomAttrsGetAttr crashes, silently dropping most of the mod's hooks.
    // The slow compile above (csc.exe) deliberately stays off the sim thread to avoid freezing the frame.
    bool reloadOk = false;
    bool ran = MonoBridge::Get().RunOnSimThread([&]() {
        // Load the new versioned assembly.
        LoadResult loadResult = LoadImageForHotReload(versionedDll);
        if (!loadResult.image) {
            LOG_ERROR("[ModLoader] Failed to load image");
            return; // reloadOk stays false; no hook state mutated yet
        }

        void* newImage = loadResult.image;

        // Fire OnUnload before removing hooks, so the mod can still restore fields and clean up state.
        FireOnUnloadCallbacks(modName);

        // Remove old hooks (only reached after a successful compile+load; earlier failures returned early with the mod's current hooks intact).
        int removedHooks = HookManager::Get().RemoveHooksForMod(modName);
        LOG_DEBUG("[ModLoader] Removed " + std::to_string(removedHooks) + " old hooks");

        // Scan the new image for hooks and register them.
        // Clear onLoadMethods first: ScanImageForHooks APPENDS the new image's OnLoad methods, and FireOnUnloadCallbacks only clears onUnloadMethods (and even then only when it has callbacks to fire).
        // Without this, onLoadMethods accumulates stale MonoMethod* from prior images across reloads, re-firing old OnLoad bodies.
        mod.onLoadMethods.clear();
        int newHooks = ScanImageForHooks(newImage, modName);
        LOG_DEBUG("[ModLoader] Registered " + std::to_string(newHooks) + " new hooks");

        // Update LoadedMod pointers to the new image/assembly
        mod.monoImage = loadResult.image;
        mod.monoAssembly = loadResult.assembly;

        // Reinstall dispatch stubs for updated hook chains.
        int reinstalled = HookManager::Get().ReinstallDispatchStubs();

        // Apply new hook chains (previously unhooked methods need a fresh install via ApplyHooks).
        int newlyApplied = HookManager::Get().ApplyHooks();

        // Re-point persisted statics at the new image (managed side knows the previous image by mod name), before OnLoad so it sees carried/tuned values
        PersistBridge::Get().OnModLoaded(mod);
        PersistBridge::Get().FlushSignal();

        // Fire OnLoad callbacks for the reloaded mod (hooks are now active)
        FireOnLoadCallbacks(modName);

        // OnLoad may register hooks via Hook.RegisterByPointer() (dynamic hooks).
        // Those land in chains created/updated after the reinstall/apply above, so do it once more - mirroring the post-OnLoad pass in ProcessMods (ReinstallDispatchStubs for chains with new hooks, ApplyHooks for brand-new chains).
        // Without this, an OnLoad-registered hook re-registers but never gets its dispatch stub installed on reload, so it silently stops firing.
        int postReinstalled = HookManager::Get().ReinstallDispatchStubs();
        int postApplied = HookManager::Get().ApplyHooks();
        if (postReinstalled > 0 || postApplied > 0) { LOG_INFO("[ModLoader] Post-OnLoad: reinstalled " + std::to_string(postReinstalled) + " stub(s), applied " + std::to_string(postApplied) + " new chain(s)"); }

        // Per-mod requested / installed / REFUSED roll-up for the reloaded mod
        HookManager::Get().LogHookSummary();

        LOG_INFO("[ModLoader] Hot-reload complete: " + std::to_string(newHooks) + " hook(s), " + std::to_string(reinstalled) + " dispatch stub(s) reinstalled, " + std::to_string(newlyApplied) + " new chain(s) applied");
        reloadOk = true;
    });

    if (!reloadOk) {
        if (!ran) { LOG_ERROR("[ModLoader] Hot-reload aborted - sim thread not pumping"); }
        mod.hotReloadVersion--; // rollback; image load failed or task never ran (no hook state changed)
        return false;
    }

    // Clean up previous version if it exists
    if (mod.hotReloadVersion > 1) {
        std::filesystem::path oldPath = hotReloadDir / (mod.sourceInfo.compiledDll.stem().string() + "_v" + std::to_string(mod.hotReloadVersion - 1) + ".dll");
        std::error_code ec;
        std::filesystem::remove(oldPath, ec);
        // Ignore errors - file might be locked
    }

    // Reaching here means compile + load + rescan all succeeded (failures returned early above).
    // Don't gate success on hook count: an [OnLoad]-only mod legitimately registers zero hooks.
    return true;
}

} // namespace S3MM
