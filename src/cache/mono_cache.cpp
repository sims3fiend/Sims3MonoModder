#include "mono_cache.h"
#include "../core/addresses.h"
#include "../util/logger.h"
#include <detours/detours.h>
#include <Windows.h>

namespace S3MM {

// SINGLETON

MonoLookupCache& MonoLookupCache::Get() {
    static MonoLookupCache instance;
    return instance;
}

// FNV-1A STRING HASH
size_t MonoLookupCache::HashStr(const char* str) {
    if (!str) return 0;
    size_t h = 2166136261u; // FNV-1a 32-bit offset basis
    while (*str) {
        h ^= static_cast<uint8_t>(*str++);
        h *= 16777619u; // FNV-1a 32-bit prime
    }
    return h;
}

// HOOK IMPLEMENTATIONS

void* __cdecl MonoLookupCache::Hook_class_from_name(void* image, const char* ns, const char* name) {
    if (!image || !name) return s_origClassFromName(image, ns, name);

    CacheKey key{reinterpret_cast<uintptr_t>(image), HashStr(ns), HashStr(name)};

    {
        std::shared_lock lk(s_classFromNameMtx);
        auto it = s_classFromNameCache.find(key);
        if (it != s_classFromNameCache.end()) {
            s_statsClassFromName.hits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    void* result = s_origClassFromName(image, ns, name);
    s_statsClassFromName.misses.fetch_add(1, std::memory_order_relaxed);

    {
        std::unique_lock lk(s_classFromNameMtx);
        s_classFromNameCache[key] = result;
    }
    return result;
}

void* __cdecl MonoLookupCache::Hook_class_get_full(void* image, uint32_t token, void* context) {
    if (!image) return s_origClassGetFull(image, token, context);

    CacheKey key{reinterpret_cast<uintptr_t>(image), token, reinterpret_cast<uintptr_t>(context)};

    {
        std::shared_lock lk(s_classGetFullMtx);
        auto it = s_classGetFullCache.find(key);
        if (it != s_classGetFullCache.end()) {
            s_statsClassGetFull.hits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    void* result = s_origClassGetFull(image, token, context);
    s_statsClassGetFull.misses.fetch_add(1, std::memory_order_relaxed);

    {
        std::unique_lock lk(s_classGetFullMtx);
        s_classGetFullCache[key] = result;
    }
    return result;
}

void* __cdecl MonoLookupCache::Hook_class_vtable(void* domain, void* klass) {
    if (!domain || !klass) return s_origClassVtable(domain, klass);

    CacheKey key{reinterpret_cast<uintptr_t>(domain), reinterpret_cast<uintptr_t>(klass), 0};

    {
        std::shared_lock lk(s_classVtableMtx);
        auto it = s_classVtableCache.find(key);
        if (it != s_classVtableCache.end()) {
            s_statsClassVtable.hits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    void* result = s_origClassVtable(domain, klass);
    s_statsClassVtable.misses.fetch_add(1, std::memory_order_relaxed);

    {
        std::unique_lock lk(s_classVtableMtx);
        s_classVtableCache[key] = result;
    }
    return result;
}

void* __cdecl MonoLookupCache::Hook_field_from_name(void* klass, const char* name) {
    if (!klass || !name) return s_origFieldFromName(klass, name);

    CacheKey key{reinterpret_cast<uintptr_t>(klass), 0, HashStr(name)};

    {
        std::shared_lock lk(s_fieldFromNameMtx);
        auto it = s_fieldFromNameCache.find(key);
        if (it != s_fieldFromNameCache.end()) {
            s_statsFieldFromName.hits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    void* result = s_origFieldFromName(klass, name);
    s_statsFieldFromName.misses.fetch_add(1, std::memory_order_relaxed);

    {
        std::unique_lock lk(s_fieldFromNameMtx);
        s_fieldFromNameCache[key] = result;
    }
    return result;
}

void* __cdecl MonoLookupCache::Hook_method_from_name(void* klass, const char* name, int param_count, int flags) {
    if (!klass || !name) return s_origMethodFromName(klass, name, param_count, flags);

    // Mix param_count and flags into the hash
    size_t h = HashStr(name);
    h ^= static_cast<size_t>(param_count + 1) * 2654435761u; // +1 because -1 is valid
    h ^= static_cast<size_t>(flags) * 2246822519u;

    CacheKey key{reinterpret_cast<uintptr_t>(klass), static_cast<uintptr_t>(flags), h};

    {
        std::shared_lock lk(s_methodFromNameMtx);
        auto it = s_methodFromNameCache.find(key);
        if (it != s_methodFromNameCache.end()) {
            s_statsMethodFromName.hits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    void* result = s_origMethodFromName(klass, name, param_count, flags);
    s_statsMethodFromName.misses.fetch_add(1, std::memory_order_relaxed);

    {
        std::unique_lock lk(s_methodFromNameMtx);
        s_methodFromNameCache[key] = result;
    }
    return result;
}

void* __cdecl MonoLookupCache::Hook_class_from_type(void* monoType, void* context) {
    if (!monoType) return s_origClassFromType(monoType, context);

    CacheKey key{reinterpret_cast<uintptr_t>(monoType), reinterpret_cast<uintptr_t>(context), 0};

    {
        std::shared_lock lk(s_classFromTypeMtx);
        auto it = s_classFromTypeCache.find(key);
        if (it != s_classFromTypeCache.end()) {
            s_statsClassFromType.hits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    void* result = s_origClassFromType(monoType, context);
    s_statsClassFromType.misses.fetch_add(1, std::memory_order_relaxed);

    {
        std::unique_lock lk(s_classFromTypeMtx);
        s_classFromTypeCache[key] = result;
    }
    return result;
}

// INSTALL

bool MonoLookupCache::Install() {
    if (m_installed) return true;

    LOG_INFO("[MonoLookupCache] Installing...");

    // Read addresses from Addresses::Classes
    struct HookEntry {
        const char* name;
        uintptr_t addr;
        void** origPtr;
        void* hookFn;
    };

    s_origClassFromName = reinterpret_cast<class_from_name_t>(Addresses::Classes.mono_class_from_name);
    s_origClassGetFull = reinterpret_cast<class_get_full_t>(Addresses::Classes.mono_class_get_full);
    s_origClassVtable = reinterpret_cast<class_vtable_t>(Addresses::Classes.mono_class_vtable);
    s_origFieldFromName = reinterpret_cast<field_from_name_t>(Addresses::Classes.mono_class_get_field_from_name);
    s_origMethodFromName = reinterpret_cast<method_from_name_t>(Addresses::Classes.mono_class_get_method_from_name_flags);
    s_origClassFromType = reinterpret_cast<class_from_type_t>(Addresses::Classes.SPEC_mono_class_from_mono_type);

    HookEntry hooks[] = {
        {"mono_class_from_name", Addresses::Classes.mono_class_from_name, reinterpret_cast<void**>(&s_origClassFromName), reinterpret_cast<void*>(Hook_class_from_name)},
        {"mono_class_get_full", Addresses::Classes.mono_class_get_full, reinterpret_cast<void**>(&s_origClassGetFull), reinterpret_cast<void*>(Hook_class_get_full)},
        {"mono_class_vtable", Addresses::Classes.mono_class_vtable, reinterpret_cast<void**>(&s_origClassVtable), reinterpret_cast<void*>(Hook_class_vtable)},
        {"mono_class_get_field_from_name", Addresses::Classes.mono_class_get_field_from_name, reinterpret_cast<void**>(&s_origFieldFromName), reinterpret_cast<void*>(Hook_field_from_name)},
        {"mono_class_get_method_from_name_flags", Addresses::Classes.mono_class_get_method_from_name_flags, reinterpret_cast<void**>(&s_origMethodFromName), reinterpret_cast<void*>(Hook_method_from_name)},
        {"SPEC_mono_class_from_mono_type", Addresses::Classes.SPEC_mono_class_from_mono_type, reinterpret_cast<void**>(&s_origClassFromType), reinterpret_cast<void*>(Hook_class_from_type)},
    };

    // Validate addresses, count how many we can hook
    m_hookCount = 0;
    for (auto& h : hooks) {
        if (h.addr != 0) {
            m_hookCount++;
        } else {
            LOG_WARN("[MonoLookupCache] Skipping " + std::string(h.name) + " - address not resolved");
        }
    }

    if (m_hookCount == 0) {
        LOG_ERROR("[MonoLookupCache] No addresses resolved, cannot install");
        return false;
    }

    // Install via Detours
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    for (auto& h : hooks) {
        if (h.addr != 0) {
            LONG err = DetourAttach(h.origPtr, h.hookFn);
            if (err != NO_ERROR) {
                LOG_WARN("[MonoLookupCache] DetourAttach failed for " + std::string(h.name) + " (error " + std::to_string(err) + ")");
                m_hookCount--;
            }
        }
    }

    LONG commitResult = DetourTransactionCommit();
    if (commitResult != NO_ERROR) {
        LOG_ERROR("[MonoLookupCache] DetourTransactionCommit failed (error " + std::to_string(commitResult) + ")");
        return false;
    }

    m_installed = true;
    LOG_INFO("[MonoLookupCache] Installed " + std::to_string(m_hookCount) + " hooks");
    return true;
}

// CACHE MANAGEMENT

void MonoLookupCache::ClearAllCaches() {
    {
        std::unique_lock lk(s_classFromNameMtx);
        s_classFromNameCache.clear();
    }
    {
        std::unique_lock lk(s_classGetFullMtx);
        s_classGetFullCache.clear();
    }
    {
        std::unique_lock lk(s_classVtableMtx);
        s_classVtableCache.clear();
    }
    {
        std::unique_lock lk(s_fieldFromNameMtx);
        s_fieldFromNameCache.clear();
    }
    {
        std::unique_lock lk(s_methodFromNameMtx);
        s_methodFromNameCache.clear();
    }
    {
        std::unique_lock lk(s_classFromTypeMtx);
        s_classFromTypeCache.clear();
    }
}

// LOGGING

void MonoLookupCache::LogStats() {
    auto logOne = [](const char* name, CacheStats& s) {
        uint64_t h = s.hits.load(), m = s.misses.load();
        if (h + m > 0) {
            double pct = 100.0 * h / (h + m);
            LOG_INFO("[MonoLookupCache] " + std::string(name) + ": " + std::to_string(h) + " hits, " + std::to_string(m) + " misses (" + std::to_string(static_cast<int>(pct)) + "%)");
        }
    };
    logOne("class_from_name", s_statsClassFromName);
    logOne("class_get_full", s_statsClassGetFull);
    logOne("class_vtable", s_statsClassVtable);
    logOne("field_from_name", s_statsFieldFromName);
    logOne("method_from_name", s_statsMethodFromName);
    logOne("class_from_type", s_statsClassFromType);
}

} // namespace S3MM
