#include "coop_save.h"
#include "net_common.h"
#include "net_manager.h"
#include "../core/log.h"

#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <ShlObj.h>
#include <Windows.h>

namespace bs1sdk {

// ─── Save File Paths ────────────────────────────────────────────────────

static std::string GetSaveDir()
{
    char appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return std::string(appData) + "\\BioshockHD\\Bioshock\\SaveGames";
    }
    return "";
}

static std::string GetCoopSaveDir()
{
    std::string base = GetSaveDir();
    if (base.empty()) return "";
    return base + "\\CoopSync";
}

// ─── Sender State ───────────────────────────────────────────────────────

static std::vector<uint8_t> s_SendBuffer;
static uint32_t s_SendOffset = 0;
static bool s_Sending = false;
static float s_SendTimer = 0.0f;
constexpr float SEND_INTERVAL = 0.005f; // 5ms between chunks (~180KB/s)
constexpr int CHUNKS_PER_TICK = 3;      // send 3 chunks per tick for speed

// ─── Receiver State ─────────────────────────────────────────────────────

static std::vector<uint8_t> s_RecvBuffer;
static uint32_t s_RecvTotalSize = 0;
static uint32_t s_RecvBytesGot = 0;
static bool s_Receiving = false;

// ─── Sender ─────────────────────────────────────────────────────────────

bool StartSaveTransfer()
{
    std::string saveDir = GetSaveDir();
    if (saveDir.empty()) {
        LOG_ERROR("[SaveSync] Can't find AppData save directory");
        return false;
    }

    // Find the most recent save (QuickSave or numbered saves)
    std::string savePath;
    std::string saveHeaderPath;

    // Try QuickSave first
    std::string qs = saveDir + "\\QuickSave\\mainSave.bsg";
    std::string qsH = saveDir + "\\QuickSave.bsh";
    if (std::filesystem::exists(qs)) {
        savePath = qs;
        saveHeaderPath = qsH;
    } else {
        // Look for numbered saves (SaveXXXX)
        for (auto& entry : std::filesystem::directory_iterator(saveDir)) {
            if (entry.is_directory()) {
                std::string bsg = entry.path().string() + "\\mainSave.bsg";
                if (std::filesystem::exists(bsg)) {
                    std::string bsh = entry.path().string() + ".bsh";
                    savePath = bsg;
                    saveHeaderPath = bsh;
                    break;
                }
            }
        }
    }

    if (savePath.empty()) {
        LOG_ERROR("[SaveSync] No save file found in {}", saveDir);
        return false;
    }

    // Read the save file into memory
    std::ifstream file(savePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_ERROR("[SaveSync] Can't open {}", savePath);
        return false;
    }

    size_t fileSize = file.tellg();
    file.seekg(0);
    s_SendBuffer.resize(fileSize);
    file.read(reinterpret_cast<char*>(s_SendBuffer.data()), fileSize);
    file.close();

    s_SendOffset = 0;
    s_Sending = true;
    s_SendTimer = 0.0f;

    LOG_INFO("[SaveSync] Starting save transfer: {} ({} bytes, {} chunks)",
             savePath, fileSize, (fileSize + SAVE_CHUNK_SIZE - 1) / SAVE_CHUNK_SIZE);

    return true;
}

void TickSaveTransfer()
{
    if (!s_Sending) return;

    // Send multiple chunks per tick for speed
    for (int c = 0; c < CHUNKS_PER_TICK; c++) {
        if (s_SendOffset >= s_SendBuffer.size()) {
            s_Sending = false;
            s_SendBuffer.clear();
            LOG_INFO("[SaveSync] Save transfer complete!");
            return;
        }

        uint32_t remaining = (uint32_t)(s_SendBuffer.size() - s_SendOffset);
        uint16_t chunkSize = (uint16_t)(remaining > SAVE_CHUNK_SIZE ? SAVE_CHUNK_SIZE : remaining);
        bool isFinal = (s_SendOffset + chunkSize >= s_SendBuffer.size());

        SaveTransferData pkt{};
        pkt.totalSize = (uint32_t)s_SendBuffer.size();
        pkt.chunkOffset = s_SendOffset;
        pkt.chunkSize = chunkSize;
        pkt.fileIndex = 0; // mainSave.bsg
        pkt.flags = isFinal ? 1 : 0;
        memcpy(pkt.data, s_SendBuffer.data() + s_SendOffset, chunkSize);

        // Only send the used portion (header + actual data, not full 900 byte buffer)
        uint16_t sendSize = (uint16_t)(12 + chunkSize); // 4+4+2+1+1 + data
        NetSendRawPacket(PacketType::SaveTransfer, &pkt, sendSize);

        s_SendOffset += chunkSize;
    }
}

