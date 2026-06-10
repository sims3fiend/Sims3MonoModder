#include "package_reader.h"
#include "../core/addresses.h"
#include "../util/logger.h"
#include <Windows.h>
#include <vector>
#include <cstring>
//obviously wip, who knows what horrors I will make with all this
namespace S3MM {

using RefpackDecompress_t = uint32_t(__stdcall*)(const void* src, uint32_t srcLen, void* dst, uint32_t dstLen, uint32_t reserved);
static RefpackDecompress_t s_refpackDecompress = nullptr;

// S3SA DECRYPTION - S3SA format: version, [name if v2], block_count, signature, key_table, encrypted_blocks

// Decrypt S3SA (Sims 3 Script Assembly) data.
// Algorithm ported from s3pi (Sims 3 Package Interface) by Peter L Jones, validated against decomp (some stuff filled in iirc idk)
// Returns output size or 0 on failure.
uint32_t DecryptS3SA(const uint8_t* input, uint32_t inputSize, uint8_t** output) {
    if (inputSize < 10) {
        LOG_ERROR("[S3SA] Input too small");
        return 0;
    }

    const uint8_t* p = input;
    const uint8_t* end = input + inputSize;

    // Version byte
    uint8_t version = *p++;
    char buf[256];
    snprintf(buf, sizeof(buf), "[S3SA] Version: %d", version);
    LOG_INFO(buf);

    if (version != 1 && version != 2) {
        snprintf(buf, sizeof(buf), "[S3SA] Unknown version %d", version);
        LOG_ERROR(buf);
        return 0;
    }

    // If v2, skip assembly name (length-prefixed wchar_t string)
    if (version == 2) {
        // Read name length as uint32 LE (number of wchar_t characters)
        if (p + 4 > end) {
            LOG_ERROR("[S3SA] Truncated at name_length");
            return 0;
        }
        uint32_t nameLen = *(uint32_t*)p; // LE
        p += 4;
        snprintf(buf, sizeof(buf), "[S3SA] Name length: %u chars", nameLen);
        LOG_INFO(buf);

        if (nameLen > 1000) {
            LOG_ERROR("[S3SA] Name length too large, invalid format");
            return 0;
        }

        // Skip the wchar_t name (nameLen * 2 bytes, UTF-16LE format)
        uint32_t nameBytes = nameLen * 2;
        if (p + nameBytes > end) {
            LOG_ERROR("[S3SA] Truncated at name string");
            return 0;
        }

        // Log the name for debugging (UTF-16LE)
        std::wstring name;
        for (uint32_t i = 0; i < nameLen; i++) {
            uint16_t wc = p[i * 2] | (p[i * 2 + 1] << 8); // UTF-16LE
            if (wc) name += (wchar_t)wc;
        }
        p += nameBytes;

        // Convert UTF-16 to narrow string for logging
        std::string narrowName;
        narrowName.reserve(name.size());
        for (wchar_t wc : name) { narrowName += (wc < 128) ? (char)wc : '?'; }
        LOG_INFO("[S3SA] GameVersion: " + narrowName);
        snprintf(buf, sizeof(buf), "[S3SA] After name, now at offset %d", (int)(p - input));
        LOG_INFO(buf);
    }

    // RSA public key token (4 bytes) - read unconditionally per S3PI reference
    if (p + 4 > end) {
        LOG_ERROR("[S3SA] Truncated at rsaPublicKey");
        return 0;
    }
    uint32_t rsaPublicKey = *(uint32_t*)p; // LE
    p += 4;
    snprintf(buf, sizeof(buf), "[S3SA] RSA Public Key: 0x%08X", rsaPublicKey);
    LOG_INFO(buf);

    // md5sum - 64 bytes (comes BEFORE block count per S3PI)
    if (p + 64 > end) {
        LOG_ERROR("[S3SA] Truncated at md5sum");
        return 0;
    }
    const uint8_t* md5sum = p;
    p += 64;
    snprintf(buf, sizeof(buf), "[S3SA] md5sum at offset %d (64 bytes)", (int)(md5sum - input));
    LOG_INFO(buf);

    // Block count (2 bytes LE - UINT16 per S3PI)
    if (p + 2 > end) {
        LOG_ERROR("[S3SA] Truncated at block_count");
        return 0;
    }
    uint16_t blockCount = *(uint16_t*)p; // LE uint16
    p += 2;
    snprintf(buf, sizeof(buf), "[S3SA] Block count: %u (at offset %d)", blockCount, (int)(p - 2 - input));
    LOG_INFO(buf);

    if (blockCount == 0 || blockCount > 50000) {
        // 50000 blocks = ~25MB max, should be enough
        LOG_ERROR("[S3SA] Invalid block count");
        return 0;
    }

    // md5table (blockCount * 8 bytes)
    uint32_t md5tableSize = blockCount * 8;
    if (p + md5tableSize > end) {
        LOG_ERROR("[S3SA] Truncated at md5table");
        return 0;
    }
    const uint8_t* md5table = p;
    p += md5tableSize;
    snprintf(buf, sizeof(buf), "[S3SA] md5table at offset %d, size %u bytes", (int)(md5table - input), md5tableSize);
    LOG_INFO(buf);

    // md5data (blockCount * 512 bytes), the encrypted assembly data
    uint32_t md5dataSize = blockCount * 512;
    if (p + md5dataSize > end) {
        // Some blocks may be skipped (flag & 1), so actual size may be smaller; read what we can.
        snprintf(buf, sizeof(buf), "[S3SA] Expected %u bytes of md5data, only %d available", md5dataSize, (int)(end - p));
        LOG_WARN(buf);
    }
    const uint8_t* md5data = p;

    // Calculate seed from md5table (sum of all uint64 LE values)
    uint64_t seed = 0;
    for (uint32_t i = 0; i < md5tableSize; i += 8) {
        seed += *(uint64_t*)(md5table + i); // LE uint64
    }
    seed = (md5tableSize - 1) & seed;
    snprintf(buf, sizeof(buf), "[S3SA] Initial seed: %llu", seed);
    LOG_INFO(buf);

    // Calculate output size and allocate
    uint32_t outputSize = blockCount * 512;
    *output = new uint8_t[outputSize];
    memset(*output, 0, outputSize);

    // Decrypt
    const uint8_t* readPtr = md5data;
    for (uint32_t i = 0; i < md5tableSize; i += 8) {
        uint8_t* blockOut = *output + (i / 8) * 512;

        if ((md5table[i] & 1) == 0) {
            // Block is present - read and decrypt 512 bytes
            if (readPtr + 512 > end) {
                snprintf(buf, sizeof(buf), "[S3SA] Truncated at block %u", i / 8);
                LOG_ERROR(buf);
                delete[] *output;
                *output = nullptr;
                return 0;
            }
            memcpy(blockOut, readPtr, 512);
            readPtr += 512;

            // XOR decrypt (S3PI algo tyyyy)
            for (int j = 0; j < 512; j++) {
                uint8_t value = blockOut[j];          // Original encrypted byte
                blockOut[j] ^= md5table[seed];        // XOR with key
                seed = (seed + value) % md5tableSize; // Update seed with ORIGINAL byte
            }
        }
        // else: block skipped (flag & 1 == 1), leave as zeros
    }

    uint32_t blocksRead = (uint32_t)(readPtr - md5data) / 512;
    snprintf(buf, sizeof(buf), "[S3SA] Decrypted %u blocks (%u bytes)", blocksRead, blocksRead * 512);
    LOG_INFO(buf);

    // Verify output starts with MZ
    if (outputSize >= 2 && (*output)[0] == 'M' && (*output)[1] == 'Z') {
        LOG_INFO("[S3SA] SUCCESS: Decrypted data has MZ header!");
    } else if (outputSize >= 2) {
        snprintf(buf, sizeof(buf), "[S3SA] Decrypted header: %02X %02X %02X %02X", (*output)[0], (*output)[1], (*output)[2], (*output)[3]);
        LOG_WARN(buf);
    }

    return outputSize;
}

// RESOURCE EXTRACTION - Extract a resource from a .package file by type and instance ID; returns allocated buffer (caller must free with delete[]) and sets outSize.
uint8_t* ExtractResourceFromPackage(const char* packagePath, uint32_t type, uint64_t instance, uint32_t* outSize) {
    *outSize = 0;

    // Open the .package file
    HANDLE hFile = CreateFileA(packagePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_ERROR("[DBPF] Failed to open: " + std::string(packagePath));
        return nullptr;
    }

    // Read header
    DBPFHeader header;
    DWORD bytesRead;
    if (!ReadFile(hFile, &header, sizeof(header), &bytesRead, NULL) || bytesRead != sizeof(header)) {
        LOG_ERROR("[DBPF] Failed to read header");
        CloseHandle(hFile);
        return nullptr;
    }

    // Verify magic
    if (memcmp(header.magic, "DBPF", 4) != 0) {
        LOG_ERROR("[DBPF] Invalid magic (not DBPF)");
        CloseHandle(hFile);
        return nullptr;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "[DBPF] Package: %s, entries=%u, indexOffset=0x%X", packagePath, header.indexEntryCount, header.indexOffset);
    LOG_INFO(buf);

    // Seek to index
    if (SetFilePointer(hFile, header.indexOffset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        LOG_ERROR("[DBPF] Failed to seek to index");
        CloseHandle(hFile);
        return nullptr;
    }

    // Sims 3 DBPF v2.0 has a variable-format index; first 4 bytes = index type flags indicating which fields are constant.
    uint32_t indexFlags = 0;
    if (!ReadFile(hFile, &indexFlags, 4, &bytesRead, NULL) || bytesRead != 4) {
        LOG_ERROR("[DBPF] Failed to read index flags");
        CloseHandle(hFile);
        return nullptr;
    }

    snprintf(buf, sizeof(buf), "[DBPF] Index flags: 0x%08X", indexFlags);
    LOG_INFO(buf);

    // Read constant fields if flagged
    uint32_t constType = 0, constGroup = 0, constInstHi = 0;
    if (indexFlags & 0x01) { ReadFile(hFile, &constType, 4, &bytesRead, NULL); }
    if (indexFlags & 0x02) { ReadFile(hFile, &constGroup, 4, &bytesRead, NULL); }
    if (indexFlags & 0x04) { ReadFile(hFile, &constInstHi, 4, &bytesRead, NULL); }

    // Calculate entry size based on which fields are per-entry
    int entrySize = 4;                        // instanceLo always present
    if (!(indexFlags & 0x01)) entrySize += 4; // type per-entry
    if (!(indexFlags & 0x02)) entrySize += 4; // group per-entry
    if (!(indexFlags & 0x04)) entrySize += 4; // instanceHi per-entry
    entrySize += 4 + 4 + 4 + 4;               // offset, fileSize, memSize, flags always present

    snprintf(buf, sizeof(buf), "[DBPF] Entry size: %d bytes, scanning %u entries", entrySize, header.indexEntryCount);
    LOG_INFO(buf);

    // Search for matching entry
    uint32_t targetInstLo = (uint32_t)(instance & 0xFFFFFFFF);
    uint32_t targetInstHi = (uint32_t)(instance >> 32);

    uint32_t foundOffset = 0, foundFileSize = 0, foundMemSize = 0;
    bool found = false;

    for (uint32_t i = 0; i < header.indexEntryCount && !found; i++) {
        uint32_t entryType = constType, entryGroup = constGroup, entryInstHi = constInstHi, entryInstLo;
        uint32_t entryOffset, entryFileSize, entryMemSize, entryFlags;

        if (!(indexFlags & 0x01)) { ReadFile(hFile, &entryType, 4, &bytesRead, NULL); }
        if (!(indexFlags & 0x02)) { ReadFile(hFile, &entryGroup, 4, &bytesRead, NULL); }
        if (!(indexFlags & 0x04)) { ReadFile(hFile, &entryInstHi, 4, &bytesRead, NULL); }
        ReadFile(hFile, &entryInstLo, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryOffset, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFileSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryMemSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFlags, 4, &bytesRead, NULL);

        if (entryType == type && entryInstLo == targetInstLo && entryInstHi == targetInstHi) {
            foundOffset = entryOffset;
            foundFileSize = entryFileSize;
            foundMemSize = entryMemSize;
            found = true;

            snprintf(buf, sizeof(buf), "[DBPF] Found at entry %u: type=%08X inst=%08X%08X", i, entryType, entryInstHi, entryInstLo);
            LOG_INFO(buf);
        }
    }

    if (!found) {
        snprintf(buf, sizeof(buf), "[DBPF] Resource not found: type=%08X instance=%016llX", type, instance);
        LOG_WARN(buf);
        CloseHandle(hFile);
        return nullptr;
    }

    // Extract size (lower 31 bits) and check compression.
    uint32_t compressedSize = foundFileSize & 0x7FFFFFFF;
    uint32_t decompressedSize = foundMemSize;
    bool isCompressed = (foundFileSize & 0x80000000) != 0;

    snprintf(buf, sizeof(buf), "[DBPF] Found: offset=0x%X, compSize=%u, decompSize=%u, compressed=%d", foundOffset, compressedSize, decompressedSize, isCompressed);
    LOG_INFO(buf);

    // Seek to resource data
    if (SetFilePointer(hFile, foundOffset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        LOG_ERROR("[DBPF] Failed to seek to resource data");
        CloseHandle(hFile);
        return nullptr;
    }

    // Read compressed data
    std::vector<uint8_t> compressedData(compressedSize);
    if (!ReadFile(hFile, compressedData.data(), compressedSize, &bytesRead, NULL) || bytesRead != compressedSize) {
        LOG_ERROR("[DBPF] Failed to read resource data");
        CloseHandle(hFile);
        return nullptr;
    }
    CloseHandle(hFile);

    // Check for Refpack signature (FB 10)
    if (compressedSize >= 2) {
        snprintf(buf, sizeof(buf), "[DBPF] Data header: %02X %02X %02X %02X", compressedData[0], compressedData[1], compressedSize > 2 ? compressedData[2] : 0, compressedSize > 3 ? compressedData[3] : 0);
        LOG_INFO(buf);
    }

    uint8_t* outputData = nullptr;

    // Refpack check: game masks (big_endian_word & 0x1FFF) == 0x10FB at 0x004EC021; accepts flag variants: 0x10FB, 0x50FB, 0x90FB, etc.
    uint16_t refpackMagic = compressedSize >= 2 ? (((uint16_t)compressedData[0] << 8) | compressedData[1]) : 0;
    if (isCompressed && compressedSize >= 5 && (refpackMagic & 0x1FFF) == 0x10FB) {
        // Refpack compressed - decompress using game's function
        LOG_INFO("[DBPF] Decompressing with Refpack...");
        if (!s_refpackDecompress) s_refpackDecompress = (RefpackDecompress_t)Addresses::Misc.RefPack_Decompress;

        outputData = new uint8_t[decompressedSize];
        uint32_t result = s_refpackDecompress(compressedData.data(), compressedSize, outputData, decompressedSize, 0);

        if (result == 0 || result == 0xFFFFFFFF) {
            snprintf(buf, sizeof(buf), "[DBPF] Refpack decompress failed: result=%u", result);
            LOG_ERROR(buf);
            delete[] outputData;
            return nullptr;
        }

        snprintf(buf, sizeof(buf), "[DBPF] Decompressed: %u bytes", result);
        LOG_INFO(buf);
        *outSize = result;

        // Check if decompressed data is S3SA encrypted (version byte 0x01 or 0x02)
        if (result > 0 && (outputData[0] == 0x01 || outputData[0] == 0x02)) {
            LOG_INFO("[DBPF] Decompressed data looks like S3SA, attempting decryption...");
            uint8_t* decrypted = nullptr;
            uint32_t decryptedSize = DecryptS3SA(outputData, result, &decrypted);

            if (decrypted && decryptedSize > 0) {
                delete[] outputData;
                outputData = decrypted;
                *outSize = decryptedSize;
                snprintf(buf, sizeof(buf), "[DBPF] S3SA decryption after Refpack successful: %u bytes", decryptedSize);
                LOG_INFO(buf);
            } else {
                LOG_WARN("[DBPF] S3SA decryption failed after Refpack, keeping decompressed data");
            }
        }
    } else {
        // Not Refpack compressed, try S3SA decryption directly
        LOG_INFO("[DBPF] Data not Refpack compressed, trying S3SA decryption...");

        uint8_t* decrypted = nullptr;
        uint32_t decryptedSize = DecryptS3SA(compressedData.data(), compressedSize, &decrypted);

        if (decrypted && decryptedSize > 0) {
            outputData = decrypted;
            *outSize = decryptedSize;
            snprintf(buf, sizeof(buf), "[DBPF] S3SA decryption successful: %u bytes", decryptedSize);
            LOG_INFO(buf);
        } else {
            // S3SA decryption failed - return raw data as fallback
            LOG_WARN("[DBPF] S3SA decryption failed, returning raw data");
            outputData = new uint8_t[compressedSize];
            memcpy(outputData, compressedData.data(), compressedSize);
            *outSize = compressedSize;
        }
    }

    // Verify it looks like a .NET assembly (MZ header); S3SA resources may have a small header before the actual DLL
    if (*outSize >= 2 && outputData[0] == 'M' && outputData[1] == 'Z') {
        LOG_INFO("[DBPF] SUCCESS: Data has MZ header at offset 0 (valid PE/DLL)");
    } else if (*outSize >= 64) {
        // Log first 64 bytes for analysis
        LOG_INFO("[DBPF] First 64 bytes of extracted data:");
        for (int row = 0; row < 4; row++) {
            int off = row * 16;
            snprintf(buf, sizeof(buf), "[DBPF]   +%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X", off, outputData[off + 0], outputData[off + 1], outputData[off + 2],
                outputData[off + 3], outputData[off + 4], outputData[off + 5], outputData[off + 6], outputData[off + 7], outputData[off + 8], outputData[off + 9], outputData[off + 10], outputData[off + 11],
                outputData[off + 12], outputData[off + 13], outputData[off + 14], outputData[off + 15]);
            LOG_INFO(buf);
        }

        // Scan entire buffer for MZ header (S3SA has variable-length header)
        int mzOffset = -1;
        for (uint32_t i = 0; i < *outSize - 1; i++) {
            if (outputData[i] == 'M' && outputData[i + 1] == 'Z') {
                mzOffset = i;
                break;
            }
        }

        if (mzOffset >= 0) {
            snprintf(buf, sizeof(buf), "[DBPF] Found MZ at offset %d", mzOffset);
            LOG_INFO(buf);

            // Verify it looks like a real PE header (check for PE signature)
            uint8_t* mzPtr = outputData + mzOffset;
            uint32_t peOffset = *(uint32_t*)(mzPtr + 0x3C); // DOS header -> PE offset
            snprintf(buf, sizeof(buf), "[DBPF] DOS header PE offset: 0x%X", peOffset);
            LOG_INFO(buf);

            if (mzOffset + peOffset + 4 < *outSize) {
                uint8_t* pePtr = mzPtr + peOffset;
                snprintf(buf, sizeof(buf), "[DBPF] PE signature check: %02X %02X %02X %02X (expect 50 45 00 00)", pePtr[0], pePtr[1], pePtr[2], pePtr[3]);
                LOG_INFO(buf);

                if (pePtr[0] == 'P' && pePtr[1] == 'E' && pePtr[2] == 0 && pePtr[3] == 0) {
                    // Valid PE - extract it
                    uint32_t dllSize = *outSize - mzOffset;
                    uint8_t* dllData = new uint8_t[dllSize];
                    memcpy(dllData, mzPtr, dllSize);
                    delete[] outputData;
                    outputData = dllData;
                    *outSize = dllSize;

                    LOG_INFO("[DBPF] SUCCESS: Valid PE/DLL extracted, " + std::to_string(dllSize) + " bytes");
                } else {
                    LOG_WARN("[DBPF] MZ found but no valid PE signature - false positive or corrupted");
                }
            } else {
                LOG_WARN("[DBPF] MZ found but PE offset out of bounds");
            }
        } else {
            LOG_WARN("[DBPF] No MZ header found in entire buffer - not a PE/DLL or different format");
        }
    }

    return outputData;
}

// PE/CLI METADATA READING - Read assembly name directly from PE/CLI metadata without Mono, needed for proactive extraction where we don't want to load as assembly.

// PE/CLI header structures
#pragma pack(push, 1)
struct IMAGE_DOS_HEADER_MINI {
    uint16_t e_magic;    // MZ
    uint8_t padding[58]; // Skip to e_lfanew
    int32_t e_lfanew;    // Offset to PE header
};

struct IMAGE_FILE_HEADER_MINI {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

struct IMAGE_OPTIONAL_HEADER32_MINI {
    uint16_t Magic;    // 0x10B = PE32
    uint8_t skip1[58]; // Skip to DataDirectory offset
    uint32_t NumberOfRvaAndSizes;
    // DataDirectory[0] = Export, [1] = Import, ... [14] = CLI Header
    struct {
        uint32_t VirtualAddress;
        uint32_t Size;
    } DataDirectory[16];
};

struct CLI_HEADER {
    uint32_t cb; // Size of header
    uint16_t MajorRuntimeVersion;
    uint16_t MinorRuntimeVersion;
    uint32_t MetaDataRVA; // RVA of metadata
    uint32_t MetaDataSize;
    uint32_t Flags;
    uint32_t EntryPointToken;
    uint32_t ResourcesRVA;
    uint32_t ResourcesSize;
    // ... more fields we don't care about
};

struct METADATA_ROOT {
    uint32_t Signature; // 0x424A5342 "BSJB"
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Reserved;
    uint32_t VersionLength; // Length of version string (rounded to 4)
    // char VersionString[VersionLength] follows, then uint16_t Flags, uint16_t StreamCount, then stream headers.
};

struct STREAM_HEADER {
    uint32_t Offset; // Relative to metadata root
    uint32_t Size;
};
#pragma pack(pop)

// Convert RVA to file offset using section table
static uint32_t RvaToFileOffset(const uint8_t* peBase, uint32_t rva) {
    const IMAGE_DOS_HEADER_MINI* dosHeader = (const IMAGE_DOS_HEADER_MINI*)peBase;
    const uint8_t* pePtr = peBase + dosHeader->e_lfanew;

    // Skip PE signature (4 bytes)
    const IMAGE_FILE_HEADER_MINI* fileHeader = (const IMAGE_FILE_HEADER_MINI*)(pePtr + 4);

    // Sections start after optional header
    const uint8_t* sectionsBase = pePtr + 4 + sizeof(IMAGE_FILE_HEADER_MINI) + fileHeader->SizeOfOptionalHeader;

    // Each section header is 40 bytes: name[8], VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData, etc.
    for (int i = 0; i < fileHeader->NumberOfSections; i++) {
        const uint8_t* sectionPtr = sectionsBase + (i * 40);
        uint32_t virtualAddress = *(uint32_t*)(sectionPtr + 12);
        uint32_t sizeOfRawData = *(uint32_t*)(sectionPtr + 16);
        uint32_t pointerToRawData = *(uint32_t*)(sectionPtr + 20);

        if (rva >= virtualAddress && rva < virtualAddress + sizeOfRawData) { return pointerToRawData + (rva - virtualAddress); }
    }
    return 0; // Not found
}

// Read assembly name from PE/CLI metadata; returns empty string on failure.
std::string ReadAssemblyNameFromPE(const uint8_t* data, uint32_t size) {
    if (!data || size < 128) return "";

    // Check MZ header
    if (data[0] != 'M' || data[1] != 'Z') return "";

    const IMAGE_DOS_HEADER_MINI* dosHeader = (const IMAGE_DOS_HEADER_MINI*)data;
    if (dosHeader->e_lfanew <= 0 || (uint32_t)dosHeader->e_lfanew + 128 > size) return "";

    // Check PE signature
    const uint8_t* pePtr = data + dosHeader->e_lfanew;
    if (pePtr[0] != 'P' || pePtr[1] != 'E' || pePtr[2] != 0 || pePtr[3] != 0) return "";

    // Get optional header (file header at pePtr+4 is skipped over via its size below)
    const IMAGE_OPTIONAL_HEADER32_MINI* optHeader = (const IMAGE_OPTIONAL_HEADER32_MINI*)(pePtr + 4 + sizeof(IMAGE_FILE_HEADER_MINI));

    // Check for PE32 (0x10B) - Sims 3 is 32-bit
    if (optHeader->Magic != 0x10B) return "";

    // Get CLI header from DataDirectory[14]
    if (optHeader->NumberOfRvaAndSizes < 15) return "";
    uint32_t cliRva = optHeader->DataDirectory[14].VirtualAddress;
    uint32_t cliSize = optHeader->DataDirectory[14].Size;
    if (cliRva == 0 || cliSize == 0) return ""; // Not a .NET assembly

    // Convert CLI RVA to file offset
    uint32_t cliOffset = RvaToFileOffset(data, cliRva);
    if (cliOffset == 0 || cliOffset + sizeof(CLI_HEADER) > size) return "";

    const CLI_HEADER* cliHeader = (const CLI_HEADER*)(data + cliOffset);

    // Get metadata root
    uint32_t metadataOffset = RvaToFileOffset(data, cliHeader->MetaDataRVA);
    if (metadataOffset == 0 || metadataOffset + 32 > size) return "";

    const METADATA_ROOT* metaRoot = (const METADATA_ROOT*)(data + metadataOffset);

    // Check BSJB signature
    if (metaRoot->Signature != 0x424A5342) return ""; // "BSJB"

    // Skip version string (length is rounded to 4-byte boundary)
    uint32_t versionLen = metaRoot->VersionLength;
    if (versionLen > 256) return ""; // Sanity check

    const uint8_t* afterVersion = (const uint8_t*)metaRoot + 16 + versionLen;
    if (afterVersion + 4 > data + size) return "";

    // Read stream count
    // uint16_t flags = *(uint16_t*)afterVersion; // Not needed
    uint16_t streamCount = *(uint16_t*)(afterVersion + 2);
    if (streamCount > 10) return ""; // Sanity check

    // Parse stream headers to find #Strings and #~ (or #-)
    const uint8_t* streamPtr = afterVersion + 4;
    uint32_t stringsOffset = 0, stringsSize = 0;
    uint32_t tablesOffset = 0, tablesSize = 0;

    for (int i = 0; i < streamCount && streamPtr + 8 < data + size; i++) {
        uint32_t offset = *(uint32_t*)streamPtr;
        uint32_t streamSize = *(uint32_t*)(streamPtr + 4);
        const char* name = (const char*)(streamPtr + 8);

        // Stream name is null-terminated, padded to 4 bytes
        size_t nameLen = strlen(name);
        size_t paddedLen = ((nameLen + 1) + 3) & ~3;

        if (strcmp(name, "#Strings") == 0) {
            stringsOffset = offset;
            stringsSize = streamSize;
        } else if (strcmp(name, "#~") == 0 || strcmp(name, "#-") == 0) {
            tablesOffset = offset;
            tablesSize = streamSize;
        }

        streamPtr += 8 + paddedLen;
    }

    if (stringsOffset == 0 || tablesOffset == 0) return "";

    // Get absolute offsets
    uint32_t stringsAbsolute = metadataOffset + stringsOffset;
    uint32_t tablesAbsolute = metadataOffset + tablesOffset;

    if (stringsAbsolute >= size || tablesAbsolute + 32 >= size) return "";

    // Parse #~ header to find Assembly table
    const uint8_t* tablesPtr = data + tablesAbsolute;

    // #~ header layout: 4 reserved, 1 major, 1 minor, 1 heapSizes, 1 reserved, 8 valid, 8 sorted.
    // heapSizes / sortedTables / heap-index widths are unused by the heuristic #Strings scan below:
    // uint8_t heapSizes = tablesPtr[6];        // bit0=wide #Strings, bit1=wide #GUID, bit2=wide #Blob
    // uint64_t sortedTables = *(uint64_t*)(tablesPtr + 16);
    uint64_t validTables = *(uint64_t*)(tablesPtr + 8);

    // Table 0x20 (32) is Assembly table
    // We need to count rows in all tables before it
    const uint8_t* rowCountsPtr = tablesPtr + 24;

    // Count tables present before Assembly (bit 0x20 = table 32)
    int tablesBeforeAssembly = 0;
    for (int t = 0; t < 32; t++) {
        if (validTables & (1ULL << t)) tablesBeforeAssembly++;
    }

    // Check if Assembly table exists
    if (!(validTables & (1ULL << 32))) return "";

    // Read row counts for all tables to find Assembly table offset (each row count is 4 bytes).
    // Skip row counts for tables before Assembly
    const uint8_t* currentPtr = rowCountsPtr;
    for (int t = 0; t < 64; t++) {
        if (validTables & (1ULL << t)) {
            if (t < 32) {
                currentPtr += 4; // Skip row count
            } else {
                break;
            }
        }
    }

    // Get Assembly row count
    uint32_t assemblyRowCount = *(uint32_t*)currentPtr;
    if (assemblyRowCount == 0) return "";

    // Scan #Strings for common assembly names
    const char* stringsHeap = (const char*)(data + stringsAbsolute);

    // First entry is always empty string; skip it and scan for valid assembly names.
    size_t pos = 1;                           // Skip null terminator of empty string
    while (pos < stringsSize && pos < 4096) { // Don't scan too far
        const char* str = stringsHeap + pos;
        size_t len = strnlen(str, stringsSize - pos);

        // Assembly names are typically: "ScriptCore", "SimIFace", "mscorlib", etc.
        // They don't contain spaces, slashes, or other special chars, and they're reasonable length (not paths or long descriptions)
        // This will probably end up biting me but oh well
        if (len > 0 && len < 128) {
            bool validName = true;
            bool hasLetter = false;
            for (size_t i = 0; i < len && validName; i++) {
                char c = str[i];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    hasLetter = true;
                else if (c >= '0' && c <= '9') {
                } else if (c == '_' || c == '.') {
                } else
                    validName = false;
            }

            // Assembly name must have letters, not start with number
            if (validName && hasLetter && (str[0] < '0' || str[0] > '9')) {
                // Found a plausible assembly name; return the first valid one (usually the assembly name).
                return std::string(str);
            }
        }

        pos += len + 1; // Move past null terminator
    }

    return "";
}

} // namespace S3MM
