#pragma once
#include <windows.h>
#include <atomic>
#include <cmath>
#include <array>
#include <vector>
#include <mutex>

// [Settings]
inline int screenW = 2560;
inline int screenH = 1440;
inline const int MAX_PLAYERS = 64;
inline const float SMOOTH_SPEED = 18.0f;
inline const float PREDICTION_SCALE = 1.8f;
inline const int PTR_UPDATE_INTERVAL_MS = 3000; // ptr thread update interval

// [������] Client.dll.json ���� ������Ʈ��
namespace Offsets {
    constexpr ptrdiff_t dwGameEntitySystem = 0x1FB89D0;
    constexpr ptrdiff_t dwLocalPlayerController = 0x1E1DC18;
    constexpr ptrdiff_t dwViewMatrix = 0x1E323D0;

    constexpr ptrdiff_t m_hPlayerPawn = 0x8FC;
    constexpr ptrdiff_t m_iHealth = 0x34C;
    constexpr ptrdiff_t m_vOldOrigin = 0x15A0;
    constexpr ptrdiff_t m_iTeamNum = 0x3EB;

    // �� ����ڴ��� ã���� ������ ���� ��
    constexpr ptrdiff_t m_pGameSceneNode = 0x330; // 816 -> 0x330
    constexpr ptrdiff_t m_modelState = 0x190;     // 400 -> 0x190
    constexpr ptrdiff_t m_pBoneArray = 0x80;      // ������
}

// [�� �ε���]
enum BoneIndex : int {
    Head = 6, Neck = 5, Spine = 4, Pelvis = 0,
    LeftShoulder = 8, LeftElbow = 9, LeftHand = 10,
    RightShoulder = 13, RightElbow = 14, RightHand = 15,
    LeftHip = 22, LeftKnee = 23, LeftFeet = 24,
    RightHip = 25, RightKnee = 26, RightFeet = 27
};

// [���� ����ü]
struct Vector3 {
    float x, y, z;
    Vector3 operator+(const Vector3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vector3 operator-(const Vector3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vector3 operator*(float s) const { return { x * s, y * s, z * s }; }
    bool IsZero() const { return x == 0 && y == 0 && z == 0; }
};
struct Vector2 { float x, y; };
struct VMatrix { float m[16]; };

// [�÷��̾� ������]
struct PlayerData {
    bool isValid = false;
    int health = 0;
    int team = 0;
    Vector3 rawPos = { 0, 0, 0 };
    Vector3 targetPos = { 0, 0, 0 };
    Vector3 renderPos = { 0, 0, 0 }; // ȭ���

    // ���̷��� ������ (30�� ��)
    std::array<Vector3, 30> boneRaw;
    std::array<Vector3, 30> boneTarget;
    std::array<Vector3, 30> boneRender;
};

// Cached player pointer data (updated by ptr thread every few seconds)
struct CachedPlayerPtr {
    uintptr_t pawnAddr = 0;
    uintptr_t boneArray = 0;
    int team = 0;
    bool valid = false;
};

// [Global State]
inline std::array<std::array<PlayerData, MAX_PLAYERS + 1>, 2> doubleBuffer;
inline std::atomic<int> readIndex(0);
inline std::atomic<bool> isRunning(true);
inline uintptr_t clientBase = 0;

// Shared ptr cache between threads
inline std::vector<CachedPlayerPtr> cachedPlayers(MAX_PLAYERS + 1);
inline std::mutex cachedPlayersMtx;

// [���� �Լ�]
inline Vector3 Lerp(const Vector3& start, const Vector3& end, float t) {
    if (t > 1.0f) t = 1.0f;
    return { start.x + (end.x - start.x) * t, start.y + (end.y - start.y) * t, start.z + (end.z - start.z) * t };
}

inline bool WorldToScreen(const Vector3& pos, Vector2& out, const VMatrix& matrix) {
    float _x = matrix.m[0] * pos.x + matrix.m[1] * pos.y + matrix.m[2] * pos.z + matrix.m[3];
    float _y = matrix.m[4] * pos.x + matrix.m[5] * pos.y + matrix.m[6] * pos.z + matrix.m[7];
    float w = matrix.m[12] * pos.x + matrix.m[13] * pos.y + matrix.m[14] * pos.z + matrix.m[15];

    if (w < 0.001f) return false;

    float inv_w = 1.f / w;
    _x *= inv_w;
    _y *= inv_w;

    float x = screenW * 0.5f;
    float y = screenH * 0.5f;

    x += 0.5f * _x * screenW + 0.5f;
    y -= 0.5f * _y * screenH + 0.5f;

    out.x = x;
    out.y = y;
    return true;
}