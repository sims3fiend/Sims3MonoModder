#pragma once
#include "mono_bridge.h"
#include <string>
#include <sstream>
#include <cstring>

namespace S3MM {

// Extract formatted exception info from a MonoObject* exception pointer.
// Returns "Namespace.ExceptionType: message", a fallback string, or empty if null.
// Guards against invalid pointers defensively (legacy SEH sentinel, corruption).
inline std::string ExtractExceptionInfo(void* exception) {
    if (!exception) return "";

    // Guard against invalid pointers (legacy sentinel or corruption)
    if (!IsValidPointer(exception)) { return "Native crash (SEH exception, no managed details)"; }

    std::ostringstream ss;

    // Get exception type name: exception -> vtable -> klass -> name/namespace
    void* excClass = MonoBridge::GetObjectClass(exception);
    if (excClass) {
        const char* ns = MonoBridge::GetClassNamespace(excClass);
        const char* typeName = MonoBridge::GetClassName(excClass);
        if (ns && std::strlen(ns) > 0) { ss << ns << "."; }
        ss << (typeName ? typeName : "Exception");
    } else {
        ss << "Exception";
    }

    // Read _message field at offset 0x10 (System.Exception layout in Mono 1.2.3.1)
    // Layout: [vtable:4][??:4][??:4][??:4][_message:4] = offset 0x10
    void* messagePtr = *(void**)((uintptr_t)exception + 0x10);
    if (messagePtr && IsValidPointer(messagePtr)) {
        std::string msg = MonoBridge::Get().ExtractString(messagePtr);
        if (!msg.empty()) { ss << ": " << msg; }
    }

    return ss.str();
}

} // namespace S3MM
