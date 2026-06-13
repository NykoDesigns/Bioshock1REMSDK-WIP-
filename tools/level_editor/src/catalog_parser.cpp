#include "catalog_parser.h"
#include <cstdio>
#include <cstring>
#include <fstream>

// BioShock CompactIndex reader (same as in bsm_document.cpp)
static int ReadCI_Cat(const uint8_t* d, size_t maxLen, size_t& bytesRead)
{
    if (maxLen == 0) { bytesRead = 0; return 0; }
    uint8_t b0 = d[0];
    bool sign = (b0 & 0x80) != 0;
    int val = b0 & 0x3F;
    bytesRead = 1;
    if (b0 & 0x40) {
        if (bytesRead >= maxLen) return val;
        uint8_t b1 = d[bytesRead++];
        val |= (b1 & 0x7F) << 6;
        if (b1 & 0x80) {
            if (bytesRead >= maxLen) return val;
            uint8_t b2 = d[bytesRead++];
            val |= (b2 & 0x7F) << 13;
            if (b2 & 0x80) {
                if (bytesRead >= maxLen) return val;
                uint8_t b3 = d[bytesRead++];
                val |= (b3 & 0x7F) << 20;
                if (b3 & 0x80) {
                    if (bytesRead >= maxLen) return val;
                    uint8_t b4 = d[bytesRead++];
                    val |= (b4 & 0x1F) << 27;
                }
            }
        }
    }
    return sign ? -val : val;
}

// Read BioShock FString: BioShock REVERSES the UE2 sign convention:
//   len > 0 → UTF-16LE, len code units (includes NUL terminator)
//   len < 0 → ANSI, |len| bytes (includes NUL terminator)
//   len == 0 → empty
static std::string ReadBioString(const uint8_t* data, size_t maxLen, size_t& bytesRead)
{
    bytesRead = 0;
    size_t br;
    int rawLen = ReadCI_Cat(data, maxLen, br);
    bytesRead += br;

    if (rawLen == 0) return "";

    if (rawLen > 0) {
        // Positive = UTF-16LE, rawLen code units including NUL
        if (rawLen > 65536) return "";
        size_t byteSize = (size_t)rawLen * 2;
        if (bytesRead + byteSize > maxLen) return "";
        std::string result;
        result.reserve(rawLen);
        for (int i = 0; i < rawLen; i++) {
            uint16_t wc;
            memcpy(&wc, data + bytesRead + (size_t)i * 2, 2);
            if (wc == 0) break;
            result += (char)(wc & 0xFF); // ASCII subset
        }
        bytesRead += byteSize;
        return result;
    } else {
        // Negative = ANSI, |rawLen| bytes including NUL
        int absLen = -rawLen;
        if (absLen > 65536) return "";
        if (bytesRead + (size_t)absLen > maxLen) return "";
        std::string result;
        result.reserve(absLen);
        for (int i = 0; i < absLen; i++) {
            char c = (char)data[bytesRead + i];
            if (c == 0) break;
            result += c;
        }
        bytesRead += (size_t)absLen;
        return result;
    }
}

