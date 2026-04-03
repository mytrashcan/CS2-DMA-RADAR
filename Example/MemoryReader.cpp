#include "MemoryReader.h"
#include "Globals.h"
#include <DMALibrary/Memory/Memory.h>
#include <vector>
#include <iostream>
#include <thread>

#pragma comment(lib, "winmm.lib")

// Velocity tracking
static std::array<Vector3, MAX_PLAYERS + 1> lastPositions = {};

// Bones to read
static const std::vector<int> targetBones = {
    Head, Neck, Spine, Pelvis,
    LeftShoulder, LeftElbow, LeftHand, RightShoulder, RightElbow, RightHand,
    LeftHip, LeftKnee, LeftFeet, RightHip, RightKnee, RightFeet
};

// ============================================================
// Thread 1: Pointer Resolution (runs every PTR_UPDATE_INTERVAL_MS)
// Resolves entity pointers using multi-stage scatter reads.
// Stores results in cachedPlayers vector.
// ============================================================
void PtrResolverThread() {
    while (isRunning) {
        uintptr_t entitySystem = 0;
        if (!mem.Read(clientBase + Offsets::dwGameEntitySystem, &entitySystem, sizeof(entitySystem)) || entitySystem == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        auto hScatter = mem.CreateScatterHandle();
        if (!hScatter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // --- Stage 1: Read all list entries (upper index) ---
        std::array<uintptr_t, MAX_PLAYERS + 1> listEntries = {};
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            mem.AddScatterReadRequest(hScatter, entitySystem + (8 * (i >> 9) + 0x10), &listEntries[i], sizeof(uintptr_t));
        }
        mem.ExecuteReadScatter(hScatter);

        // --- Stage 2: Read all controllers ---
        std::array<uintptr_t, MAX_PLAYERS + 1> controllers = {};
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (listEntries[i] == 0) continue;
            mem.AddScatterReadRequest(hScatter, listEntries[i] + 112 * (i & 0x1FF), &controllers[i], sizeof(uintptr_t));
        }
        mem.ExecuteReadScatter(hScatter);

        // --- Stage 3: Read pawn handles ---
        std::array<uint32_t, MAX_PLAYERS + 1> pawnHandles = {};
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (controllers[i] == 0) continue;
            mem.AddScatterReadRequest(hScatter, controllers[i] + Offsets::m_hPlayerPawn, &pawnHandles[i], sizeof(uint32_t));
        }
        mem.ExecuteReadScatter(hScatter);

        // --- Stage 4: Read pawn list entries ---
        std::array<uintptr_t, MAX_PLAYERS + 1> pawnListEntries = {};
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (pawnHandles[i] == 0 || pawnHandles[i] == 0xFFFFFFFF) continue;
            mem.AddScatterReadRequest(hScatter, entitySystem + (8 * ((pawnHandles[i] & 0x7FFF) >> 9) + 0x10), &pawnListEntries[i], sizeof(uintptr_t));
        }
        mem.ExecuteReadScatter(hScatter);

        // --- Stage 5: Read pawn addresses ---
        std::array<uintptr_t, MAX_PLAYERS + 1> pawnAddrs = {};
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (pawnListEntries[i] == 0) continue;
            mem.AddScatterReadRequest(hScatter, pawnListEntries[i] + 112 * (pawnHandles[i] & 0x1FF), &pawnAddrs[i], sizeof(uintptr_t));
        }
        mem.ExecuteReadScatter(hScatter);

        // --- Stage 6: Read GameSceneNode + team ---
        std::array<uintptr_t, MAX_PLAYERS + 1> sceneNodes = {};
        std::array<int, MAX_PLAYERS + 1> teams = {};
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (pawnAddrs[i] == 0) continue;
            mem.AddScatterReadRequest(hScatter, pawnAddrs[i] + Offsets::m_pGameSceneNode, &sceneNodes[i], sizeof(uintptr_t));
            mem.AddScatterReadRequest(hScatter, pawnAddrs[i] + Offsets::m_iTeamNum, &teams[i], sizeof(int));
        }
        mem.ExecuteReadScatter(hScatter);

        // --- Stage 7: Read bone array pointer ---
        std::array<uintptr_t, MAX_PLAYERS + 1> boneArrays = {};
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (sceneNodes[i] == 0) continue;
            mem.AddScatterReadRequest(hScatter, sceneNodes[i] + Offsets::m_modelState + Offsets::m_pBoneArray, &boneArrays[i], sizeof(uintptr_t));
        }
        mem.ExecuteReadScatter(hScatter);

        mem.CloseScatterHandle(hScatter);

        // Build the new cached data
        std::vector<CachedPlayerPtr> newCache(MAX_PLAYERS + 1);
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (pawnAddrs[i] != 0 && boneArrays[i] != 0) {
                newCache[i].pawnAddr = pawnAddrs[i];
                newCache[i].boneArray = boneArrays[i];
                newCache[i].team = teams[i];
                newCache[i].valid = true;
            }
        }

        // Swap into shared cache
        {
            std::lock_guard<std::mutex> lock(cachedPlayersMtx);
            cachedPlayers = std::move(newCache);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(PTR_UPDATE_INTERVAL_MS));
    }
}

