#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <map>
#include <set>

// Lightmap Probe v0.3 - Parse BSM texture export serial data for BulkData headers
// The BSM file (BioShock Remastered) is uncompressed at 214MB.
// Texture exports with HasBeenStripped=true have their mip data in .PackagePatch.
// After tagged properties end ([None] sentinel), native texture serialization contains
// FByteBulkData headers for each mip level.

constexpr uint32_t UE_MAGIC = 0x9E2A83C1;

// Read compact index (BioShock/UE2 variable-length int)
int ReadCI(const uint8_t* data, size_t maxLen, size_t& bytesRead) {
    bytesRead = 0;
    if (maxLen == 0) return 0;
    uint8_t b0 = data[bytesRead++];
    bool negative = (b0 & 0x80) != 0;
    bool more = (b0 & 0x40) != 0;
    int result = b0 & 0x3F;
    int shift = 6;
    while (more && bytesRead < maxLen) {
        uint8_t b = data[bytesRead++];
        more = (b & 0x80) != 0;
        result |= (b & 0x7F) << shift;
        shift += 7;
    }
    return negative ? -result : result;
}

struct ExportEntry {
    std::string className;
    std::string objectName;
    int32_t outerIndex;
    int32_t serialSize;
    int32_t serialOffset;
};

int main(int argc, char* argv[]) {
    printf("Lightmap Probe v0.3 - BSM Texture BulkData Header Parser\n\n");
    
    std::string bsmPath = "D:\\SteamLibrary\\steamapps\\common\\BioShock Remastered\\ContentBaked\\pc\\Maps\\1-Medical.bsm";
    if (argc > 1) bsmPath = argv[1];
    
    std::string patchPath = bsmPath + ".PackagePatch";
    
    if (!std::filesystem::exists(bsmPath)) {
        printf("BSM not found: %s\n", bsmPath.c_str());
        return 1;
    }
    if (!std::filesystem::exists(patchPath)) {
        printf("PackagePatch not found: %s\n", patchPath.c_str());
        return 1;
    }
    
    int64_t bsmSize = std::filesystem::file_size(bsmPath);
    int64_t ppSize = std::filesystem::file_size(patchPath);
    printf("BSM: %lld bytes\n", (long long)bsmSize);
    printf("PackagePatch: %lld bytes\n\n", (long long)ppSize);
    
    // Load BSM into memory
    std::vector<uint8_t> bsm(bsmSize);
    {
        FILE* f = fopen(bsmPath.c_str(), "rb");
        fread(bsm.data(), 1, bsmSize, f);
        fclose(f);
    }
    
    uint8_t* d = bsm.data();
    if (*(uint32_t*)d != UE_MAGIC) {
        printf("Not a valid UE package!\n");
        return 1;
    }
    
    // Parse header
    int32_t nameCount = *(int32_t*)(d + 12);
    int32_t nameOff   = *(int32_t*)(d + 16);
    int32_t expCount  = *(int32_t*)(d + 20);
    int32_t expOff    = *(int32_t*)(d + 24);
    int32_t impCount  = *(int32_t*)(d + 28);
    int32_t impOff    = *(int32_t*)(d + 32);
    printf("Names: %d, Exports: %d, Imports: %d\n", nameCount, expCount, impCount);
    
    // Parse name table
    std::vector<std::string> names;
    {
        size_t pos = nameOff;
        for (int i = 0; i < nameCount && pos < (size_t)bsmSize; i++) {
            size_t br;
            int rawLen = ReadCI(d + pos, bsmSize - pos, br); pos += br;
            int charCount = (rawLen < 0) ? -rawLen : rawLen;
            if (charCount <= 0 || charCount > 65536) break;
            std::string name;
            for (int c = 0; c < charCount && pos + 1 < (size_t)bsmSize; c++) {
                uint16_t wc = *(uint16_t*)(d + pos); pos += 2;
                if (wc == 0) break;
                name += (wc < 128) ? (char)wc : '?';
            }
            if (pos + 1 < (size_t)bsmSize && *(uint16_t*)(d + pos) == 0) pos += 2;
            pos += 8; // QWORD flags
            names.push_back(name);
        }
    }
    printf("Parsed %d names\n", (int)names.size());
    
    // Find "None" name index
    int noneIdx = -1;
    for (int i = 0; i < (int)names.size(); i++) {
        if (names[i] == "None") { noneIdx = i; break; }
    }
    
    // Parse import table (just class/object names)
    std::vector<std::string> importNames;
    {
        size_t pos = impOff;
        for (int i = 0; i < impCount && pos < (size_t)bsmSize; i++) {
            size_t br;
            ReadCI(d + pos, bsmSize - pos, br); pos += br; pos += 4;
            int clsIdx = ReadCI(d + pos, bsmSize - pos, br); pos += br; pos += 4;
            pos += 4; // outer
            int objIdx = ReadCI(d + pos, bsmSize - pos, br); pos += br; pos += 4;
            std::string objName = (objIdx >= 0 && objIdx < (int)names.size()) ? names[objIdx] : "?";
            importNames.push_back(objName);
        }
    }
    
    // Parse export table
    std::vector<ExportEntry> exports;
    {
        size_t pos = expOff;
        for (int i = 0; i < expCount && pos < (size_t)bsmSize; i++) {
            size_t br;
            int classIdx = ReadCI(d + pos, bsmSize - pos, br); pos += br;
            ReadCI(d + pos, bsmSize - pos, br); pos += br; // super
            int32_t outerIdx = *(int32_t*)(d + pos); pos += 4;
            pos += 4; // unknownBS1
            int objNameIdx = ReadCI(d + pos, bsmSize - pos, br); pos += br;
            int32_t objNameNum = *(int32_t*)(d + pos) - 1; pos += 4;
            pos += 8; // objectFlags
            int serialSize = ReadCI(d + pos, bsmSize - pos, br); pos += br;
            int serialOffset = 0;
            if (serialSize > 0) {
                serialOffset = ReadCI(d + pos, bsmSize - pos, br); pos += br;
            }
            pos += 4; // unknownBS2
            
            std::string className;
            if (classIdx == 0) className = "Class";
            else if (classIdx < 0) {
                int idx = -classIdx - 1;
                className = (idx < (int)importNames.size()) ? importNames[idx] : "?";
            } else className = "Export";
            
            std::string objName = (objNameIdx >= 0 && objNameIdx < (int)names.size()) ? names[objNameIdx] : "?";
            if (objNameNum >= 0) objName += "_" + std::to_string(objNameNum);
            
            exports.push_back({className, objName, outerIdx, serialSize, serialOffset});
        }
    }
    printf("Parsed %d exports\n\n", (int)exports.size());
    
    // Search for lightmap-related names
    printf("=== Searching for LightMap/Shadow names ===\n");
    for (int i = 0; i < (int)names.size(); i++) {
        if (names[i].find("Light") != std::string::npos || names[i].find("Shadow") != std::string::npos ||
            names[i].find("light") != std::string::npos || names[i].find("shadow") != std::string::npos) {
            printf("  Name[%d] = '%s'\n", i, names[i].c_str());
        }
    }
    printf("  Import classes with Light/Shadow:\n");
    for (int i = 0; i < (int)importNames.size(); i++) {
        if (importNames[i].find("Light") != std::string::npos || importNames[i].find("Shadow") != std::string::npos)
            printf("    Import[%d] = '%s'\n", i, importNames[i].c_str());
    }
    
    // Find ALL texture-class exports and count by class
    printf("\n=== All Texture-class exports (summary) ===\n");
    std::map<std::string, int> texClassCount;
    int texTotal = 0;
    for (auto& e : exports) {
        if (e.className.find("Texture") != std::string::npos || 
            e.className.find("Shadow") != std::string::npos ||
            e.className.find("Bitmap") != std::string::npos) {
            texClassCount[e.className]++;
            texTotal++;
        }
    }
    for (auto& [cls, cnt] : texClassCount) printf("  %s: %d exports\n", cls.c_str(), cnt);
    printf("  Total texture-like exports: %d\n", texTotal);
    
    // Find the LightMaps_BSP group export
    int lightmapGroupIdx = -1;
    for (int i = 0; i < (int)exports.size(); i++) {
        if (exports[i].objectName == "LightMaps_BSP") {
            lightmapGroupIdx = i + 1; // export refs are 1-based
            printf("\n=== LightMaps_BSP found at export[%d] ===\n", i);
            break;
        }
    }
    
    // Find Texture exports whose outer is LightMaps_BSP
    printf("\n=== Lightmap Texture exports (outer=LightMaps_BSP) ===\n");
    std::vector<int> textureExports;
    for (int i = 0; i < (int)exports.size(); i++) {
        auto& e = exports[i];
        if (e.className == "Texture" && e.outerIndex == lightmapGroupIdx) {
            printf("  Export[%d] '%s' serial=%d offset=0x%X\n", 
                   i, e.objectName.c_str(), e.serialSize, e.serialOffset);
            textureExports.push_back(i);
        }
    }
    if (textureExports.empty() && lightmapGroupIdx > 0) {
        // Maybe lightmaps are nested deeper — check textures whose outer's outer is LightMaps_BSP
        printf("  (none found directly, checking nested...)\n");
        for (int i = 0; i < (int)exports.size(); i++) {
            auto& e = exports[i];
            if (e.className == "Texture" && e.outerIndex > 0) {
                int parentIdx = e.outerIndex - 1;
                if (parentIdx < (int)exports.size() && exports[parentIdx].outerIndex == lightmapGroupIdx) {
                    printf("  Export[%d] '%s' (via '%s') serial=%d offset=0x%X\n",
                           i, e.objectName.c_str(), exports[parentIdx].objectName.c_str(),
                           e.serialSize, e.serialOffset);
                    textureExports.push_back(i);
                }
            }
        }
    }
    printf("Found %d lightmap texture exports\n\n", (int)textureExports.size());
    
    // For each lightmap texture, parse serial data to find bulk data headers
    // Format after properties: native mip serialization
    // Skip tagged properties by looking for "None" sentinel
    printf("=== Parsing texture serial data for BulkData headers ===\n\n");
    
    for (int texIdx : textureExports) {
        auto& e = exports[texIdx];
        printf("--- %s (export %d, serial %d bytes at 0x%X) ---\n", 
               e.objectName.c_str(), texIdx, e.serialSize, e.serialOffset);
        
        if (e.serialOffset + e.serialSize > (int)bsmSize) {
            printf("  ERROR: serial data out of bounds\n");
            continue;
        }
        
        const uint8_t* serial = d + e.serialOffset;
        int slen = e.serialSize;
        
        // Skip the 8-byte Vengeance per-object header
        int pos = 8;
        
        // Read tagged properties - capture key values
        int propCount = 0;
        int texFormat = -1, texUSize = -1, texVSize = -1;
        int strippedMips = -1, hasBeenStripped = -1;
        int64_t cachedBulkSize = -1;
        int texUClamp = -1, texVClamp = -1, lodSet = -1;
        bool isFirstTex = (texIdx == textureExports[0]);
        
        while (pos < slen - 5) {
            size_t br;
            int nameIdx = ReadCI(serial + pos, slen - pos, br); pos += (int)br;
            int32_t nameNum = *(int32_t*)(serial + pos); pos += 4;
            
            // Check for None sentinel
            if (nameIdx == noneIdx || (nameIdx >= 0 && nameIdx < (int)names.size() && names[nameIdx] == "None")) {
                break;
            }
            
            if (nameIdx < 0 || nameIdx >= (int)names.size()) {
                pos = slen;
                break;
            }
            
            std::string propName = names[nameIdx];
            
            // Read info byte
            if (pos >= slen) break;
            uint8_t info = serial[pos++];
            int type = info & 0x0F;
            int sizeBits = (info >> 4) & 0x07;
            int arrayFlag = (info >> 7) & 1;
            
            // Struct name ref
            if (type == 10) {
                ReadCI(serial + pos, slen - pos, br); pos += (int)br;
                pos += 4;
            }
            
            // Decode property size
            int propSize = 0;
            switch (sizeBits) {
                case 0: propSize = 1; break;
                case 1: propSize = 2; break;
                case 2: propSize = 4; break;
                case 3: propSize = 12; break;
                case 4: propSize = 16; break;
                case 5: propSize = serial[pos++]; break;
                case 6: { uint16_t v; memcpy(&v, serial+pos, 2); pos += 2; propSize = v; } break;
                case 7: { uint32_t v; memcpy(&v, serial+pos, 4); pos += 4; propSize = (int)v; } break;
            }
            
            int valuePos = pos; // save position before array index
            
            // Bool has no payload
            if (type == 3) {
                // Bool value is in the info byte's array flag
                if (propName == "HasBeenStripped") hasBeenStripped = arrayFlag;
            } else {
                if (arrayFlag) {
                    uint8_t b = serial[pos++];
                    if ((b & 0xC0) == 0x80) pos++;
                    else if ((b & 0xC0) == 0xC0) pos += 3;
                    valuePos = pos;
                }
                // Capture values
                if (propName == "Format" && propSize >= 1) {
                    texFormat = serial[valuePos];
                } else if (propName == "USize" && propSize >= 4) {
                    texUSize = *(int32_t*)(serial + valuePos);
                } else if (propName == "VSize" && propSize >= 4) {
                    texVSize = *(int32_t*)(serial + valuePos);
                } else if (propName == "UClampMode" && propSize >= 1) {
                    texUClamp = serial[valuePos];
                } else if (propName == "VClampMode" && propSize >= 1) {
                    texVClamp = serial[valuePos];
                } else if (propName == "LODSet" && propSize >= 1) {
                    lodSet = serial[valuePos];
                } else if (propName == "StrippedNumMips" && propSize >= 1) {
                    strippedMips = serial[valuePos];
                } else if (propName == "CachedBulkDataSize" && propSize >= 8) {
                    cachedBulkSize = *(int64_t*)(serial + valuePos);
                }
                // Dump ALL properties for the first lightmap
                if (isFirstTex) {
                    printf("    PROP '%s' type=%d size=%d val=", propName.c_str(), type, propSize);
                    if (type == 1 && propSize == 1) printf("%d", serial[valuePos]);
                    else if (type == 1 && propSize == 4) printf("%d", *(int32_t*)(serial+valuePos));
                    else if (type == 4 && propSize == 4) printf("%.3f", *(float*)(serial+valuePos));
                    else if (type == 5 && propSize >= 4) printf("ref=%d", *(int32_t*)(serial+valuePos));
                    else { for(int b=0;b<std::min(propSize,16);b++) printf("%02X ",serial[valuePos+b]); }
                    printf("\n");
                }
                pos += propSize;
            }
            propCount++;
        }
        
        printf("  Properties: %d, ended at offset %d/%d\n", propCount, pos, slen);
        printf("  Format=%d USize=%d VSize=%d StrippedMips=%d Stripped=%d BulkSize=%lld UClamp=%d VClamp=%d LODSet=%d\n",
               texFormat, texUSize, texVSize, strippedMips, hasBeenStripped, (long long)cachedBulkSize,
               texUClamp, texVClamp, lodSet);
        
        // Now we're past the None sentinel — native texture data follows
        int remaining = slen - pos;
        printf("  Native data: %d bytes remaining\n", remaining);
        
        if (remaining < 16) {
            printf("  Too few bytes for bulk data headers\n");
            continue;
        }
        
        // Parse native header
        int32_t nativeVer = *(int32_t*)(serial + pos);
        int32_t nativeSubVer = *(int32_t*)(serial + pos + 4);
        int64_t totalBulkSize = *(int64_t*)(serial + pos + 8);
        uint8_t mipCount = serial[pos + 16];
        printf("  NativeHeader: ver=%d subVer=%d totalBulkSize=%lld mipCount=%d\n",
               nativeVer, nativeSubVer, (long long)totalBulkSize, mipCount);
        
        // Dump bytes around the entry 3/4/5 boundary (bytes 100-220)
        printf("  Native bytes 100-220 (entry 3/4 boundary):\n");
        for (int i = 100; i < 220 && i < remaining; i += 16) {
            printf("    +%04X: ", i);
            for (int j = 0; j < 16 && i+j < remaining; j++)
                printf("%02X ", serial[pos + i + j]);
            printf("\n");
        }
        
        // Better parsing: use variable-length entries
        // Format per mip: [30-byte header] [CI dataSize] [if dataSize>0: pixels]
        printf("  Parsing mip entries (variable-length):\n");
        int mipPos = 17; // after 8+8+1 = 17 byte native prefix
        for (int m = 0; m < mipCount && mipPos < remaining - 16; m++) {
            // Check signature
            int32_t sig0 = *(int32_t*)(serial + pos + mipPos);
            int32_t sig1 = *(int32_t*)(serial + pos + mipPos + 4);
            if (sig0 != 4 || sig1 != 2) {
                printf("    [%d] BAD signature at +%d: %d, %d\n", m, mipPos, sig0, sig1);
                break;
            }
            int64_t selfOff = *(int64_t*)(serial + pos + mipPos + 8);
            uint8_t b16 = serial[pos + mipPos + 16];
            uint8_t b17 = serial[pos + mipPos + 17];
            
            // Try reading bytes 18-29 as raw for analysis
            printf("    [%d] +%d: selfOff=0x%llX b16-17=%02X%02X raw[18..31]: ",
                   m, mipPos, (long long)selfOff, b16, b17);
            for (int b = 18; b < 32 && mipPos + b < remaining; b++)
                printf("%02X ", serial[pos + mipPos + b]);
            printf("\n");
            
            // Advance past the 30-byte header portion + read CI
            // Try: header = bytes 0-29, then CI at byte 30
            int ciPos = mipPos + 30;
            if (ciPos >= remaining) break;
            size_t ciBr;
            int ciVal = ReadCI(serial + pos + ciPos, remaining - ciPos, ciBr);
            printf("         CI at +%d = %d (%d bytes)\n", ciPos, ciVal, (int)ciBr);
            
            // If CI > 0, pixel data follows
            int hdrEnd = ciPos + (int)ciBr;
            if (ciVal > 0 && hdrEnd + ciVal <= remaining) {
                printf("         INLINE data: %d bytes at +%d\n", ciVal, hdrEnd);
                mipPos = hdrEnd + ciVal;
            } else {
                // No inline data (stripped)
                mipPos = hdrEnd;
            }
        }
        printf("  Final mip parse position: +%d (of %d)\n\n", mipPos, remaining);
        
    }
    
    // ===== Now try to extract DXT1 data from PackagePatch =====
    printf("\n=== PackagePatch Analysis ===\n");
    printf("PackagePatch size: %lld bytes\n", (long long)ppSize);
    
    // Load PackagePatch
    std::vector<uint8_t> pp(ppSize);
    {
        FILE* f = fopen(patchPath.c_str(), "rb");
        fread(pp.data(), 1, ppSize, f);
        fclose(f);
    }
    
    // Parse header
    int32_t ppVersion = *(int32_t*)(pp.data() + 0);
    int32_t ppDataOffset = *(int32_t*)(pp.data() + 4);
    int32_t ppNumPatches = *(int32_t*)(pp.data() + 8);
    int32_t ppDataOffset2 = *(int32_t*)(pp.data() + 12);
    printf("  Version: %d\n", ppVersion);
    printf("  Data offset: %d (0x%X)\n", ppDataOffset, ppDataOffset);
    printf("  Num patches: %d\n", ppNumPatches);
    printf("  Data offset2: %d (0x%X)\n", ppDataOffset2, ppDataOffset2);
    printf("  Bulk data region: %lld bytes (from offset %d to EOF)\n",
           (long long)(ppSize - ppDataOffset), ppDataOffset);
    
    // Try writing first 524288 bytes from bulk data start as a DDS file
    // DDS header for DXT1 1024x1024
    int64_t bulkStart = ppDataOffset;
    int mip0Size = 524288; // DXT1 1024x1024
    
    if (bulkStart + mip0Size <= ppSize) {
        // Check entropy of first 4KB at bulk start
        double entropy = 0;
        int freq[256] = {};
        for (int i = 0; i < 4096; i++) freq[pp[bulkStart + i]]++;
        for (int i = 0; i < 256; i++) {
            if (freq[i] == 0) continue;
            double p = freq[i] / 4096.0;
            entropy -= p * log2(p);
        }
        printf("\n  Entropy at bulk start (0x%X): %.3f", (int)bulkStart, entropy);
        if (entropy > 7.0) printf(" [HIGH - likely compressed/pixel data]");
        else if (entropy > 5.0) printf(" [MEDIUM - structured data with variation]");
        else printf(" [LOW - structured/repetitive]");
        printf("\n");
        
        // Show first 64 bytes as DXT1 blocks
        printf("  First 8 DXT1 blocks at bulk start:\n");
        for (int i = 0; i < 64; i += 8) {
            uint16_t c0 = *(uint16_t*)(pp.data() + bulkStart + i);
            uint16_t c1 = *(uint16_t*)(pp.data() + bulkStart + i + 2);
            uint32_t bits = *(uint32_t*)(pp.data() + bulkStart + i + 4);
            printf("    Block %d: c0=%04X c1=%04X bits=%08X", i/8, c0, c1, bits);
            // Check if this looks like valid DXT1 (colors should be nonzero, bits should vary)
            if (c0 == 0 && c1 == 0 && bits == 0) printf(" [ALL ZERO]");
            printf("\n");
        }
        
        // Write DDS file for first candidate texture
        std::string ddsPath = "tools/lightmap_probe/lightmap_test_offset0.dds";
        FILE* dds = fopen(ddsPath.c_str(), "wb");
        if (dds) {
            // DDS header (128 bytes)
            uint8_t header[128] = {};
            *(uint32_t*)(header + 0) = 0x20534444; // "DDS "
            *(uint32_t*)(header + 4) = 124; // header size
            *(uint32_t*)(header + 8) = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000; // flags: CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE
            *(uint32_t*)(header + 12) = 1024; // height
            *(uint32_t*)(header + 16) = 1024; // width
            *(uint32_t*)(header + 20) = mip0Size; // linear size
            *(uint32_t*)(header + 28) = 1; // mip count
            // Pixel format at offset 76
            *(uint32_t*)(header + 76) = 32; // PF size
            *(uint32_t*)(header + 80) = 0x4; // DDPF_FOURCC
            *(uint32_t*)(header + 84) = 0x31545844; // "DXT1"
            // Caps at offset 108
            *(uint32_t*)(header + 108) = 0x1000; // DDSCAPS_TEXTURE
            
            fwrite(header, 1, 128, dds);
            fwrite(pp.data() + bulkStart, 1, mip0Size, dds);
            fclose(dds);
            printf("\n  Wrote DDS: %s (524288 bytes from offset %d)\n", ddsPath.c_str(), (int)bulkStart);
        }
        
    }
    
    // ===== Parse PackagePatch structure properly =====
    // First: raw hex dump from offset 0 to understand format
    printf("\n=== PackagePatch raw hex dump (first 512 bytes) ===\n");
    for (int i = 0; i < 512 && i < ppSize; i += 16) {
        printf("  %04X: ", i);
        for (int j = 0; j < 16 && i + j < ppSize; j++)
            printf("%02X ", pp[i + j]);
        for (int j = std::min((int)ppSize - i, 16); j < 16; j++) printf("   ");
        printf(" |");
        for (int j = 0; j < 16 && i + j < ppSize; j++) {
            char c = pp[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
    
    // Step 1: Parse name table
    // Format: BYTE charCount(incl null) + UTF16[charCount] + QWORD flags + BYTE(01) + INT32(nameIdx)
    printf("\n=== Parsing PackagePatch name table ===\n");
    int ppPos = 16; // after 16-byte header
    std::vector<std::string> ppNames;
    for (int i = 0; i < ppNumPatches && ppPos < ppSize - 14; i++) {
        uint8_t charCount = pp[ppPos]; ppPos++;
        if (charCount == 0 || charCount > 200) {
            printf("  [%d] END: bad charCount=%d at pos=0x%X\n", i, charCount, ppPos-1);
            ppPos--;
            break;
        }
        std::string name;
        for (int c = 0; c < charCount; c++) {
            uint16_t ch = *(uint16_t*)(pp.data() + ppPos); ppPos += 2;
            if (ch == 0) continue; // null terminator char
            if (ch < 128) name += (char)ch; else name += '?';
        }
        // QWORD flags (8 bytes)
        uint64_t flags = *(uint64_t*)(pp.data() + ppPos); ppPos += 8;
        // BYTE + INT32 suffix (5 bytes)
        uint8_t suffixByte = pp[ppPos]; ppPos++;
        int32_t nameIdx = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        
        ppNames.push_back(name);
        printf("  [%2d] \"%s\" flags=0x%llX idx=%d (end=0x%X)\n",
               i, name.c_str(), (unsigned long long)flags, nameIdx, ppPos);
    }
    printf("  Name table ended at offset 0x%X (%d), parsed %d names\n", ppPos, ppPos, (int)ppNames.size());
    
    // Step 2: Parse export patch section
    // Header: INT32(0) + INT32(exportCount=49140) + INT32(numPatches=20) + INT32(exportCount)
    printf("\n=== Export Patch Section at 0x%X ===\n", ppPos);
    int32_t expHdr0 = *(int32_t*)(pp.data() + ppPos);
    int32_t expHdr1 = *(int32_t*)(pp.data() + ppPos + 4);
    int32_t numExpPatches = *(int32_t*)(pp.data() + ppPos + 8);
    int32_t expHdr3 = *(int32_t*)(pp.data() + ppPos + 12);
    printf("  Header: %d, %d, numPatches=%d, %d\n", expHdr0, expHdr1, numExpPatches, expHdr3);
    ppPos += 16;
    
    // Parse BSM-style export table entries
    // Format: CI classRef, CI superRef, INT32 package, CI nameRef, INT32 nameNum,
    //         INT32 archetype, INT32 flags, INT32 serialSize, INT32 serialOffset, extras...
    printf("  Parsing %d export patch entries starting at 0x%X:\n", numExpPatches, ppPos);
    
    struct PPExport {
        int classRef, superRef, nameRef;
        int32_t package, nameNum, archetype, flags, serialSize, serialOffset;
    };
    std::vector<PPExport> ppExports;
    
    for (int i = 0; i < numExpPatches && ppPos < ppSize - 20; i++) {
        PPExport ex = {};
        size_t br;
        ex.classRef = ReadCI(pp.data() + ppPos, ppSize - ppPos, br); ppPos += (int)br;
        ex.superRef = ReadCI(pp.data() + ppPos, ppSize - ppPos, br); ppPos += (int)br;
        ex.package = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        ex.nameRef = ReadCI(pp.data() + ppPos, ppSize - ppPos, br); ppPos += (int)br;
        ex.nameNum = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        ex.archetype = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        ex.flags = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        ex.serialSize = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        ex.serialOffset = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        
        // BioShock extra fields: try to read export flags + new export index
        int32_t extra1 = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        int32_t extra2 = *(int32_t*)(pp.data() + ppPos); ppPos += 4;
        // CI for generation info
        int genCI = ReadCI(pp.data() + ppPos, ppSize - ppPos, br); ppPos += (int)br;
        
        ppExports.push_back(ex);
        
        // Resolve name if possible
        std::string eName = "?";
        if (ex.nameRef >= 0 && ex.nameRef < (int)names.size()) eName = names[ex.nameRef];
        else if (ex.nameRef >= 29296 && ex.nameRef - 29296 < (int)ppNames.size())
            eName = ppNames[ex.nameRef - 29296];
        
        printf("    [%2d] class=%d super=%d pkg=%d name='%s'(%d) num=%d flags=0x%X\n"
               "         serialSize=%d serialOff=0x%X extra=(%d,%d) gen=%d (pos=0x%X)\n",
               i, ex.classRef, ex.superRef, ex.package, eName.c_str(), ex.nameRef,
               ex.nameNum, ex.flags, ex.serialSize, ex.serialOffset, extra1, extra2, genCI, ppPos);
    }
    printf("  Export table ended at offset 0x%X\n", ppPos);
    
    // Hex dump region after export table to understand intermediate structure
    printf("\n=== Region after export table (0x%X) ===\n", ppPos);
    // Check if there's another section header (like the name/export ones)
    if (ppPos + 16 < ppSize) {
        int32_t sec3_0 = *(int32_t*)(pp.data() + ppPos);
        int32_t sec3_1 = *(int32_t*)(pp.data() + ppPos + 4);
        int32_t sec3_2 = *(int32_t*)(pp.data() + ppPos + 8);
        int32_t sec3_3 = *(int32_t*)(pp.data() + ppPos + 12);
        printf("  Next 4 INT32s: %d, %d, %d, %d\n", sec3_0, sec3_1, sec3_2, sec3_3);
        printf("  Hex: %08X %08X %08X %08X\n", (uint32_t)sec3_0, (uint32_t)sec3_1,
               (uint32_t)sec3_2, (uint32_t)sec3_3);
    }
    
    // Dump first 256 bytes after export table
    printf("  Hex dump (first 256 bytes after export table):\n");
    for (int i = 0; i < 256 && ppPos + i < ppSize; i += 16) {
        printf("    %04X: ", ppPos + i);
        for (int j = 0; j < 16 && ppPos + i + j < ppSize; j++)
            printf("%02X ", pp[ppPos + i + j]);
        printf("\n");
    }
    
    // Also look for a pattern: repeated INT32 pairs that could be (exportIdx, dataOffset)
    printf("\n  Scanning for section-like patterns:\n");
    for (int scan = ppPos; scan < ppPos + 1024 && scan + 16 < ppSize; scan += 4) {
        int32_t v0 = *(int32_t*)(pp.data() + scan);
        int32_t v1 = *(int32_t*)(pp.data() + scan + 4);
        int32_t v2 = *(int32_t*)(pp.data() + scan + 8);
        // Look for another section header pattern: INT32(0) INT32(known_count) INT32(num) INT32(repeat)
        if (v0 == 0 && v1 > 0 && v1 < 100000 && v2 > 0 && v2 < 1000 && v1 == *(int32_t*)(pp.data()+scan+12)) {
            printf("    Possible section header at 0x%X: [%d, %d, %d, %d]\n", scan,
                   v0, v1, v2, *(int32_t*)(pp.data()+scan+12));
        }
        // Look for serial sizes (around 700KB-3MB for textures)
        if (v0 > 500000 && v0 < 5000000 && v1 > 0 && v1 < ppSize) {
            // Could be (serialSize, serialOffset) pair
            if (scan < ppPos + 512)
                printf("    Possible (size=%d, offset=0x%X) at 0x%X\n", v0, v1, scan);
        }
    }
    
    // Find pixel data start with finer granularity
    printf("\n=== Scanning for pixel data start (256-byte steps, entropy > 7.0) ===\n");
    int64_t pixelStart = -1;
    for (int64_t off = ppPos; off < ppSize - 1024; off += 256) {
        int freq[256] = {};
        for (int i = 0; i < 1024; i++) freq[pp[off + i]]++;
        double ent = 0;
        for (int i = 0; i < 256; i++) {
            if (freq[i] == 0) continue;
            double p = freq[i] / 1024.0;
            ent -= p * log2(p);
        }
        if (ent > 7.0) {
            pixelStart = off;
            printf("  Pixel data starts at offset 0x%llX (%lld) — entropy %.3f\n",
                   (long long)off, (long long)off, ent);
            printf("  Gap from export table end: %lld bytes\n", (long long)(off - ppPos));
            break;
        }
    }
    
    // ===== Analyze PackagePatch pixel data to find lightmaps =====
    printf("\n=== Analyzing PackagePatch pixel data for lightmaps ===\n");
    printf("  Pixel data starts at 0x%llX, total pixel region: %lld bytes\n",
           (long long)pixelStart, (long long)(ppSize - pixelStart));
    
    // Strategy: decode DXT1 colors at regular intervals (every 696320 bytes)
    // Lightmaps have low saturation (warm whites/cool blues)
    // Material textures have high saturation (vibrant colors)
    int64_t pixelRegionSize = ppSize - pixelStart;
    int texStride = 696320; // 5 mips of 1024x1024 DXT1
    int numSlots = (int)(pixelRegionSize / texStride);
    printf("  Pixel region: %lld bytes = ~%d slots of 696320\n",
           (long long)pixelRegionSize, numSlots);
    
    printf("\n  Sampling DXT1 at each 696320-byte slot (color delta metric):\n");
    printf("  Legend: delta = avg |c0-c1| per block (low = smooth gradient = lightmap)\n\n");
    for (int slot = 0; slot < numSlots && slot < 40; slot++) {
        int64_t off = pixelStart + (int64_t)slot * texStride;
        if (off + 64 >= ppSize) break;
        
        // Sample 64 blocks spread across the first 524288 bytes
        float totalDelta = 0;
        float totalSat = 0;
        int sampleCount = 0;
        for (int s = 0; s < 64; s++) {
            int64_t sampleOff = off + s * 8192; // sample every 8KB
            if (sampleOff + 8 >= ppSize) break;
            uint16_t c0 = *(uint16_t*)(pp.data() + sampleOff);
            uint16_t c1 = *(uint16_t*)(pp.data() + sampleOff + 2);
            int r0 = ((c0 >> 11) & 0x1F), g0 = ((c0 >> 5) & 0x3F), b0 = (c0 & 0x1F);
            int r1 = ((c1 >> 11) & 0x1F), g1 = ((c1 >> 5) & 0x3F), b1 = (c1 & 0x1F);
            // Color delta: euclidean distance between c0 and c1
            float dr = (r0 - r1) / 31.0f, dg = (g0 - g1) / 63.0f, db = (b0 - b1) / 31.0f;
            totalDelta += sqrtf(dr*dr + dg*dg + db*db);
            // Also compute saturation of average
            float r = (r0/31.0f + r1/31.0f)/2, g = (g0/63.0f + g1/63.0f)/2, b = (b0/31.0f + b1/31.0f)/2;
            float maxC = std::max({r,g,b}), minC = std::min({r,g,b});
            totalSat += (maxC > 0.01f) ? (maxC - minC) / maxC : 0;
            sampleCount++;
        }
        float avgDelta = totalDelta / sampleCount;
        float avgSat = totalSat / sampleCount;
        
        // Lightmap criterion: low delta (smooth) — threshold ~0.15
        const char* guess = (avgDelta < 0.15f) ? "LIGHTMAP" : "material";
        printf("    Slot %2d @ 0x%07llX: delta=%.3f sat=%.3f %s\n",
               slot, (long long)off, avgDelta, avgSat, guess);
    }
    
    // ===== Analyze FBspSurf 20B tail for lightmap references =====
    printf("\n=== FBspSurf Lightmap Field Analysis ===\n");
    {
        // Find the LARGEST Model export (the main level BSP UModel)
        int modelExIdx = -1;
        int maxModelSize = 0;
        for (int i = 0; i < (int)exports.size(); i++) {
            if (exports[i].className == "Model" && exports[i].serialSize > maxModelSize) {
                maxModelSize = exports[i].serialSize;
                modelExIdx = i;
            }
        }
        if (modelExIdx >= 0) {
            auto& me = exports[modelExIdx];
            printf("  Model export[%d] '%s': serial %d bytes at 0x%X\n",
                   modelExIdx, me.objectName.c_str(), me.serialSize, me.serialOffset);
            
            const uint8_t* ms = d + me.serialOffset;
            int msLen = me.serialSize;
            
            // Skip tagged properties
            int mpos = 8; // Vengeance header
            while (mpos < msLen - 5) {
                size_t br;
                int ni = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
                int32_t nn = *(int32_t*)(ms + mpos); mpos += 4;
                if (ni == noneIdx || (ni >= 0 && ni < (int)names.size() && names[ni] == "None")) break;
                if (ni < 0 || ni >= (int)names.size()) { mpos = msLen; break; }
                uint8_t info = ms[mpos++];
                int type = info & 0x0F;
                int sizeBits = (info >> 4) & 0x07;
                int arrayFlag = (info >> 7) & 1;
                if (type == 10) { ReadCI(ms+mpos, msLen-mpos, br); mpos += (int)br; mpos += 4; }
                int propSize = 0;
                switch(sizeBits) {
                    case 0: propSize=1; break; case 1: propSize=2; break; case 2: propSize=4; break;
                    case 3: propSize=12; break; case 4: propSize=16; break;
                    case 5: propSize=ms[mpos++]; break;
                    case 6: { uint16_t v; memcpy(&v,ms+mpos,2); mpos+=2; propSize=v; } break;
                    case 7: { uint32_t v; memcpy(&v,ms+mpos,4); mpos+=4; propSize=(int)v; } break;
                }
                if (type == 3) { /* bool: no payload */ }
                else { if(arrayFlag) { uint8_t b=ms[mpos++]; if((b&0xC0)==0x80) mpos++; else if((b&0xC0)==0xC0) mpos+=3; } mpos += propSize; }
            }
            printf("  Properties end at +%d\n", mpos);
            
            // UPrimitive base: FBox(25B) + FSphere(16B) = 41B
            mpos += 41;
            // Vengeance class header for UModel: check=4, sv
            int32_t mdlCheck = *(int32_t*)(ms + mpos);
            int32_t mdlSV = *(int32_t*)(ms + mpos + 4);
            printf("  UPrimitive+VengHeader: mdlCheck=%d mdlSV=%d\n", mdlCheck, mdlSV);
            mpos += 8;
            
            // Now parse native UModel data: Vectors CI count, Points CI count, etc.
            // Array format: CI(count) + count * stride
            size_t br;
            
            // 1. Vectors (12B each: FVector)
            int numVecs = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
            printf("  Vectors: %d (skipping %d bytes)\n", numVecs, numVecs * 12);
            mpos += numVecs * 12;
            
            // 2. Points (12B each: FVector)
            int numPts = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
            printf("  Points: %d (skipping %d bytes)\n", numPts, numPts * 12);
            mpos += numPts * 12;
            
            // 3. Nodes (100B each: FBspNode)
            int numNodes = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
            printf("  Nodes: %d (skipping %d bytes)\n", numNodes, numNodes * 100);
            mpos += numNodes * 100;
            
            // 4. Surfs (variable length)
            int numSurfs = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
            printf("  Surfs: %d — dumping 20B tail of first 20 surfs:\n", numSurfs);
            
            int dumpCount = std::min(numSurfs, 20);
            for (int si = 0; si < dumpCount && mpos < msLen - 8; si++) {
                // 8B Vengeance header
                mpos += 8;
                // CI Material
                int matRef = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
                // 24B fixed
                mpos += 24;
                // CI Actor
                int actRef = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
                // 20B remaining — THIS IS WHAT WE WANT TO EXAMINE
                if (mpos + 20 > msLen) { printf("    [%d] truncated\n", si); break; }
                
                // Interpret as: FPlane(16B: 4 floats) + float(LightMapScale 4B)
                float pX = *(float*)(ms + mpos);
                float pY = *(float*)(ms + mpos + 4);
                float pZ = *(float*)(ms + mpos + 8);
                float pW = *(float*)(ms + mpos + 12);
                float lmScale = *(float*)(ms + mpos + 16);
                int32_t asInt = *(int32_t*)(ms + mpos + 16);
                
                // Also try: maybe the last 4B is iLightMap (int) not float
                printf("    [%3d] mat=%d act=%d plane=(%.2f,%.2f,%.2f,%.2f) last4B=", 
                       si, matRef, actRef, pX, pY, pZ, pW);
                // Show as both float and int
                printf("%.3f / %d  hex:", lmScale, asInt);
                for (int b = 0; b < 20; b++) printf(" %02X", ms[mpos + b]);
                printf("\n");
                
                mpos += 20;
            }
            
            // Continue past remaining surfs
            for (int si = dumpCount; si < numSurfs && mpos < msLen - 8; si++) {
                mpos += 8;
                ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
                mpos += 24;
                ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
                mpos += 20;
            }
            printf("  After surfs: +%d\n", mpos);
            
            // 5. Verts
            int numVerts = ReadCI(ms + mpos, msLen - mpos, br); mpos += (int)br;
            printf("  Verts: %d (skipping %d bytes)\n", numVerts, numVerts * 8);
            mpos += numVerts * 8;
            
            printf("  After verts: +%d (0x%X), remaining %d bytes\n", mpos, mpos, msLen - mpos);
            
            // Dump first 512 bytes after Verts as hex + multiple interpretations
            printf("  Raw hex dump (first 512 bytes after Verts):\n");
            for (int row = 0; row < 32 && mpos + row*16 < msLen; row++) {
                int off = mpos + row*16;
                printf("    +%06X: ", off);
                for (int j = 0; j < 16 && off + j < msLen; j++)
                    printf("%02X ", ms[off + j]);
                // ASCII
                printf(" |");
                for (int j = 0; j < 16 && off + j < msLen; j++) {
                    uint8_t c = ms[off+j];
                    printf("%c", (c >= 32 && c < 127) ? c : '.');
                }
                printf("|\n");
            }
            
            // Try standard UE2 UModel post-Verts sequence:
            // 1. NumSharedSides (INT32)
            // 2. NumZones (INT32)  
            // 3. Zone connectivity/visibility (variable)
            // 4. CI Polys ref
            // 5. CI LeafHulls count + data
            // etc.
            printf("\n  Interpreting post-Verts data:\n");
            int pvPos = mpos;
            
            // Try INT32 at current pos
            if (pvPos + 4 <= msLen) {
                int32_t val0 = *(int32_t*)(ms + pvPos);
                printf("    INT32 at +%d = %d (0x%X)\n", pvPos, val0, (uint32_t)val0);
            }
            
            // Read CI values sequentially
            printf("    Sequential CI reads:\n");
            int tmpPos = pvPos;
            for (int c = 0; c < 30 && tmpPos < msLen - 4; c++) {
                int ciVal = ReadCI(ms + tmpPos, msLen - tmpPos, br);
                int ciSize = (int)br;
                printf("      CI[%d] at +%d: value=%d (size=%d)\n", c, tmpPos, ciVal, ciSize);
                tmpPos += ciSize;
                if (ciVal < 0 || ciVal > 100000) break;
            }
            
            // Also read INT32 values sequentially from pvPos
            printf("    Sequential INT32 reads:\n");
            for (int i = 0; i < 20 && pvPos + i*4 + 4 <= msLen; i++) {
                int32_t v = *(int32_t*)(ms + pvPos + i*4);
                printf("      INT32[%d] at +%d = %d (0x%08X)\n", i, pvPos + i*4, v, (uint32_t)v);
            }
            
            // ============================================================
            // SEQUENTIAL UModel POST-VERTS PARSING
            // Standard UE2 order: NumSharedSides, NumZones, Zones,
            // Polys, LeafHulls, Leaves, Lights, LightMap, LightBits
            // ============================================================
            printf("\n  === SEQUENTIAL POST-VERTS PARSING ===\n");
            int sp = mpos; // serial position
            
            // 1. NumSharedSides (INT32)
            int32_t numSharedSides = *(int32_t*)(ms + sp); sp += 4;
            printf("  [1] NumSharedSides = %d at +%d\n", numSharedSides, sp-4);
            
            // 2. NumZones (INT32)
            int32_t numZonesVal = *(int32_t*)(ms + sp); sp += 4;
            printf("  [2] NumZones = %d at +%d\n", numZonesVal, sp-4);
            
            // 3. Zones: try different FZoneProperties sizes
            // Vengeance FZoneProperties: ZoneActor(CI) + Connectivity(16B) + Visibility(16B)
            // But actor ref might be INT32 instead of CI
            // Let's try: INT32 ZoneActor + 128bit conn + 128bit vis = 4+16+16 = 36B
            printf("  [3] Zones: trying stride 36B for %d zones...\n", numZonesVal);
            if (numZonesVal > 0 && numZonesVal <= 128) {
                // Try strides 36, 40, 44, 48, 68, 72 and see which leads to valid next CI
                for (int stride : {36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80}) {
                    int testPos = sp + numZonesVal * stride;
                    if (testPos + 4 >= msLen) continue;
                    int nextCI = ReadCI(ms + testPos, msLen - testPos, br);
                    // After zones should be Polys ref (a CI object ref, could be 0 or small)
                    // Then LeafHulls count (CI, should be > 0 and < 100000)
                    int nextPos = testPos + (int)br;
                    int nextCI2 = ReadCI(ms + nextPos, msLen - nextPos, br);
                    printf("    stride=%d → next CI=%d, CI2=%d at +%d\n", 
                           stride, nextCI, nextCI2, testPos);
                    
                    // Validate: after Polys ref, LeafHulls count should be reasonable
                    if (nextCI2 > 0 && nextCI2 < 100000) {
                        // Check if skipping LeafHulls gives another reasonable CI
                        int lhEnd = nextPos + (int)br + nextCI2 * 4;
                        if (lhEnd + 4 < msLen) {
                            int leavesCI = ReadCI(ms + lhEnd, msLen - lhEnd, br);
                            printf("      → LeafHulls=%d → Leaves=%d at +%d", nextCI2, leavesCI, lhEnd);
                            if (leavesCI > 0 && leavesCI < 100000) printf(" ✓ VALID CHAIN");
                            printf("\n");
                        }
                    }
                }
            }
            
            // Try the most likely stride: scan for valid chain
            printf("\n  Scanning for valid zone stride...\n");
            int bestStride = 0;
            int bestScore = 0;
            for (int stride : {36, 40, 44, 48, 52, 56, 60, 64, 68, 72}) {
                int testPos = sp + numZonesVal * stride;
                if (testPos + 20 >= msLen) continue;
                
                // Read Polys ref (CI), LeafHulls count (CI), then validate
                int polyRef = ReadCI(ms + testPos, msLen - testPos, br);
                int p2 = testPos + (int)br;
                int lhCount = ReadCI(ms + p2, msLen - p2, br);
                int p3 = p2 + (int)br;
                
                if (lhCount <= 0 || lhCount >= 100000) continue;
                int p4 = p3 + lhCount * 4; // skip LeafHulls (INT32 array)
                if (p4 + 4 >= msLen) continue;
                
                int leavesCount = ReadCI(ms + p4, msLen - p4, br);
                if (leavesCount <= 0 || leavesCount >= 100000) continue;
                int p5 = p4 + (int)br + leavesCount * 12; // FLeaf = 12B
                if (p5 + 4 >= msLen) continue;
                
                int lightsCount = ReadCI(ms + p5, msLen - p5, br);
                int score = 0;
                if (lhCount > 100 && lhCount < 50000) score++;
                if (leavesCount > 10 && leavesCount < 50000) score++;
                if (lightsCount >= 0 && lightsCount < 1000) score += 2;
                
                printf("    stride=%d: Polys=%d LH=%d Leaves=%d Lights=%d score=%d\n",
                       stride, polyRef, lhCount, leavesCount, lightsCount, score);
                
                if (score > bestScore) {
                    bestScore = score;
                    bestStride = stride;
                }
            }
            
            if (bestStride > 0) {
                printf("\n  BEST stride: %d — parsing full chain...\n", bestStride);
                sp += numZonesVal * bestStride;
                printf("  After zones: +%d\n", sp);
                
                // Polys ref (CI)
                int polyRef = ReadCI(ms + sp, msLen - sp, br); sp += (int)br;
                printf("  [4] Polys ref = %d\n", polyRef);
                
                // LeafHulls (CI count + INT32 array)
                int lhCount = ReadCI(ms + sp, msLen - sp, br); sp += (int)br;
                printf("  [5] LeafHulls = %d entries (%d bytes)\n", lhCount, lhCount * 4);
                sp += lhCount * 4;
                
                // Leaves (CI count + FLeaf array, 12B each)
                int leavesCount = ReadCI(ms + sp, msLen - sp, br); sp += (int)br;
                printf("  [6] Leaves = %d entries (%d bytes)\n", leavesCount, leavesCount * 12);
                sp += leavesCount * 12;
                
                // Lights (CI count + CI ref array) — each is a CI object ref
                int lightsCount = ReadCI(ms + sp, msLen - sp, br); sp += (int)br;
                printf("  [7] Lights = %d entries at +%d\n", lightsCount, sp);
                // Lights are CI object refs (variable size each)
                for (int li = 0; li < lightsCount; li++) {
                    int lightRef = ReadCI(ms + sp, msLen - sp, br); sp += (int)br;
                    if (li < 5) printf("    Light[%d] = %d\n", li, lightRef);
                }
                if (lightsCount > 5) printf("    ... (%d more)\n", lightsCount - 5);
                
                printf("  After Lights: +%d (0x%X), remaining %d bytes\n", sp, sp, msLen - sp);
                
                // LightMap: CI count + FLightMapIndex array
                // FLightMapIndex in UE2:
                //   INT32 DataOffset
                //   INT32 iLightActors (count, then CI refs)
                //   Actually variable — let's just read count and dump first entries
                int lmCount = ReadCI(ms + sp, msLen - sp, br); sp += (int)br;
                printf("  [8] LightMap array: count = %d at +%d\n", lmCount, sp - (int)br);
                
                if (lmCount > 0 && lmCount <= numSurfs * 2) {
                    printf("  *** FOUND LIGHTMAP ARRAY! count=%d (numSurfs=%d) ***\n", lmCount, numSurfs);
                    
                    // Dump first 20 entries raw, trying different strides
                    printf("  Raw hex of first 256 bytes:\n");
                    for (int row = 0; row < 16 && sp + row*16 < msLen; row++) {
                        printf("    +%06X: ", sp + row*16);
                        for (int j = 0; j < 16 && sp + row*16 + j < msLen; j++)
                            printf("%02X ", ms[sp + row*16 + j]);
                        printf("\n");
                    }
                    
                    // Try fixed strides for FLightMapIndex
                    printf("\n  Trying FLightMapIndex strides:\n");
                    for (int stride : {4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64}) {
                        int endPos = sp + lmCount * stride;
                        if (endPos + 4 >= msLen) continue;
                        int nextCI = ReadCI(ms + endPos, msLen - endPos, br);
                        // After LightMap should be LightBits (CI count + BYTE array)
                        if (nextCI >= 0 && nextCI < 10000000) {
                            printf("    stride=%d: nextCI=%d at +%d", stride, nextCI, endPos);
                            // Verify: LightBits should be a byte array
                            int afterLB = endPos + (int)br + nextCI;
                            if (afterLB < msLen) {
                                int moreCI = ReadCI(ms + afterLB, msLen - afterLB, br);
                                printf(" → after LB=%d", moreCI);
                            }
                            printf("\n");
                        }
                    }
                    
                    // For the first 20 entries, dump as both INT32s and floats at stride 4
                    printf("\n  First 50 values as INT32/float:\n");
                    for (int i = 0; i < 50 && sp + i*4 + 4 <= msLen; i++) {
                        int32_t iv = *(int32_t*)(ms + sp + i*4);
                        float fv = *(float*)(ms + sp + i*4);
                        printf("    [%2d] int=%-10d float=%-14.6f", i, iv, fv);
                        if (fv >= 0.0f && fv <= 1.0f && fv > 0.001f) printf(" [UV]");
                        if (fv >= 1.0f && fv <= 1024.0f) printf(" [scale]");
                        if (iv >= 0 && iv < 256) printf(" [byte-range]");
                        if (iv >= 0 && iv < 10000) printf(" [idx]");
                        printf("\n");
                    }
                } else {
                    printf("  LightMap count %d seems wrong (expected near %d)\n", lmCount, numSurfs);
                    // Dump the area for inspection
                    printf("  Next 128 bytes:\n");
                    for (int row = 0; row < 8 && sp + row*16 < msLen; row++) {
                        printf("    +%06X: ", sp + row*16);
                        for (int j = 0; j < 16 && sp + row*16 + j < msLen; j++)
                            printf("%02X ", ms[sp + row*16 + j]);
                        printf("\n");
                    }
                }
            } else {
                printf("  ERROR: Could not determine zone stride!\n");
            }
            
            // ============================================================
            // BRUTE FORCE: scan for CI values near numSurfs in post-Verts
            // ============================================================
            printf("\n  === BRUTE FORCE: scan for key CI values ===\n");
            printf("  Looking for CI = %d (numSurfs) or nearby...\n", numSurfs);
            for (int off = pvPos; off + 4 < msLen; off++) {
                int ciVal = ReadCI(ms + off, msLen - off, br);
                // Match numSurfs, numNodes, numVerts, or common array sizes
                if (ciVal == numSurfs || ciVal == numNodes || ciVal == numVerts ||
                    (ciVal > numSurfs - 10 && ciVal < numSurfs + 10) ||
                    ciVal == numSurfs * 2) {
                    printf("    CI=%d at +%d (0x%X), ciSize=%d\n", ciVal, off, off, (int)br);
                    // Try stride detection: what stride makes the next value a valid CI?
                    int dataStart = off + (int)br;
                    for (int stride : {4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64}) {
                        int endPos = dataStart + ciVal * stride;
                        if (endPos + 4 >= msLen) continue;
                        int nextCI = ReadCI(ms + endPos, msLen - endPos, br);
                        if (nextCI > 0 && nextCI < 1000000) {
                            printf("      stride=%d → nextCI=%d at +%d\n", stride, nextCI, endPos);
                        }
                    }
                }
            }
            
            // ============================================================
            // CHAIN SCANNER: find longest CI-counted array chain
            // ============================================================
            printf("\n  === CHAIN SCANNER: find CI-counted array chains ===\n");
            int bestChainLen = 0;
            int bestChainStart = 0;
            struct ChainStep { int off; int count; int stride; };
            std::vector<ChainStep> bestChain;
            
            for (int startOff = pvPos; startOff < pvPos + 1000; startOff++) {
                std::vector<ChainStep> chain;
                int pos = startOff;
                while (pos + 4 < msLen && chain.size() < 20) {
                    int count = ReadCI(ms + pos, msLen - pos, br);
                    if (count <= 0 || count > 1000000) break;
                    int dataPos = pos + (int)br;
                    
                    // Find a stride that leads to next valid array
                    bool found = false;
                    for (int stride : {1, 2, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64}) {
                        int endPos = dataPos + count * stride;
                        if (endPos + 4 >= msLen) continue;
                        int nextCount = ReadCI(ms + endPos, msLen - endPos, br);
                        if (nextCount > 0 && nextCount < 1000000) {
                            chain.push_back({pos, count, stride});
                            pos = endPos;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        // Try count=0 (empty array)
                        if (count == 0) {
                            chain.push_back({pos, 0, 0});
                            pos = dataPos;
                        } else break;
                    }
                }
                if ((int)chain.size() > bestChainLen) {
                    bestChainLen = (int)chain.size();
                    bestChainStart = startOff;
                    bestChain = chain;
                }
            }
            
            printf("  Best chain: %d arrays starting at +%d (0x%X)\n", 
                   bestChainLen, bestChainStart, bestChainStart);
            for (auto& step : bestChain) {
                printf("    +%d: count=%d, stride=%d (%d bytes total)\n", 
                       step.off, step.count, step.stride, step.count * step.stride);
            }
        } else {
            printf("  ERROR: No Model export found!\n");
        }
    }
    
    // ===== Parse Catalog.bdc to map exports to BulkChunk files =====
    printf("\n=== Catalog.bdc Analysis ===\n");
    std::filesystem::path bdcPath = "D:\\SteamLibrary\\steamapps\\common\\BioShock Remastered\\ContentBaked\\pc\\BulkContent\\Catalog.bdc";
    std::ifstream bdcFile(bdcPath, std::ios::binary);
    if (!bdcFile) {
        printf("  ERROR: Cannot open Catalog.bdc\n");
        return 1;
    }
    std::vector<uint8_t> bdc((std::istreambuf_iterator<char>(bdcFile)), {});
    bdcFile.close();
    printf("  Catalog.bdc size: %d bytes\n", (int)bdc.size());
    
    // Dump first 256 bytes for entry format analysis
    printf("  First 256 bytes (entry format):\n");
    for (int row = 0; row < 16; row++) {
        printf("    +%04X: ", row * 16);
        for (int j = 0; j < 16; j++) printf("%02X ", bdc[row * 16 + j]);
        printf(" |");
        for (int j = 0; j < 16; j++) {
            uint8_t c = bdc[row*16+j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
    
    // ===== Parse catalog for each lightmap texture =====
    printf("\n=== Catalog Lookup for Lightmap Textures ===\n");
    
    // Build names from actual lightmap exports (remove underscore for catalog format)
    std::vector<std::string> lmNameStrs;
    for (int idx : textureExports) {
        std::string catName = exports[idx].objectName;
        // Catalog uses "Texture42" not "Texture_42" — remove the underscore
        size_t upos = catName.find('_');
        if (upos != std::string::npos) catName.erase(upos, 1);
        lmNameStrs.push_back(catName);
    }
    printf("  Looking up %d textures in catalog\n", (int)lmNameStrs.size());
    
    struct CatEntry {
        std::string name;
        int catOff;         // offset in catalog where name was found
        char chunkFile[64]; // .blk filename
        int64_t bulkOffset; // offset in .blk
        int32_t bulkSize;   // size in .blk
    };
    std::vector<CatEntry> catEntries;
    
    for (int n = 0; n < (int)lmNameStrs.size(); n++) {
        const char* target = lmNameStrs[n].c_str();
        int targetLen = (int)strlen(target);
        
        // Search for UTF-16LE string in catalog
        for (int off = 0; off < (int)bdc.size() - targetLen * 2 - 2; off += 2) {
            bool match = true;
            for (int i = 0; i < targetLen; i++) {
                if (bdc[off + i*2] != (uint8_t)target[i] || bdc[off + i*2 + 1] != 0) {
                    match = false; break;
                }
            }
            // Check null terminator
            if (match && bdc[off + targetLen*2] == 0 && bdc[off + targetLen*2 + 1] == 0) {
                // Check if followed by "1-Medical" package name
                int afterName = off + (targetLen + 1) * 2; // after null
                int pkgLenByte = bdc[afterName]; // should be 10 for "1-Medical\0"
                bool isMedical = false;
                if (pkgLenByte == 10 && afterName + 1 + 10*2 < (int)bdc.size()) {
                    const char* med = "1-Medical";
                    isMedical = true;
                    for (int i = 0; i < 9; i++) {
                        if (bdc[afterName+1+i*2] != (uint8_t)med[i] || bdc[afterName+1+i*2+1] != 0) {
                            isMedical = false; break;
                        }
                    }
                }
                
                if (isMedical) {
                    // Found! Now read the metadata after "1-Medical\0"
                    int metaStart = afterName + 1 + 10 * 2; // after "1-Medical\0"
                    
                    // Read 20 bytes of metadata as 5 INT32s
                    int32_t meta[5] = {};
                    for (int i = 0; i < 5 && metaStart + i*4 + 3 < (int)bdc.size(); i++) {
                        meta[i] = *(int32_t*)(bdc.data() + metaStart + i*4);
                    }
                    
                    // Now find the chunk filename that PRECEDES this texture name
                    // It should be a few bytes before 'off' (the texture name)
                    // Format: [len_byte][UTF16 chunk name][null][len_byte=texture name len][texture name]
                    // So the texture name length byte is at off-1
                    int chunkEnd = off - 2; // skip length byte and separator byte
                    // Scan backwards from chunkEnd to find start of chunk filename
                    char chunkName[64] = {};
                    int chunkNameOff = -1;
                    // Chunk name is before texture name: [...metadata...][len][chunk_str][null][sep][len][texture_name]
                    for (int scan = chunkEnd; scan >= std::max(0, chunkEnd - 500); scan -= 2) {
                        if (bdc[scan] == 'B' && bdc[scan+1] == 0 && bdc[scan+2] == 'u') {
                            // Found "Bu" — likely start of "BulkChunk..."
                            int cLen = 0;
                            while (scan + cLen*2 + 1 < (int)bdc.size() && cLen < 60) {
                                uint16_t ch = *(uint16_t*)(bdc.data() + scan + cLen*2);
                                if (ch == 0) break;
                                chunkName[cLen] = (char)ch;
                                cLen++;
                            }
                            chunkNameOff = scan;
                            break;
                        }
                    }
                    
                    CatEntry e;
                    e.name = target;
                    e.catOff = off;
                    strncpy(e.chunkFile, chunkName, 63);
                    e.bulkOffset = meta[1]; // offset in .blk file
                    e.bulkSize = meta[2];
                    catEntries.push_back(e);
                    
                    // Hex dump 32 bytes at metaStart for first few entries
                    if (catEntries.size() <= 3) {
                        printf("  %s: meta hex @0x%X: ", target, metaStart);
                        for (int b = 0; b < 32 && metaStart+b < (int)bdc.size(); b++)
                            printf("%02X ", bdc[metaStart+b]);
                        printf("\n");
                    }
                    printf("  %s: chunk='%s' meta=[%d, %d, %d, %d, %d]\n",
                           target, chunkName, meta[0], meta[1], meta[2], meta[3], meta[4]);
                    break;
                }
            }
        }
    }
    printf("  Found %d catalog entries\n", (int)catEntries.size());
    
    // Helper: write a DDS file (DXT1)
    auto writeDDS = [](const char* path, const uint8_t* data, int64_t dataSize, 
                       int w, int h, int mipCount) {
        FILE* f = fopen(path, "wb");
        if (!f) return;
        uint8_t hdr[128] = {};
        *(uint32_t*)(hdr + 0) = 0x20534444;           // 'DDS '
        *(uint32_t*)(hdr + 4) = 124;                   // header size
        uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000;     // CAPS|HEIGHT|WIDTH|PIXELFORMAT
        if (mipCount > 1) flags |= 0x20000;             // MIPMAPCOUNT
        flags |= 0x80000;                               // LINEARSIZE
        *(uint32_t*)(hdr + 8) = flags;
        *(uint32_t*)(hdr + 12) = h;
        *(uint32_t*)(hdr + 16) = w;
        int linearSize = std::max(1, (w+3)/4) * std::max(1, (h+3)/4) * 8;
        *(uint32_t*)(hdr + 20) = linearSize;
        *(uint32_t*)(hdr + 28) = mipCount;
        *(uint32_t*)(hdr + 76) = 32;                    // ddspf size
        *(uint32_t*)(hdr + 80) = 0x4;                   // DDPF_FOURCC
        *(uint32_t*)(hdr + 84) = 0x31545844;            // 'DXT1'
        uint32_t caps = 0x1000;                          // TEXTURE
        if (mipCount > 1) caps |= 0x8 | 0x400000;       // COMPLEX|MIPMAP
        *(uint32_t*)(hdr + 108) = caps;
        fwrite(hdr, 1, 128, f);
        fwrite(data, 1, dataSize, f);
        fclose(f);
    };
    
    // Helper: compute DXT1 delta + saturation
    auto computeDelta = [](const uint8_t* data, int64_t size) -> std::pair<float,float> {
        float totalD = 0, totalS = 0; int cnt = 0;
        for (int s = 0; s < 64; s++) {
            int64_t off = s * 8192;
            if (off + 8 > size) break;
            uint16_t c0 = *(const uint16_t*)(data + off);
            uint16_t c1 = *(const uint16_t*)(data + off + 2);
            int r0=(c0>>11)&0x1F, g0=(c0>>5)&0x3F, b0=c0&0x1F;
            int r1=(c1>>11)&0x1F, g1=(c1>>5)&0x3F, b1=c1&0x1F;
            float dr=(r0-r1)/31.0f, dg=(g0-g1)/63.0f, db=(b0-b1)/31.0f;
            totalD += sqrtf(dr*dr+dg*dg+db*db);
            float r=(r0/31.0f+r1/31.0f)/2, g=(g0/63.0f+g1/63.0f)/2, b=(b0/31.0f+b1/31.0f)/2;
            float mx=std::max({r,g,b}), mn=std::min({r,g,b});
            totalS += (mx>0.01f) ? (mx-mn)/mx : 0;
            cnt++;
        }
        return cnt > 0 ? std::make_pair(totalD/cnt, totalS/cnt) : std::make_pair(0.f, 0.f);
    };
    
    // Create output directory
    std::filesystem::create_directories("tools/lightmap_probe/variants");
    
    // Now open the .blk file and extract multiple DDS variants
    if (!catEntries.empty()) {
        std::string chunkDir = "D:\\SteamLibrary\\steamapps\\common\\BioShock Remastered\\ContentBaked\\pc\\BulkContent\\";
        int64_t totalBulkSize = 696320; // all 4 mips
        
        printf("\n  === Multi-variant lightmap extraction ===\n");
        for (int i = 0; i < (int)catEntries.size(); i++) {
            auto& ce = catEntries[i];
            std::string blkFullPath = chunkDir + ce.chunkFile;
            int64_t blkFileSize = std::filesystem::file_size(blkFullPath);
            
            // Read the entire bulk region (all mips) from meta[1]
            int64_t baseOff = ce.bulkOffset;
            int64_t readMax = std::min(totalBulkSize, blkFileSize - baseOff);
            if (baseOff >= blkFileSize || readMax < mip0Size) {
                printf("    %s: SKIP (offset 0x%llX out of range, file=%lld)\n",
                       ce.name.c_str(), (long long)baseOff, (long long)blkFileSize);
                continue;
            }
            
            std::vector<uint8_t> bulk(readMax);
            {
                std::ifstream bf(blkFullPath, std::ios::binary);
                bf.seekg(baseOff);
                bf.read((char*)bulk.data(), readMax);
            }
            
            // Find first non-zero byte offset (relative to baseOff)
            int64_t firstNZ = -1;
            for (int64_t b = 0; b < readMax; b++) {
                if (bulk[b] != 0) { firstNZ = b; break; }
            }
            
            printf("    %s: chunk=%s off=0x%llX firstNZ=+0x%llX\n",
                   ce.name.c_str(), ce.chunkFile, (long long)baseOff,
                   firstNZ >= 0 ? (long long)firstNZ : -1LL);
            
            // === Variant A: raw at meta[1], mip0 only ===
            {
                auto [d,s] = computeDelta(bulk.data(), mip0Size);
                char fn[256];
                snprintf(fn, sizeof(fn), "tools/lightmap_probe/variants/%s_A_raw.dds", ce.name.c_str());
                writeDDS(fn, bulk.data(), mip0Size, 1024, 1024, 1);
                printf("      A(raw,mip0)   delta=%.3f sat=%.3f\n", d, s);
            }
            
            // === Variant B: offset +0xD0 (208 bytes), mip0 only ===
            if (0xD0 + mip0Size <= readMax) {
                auto [d,s] = computeDelta(bulk.data() + 0xD0, mip0Size);
                char fn[256];
                snprintf(fn, sizeof(fn), "tools/lightmap_probe/variants/%s_B_plus0xD0.dds", ce.name.c_str());
                writeDDS(fn, bulk.data() + 0xD0, mip0Size, 1024, 1024, 1);
                printf("      B(+0xD0,mip0) delta=%.3f sat=%.3f\n", d, s);
            }
            
            // === Variant C: first non-zero byte, mip0 only ===
            if (firstNZ >= 0 && firstNZ + mip0Size <= readMax && firstNZ != 0 && firstNZ != 0xD0) {
                auto [d,s] = computeDelta(bulk.data() + firstNZ, mip0Size);
                char fn[256];
                snprintf(fn, sizeof(fn), "tools/lightmap_probe/variants/%s_C_firstNZ.dds", ce.name.c_str());
                writeDDS(fn, bulk.data() + firstNZ, mip0Size, 1024, 1024, 1);
                printf("      C(firstNZ+0x%llX,mip0) delta=%.3f sat=%.3f\n", (long long)firstNZ, d, s);
            }
            
            // === Variant D: raw at meta[1], ALL mips (mipCount=4) ===
            if (readMax >= totalBulkSize) {
                auto [d,s] = computeDelta(bulk.data(), mip0Size);
                char fn[256];
                snprintf(fn, sizeof(fn), "tools/lightmap_probe/variants/%s_D_allmips.dds", ce.name.c_str());
                writeDDS(fn, bulk.data(), totalBulkSize, 1024, 1024, 4);
                printf("      D(raw,4mips)  delta=%.3f sat=%.3f\n", d, s);
            }
            
            // === Variant E: interpret as 512x512 (quarter-size), mip0=131072 ===
            {
                int smallMip0 = 131072; // 512x512 DXT1
                if (smallMip0 <= readMax) {
                    auto [d,s] = computeDelta(bulk.data(), smallMip0);
                    char fn[256];
                    snprintf(fn, sizeof(fn), "tools/lightmap_probe/variants/%s_E_512x512.dds", ce.name.c_str());
                    writeDDS(fn, bulk.data(), smallMip0, 512, 512, 1);
                    printf("      E(512x512)    delta=%.3f sat=%.3f\n", d, s);
                }
            }
            
            // === First 64 bytes hex dump ===
            printf("      hex[0..63]: ");
            for (int b = 0; b < 64 && b < (int)readMax; b++) {
                printf("%02X", bulk[b]);
                if (b % 8 == 7) printf(" ");
            }
            printf("\n");
        }
    }
    
    return 0;
}
