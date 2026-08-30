#include "persist_bridge.h"
#include "mod_loader.h"
#include "../core/mono_bridge.h"
#include "../core/mono_types.h"
#include "../core/exception_helper.h"
#include "../util/config.h"
#include "../util/logger.h"
#include <cstring>

namespace S3MM {

PersistBridge& PersistBridge::Get() {
    static PersistBridge instance;
    return instance;
}

void* PersistBridge::FindMethod(void* klass, const char* name) {
    if (!klass || !name) return nullptr;
    int iter = 0;
    while (void* method = MonoBridge::Get().GetNextMethod(klass, &iter)) {
        const char* mName = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);
        if (mName && strcmp(mName, name) == 0) return method;
    }
    return nullptr;
}

bool PersistBridge::Initialize(void* helperImage) {
    if (m_ready) return true;

    if (!Config::Get().IsPersistAutoRegister()) {
        LOG_INFO("[PersistBridge] Disabled by config (PersistAutoRegister = false) - [PersistableStatic] fields of S3MM mods will NOT persist");
        return false;
    }
    if (!helperImage) {
        LOG_WARN("[PersistBridge] S3MM.Helper image not available - auto-registration disabled");
        return false;
    }

    void* klass = MonoBridge::Get().GetClassFromNameInImage(helperImage, "S3MM", "PersistBridge");
    if (!klass) {
        LOG_WARN("[PersistBridge] S3MM.PersistBridge not found in S3MM.Helper (stale helper?) - auto-registration disabled");
        return false;
    }

    void* install = FindMethod(klass, "Install");
    m_onModLoaded = FindMethod(klass, "OnModLoaded");
    m_flushSignal = FindMethod(klass, "FlushSignal");
    if (!install || !m_onModLoaded || !m_flushSignal) {
        LOG_WARN("[PersistBridge] S3MM.PersistBridge is missing expected methods - auto-registration disabled");
        return false;
    }

    if (!Invoke(install, nullptr, "Install")) return false;

    m_ready = true;
    return true;
}

bool PersistBridge::Invoke(void* method, void** params, const std::string& what) {
    MonoInvokeState state{};
    bool ok = MonoBridge::Get().InvokeMethodOnSimThread(method, nullptr, params, &state);
    if (!ok) {
        std::string exc = state.pThrowExc ? ExtractExceptionInfo(state.pThrowExc) : "invoke failed (sim thread not pumping or native fault)";
        LOG_ERROR("[PersistBridge] " + what + " failed: " + exc);
    }
    return ok;
}

void PersistBridge::OnModLoaded(const LoadedMod& mod) {
    if (!m_ready || !mod.monoImage) return;
    const char* asmName = MonoBridge::GetImageName(mod.monoImage);
    // MonoString* args stay rooted via the native stack for the duration of the invoke
    void* params[2] = {MonoBridge::Get().CreateString(asmName ? asmName : ""), MonoBridge::Get().CreateString(mod.name.c_str())};
    Invoke(m_onModLoaded, params, "OnModLoaded(" + mod.name + ")");
}

void PersistBridge::FlushSignal() {
    if (!m_ready) return;
    Invoke(m_flushSignal, nullptr, "FlushSignal");
}

} // namespace S3MM