// ============================================================
// Thread 2: Real-time Data Reader (no sleep, max speed)
// Uses cached pointers to scatter-read position, health, bones.
// ============================================================
void DataReaderThread() {
    timeBeginPeriod(1);

    struct RealtimeRequest {
        int index;
        int health;
        Vector3 pos;
        std::array<Vector3, 30> bones;
        bool valid;
    };
    std::vector<RealtimeRequest> requests(MAX_PLAYERS + 1);

    while (isRunning) {
        // Snapshot the cached pointers
        std::vector<CachedPlayerPtr> snapshot;
        {
            std::lock_guard<std::mutex> lock(cachedPlayersMtx);
            snapshot = cachedPlayers;
        }

        auto hScatter = mem.CreateScatterHandle();
        if (!hScatter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        int validCount = 0;
        for (int i = 1; i <= MAX_PLAYERS; i++) {
            requests[i].valid = false;
            if (!snapshot[i].valid) continue;

            uintptr_t pawn = snapshot[i].pawnAddr;
            uintptr_t boneArr = snapshot[i].boneArray;

            // Scatter: health, position
            mem.AddScatterReadRequest(hScatter, pawn + Offsets::m_iHealth, &requests[i].health, sizeof(int));
            mem.AddScatterReadRequest(hScatter, pawn + Offsets::m_vOldOrigin, &requests[i].pos, sizeof(Vector3));

            // Scatter: bones
            for (int boneIdx : targetBones) {
                mem.AddScatterReadRequest(hScatter, boneArr + (boneIdx * 32), &requests[i].bones[boneIdx], sizeof(Vector3));
            }

            requests[i].index = i;
            requests[i].valid = true;
            validCount++;
        }

        if (validCount == 0) {
            mem.CloseScatterHandle(hScatter);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Single scatter execute for ALL player data
        mem.ExecuteReadScatter(hScatter);
        mem.CloseScatterHandle(hScatter);

        // Process results into double buffer
        int writeIdx = (readIndex.load() == 0) ? 1 : 0;
        auto& buffer = doubleBuffer[writeIdx];

        for (int i = 1; i <= MAX_PLAYERS; i++) {
            auto& req = requests[i];
            auto& p = buffer[i];

            if (req.valid && req.health > 0 && req.health <= 100) {
                p.isValid = true;
                p.health = req.health;
                p.team = snapshot[i].team;
                p.rawPos = req.pos;

                // Velocity calculation
                Vector3 velocity = p.rawPos - lastPositions[i];
                if (std::abs(velocity.x) > 50.0f || std::abs(velocity.y) > 50.0f) velocity = { 0, 0, 0 };
                if (std::abs(velocity.x) < 0.1f) velocity.x = 0;
                if (std::abs(velocity.y) < 0.1f) velocity.y = 0;

                // Position prediction
                p.targetPos = p.rawPos + (velocity * PREDICTION_SCALE);

                // Bone prediction
                for (int boneIdx : targetBones) {
                    p.boneRaw[boneIdx] = req.bones[boneIdx];
                    p.boneTarget[boneIdx] = p.boneRaw[boneIdx] + (velocity * PREDICTION_SCALE);
                }

                lastPositions[i] = p.rawPos;
            }
            else {
                p.isValid = false;
            }
        }
        readIndex.store(writeIdx);
    }
    timeEndPeriod(1);
}

void StartMemoryThread() {
    std::thread(PtrResolverThread).detach();
    std::thread(DataReaderThread).detach();
}