bool CatalogParser::Load(const std::string& bdcPath)
{
    m_Entries.clear();
    m_NameToIndex.clear();

    std::ifstream file(bdcPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        printf("[Catalog] Failed to open: %s\n", bdcPath.c_str());
        return false;
    }

    size_t fileSize = (size_t)file.tellg();
    file.seekg(0);
    std::vector<uint8_t> buf(fileSize);
    file.read((char*)buf.data(), fileSize);
    file.close();

    const uint8_t* d = buf.data();
    size_t pos = 0;
    size_t br;

    // BioBulkCatalog header
    if (pos + 1 > fileSize) return false;
    uint8_t endian = d[pos++]; // 0 = PC (little-endian)
    (void)endian;

    if (pos + 12 > fileSize) return false;
    // int64 f4
    pos += 8;
    // int fC
    pos += 4;

    // TArray<BioBulkCatalogFile> — CI count (no BioShock extra in standalone .bdc files)
    int numFiles = ReadCI_Cat(d + pos, fileSize - pos, br); pos += br;
    if (numFiles <= 0 || numFiles > 10000) {
        printf("[Catalog] Bad file count: %d\n", numFiles);
        return false;
    }

    printf("[Catalog] Parsing %s: %d bulk files\n", bdcPath.c_str(), numFiles);

    for (int fi = 0; fi < numFiles && pos < fileSize; fi++) {
        // BioBulkCatalogFile: int64 f0, FString Filename, TArray<Items>
        if (pos + 8 > fileSize) break;
        pos += 8; // int64 f0

        // FString Filename
        std::string blkFilename = ReadBioString(d + pos, fileSize - pos, br); pos += br;

        // TArray<BioBulkCatalogItem> — CI count
        int numItems = ReadCI_Cat(d + pos, fileSize - pos, br); pos += br;
        if (numItems < 0 || numItems > 100000) {
            printf("[Catalog] Bad item count %d in file %s\n", numItems, blkFilename.c_str());
            break;
        }

        for (int ii = 0; ii < numItems && pos < fileSize; ii++) {
            // BioBulkCatalogItem: FString ObjectName, FString PackageName,
            //                     int f10, int DataOffset, int DataSize, int DataSize2, int f20
            std::string objName = ReadBioString(d + pos, fileSize - pos, br); pos += br;
            std::string pkgName = ReadBioString(d + pos, fileSize - pos, br); pos += br;

            if (pos + 20 > fileSize) break;
            // int32 f10 (always 0)
            pos += 4;
            int32_t dataOffset = *(int32_t*)(d + pos); pos += 4;
            int32_t dataSize   = *(int32_t*)(d + pos); pos += 4;
            // int32 DataSize2 (same on PC)
            pos += 4;
            // int32 f20
            pos += 4;

            if (!objName.empty() && dataSize > 0) {
                int idx = (int)m_Entries.size();
                CatalogEntry entry;
                entry.objectName = objName;
                entry.packageName = pkgName;
                entry.blkFilename = blkFilename;
                entry.dataOffset = dataOffset;
                entry.dataSize = dataSize;
                m_Entries.push_back(std::move(entry));
                m_NameToIndex[m_Entries[idx].objectName] = idx;
            }
        }
    }

    // Store the bulk directory (same dir as the .bdc file)
    m_BulkDir = bdcPath;
    size_t lastSlash = m_BulkDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) m_BulkDir = m_BulkDir.substr(0, lastSlash);

    printf("[Catalog] Loaded %d entries from %d bulk files\n", (int)m_Entries.size(), numFiles);
    return !m_Entries.empty();
}

const CatalogEntry* CatalogParser::Find(const std::string& objectName) const
{
    auto it = m_NameToIndex.find(objectName);
    if (it == m_NameToIndex.end()) return nullptr;
    return &m_Entries[it->second];
}

std::vector<uint8_t> CatalogParser::ReadBulkData(const std::string& objectName,
                                                   const std::string& bulkContentDir) const
{
    const CatalogEntry* entry = Find(objectName);
    if (!entry) return {};

    std::string blkPath = bulkContentDir + "\\" + entry->blkFilename;
    std::ifstream blk(blkPath, std::ios::binary);
    if (!blk.is_open()) {
        printf("[Catalog] Cannot open %s\n", blkPath.c_str());
        return {};
    }

    blk.seekg(entry->dataOffset);
    std::vector<uint8_t> data(entry->dataSize);
    blk.read((char*)data.data(), entry->dataSize);
    if (!blk.good()) {
        printf("[Catalog] Read failed: %s at offset %d, size %d\n",
               blkPath.c_str(), entry->dataOffset, entry->dataSize);
        return {};
    }

    return data;
}

int CatalogParser::CountEntriesWithPrefix(const std::string& prefix) const
{
    int count = 0;
    for (auto& e : m_Entries) {
        if (e.objectName.size() >= prefix.size() &&
            e.objectName.compare(0, prefix.size(), prefix) == 0) {
            count++;
        }
    }
    return count;
}

std::vector<const CatalogEntry*> CatalogParser::FindByPackage(const std::string& packageSubstr) const
{
    std::vector<const CatalogEntry*> results;
    for (auto& e : m_Entries) {
        if (e.packageName.find(packageSubstr) != std::string::npos) {
            results.push_back(&e);
        }
    }
    return results;
}
