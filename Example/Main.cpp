#include <iostream>
#include <DMALibrary/Memory/Memory.h>
#include "Globals.h"
#include "MemoryReader.h"
#include "Overlay.h"

int main()
{
    // 1. DMA Init
    if (!mem.Init("cs2.exe", true, true)) {
        std::cout << "[ERROR] DMA Init Failed!" << std::endl;
        return 1;
    }
    mem.FixCr3();
    clientBase = mem.GetBaseDaddy("client.dll");
    std::cout << "[INFO] DMA Initialized. Base: " << std::hex << clientBase << std::dec << std::endl;

    // 2. Memory Thread
    StartMemoryThread();
    std::cout << "[INFO] Scatter Memory Thread Started (Prediction + Bones)" << std::endl;

    // 3. Overlay
    if (InitOverlay("FuserDX")) {
        std::cout << "[INFO] Overlay Started. (Smooth + 32 Players)" << std::endl;
        OverlayLoop();
    }
    else {
        std::cout << "[ERROR] Overlay Init Failed!" << std::endl;
    }

    CleanupOverlay();
    isRunning = false;
    return 0;
}