bool IsSaveTransferActive()
{
    return s_Sending || s_Receiving;
}

float GetSaveTransferProgress()
{
    if (s_Sending && !s_SendBuffer.empty()) {
        return (float)s_SendOffset / (float)s_SendBuffer.size();
    }
    if (s_Receiving && s_RecvTotalSize > 0) {
        return (float)s_RecvBytesGot / (float)s_RecvTotalSize;
    }
    return 0.0f;
}

// ─── Receiver ───────────────────────────────────────────────────────────

void OnSaveChunkReceived(const void* data, int size)
{
    if (size < 12) return; // minimum header

    const SaveTransferData* pkt = reinterpret_cast<const SaveTransferData*>(data);

    // First chunk? Initialize receive buffer.
    if (pkt->chunkOffset == 0 || !s_Receiving) {
        s_RecvTotalSize = pkt->totalSize;
        s_RecvBuffer.resize(pkt->totalSize, 0);
        s_RecvBytesGot = 0;
        s_Receiving = true;
        LOG_INFO("[SaveSync] Receiving save: {} bytes", pkt->totalSize);
    }

    // Write chunk into buffer
    if (pkt->chunkOffset + pkt->chunkSize <= s_RecvBuffer.size()) {
        memcpy(s_RecvBuffer.data() + pkt->chunkOffset, pkt->data, pkt->chunkSize);
        s_RecvBytesGot = pkt->chunkOffset + pkt->chunkSize;
    }

    // Log progress every ~10%
    float pct = (float)s_RecvBytesGot / (float)s_RecvTotalSize * 100.0f;
    static int lastPctLog = -1;
    int pctInt = (int)(pct / 10) * 10;
    if (pctInt != lastPctLog) {
        lastPctLog = pctInt;
        LOG_INFO("[SaveSync] Receiving: {:.0f}% ({}/{} bytes)", pct, s_RecvBytesGot, s_RecvTotalSize);
    }

    // Final chunk — write to disk
    if (pkt->flags & 1) {
        s_Receiving = false;
        lastPctLog = -1;

        std::string coopDir = GetCoopSaveDir();
        if (coopDir.empty()) {
            LOG_ERROR("[SaveSync] Can't determine save directory");
            return;
        }

        // Create CoopSync save directory
        std::filesystem::create_directories(coopDir);

        // Write the save file
        std::string outPath = coopDir + "\\mainSave.bsg";
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            LOG_ERROR("[SaveSync] Can't write {}", outPath);
            return;
        }
        out.write(reinterpret_cast<const char*>(s_RecvBuffer.data()), s_RecvBuffer.size());
        out.close();

        // Write a minimal .bsh header so the game recognizes it
        std::string bshPath = GetSaveDir() + "\\CoopSync.bsh";
        std::ofstream bsh(bshPath, std::ios::binary | std::ios::trunc);
        if (bsh.is_open()) {
            // Minimal header — the game just needs the file to exist
            const char header[] = "CoopSync Save";
            bsh.write(header, sizeof(header));
            bsh.close();
        }

        s_RecvBuffer.clear();

        // Send ack
        SaveTransferAckData ack{};
        ack.bytesReceived = s_RecvBytesGot;
        ack.fileIndex = 0;
        ack.status = 1; // complete
        NetSendRawPacket(PacketType::SaveTransferAck, &ack, sizeof(ack));

        LOG_INFO("[SaveSync] Save received and written to {}", outPath);
        LOG_INFO("[SaveSync] >>> Load 'CoopSync' from the in-game menu to sync worlds! <<<");
    }
}

void OnSaveAckReceived(const void* data, int size)
{
    if (size < sizeof(SaveTransferAckData)) return;
    const SaveTransferAckData* ack = reinterpret_cast<const SaveTransferAckData*>(data);

    if (ack->status == 1) {
        LOG_INFO("[SaveSync] Partner confirmed save received ({} bytes)", ack->bytesReceived);
    }
}

} // namespace bs1sdk
