#pragma once
#include <cstdint>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <string>

namespace S3MM {

class MonoLookupCache {
  public:
    static MonoLookupCache& Get();

    bool Install();
    bool IsInstalled() const { return m_installed; }

    void LogStats();
    void ClearAllCaches();

    struct CacheStats {
        std::atomic<uint64_t> hits{0};
        std::atomic<uint64_t> misses{0};
    };

  private:
    MonoLookupCache() = default;

    // CACHEKEY
    struct CacheKey {
        uintptr_t ptr1;
        uintptr_t ptr2;
        size_t hash;
        bool operator==(const CacheKey& o) const { return ptr1 == o.ptr1 && ptr2 == o.ptr2 && hash == o.hash; }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            size_t h = 2166136261U;
            h ^= k.ptr1;
            h *= 16777619U;
            h ^= k.ptr2;
            h *= 16777619U;
            h ^= k.hash;
            h *= 16777619U;
            return h;
        }
    };

    static size_t HashStr(const char* str);

    // ORIGINAL FUNCTION POINTER TYPES
    using class_from_name_t = void*(__cdecl*)(void* image, const char* ns, const char* name);
    using class_get_full_t = void*(__cdecl*)(void* image, uint32_t token, void* context);
    using class_vtable_t = void*(__cdecl*)(void* domain, void* klass);
    using field_from_name_t = void*(__cdecl*)(void* klass, const char* name);
    using method_from_name_t = void*(__cdecl*)(void* klass, const char* name, int param_count, int flags);
    using class_from_type_t = void*(__cdecl*)(void* monoType, void* context);

    // ORIGINAL FUNCTION POINTERS - Detours patches these in-place
    static inline class_from_name_t s_origClassFromName = nullptr;
    static inline class_get_full_t s_origClassGetFull = nullptr;
    static inline class_vtable_t s_origClassVtable = nullptr;
    static inline field_from_name_t s_origFieldFromName = nullptr;
    static inline method_from_name_t s_origMethodFromName = nullptr;
    static inline class_from_type_t s_origClassFromType = nullptr;

    // CACHES
    static inline std::unordered_map<CacheKey, void*, CacheKeyHash> s_classFromNameCache;
    static inline std::unordered_map<CacheKey, void*, CacheKeyHash> s_classGetFullCache;
    static inline std::unordered_map<CacheKey, void*, CacheKeyHash> s_classVtableCache;
    static inline std::unordered_map<CacheKey, void*, CacheKeyHash> s_fieldFromNameCache;
    static inline std::unordered_map<CacheKey, void*, CacheKeyHash> s_methodFromNameCache;
    static inline std::unordered_map<CacheKey, void*, CacheKeyHash> s_classFromTypeCache;

    // LOCKS
    static inline std::shared_mutex s_classFromNameMtx;
    static inline std::shared_mutex s_classGetFullMtx;
    static inline std::shared_mutex s_classVtableMtx;
    static inline std::shared_mutex s_fieldFromNameMtx;
    static inline std::shared_mutex s_methodFromNameMtx;
    static inline std::shared_mutex s_classFromTypeMtx;

    // STATS
    static inline CacheStats s_statsClassFromName;
    static inline CacheStats s_statsClassGetFull;
    static inline CacheStats s_statsClassVtable;
    static inline CacheStats s_statsFieldFromName;
    static inline CacheStats s_statsMethodFromName;
    static inline CacheStats s_statsClassFromType;

    // HOOK FUNCTIONS
    static void* __cdecl Hook_class_from_name(void* image, const char* ns, const char* name);
    static void* __cdecl Hook_class_get_full(void* image, uint32_t token, void* context);
    static void* __cdecl Hook_class_vtable(void* domain, void* klass);
    static void* __cdecl Hook_field_from_name(void* klass, const char* name);
    static void* __cdecl Hook_method_from_name(void* klass, const char* name, int param_count, int flags);
    static void* __cdecl Hook_class_from_type(void* monoType, void* context);

    int m_hookCount = 0;
    bool m_installed = false;
};

} // namespace S3MM
