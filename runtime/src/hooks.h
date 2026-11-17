// Seams for the settings overlay (overlay.cpp, Windows for now): the shared runtime calls these when they are set, so
// the platforms without the overlay build and run as before. Set once by overlay::install() before the game starts.
#pragma once
#include <cstdint>

#include "platform.h"

namespace nx::hooks {

// The present pass (nvn_present.cpp), inside its render pass on the swapchain image, after the game image: the
// overlay records itself there, so it is in what the window shows and in nothing the game renders or captures.
// cb: VkCommandBuffer; render_pass: VkRenderPass; images: the swapchain's image count.
using PresentOverlay = void (*)(void* cb, uint64_t render_pass, uint32_t width, uint32_t height, uint32_t images);
extern PresentOverlay present_overlay;  // defined in nvn_present.cpp
// Whether the overlay has something to draw this frame (open, or the performance indicator on). On the blit path
// (the PC's default) the present then adds a small render pass of its own over the swapchain image for it; otherwise
// that path is as before.
using OverlayWanted = bool (*)();
extern OverlayWanted overlay_wanted;  // defined in nvn_present.cpp

// Player 1's pad as the platform read it (hle_hid.cpp), before the game sees it: the overlay takes it while it is
// open (and until every button is released after it closes). True: the game gets nothing this time.
using PadFilter = bool (*)(plat::PadState& p);
extern PadFilter pad_filter;  // defined in hle_hid.cpp

// What the performance page shows: the replay's own counters, averaged per frame over its last 60-frame window
// (nvn_vk.cpp log_timing), and the last presented game image. Defined in nvn_vk.cpp.
struct PerfSnapshot {
    double fps = 0, replay_ms = 0, encode_ms = 0, fence_ms = 0, present_ms = 0, upload_mb = 0, submits = 0;
    uint32_t game_w = 0, game_h = 0;  // the image the game presented (its dynamic resolution)
    const char* present_mode = "";
};
PerfSnapshot perf_snapshot();

}  // namespace nx::hooks
