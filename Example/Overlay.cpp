#include "Overlay.h"
#include "Globals.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <vector>
#include <DMALibrary/Memory/Memory.h> 

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")

// 전역 변수
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND hwndOverlay = nullptr;

// 렌더링용 로컬 상태 (보간)
static std::array<Vector3, MAX_PLAYERS + 1> currentRenderPos = { 0 };

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    if (msg == WM_SIZE && g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
        if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
        g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* pBackBuffer; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView); pBackBuffer->Release();
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd; ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2; sd.BufferDesc.Width = 0; sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT createDeviceFlags = 0; D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK) return false;
    ID3D11Texture2D* pBackBuffer; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView); pBackBuffer->Release(); return true;
}

bool InitOverlay(const char* windowName) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, windowName, NULL };
    RegisterClassEx(&wc);
    hwndOverlay = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT, windowName, "ESP", WS_POPUP, 0, 0, screenW, screenH, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(hwndOverlay, 0, 255, LWA_ALPHA);
    if (!CreateDeviceD3D(hwndOverlay)) { CleanupOverlay(); return false; }
    ShowWindow(hwndOverlay, SW_SHOWDEFAULT); UpdateWindow(hwndOverlay);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwndOverlay); ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    return true;
}

void CleanupOverlay() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
}

void DrawSkeleton(ImDrawList* drawList, PlayerData& p, const VMatrix& viewMatrix, ImU32 color) {
    static const std::vector<std::pair<int, int>> bonePairs = {
        {Head, Neck}, {Neck, Spine}, {Spine, Pelvis},
        {Neck, LeftShoulder}, {LeftShoulder, LeftElbow}, {LeftElbow, LeftHand},
        {Neck, RightShoulder}, {RightShoulder, RightElbow}, {RightElbow, RightHand},
        {Pelvis, LeftHip}, {LeftHip, LeftKnee}, {LeftKnee, LeftFeet},
        {Pelvis, RightHip}, {RightHip, RightKnee}, {RightKnee, RightFeet}
    };

    for (const auto& pair : bonePairs) {
        Vector3 pos1 = p.boneRender[pair.first];
        Vector3 pos2 = p.boneRender[pair.second];
        if (pos1.IsZero() || pos2.IsZero()) continue;

        Vector2 s1, s2;
        if (WorldToScreen(pos1, s1, viewMatrix) && WorldToScreen(pos2, s2, viewMatrix)) {
            drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), color, 1.0f);
        }
    }
}

void OverlayLoop() {
    const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    MSG msg; ZeroMemory(&msg, sizeof(msg));

    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg); continue;
        }

        VMatrix viewMatrix = mem.Read<VMatrix>(clientBase + Offsets::dwViewMatrix);

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        int idx = readIndex.load();
        auto& players = doubleBuffer[idx];
        float deltaTime = ImGui::GetIO().DeltaTime;

        for (int i = 1; i <= MAX_PLAYERS; i++) {
            if (!players[i].isValid) {
                currentRenderPos[i] = { 0,0,0 };
                for (auto& b : players[i].boneRender) b = { 0,0,0 };
                continue;
            }

            if (currentRenderPos[i].IsZero()) {
                currentRenderPos[i] = players[i].targetPos;
                players[i].boneRender = players[i].boneTarget;
            }

            // [보간] 박스 및 뼈
            currentRenderPos[i] = Lerp(currentRenderPos[i], players[i].targetPos, deltaTime * SMOOTH_SPEED);
            static const std::vector<int> allBones = {
                Head, Neck, Spine, Pelvis, LeftShoulder, LeftElbow, LeftHand,
                RightShoulder, RightElbow, RightHand, LeftHip, LeftKnee, LeftFeet, RightHip, RightKnee, RightFeet
            };
            for (int boneIdx : allBones) {
                players[i].boneRender[boneIdx] = Lerp(players[i].boneRender[boneIdx], players[i].boneTarget[boneIdx], deltaTime * SMOOTH_SPEED);
            }

            Vector3 headPos = { currentRenderPos[i].x, currentRenderPos[i].y, currentRenderPos[i].z + 72.0f };
            Vector2 screenHead, screenFeet;

            if (WorldToScreen(headPos, screenHead, viewMatrix) &&
                WorldToScreen(currentRenderPos[i], screenFeet, viewMatrix)) {

                ImU32 color = (players[i].team == 2) ? IM_COL32(255, 50, 50, 255) : IM_COL32(50, 100, 255, 255);

                // Box
                float h = screenFeet.y - screenHead.y;
                float w = h / 2.0f;
                drawList->AddRect(ImVec2(screenHead.x - w / 2, screenHead.y), ImVec2(screenHead.x + w / 2, screenHead.y + h), color, 0.0f, 0, 1.5f);

                // Skeleton
                DrawSkeleton(drawList, players[i], viewMatrix, IM_COL32(255, 255, 255, 200));

                // HP Bar
                float hpHeight = h * (players[i].health / 100.0f);
                drawList->AddRectFilled(ImVec2(screenHead.x - w / 2 - 4, screenHead.y + h - hpHeight), ImVec2(screenHead.x - w / 2 - 2, screenHead.y + h), IM_COL32(0, 255, 0, 255));
            }
        }

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(0, 0);
    }
}