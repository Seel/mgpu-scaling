#pragma once

// Frames in flight. Every per-frame resource (the swap chain's back buffers, the D3D11 shared
// textures, the bridge's staging slots and mirrors, the FSR 4.1 outputs) is indexed by the FFX
// swap chain's back-buffer index and exists DX12SwapChain::frameSlots times: 2, or 3 with
// bExtraFrameInFlight. Arrays are sized for the most.
//
// Why 3 can be faster: FFX's Present for frame N returns only once the frame that used the next
// back buffer is on screen. With 2 buffers that is frame N-1, so the game cannot start frame N+1
// until frame N-1 went through the whole chain (render GPU, PCIe copy, display GPU, frame
// generation and pacing); with 3 it is frame N-2, one frame time later. Costs one frame of latency.
inline constexpr unsigned int kMaxFrameSlots = 3;
