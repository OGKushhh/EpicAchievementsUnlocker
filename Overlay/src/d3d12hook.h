#pragma once
#include "pch.h"

namespace D3D12Hook {
    void Init();     // Call from Overlay::Init async thread when d3d12.dll is loaded
    void Shutdown(); // Call from Overlay::Shutdown
}
