#include "resource_system.h"
#include "addresses.h"
#include "mono_bridge.h"               // IsValidPointer
#include "../modding/package_reader.h" // DBPFHeader
#include "../util/logger.h"
#include "../util/config_paths.h"
#include <Windows.h>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace S3MM {

// HELPERS - SEH-safe vtable calls

// SEH helpers - standalone functions with no C++ objects (MSVC C2712 workaround xDDdDdDDdDDDDDDDDDDD)
static bool SEH_ReadDBArray(void* manager, uintptr_t** outStart, uintptr_t** outEnd) {
    __try {
        *outStart = *(uintptr_t**)((uintptr_t)manager + 0x30);
        *outEnd = *(uintptr_t**)((uintptr_t)manager + 0x34);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SEH_ReadDBEntry(uintptr_t* entry, void** outPtr, int* outPriority) {
    __try {
        *outPtr = (void*)*entry;
        *outPriority = (int)*(entry + 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-wrapped vtable call: Database->vtable[idx]() returning uint32_t
static uint32_t SafeVtableCallUint(void* obj, uint32_t vtableOffset) {
    __try {
        void* vtable = *(void**)obj;
        if (!vtable || !IsValidPointer(vtable)) return 0;
        auto fn = *(uint32_t(__thiscall**)(void*))((uintptr_t)vtable + vtableOffset);
        if (!fn || !IsValidPointer((void*)fn)) return 0;
        return fn(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// SEH-wrapped vtable call: Database->vtable[idx]() returning wchar_t*
static const wchar_t* SafeVtableCallWstr(void* obj, uint32_t vtableOffset) {
    __try {
        void* vtable = *(void**)obj;
        if (!vtable || !IsValidPointer(vtable)) return nullptr;
        auto fn = *(const wchar_t*(__thiscall**)(void*))((uintptr_t)vtable + vtableOffset);
        if (!fn || !IsValidPointer((void*)fn)) return nullptr;
        const wchar_t* result = fn(obj);
        if (result && !IsValidPointer((void*)result)) return nullptr;
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// RESOURCE KEY - Format implementation

std::string ResourceKey::Format() const {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << group << "!"
       << "0x" << std::setw(8) << instance_hi << std::setw(8) << instance_lo << ".0x" << std::setw(8) << type;
    return ss.str();
}

// COMMON TYPE NAME TABLE - Initially sourced from S3PI Extensions.txt, validated against factory registration strings (Sims3::ResourceMan::FactoryBase ctors)
// Runtime fallbacks Manager::GetTypenameFromType (vtable[37]) for unmapped types
static const std::unordered_map<uint32_t, std::string> s_knownTypes = {
    // 3D / Geometry
    {0x00AE6C67, "BONE (.skcon)"},
    {0x015A1849, "GEOM (.simgeom)"},
    {0x0166038C, "KEY (.keyname)"},
    {0x01661233, "MODL (.model)"},
    {0x021D7E8C, "SPT2 (.speedtree)"},
    {0x067CAA11, "BGEO (.blendgeom)"},
    {0x736884F1, "VPXY (.proxy)"},
    {0x8EAF13DE, "RIG (.grannyrig)"},
    {0xCF9A4ACE, "MDLR (.modular)"},
    {0xD3044521, "RSLT (.slot)"},
    {0xD382BF57, "FTPT (.footprint)"},

    // Textures / Images
    {0x00B2D882, "IMG (.dds)"},
    {0x00B552EA, "SPT (.tree)"},
    {0x2F7D0002, "IMAG (.jpg)"},
    {0x2F7D0004, "IMAG (.png)"},
    {0x2F7D0006, "IMAG (.tga)"},
    {0x2F7D0008, "UITX (.uitexture)"},
    {0x3F8662EA, "JPEG (.jpg)"},
    {0x8FFB80F6, "ADS (.dds)"},

    // Materials
    {0x01D0E75D, "MATD (.material)"},
    {0x02019972, "MTST (.mtst)"},
    {0x044AE110, "COMP (.complate)"},
    {0x0341ACC9, "TXTF (.fabric)"},
    {0xD4D9FBE5, "PTRN (.patternlist)"},

    // Animation
    {0x01D10F34, "MLOD (.lod)"},
    {0x02D5DF13, "JAZZ (.jazz)"},
    {0x033260E3, "TkMk (.trackmask)"},
    {0x6B20C4F3, "CLIP (.animation)"},
    {0x63A33EA7, "ANIM (.animimg)"},

    // Scripts
    {0x073FAA07, "S3SA (.s3sa)"},     // SignedAssemblyResourceFactory
    {0x08646069, "AMNF (.manifest)"}, // Sims3::Scripting::AssemblyManifestResourceFactory
    {0x0175E5CD, "SCPT (.oldscript)"},

    // Tuning / Data
    {0x0333406C, "XML (.xml)"},
    {0x03B33DDF, "ITUN (.tuning)"},
    {0x220557DA, "STBL (.stbl)"},
    {0x73E93EEB, "XML (.manifest)"},
    {0xA8D58BE5, "SKIL (.xml)"},
    {0xDD3223A7, "BUFF (.xml)"},
    {0xE5105066, "XML (.xml)"},
    {0xE5105067, "RMLS (.xml)"},
    {0xE5105068, "XML (.xml)"},

    // Objects / World
    {0x319E4F1D, "OBJD (.object)"},
    {0x02DC343F, "OBJK (.objkey)"},
    {0x4D1A5589, "OBJN (.objn)"},
    {0x4E97BAD8, "OBSC (.objscript)"},
    {0x06B981ED, "OBJS (.objs)"},
    {0xD063545B, "LDES (.lotdesc)"},
    {0x06CE4804, "META (.worldmeta)"},

    // CAS / Sim
    {0x025ED6F4, "SIMO (.outfit)"},
    {0x033A1435, "TXTC (.compositor)"},
    {0x034AEECB, "CASP (.caspart)"},
    {0x0354796A, "TONE (.skintone)"},
    {0x03555BA8, "TONE (.hairtone)"},
    {0x0355E0A6, "BOND (.bonedelta)"},
    {0x0358B08A, "FACE (.faceblend)"},
    {0x04F51033, "SBNO (.binoutfit)"},
    {0x062C8204, "BBLN (.bodyblend)"},
    {0x3A65AF29, "MINF (.makeup)"},
    {0x72683C15, "STPR (.sktonepreset)"},
    {0x0591B1AF, "UPST (.usercastpreset)"},
    {0xB52F5055, "FBLN (.blendunit)"},
    {0x051DF2DD, "CBLN (.compblend)"},
    {0x095576E0, "SPDF (.speciesdef)"}, // Sims3::CAS::SpeciesDefinitionResourceFactory
    {0xDEA2951C, "PETB (.breed)"},

    // Color Info
    {0x06302271, "CINF (.color)"},
    {0x063261DA, "HINF (.haircolor)"},
    {0x06326213, "OBCI (.objcolor)"},

    // Audio
    {0x010077BB, "AUD (.mp3)"},
    {0x010077C4, "AUD (.wav)"},
    {0x010077CA, "AUD (.xa)"},
    {0x0181B0D2, "AUD (.abk)"},
    {0x01A527DB, "AUD (.snr)"},
    {0x01EEF63A, "AUD (.sns)"},
    {0x029E333B, "VOCE (.voicemix)"},
    {0x02B9F662, "PROP (.audprop)"},
    {0x02C9EFF2, "MIXR (.audmix)"},
    {0x8070223D, "AUDT (.audtun)"},

    // Lighting / Effects
    {0x03B4C61D, "LITE (.light)"},
    {0xD55F7CAF, "LRIG (.lightrig)"},
    {0xEA5118B0, "SWB (.effects)"},
    {0xF0FF5598, "TRIG (.triggers)"},
    {0x11C258C0, "CTRG (.bnry)"},

    // Catalog
    {0x0418FE2A, "CFEN (.fence)"},
    {0x049CA4CD, "CSTR (.stairs)"},
    {0x04AC5D93, "CPRX (.proxyprod)"},
    {0x04B30669, "CTTL (.terraintool)"},
    {0x04C58103, "CRAL (.railing)"},
    {0x04ED4BB2, "CTPT (.terrainpaint)"},
    {0x04F3CC01, "CFIR (.fireplace)"},
    {0x060B390C, "CWAT (.water)"},
    {0x316C78F2, "CFND (.foundation)"},
    {0x515CA4CD, "CWAL (.wall)"},
    {0x9151E6BC, "CWST (.wallstyle)"},
    {0x91EDBD3E, "CRST (.roofstyle)"},
    {0xF1EDBD86, "CRMT (.roofmatrl)"},

    // World Description
    {0x022B756C, "WRDH (.wdescnamehash)"},
    {0x04A4D951, "WDET (.wrlddetail)"},
    {0x0668F628, "WDDT (.wdescdetail)"},
    {0x0668F630, "WDNL (.numlots)"},
    {0x0668F639, "TWNP (.imgpath)"},
    {0x35A33E29, "WDSH (.wdeschash)"},
    {0x9063660D, "WTXT (.wrldtxtrmap)"},
    {0xD9BD0909, "WDNM (.wdescname)"},
    {0xF609FD60, "WPID (.wpackid)"},

    // UI / Config
    {0x024A0E52, "CNFG (.cfg)"},
    {0x025C90A6, "CSS (.css)"},
    {0x025C95B6, "LAYO (.layout)"},
    {0x1F886EAD, "INI (.ini)"},
    {0x4148EF9B, "GFX (.swf)"}, // ScaleformResourceFactory
    {0xDD6233D6, "HTML (.html)"},

    // Misc
    {0x03D843C2, "TCDD (.tcdependency)"}, // S3PI: "CCHE", PDB: Sims3::CAS::TCDependencyResourceFactory
    {0x03D86EA4, "DETL (.details)"},
    {0x04D82D90, "CMRU (.cachemru)"},
    {0x04F88964, "SIME (.simexport)"},
    {0x05ED1226, "REFS (.references)"},
    {0x05FF6BA4, "2ARY (.bnry)"},
    {0x0604ABDA, "DMTR (.dreamtree)"},
    {0x062853A8, "FAMD (.household)"},
    {0x062E9EE0, "TTF (.ttf)"},
    {0x065DF841, "CPCA (.cpcache)"},
    {0x0A36F07A, "CCFP (.fountain)"},
    {0x0A98EAF0, "JSON (.json)"},
    {0x0C07456D, "COLL (.bnry)"},
    {0x0C07469F, "CLDR (.bnry)"},
    {0x0C37A5B5, "LOOK (.lookuptab)"},
    {0x1A3201CD, "MOD (.override)"},
    {0x67771F5C, "JSON (.json)"},
    {0x82B43584, "CRC (.checksum)"},
    {0x94C5D14A, "SIGR (.signature)"},
    {0xB1CC1AF6, "VID (.video)"},
    {0xB4DD716B, "INV (.inventory)"},
    {0xF3A38370, "NGMP (.guidmap)"},

    // Thumbnails (THUM)
    {0x0580A2B4, "THUM"},
    {0x0580A2B5, "THUM"},
    {0x0580A2B6, "THUM"},
    {0x0580A2CD, "SNAP"},
    {0x0580A2CE, "SNAP"},
    {0x0580A2CF, "SNAP"},
    {0x0589DC44, "THUM"},
    {0x0589DC45, "THUM"},
    {0x0589DC46, "THUM"},
    {0x0589DC47, "THUM"},
    {0x05B17698, "THUM"},
    {0x05B17699, "THUM"},
    {0x05B1769A, "THUM"},
    {0x05B1B524, "THUM"},
    {0x05B1B525, "THUM"},
    {0x05B1B526, "THUM"},
    {0x0668F635, "TWNI"},
    {0x2653E3C8, "THUM"},
    {0x2653E3C9, "THUM"},
    {0x2653E3CA, "THUM"},
    {0x2D4284F0, "THUM"},
    {0x2D4284F1, "THUM"},
    {0x2D4284F2, "THUM"},
    {0x2E75C764, "ICON"},
    {0x2E75C765, "ICON"},
    {0x2E75C766, "ICON"},
    {0x2E75C767, "ICON"},
    {0x5DE9DBA0, "THUM"},
    {0x5DE9DBA1, "THUM"},
    {0x5DE9DBA2, "THUM"},
    {0x626F60CC, "THUM"},
    {0x626F60CD, "THUM"},
    {0x626F60CE, "THUM"},
    {0x6B6D837D, "SNAP"},
    {0x6B6D837E, "SNAP"},
    {0x6B6D837F, "SNAP"},
    {0xAD366F95, "THUM"},
    {0xAD366F96, "THUM"},
    {0xD84E7FC5, "ICON"},
    {0xD84E7FC6, "ICON"},
    {0xD84E7FC7, "ICON"},
    {0xFCEAB65B, "THUM"},
};

// RESOURCE SYSTEM API - Implementation

void* ResourceSystem::GetManager() {
    uintptr_t addr = Addresses::ScriptHost.GetResourceManager;
    if (!addr) return nullptr;

    auto fn = (void*(__cdecl*)())addr;
    __try {
        void* mgr = fn();
        if (mgr && IsValidPointer(mgr)) return mgr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

std::vector<DatabaseInfo> ResourceSystem::GetRegisteredDatabases() {
    std::vector<DatabaseInfo> result;

    void* manager = GetManager();
    if (!manager) {
        LOG_WARN("[ResourceSystem] GetResourceManager returned null");
        return result;
    }

    // DBArray at Manager+0x30 (begin) / +0x34 (end)
    // Each entry: 8 bytes = { Database* ptr, int priority }
    uintptr_t* dbStart = nullptr;
    uintptr_t* dbEnd = nullptr;

    if (!SEH_ReadDBArray(manager, &dbStart, &dbEnd)) {
        LOG_ERROR("[ResourceSystem] Failed to read DBArray pointers");
        return result;
    }

    if (!dbStart || !dbEnd || !IsValidPointer(dbStart) || !IsValidPointer(dbEnd)) {
        LOG_WARN("[ResourceSystem] DBArray pointers invalid");
        return result;
    }

    size_t rawEntryCount = (dbEnd - dbStart) / 2;
    LOG_INFO("[ResourceSystem] DBArray: mgr=" + ToHex((uintptr_t)manager) + " start=" + ToHex((uintptr_t)dbStart) + " end=" + ToHex((uintptr_t)dbEnd) + " rawEntries=" + std::to_string(rawEntryCount));

    // Walk DBArray entries (already sorted by priority descending)
    size_t skippedSEH = 0, skippedPtr = 0;
    for (uintptr_t* entry = dbStart; entry < dbEnd; entry += 2) {
        DatabaseInfo info;

        if (!SEH_ReadDBEntry(entry, &info.ptr, &info.priority)) {
            skippedSEH++;
            continue;
        }
        if (!info.ptr || !IsValidPointer(info.ptr)) {
            skippedPtr++;
            continue;
        }

        // Get database type ID via vtable[3] (GetTypeId)
        info.typeId = SafeVtableCallUint(info.ptr, DatabaseVtable::GetTypeId);

        if (info.typeId == DB_TYPE_DIRECTORY_FILES) {
            info.typeName = "DirectoryFiles";
        } else if (info.typeId == DB_TYPE_PACKED_FILE) {
            info.typeName = "PackedFile";
        } else if (info.typeId == DB_TYPE_PACKED_STREAM) {
            info.typeName = "PackedStream";
        } else {
            info.typeName = "Unknown (0x" + ToHex(info.typeId) + ")";
        }

        // Get path via vtable[0x0A] (GetPath)
        const wchar_t* pathW = SafeVtableCallWstr(info.ptr, DatabaseVtable::GetPath);
        if (pathW) {
            info.path = ConfigPaths::WideToUtf8(pathW);
            // Extract filename
            size_t lastSlash = info.path.find_last_of("\\/");
            info.filename = (lastSlash != std::string::npos) ? info.path.substr(lastSlash + 1) : info.path;
        } else {
            info.path = "<unknown>";
            info.filename = "<unknown>";
        }

        result.push_back(std::move(info));
    }

    LOG_INFO("[ResourceSystem] Enumerated " + std::to_string(result.size()) + " databases" + " (skippedSEH=" + std::to_string(skippedSEH) + " skippedPtr=" + std::to_string(skippedPtr) + ")");
    return result;
}

// SEH-wrapped Manager::GetTypenameFromType vtable[37], wchar_t* __thiscall (Manager*, uint32_t typeId)
static const wchar_t* SEH_GetTypenameFromType(void* manager, uint32_t typeId) {
    __try {
        void* vtable = *(void**)manager;
        if (!vtable || !IsValidPointer(vtable)) return nullptr;
        auto fn = *(const wchar_t*(__thiscall**)(void*, uint32_t))((uintptr_t)vtable + ManagerVtable::GetTypenameFromType);
        if (!fn || !IsValidPointer((void*)fn)) return nullptr;
        const wchar_t* result = fn(manager, typeId);
        if (result && !IsValidPointer((void*)result)) return nullptr;
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

std::string ResourceSystem::GetTypeName(uint32_t typeId) {
    // Check built-in table first
    auto it = s_knownTypes.find(typeId);
    if (it != s_knownTypes.end()) return it->second;

    // Try Manager's TNMap via vtable[37] (GetTypenameFromType)
    void* manager = GetManager();
    if (manager) {
        const wchar_t* name = SEH_GetTypenameFromType(manager, typeId);
        if (name) {
            std::string result = ConfigPaths::WideToUtf8(name);
            if (!result.empty()) return result;
        }
    }

    return "";
}

bool ResourceSystem::DatabaseHasResource(void* database, const ResourceKey& key) {
    if (!database || !IsValidPointer(database)) return false;

    __try {
        void* vtable = *(void**)database;
        if (!vtable || !IsValidPointer(vtable)) return false;

        // HasResource: vtable[0x0D] - bool(Key*, int, int, int, int, int)
        auto fn = *(bool(__thiscall**)(void*, const ResourceKey*, int, int, int, int, int))((uintptr_t)vtable + DatabaseVtable::HasResource);
        if (!fn || !IsValidPointer((void*)fn)) return false;

        return fn(database, &key, 0, 1, 6, 1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

DatabaseInfo ResourceSystem::FindWinningDatabase(const ResourceKey& key) {
    auto databases = GetRegisteredDatabases();

    for (auto& db : databases) {
        if (DatabaseHasResource(db.ptr, key)) { return db; }
    }

    return DatabaseInfo{nullptr, 0, "", "", 0, ""};
}

// DBPF INDEX ENUMERATION - Pattern extracted from vanilla_extractor.cpp EnumerateS3SAInPackage()

std::vector<PackageKeyInfo> ResourceSystem::EnumeratePackageKeys(const std::string& path) {
    std::vector<PackageKeyInfo> result;

    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_WARN("[ResourceSystem] Failed to open package: " + path);
        return result;
    }

    DWORD bytesRead;
    DBPFHeader header;
    if (!ReadFile(hFile, &header, sizeof(header), &bytesRead, NULL) || bytesRead != sizeof(header)) {
        LOG_ERROR("[ResourceSystem] Failed to read DBPF header: " + path);
        CloseHandle(hFile);
        return result;
    }

    if (memcmp(header.magic, "DBPF", 4) != 0) {
        LOG_ERROR("[ResourceSystem] Not a DBPF file: " + path);
        CloseHandle(hFile);
        return result;
    }

    if (header.indexEntryCount == 0 || header.indexOffset == 0) {
        CloseHandle(hFile);
        return result;
    }

    // Seek to index
    if (SetFilePointer(hFile, header.indexOffset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        LOG_ERROR("[ResourceSystem] Failed to seek to index: " + path);
        CloseHandle(hFile);
        return result;
    }

    // Read index flags (determines which fields are constant vs per-entry)
    uint32_t indexFlags = 0;
    if (!ReadFile(hFile, &indexFlags, 4, &bytesRead, NULL) || bytesRead != 4) {
        CloseHandle(hFile);
        return result;
    }

    uint32_t constType = 0, constGroup = 0, constInstHi = 0;
    if (indexFlags & 0x01) ReadFile(hFile, &constType, 4, &bytesRead, NULL);
    if (indexFlags & 0x02) ReadFile(hFile, &constGroup, 4, &bytesRead, NULL);
    if (indexFlags & 0x04) ReadFile(hFile, &constInstHi, 4, &bytesRead, NULL);

    result.reserve(header.indexEntryCount);

    for (uint32_t i = 0; i < header.indexEntryCount; i++) {
        uint32_t entryType = constType, entryGroup = constGroup;
        uint32_t entryInstHi = constInstHi, entryInstLo;
        uint32_t entryOffset, entryFileSize, entryMemSize, entryFlags;

        if (!(indexFlags & 0x01)) ReadFile(hFile, &entryType, 4, &bytesRead, NULL);
        if (!(indexFlags & 0x02)) ReadFile(hFile, &entryGroup, 4, &bytesRead, NULL);
        if (!(indexFlags & 0x04)) ReadFile(hFile, &entryInstHi, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryInstLo, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryOffset, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFileSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryMemSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFlags, 4, &bytesRead, NULL);

        PackageKeyInfo info;
        info.key.type = entryType;
        info.key.group = entryGroup;
        info.key.instance_hi = entryInstHi;
        info.key.instance_lo = entryInstLo;
        info.offset = entryOffset;
        info.fileSize = entryFileSize & 0x7FFFFFFF;
        info.memSize = entryMemSize;
        info.compressed = (entryFileSize & 0x80000000) != 0;

        result.push_back(info);
    }

    CloseHandle(hFile);
    LOG_INFO("[ResourceSystem] Enumerated " + std::to_string(result.size()) + " keys from " + path);
    return result;
}

// DDF DIRECTORY ENUMERATION - Parses loose files with DDF naming: 0xGROUP!0xINSTHILO.0xTYPE

static bool ParseDDFFilename(const std::wstring& filename, ResourceKey& outKey) {
    // Expected: "0xGGGGGGGG!0xIIIIIIIIIIIIIIII.0xTTTTTTTT"
    // or:       "0xGGGGGGGG!0xIIIIIIIIIIIIIIII.ext"
    if (filename.size() < 10) return false;
    if (filename[0] != L'0' || filename[1] != L'x') return false;

    // Find '!'
    size_t bangPos = filename.find(L'!');
    if (bangPos == std::wstring::npos || bangPos < 3) return false;

    // Find last '.'
    size_t dotPos = filename.rfind(L'.');
    if (dotPos == std::wstring::npos || dotPos <= bangPos) return false;

    // Parse group: "0xGGGGGGGG"
    std::wstring groupStr = filename.substr(2, bangPos - 2);
    outKey.group = (uint32_t)wcstoul(groupStr.c_str(), nullptr, 16);

    // Parse instance: "0xIIIIIIIIIIIIIIII" (up to 16 hex digits)
    std::wstring instStr = filename.substr(bangPos + 3, dotPos - bangPos - 3); // skip "0x"
    if (filename[bangPos + 1] != L'0' || filename[bangPos + 2] != L'x') return false;
    uint64_t inst = _wcstoui64(instStr.c_str(), nullptr, 16);
    outKey.instance_lo = (uint32_t)(inst & 0xFFFFFFFF);
    outKey.instance_hi = (uint32_t)(inst >> 32);

    // Parse type: "0xTTTTTTTT" or ".ext"
    std::wstring typeStr = filename.substr(dotPos + 1);
    if (typeStr.size() > 2 && typeStr[0] == L'0' && typeStr[1] == L'x') {
        outKey.type = (uint32_t)wcstoul(typeStr.substr(2).c_str(), nullptr, 16);
    } else {
        // Extension-based, would need reverse type map; store 0 for now
        outKey.type = 0;
    }

    return true;
}

std::vector<ResourceKey> ResourceSystem::EnumerateDDFKeys(const std::string& directoryPath) {
    std::vector<ResourceKey> result;

    std::wstring searchPath = ConfigPaths::Utf8ToWide(directoryPath) + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        LOG_WARN("[ResourceSystem] Failed to enumerate DDF directory: " + directoryPath);
        return result;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        ResourceKey key;
        if (ParseDDFFilename(fd.cFileName, key)) { result.push_back(key); }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    LOG_INFO("[ResourceSystem] Enumerated " + std::to_string(result.size()) + " DDF keys from " + directoryPath);
    return result;
}

// TYPE EXTENSION MAP - Parses extension from the known types table

std::string ResourceSystem::GetTypeExtension(uint32_t typeId) {
    auto it = s_knownTypes.find(typeId);
    if (it != s_knownTypes.end()) {
        // Parse extension from "TAG (.ext)" format
        const auto& val = it->second;
        size_t parenOpen = val.rfind('(');
        size_t parenClose = val.rfind(')');
        if (parenOpen != std::string::npos && parenClose != std::string::npos && parenClose > parenOpen) { return val.substr(parenOpen + 1, parenClose - parenOpen - 1); }
    }
    return ".dat";
}

// NAME MAP PARSER - Type 0x0166038C, format: u32 version(=1), i32 count, count * {u64 instanceID, i32 nameLen, char[nameLen]}

// Refpack decompression - __stdcall, 5 params (RET 0x14 confirmed in disasm at 0x004EC010). 5th param unreferenced, vtable artifact.
using RefpackDecompress_t = uint32_t(__stdcall*)(const void* src, uint32_t srcLen, void* dst, uint32_t dstLen, uint32_t reserved);
static RefpackDecompress_t s_refpackDecompress = nullptr;

// Read raw bytes from a DBPF entry at given offset/size
static std::vector<uint8_t> ReadDBPFEntryRaw(HANDLE hFile, uint32_t offset, uint32_t fileSize) {
    std::vector<uint8_t> data;
    if (SetFilePointer(hFile, offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return data;

    data.resize(fileSize);
    DWORD bytesRead;
    if (!ReadFile(hFile, data.data(), fileSize, &bytesRead, NULL) || bytesRead != fileSize) { data.clear(); }
    return data;
}

// Check Refpack header using the game's own logic at 0x004EC021:
//   big_endian = (byte[0] << 8) | byte[1];  AND 0x1FFF;  CMP 0x10FB
// Accepts flag variants: 0x10FB, 0x50FB, 0x90FB, etc.
static bool IsRefpackHeader(const uint8_t* data, size_t size) {
    if (size < 5) return false; // minimum: 2-byte header + 3-byte decompressed size
    uint16_t be = ((uint16_t)data[0] << 8) | data[1];
    return (be & 0x1FFF) == 0x10FB;
}

// Decompress Refpack data if needed, returns decompressed or raw data
static std::vector<uint8_t> DecompressIfRefpack(const std::vector<uint8_t>& raw, uint32_t memSize) {
    if (IsRefpackHeader(raw.data(), raw.size())) {
        // Refpack compressed - direct call (same pattern as package_reader.cpp)
        if (!s_refpackDecompress) s_refpackDecompress = (RefpackDecompress_t)Addresses::Misc.RefPack_Decompress;
        if (!s_refpackDecompress) return {};

        std::vector<uint8_t> decompressed(memSize);
        uint32_t result = s_refpackDecompress(raw.data(), (uint32_t)raw.size(), decompressed.data(), memSize, 0);
        if (result == 0 || result == 0xFFFFFFFF) {
            LOG_ERROR("[ResourceSystem] Refpack decompress failed: result=" + std::to_string(result));
            return {};
        }
        decompressed.resize(result);
        return decompressed;
    }
    // Not compressed, return as-is
    return raw;
}

std::unordered_map<uint64_t, std::string> ResourceSystem::ParseNameMap(const std::string& packagePath) {
    std::unordered_map<uint64_t, std::string> nameMap;

    HANDLE hFile = CreateFileA(packagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return nameMap;

    DWORD bytesRead;
    DBPFHeader header;
    if (!ReadFile(hFile, &header, sizeof(header), &bytesRead, NULL) || bytesRead != sizeof(header)) {
        CloseHandle(hFile);
        return nameMap;
    }
    if (memcmp(header.magic, "DBPF", 4) != 0 || header.indexEntryCount == 0 || header.indexOffset == 0) {
        CloseHandle(hFile);
        return nameMap;
    }

    // Seek to index
    if (SetFilePointer(hFile, header.indexOffset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return nameMap;
    }

    // Read index flags + constant fields
    uint32_t indexFlags = 0;
    ReadFile(hFile, &indexFlags, 4, &bytesRead, NULL);

    uint32_t constType = 0, constGroup = 0, constInstHi = 0;
    if (indexFlags & 0x01) ReadFile(hFile, &constType, 4, &bytesRead, NULL);
    if (indexFlags & 0x02) ReadFile(hFile, &constGroup, 4, &bytesRead, NULL);
    if (indexFlags & 0x04) ReadFile(hFile, &constInstHi, 4, &bytesRead, NULL);

    // Find ALL Name Map entries (type 0x0166038C). A package can have multiple KEY entries
    // (different group/instance IDs), each covering different resources - S3PE loads all of them.
    constexpr uint32_t NAME_MAP_TYPE = 0x0166038C;
    struct NMEntry {
        uint32_t offset;
        uint32_t fileSize;
        uint32_t memSize;
    };
    std::vector<NMEntry> nmEntries;

    for (uint32_t i = 0; i < header.indexEntryCount; i++) {
        uint32_t entryType = constType, entryGroup = constGroup;
        uint32_t entryInstHi = constInstHi, entryInstLo;
        uint32_t entryOffset, entryFileSize, entryMemSize, entryFlags;

        if (!(indexFlags & 0x01)) ReadFile(hFile, &entryType, 4, &bytesRead, NULL);
        if (!(indexFlags & 0x02)) ReadFile(hFile, &entryGroup, 4, &bytesRead, NULL);
        if (!(indexFlags & 0x04)) ReadFile(hFile, &entryInstHi, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryInstLo, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryOffset, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFileSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryMemSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFlags, 4, &bytesRead, NULL);

        if (entryType == NAME_MAP_TYPE) { nmEntries.push_back({entryOffset, entryFileSize & 0x7FFFFFFF, entryMemSize}); }
    }

    if (nmEntries.empty()) {
        CloseHandle(hFile);
        return nameMap; // No Name Map in this package, that's fine
    }

    // Parse each Name Map entry and merge into one map
    for (const auto& nm : nmEntries) {
        auto raw = ReadDBPFEntryRaw(hFile, nm.offset, nm.fileSize);
        if (raw.empty()) continue;

        // Always try Refpack decompression based on data header (some entries are compressed without the index flag being set)
        std::vector<uint8_t> data = DecompressIfRefpack(raw, nm.memSize);
        if (data.size() < 8) continue;

        const uint8_t* p = data.data();
        const uint8_t* end = p + data.size();

        uint32_t version = *(uint32_t*)p;
        p += 4;
        if (version != 1) {
            // Partitioned packages (_p##.package) may have stale index entries pointing to data in the base partition
            LOG_DEBUG("[ResourceSystem] Name Map version " + std::to_string(version) + " (expected 1) - skipping");
            continue;
        }

        int32_t count = *(int32_t*)p;
        p += 4;
        if (count < 0 || count > 1000000) continue;

        nameMap.reserve(nameMap.size() + count);
        for (int32_t i = 0; i < count && p + 12 <= end; i++) {
            uint64_t instanceId = *(uint64_t*)p;
            p += 8;
            int32_t nameLen = *(int32_t*)p;
            p += 4;

            if (nameLen < 0 || nameLen > 4096 || p + nameLen > end) break;

            std::string name((const char*)p, nameLen);
            p += nameLen;
            nameMap.emplace(instanceId, std::move(name)); // first entry wins (highest priority)
        }
    }

    CloseHandle(hFile);

    LOG_INFO("[ResourceSystem] Parsed " + std::to_string(nmEntries.size()) + " Name Map(s): " + std::to_string(nameMap.size()) + " entries from " + packagePath);
    return nameMap;
}

// RESOURCE EXTRACTION - Read and decompress entries from .package files

std::vector<uint8_t> ResourceSystem::ExtractResourceData(const std::string& packagePath, const PackageKeyInfo& entry) {
    HANDLE hFile = CreateFileA(packagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_WARN("[ResourceSystem] Failed to open for extraction: " + packagePath);
        return {};
    }

    auto raw = ReadDBPFEntryRaw(hFile, entry.offset, entry.fileSize);
    CloseHandle(hFile);

    if (raw.empty()) {
        LOG_ERROR("[ResourceSystem] Failed to read entry data at offset " + ToHex(entry.offset));
        return {};
    }

    // Always check for Refpack header regardless of compressed flag
    return DecompressIfRefpack(raw, entry.memSize);
}

// Create directory tree recursively (handles both / and \ separators)
static bool CreateDirectoryTree(const std::string& path) {
    std::string dir;
    for (size_t i = 0; i < path.size(); i++) {
        char c = path[i];
        dir += c;
        if ((c == '\\' || c == '/') && dir.size() > 1) {
            // Skip drive letter (e.g., "C:\")
            if (dir.size() == 3 && dir[1] == ':') continue;
            CreateDirectoryA(dir.c_str(), NULL);
        }
    }
    CreateDirectoryA(dir.c_str(), NULL);
    return GetFileAttributesA(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ResourceSystem::ExtractResourceToFile(const std::string& packagePath, const PackageKeyInfo& entry, const std::string& outputPath) {
    auto data = ExtractResourceData(packagePath, entry);
    if (data.empty()) {
        LOG_ERROR("[ResourceSystem] No data to extract");
        return false;
    }

    // Create output directory
    size_t lastSlash = outputPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        std::string dir = outputPath.substr(0, lastSlash);
        CreateDirectoryTree(dir);
    }

    // Write to file
    HANDLE hOut = CreateFileA(outputPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        LOG_ERROR("[ResourceSystem] Failed to create output file: " + outputPath);
        return false;
    }

    DWORD written;
    BOOL ok = WriteFile(hOut, data.data(), (DWORD)data.size(), &written, NULL);
    CloseHandle(hOut);

    if (!ok || written != data.size()) {
        LOG_ERROR("[ResourceSystem] Write failed: " + outputPath);
        return false;
    }

    LOG_INFO("[ResourceSystem] Extracted " + std::to_string(data.size()) + " bytes to " + outputPath);
    return true;
}

} // namespace S3MM
