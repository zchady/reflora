// The replay's state commands (split from nvn_draw.cpp): binds, viewports, uniform updates, counters; draws and
// dispatches go back to nvn_draw.cpp.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "timeline.h"
#include "nvn_draw_state.h"
#include "nvn_tiling.h"
#include <nx/game.h>
#include <nx/runtime.h>
#include "platform.h"

namespace nx::nvn {

// SetRenderEnableConditional (NVN 2 builds; NX_COND_RENDER=0|1 forces it). The game wraps its clears in it: "clear
// this target only the first time this frame" is SetRenderEnableConditional(RENDER_IF_NOT_EQUAL, A), the clear, then a
// TIMESTAMP_TOP report into A (counter 0, equal to A + 16's: later clears of it are skipped until the CPU resets A).
// It was taken as always on, and clears ignored render enable: the SSAO pass was cleared again between its two draws,
// the AO target stayed 0, and characters read "fully occluded" (dark clothes, bluish white). The hardware compares the
// two 64-bit counters at the address and 16 bytes on, in command order, and it gates clears as well as draws.
bool conditional_render_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_COND_RENDER");
        if (e) return e[0] == '1';
        return gob_layout() == GobLayout::Nvn2;
    }();
    return on;
}

using namespace replay;

void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets) {
    State& s = g_state;
    auto blob = [&](size_t size) -> const uint8_t* {
        return c.blob < list.blobs.size() && list.blobs[c.blob].size() >= size ? list.blobs[c.blob].data() : nullptr;
    };
    switch (c.op) {
        case Cmd::SetViewport:
            std::memcpy(s.viewport, c.i, sizeof s.viewport), s.has_viewport = true;
            break;
        case Cmd::SetRasterizerDiscard: s.rasterizer_discard = c.n != 0; break;
        case Cmd::BindImage:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 16) s.images[c.i[0]][c.i[1]] = c.a[0];
            break;
        case Cmd::DispatchCompute: dispatch_call(cb, c); break;
        case Cmd::SetRenderEnable:
            s.render_enable = c.n != 0;
            if ((c.mask & 1) && conditional_render_on())  // SetRenderEnableConditional, evaluated in command order
                if (const uint8_t* p = guest_bytes(c.a[0])) {
                    uint64_t a = 0, b = 0;
                    std::memcpy(&a, p, 8), std::memcpy(&b, p + 16, 8);
                    s.render_enable = c.i[0] == 0 ? a == b : a != b;  // RENDER_IF_EQUAL 0, RENDER_IF_NOT_EQUAL 1
                }
            if (c.mask & 1) {  // SetRenderEnableConditional: log what the condition would read (NX_TRACE_COND_RENDER)
                static const bool trace = std::getenv("NX_TRACE_COND_RENDER") != nullptr;
                static int traced = 0;
                if (trace && traced++ < 200)
                    if (const uint8_t* p = guest_bytes(c.a[0])) {
                        uint64_t v[4];
                        std::memcpy(v, p, sizeof v);
                        log("cond render: mode %d at %llx: %llx %llx | %llx %llx (after draw %llu)", c.i[0],
                            (unsigned long long)c.a[0], (unsigned long long)v[0], (unsigned long long)v[1],
                            (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)g_stats.done);
                    }
            }
            break;
        case Cmd::SetScissor:
            std::memcpy(s.scissor, c.i, sizeof s.scissor), s.has_scissor = true;
            break;
        case Cmd::SetDepthRange:
            s.depth_near = c.f[0], s.depth_far = c.f[1];
            break;
        case Cmd::BindProgram: s.program = c.a[0]; break;
        case Cmd::BindUniformBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 18) s.uniforms[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            break;
        case Cmd::BindStorageBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 16) s.storage[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            break;
        case Cmd::BindDeferred: {
            const DeferredValues* dv = current_deferred();
            auto it = dv ? dv->find(c.a[0]) : DeferredValues::const_iterator{};
            uint32_t words = c.mask >= 2 ? 2 : 1;
            int st = c.i[0];
            if (st < 0 || st >= kStages) break;
            for (uint32_t k = 0; k < c.n; k++) {
                auto value = [&](uint32_t w) {  // the submit's snapshot, else the memory now
                    size_t at = size_t(k) * words + w;
                    return dv && it != dv->end() && at < it->second.size() ? it->second[at]
                                                                            : ld<uint64_t>(c.a[0] + 8 * at);
                };
                int slot = c.i[1] + int(k);
                if (c.mask == 0 && slot >= 0 && slot < 32) s.textures[st][slot] = value(0);
                if (c.mask == 1 && slot >= 0 && slot < 16) s.images[st][slot] = value(0);
                if (c.mask == 2 && slot >= 0 && slot < 18) s.uniforms[st][slot] = {value(0), value(1)};
                if (c.mask == 3 && slot >= 0 && slot < 16) s.storage[st][slot] = {value(0), value(1)};
            }
            break;
        }
        case Cmd::BindTexture:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 32) s.textures[c.i[0]][c.i[1]] = c.a[0];
            break;
        case Cmd::BindVertexBuffer:
            if (c.i[0] >= 0 && c.i[0] < 16) s.vertex_buffers[c.i[0]] = {c.a[0], c.a[1]};
            break;
        case Cmd::SetTexturePool: s.texture_pool = c.a[0]; break;
        case Cmd::SetSamplerPool: s.sampler_pool = c.a[0]; break;
        case Cmd::BindVertexAttribs:
            if (auto* p = blob(c.n * sizeof(VertexAttrib)))
                s.attribs.assign(reinterpret_cast<const VertexAttrib*>(p), reinterpret_cast<const VertexAttrib*>(p) + c.n);
            break;
        case Cmd::BindVertexStreams:
            if (auto* p = blob(c.n * sizeof(VertexStream)))
                s.streams.assign(reinterpret_cast<const VertexStream*>(p), reinterpret_cast<const VertexStream*>(p) + c.n);
            break;
        case Cmd::BindBlendState:
            if (auto* p = blob(sizeof(BlendTarget))) {
                BlendTarget b;
                std::memcpy(&b, p, sizeof b);
                if (b.target < 8) s.blend[b.target] = b;
            }
            break;
        case Cmd::BindColorState:
            if (auto* p = blob(sizeof(ColorStateInfo))) std::memcpy(&s.color, p, sizeof s.color);
            break;
        case Cmd::BindChannelMaskState:
            if (auto* p = blob(sizeof(ChannelMaskInfo))) std::memcpy(&s.mask, p, sizeof s.mask);
            break;
        case Cmd::BindDepthStencilState:
            if (auto* p = blob(sizeof(DepthStencilInfo))) std::memcpy(&s.depth, p, sizeof s.depth);
            break;
        case Cmd::BindPolygonState:
            if (auto* p = blob(sizeof(PolygonInfo))) std::memcpy(&s.polygon, p, sizeof s.polygon);
            break;
        case Cmd::BindMultisampleState: s.alpha_to_coverage = c.n != 0; break;
        case Cmd::SetBlendColor: std::memcpy(s.blend_color, c.f, sizeof s.blend_color); break;
        case Cmd::UpdateUniformBuffer: {  // (buffer address, size, offset, data): written in order, like the GPU
            uint64_t off = c.a[2];
            // NX_TRACE_UUB=<first draw>: logs 40 updates from that draw on.
            static const char* uub = std::getenv("NX_TRACE_UUB");
            static const uint64_t uub_from = uub ? std::strtoull(uub, nullptr, 10) : 0;
            static int uub_n = 0;
            if (uub && g_stats.done >= uub_from && uub_n++ < 1500 && c.blob < list.blobs.size()) {
                const auto& b = list.blobs[c.blob];
                float f[8] = {};
                std::memcpy(f, b.data(), std::min<size_t>(b.size(), sizeof f));
                log("uub before draw %llu: buffer %llx size %llu offset %llu data %zu bytes: %g %g %g %g | %g %g %g %g",
                    (unsigned long long)g_stats.done, (unsigned long long)c.a[0], (unsigned long long)c.a[1],
                    (unsigned long long)off, b.size(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
            if (c.blob < list.blobs.size())
                if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0] + off))) {
                    std::memcpy(p, list.blobs[c.blob].data(), list.blobs[c.blob].size());
                    dedup_invalidate(p, list.blobs[c.blob].size());
                    written_cpu_write(p, list.blobs[c.blob].size());
                }
            break;
        }
        case Cmd::ReportCounter: {  // {value, timestamp}: the work before it is done when the replay gets here
            static uint64_t reports = 0, types_seen = 0;
            if (c.n < 64 && !(types_seen >> c.n & 1)) types_seen |= 1ull << c.n, log("nvn: first counter report of type %u", c.n);
            if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0]))) {
                // NX_FAKE_GPU_TIME=1: every report of a submit carries the submit's start (a GPU with no cost):
                // the game picks its present interval from the timings it measures (bisecting its 30 fps mode).
                static const bool fake = std::getenv("NX_FAKE_GPU_TIME") != nullptr;
                // Less the host-only work of this frame's replay (nvn_pipeline_cache.cpp): not GPU time on the console;
                // and less the time the submit waited to be handed over (nvn_queue.cpp, submit_at): the console's GPU
                // starts on it when it is submitted, and the game measures its frame time from these reports.
                uint64_t now = fake ? submit_start_ns() : plat::monotonic_ns() - host_work_ns() - replay_delay_ns();
                // NX_GPU_TIME_SCALE=<x>: the time since the submit's start reads x times longer (a slow GPU on a fast
                // one: puts the game in its dynamic resolution, as on the Mali, WONDER_SWITCH2_PROGRESS.md §5zd18).
                static const double scale = std::getenv("NX_GPU_TIME_SCALE") ? std::atof(std::getenv("NX_GPU_TIME_SCALE")) : 0;
                if (!fake && scale > 0 && now > submit_start_ns())
                    now = submit_start_ns() + uint64_t(double(now - submit_start_ns()) * scale);
                static const uint64_t trace_reports = std::getenv("NX_TRACE_REPORTS") ? std::strtoull(std::getenv("NX_TRACE_REPORTS"), nullptr, 10) : 0;
                static int traced_reports = 0;
                if (trace_reports && g_frame >= trace_reports && traced_reports < 60)
                    traced_reports++, log("report frame %llu type %u at %.3f ms: host work %.3f ms, hand-over delay %.3f ms",
                                          (unsigned long long)g_frame.load(), c.n, double(now % 1000000000ull) / 1e6,
                                          double(host_work_ns()) / 1e6, double(replay_delay_ns()) / 1e6);
                uint64_t v[2] = {++reports, now};
                if (game::kInfo.tick_hz) v[1] = now / 1000000000u * game::kInfo.tick_hz + now % 1000000000u * game::kInfo.tick_hz / 1000000000u;
                // TIMESTAMP (0) and TIMESTAMP_TOP (0x10) reports put 0 in the counter field, as the hardware does;
                // games compare those words for conditional rendering (conditional_render_on below).
                if (conditional_render_on() && (c.n == 0 || c.n == 0x10)) v[0] = 0;
                std::memcpy(p, v, sizeof v);
                dedup_invalidate(p, sizeof v);
                written_cpu_write(p, sizeof v);
                if (tl::on()) tl::record(tl::kNvn, tl::kReport, c.a[0], c.n, nullptr);
            }
            break;
        }
        case Cmd::SignalEvent:  // the work before it is done when the replay gets here, as for ReportCounter
            if (c.a[0]) {
                uint32_t* p = reinterpret_cast<uint32_t*>(c.a[0]);
                uint32_t v = c.n == 1 ? *p + uint32_t(c.a[1]) : uint32_t(c.a[1]);
                __atomic_store_n(p, v, __ATOMIC_RELEASE);
                written_cpu_write(reinterpret_cast<const uint8_t*>(p), 4);
            }
            break;
        case Cmd::DrawArrays: draw_call(cb, targets, c.n, false, c); break;
        case Cmd::DrawElements: draw_call(cb, targets, c.n, true, c); break;
        default: raster_command(c); break;
    }
}

}  // namespace nx::nvn
