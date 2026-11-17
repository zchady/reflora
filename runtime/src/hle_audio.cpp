// nn::audio AudioOut at the API level. FMOD (inside main) mixes s16 stereo 48 kHz into AudioOutBuffers and appends
// them; a player thread hands each one to the platform's audio device (XAudio2 / AAudio / SDL3) and gives it back to
// the game (released + system event signalled) when it has been played, so the device clock paces FMOD's mixer.
// Without a device the same pacing runs on a timer. Audio input reports no devices. SDK 16 games (Wonder) use the
// AudioRenderer instead, rendered in process; its final mix goes to the same device (see below).
#include <algorithm>
#include <atomic>
#include <string>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "collect.h"
#include "kernel.h"
#include "platform.h"

namespace nx {

namespace {

constexpr uint64_t kBytesPerSecond = 48000 * 2 * 2;
constexpr uint32_t kMaxInFlight = 4;  // buffers queued in the device (4 x ~21 ms)

// Our AudioOutBuffer layout (only our HLE reads it): {void* data; u64 capacity; u64 size}.
struct Out {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<uint64_t> queued, released;
    std::shared_ptr<EventState> event = std::make_shared<EventState>();
    bool started = false;
};
Out g_out;

void release(void* ctx) {
    std::lock_guard lk(g_out.lock);
    g_out.released.push_back(reinterpret_cast<uint64_t>(ctx));
    g_out.event->signaled = true;
    notify_all();
    g_out.cv.notify_all();
}

// The platform device has one completion callback: AudioOut buffers are guest addresses, the renderer's blocks
// live in g_render (below) and go back to its free list.
void render_block_done(void* ctx);
bool render_owns(void* ctx);

void device_done(void* ctx) {
    if (render_owns(ctx))
        render_block_done(ctx);
    else
        release(ctx);
}

bool device_open() {
    static const bool ok = plat::audio_open(device_done);
    return ok;
}

void player() {
    bool device = device_open();
    log("audio: %s", device ? "device output (48 kHz stereo s16)" : "no audio device, timer pacing");
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        uint64_t buf = 0, size = 0;
        {
            std::unique_lock lk(g_out.lock);
            auto ready = [&] {
                return g_out.started && !g_out.queued.empty() && (!device || plat::audio_queued() < kMaxInFlight);
            };
            g_out.cv.wait_for(lk, std::chrono::milliseconds(5), ready);
            if (!ready()) continue;
            buf = g_out.queued.front();
            g_out.queued.pop_front();
            size = ld<uint64_t>(buf + 16);
        }
        if (device && plat::audio_submit(reinterpret_cast<const void*>(ld<uint64_t>(buf)), size,
                                         reinterpret_cast<void*>(buf)))
            continue;
        next = std::max(next, clock::now() - std::chrono::milliseconds(50));
        next += std::chrono::microseconds(size * 1000000 / kBytesPerSecond);
        std::this_thread::sleep_until(next);
        release(reinterpret_cast<void*>(buf));
    }
}

void InitializeAudioOutParameter(CPU& c) {  // {u32 sample_rate; u16 channels; u16 reserved}
    st<uint32_t>(c.x[0], 0);
    st<uint16_t>(c.x[0] + 4, 0);
    st<uint16_t>(c.x[0] + 6, 0);
}

void OpenDefaultAudioOut(CPU& c) {  // (AudioOut*, os::SystemEvent*, const AudioOutParameter&)
    uint64_t out = c.x[0], sysev = c.x[1];
    // Give the guest a system event backed by our kernel event: nn::os::AttachSystemEvent(type, read, managed,
    // write, managed, clear mode = manual).
    auto w = std::make_shared<KEvent>();
    auto r = std::make_shared<KEvent>();
    w->state = r->state = g_out.event;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = sysev;
    c.x[1] = hr;
    c.x[2] = 1;
    c.x[3] = hw;
    c.x[4] = 1;
    c.x[5] = 1;  // manual clear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    st<uint64_t>(out, 0xA0D10);  // opaque
    static std::once_flag once;
    std::call_once(once, [] { std::thread(player).detach(); });
    c.x[0] = 0;
}

void StartAudioOut(CPU& c) {
    std::lock_guard lk(g_out.lock);
    g_out.started = true;
    g_out.cv.notify_all();
    c.x[0] = 0;
}

void StopAudioOut(CPU&) {
    std::lock_guard lk(g_out.lock);
    g_out.started = false;
}

void CloseAudioOut(CPU&) {}

void SetAudioOutBufferInfo(CPU& c) {  // (AudioOutBuffer*, void* data, size_t capacity, size_t size)
    st<uint64_t>(c.x[0], c.x[1]);
    st<uint64_t>(c.x[0] + 8, c.x[2]);
    st<uint64_t>(c.x[0] + 16, c.x[3]);
}

void GetAudioOutBufferDataPointer(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0]); }

void AppendAudioOutBuffer(CPU& c) {  // (AudioOut*, AudioOutBuffer*)
    std::lock_guard lk(g_out.lock);
    g_out.queued.push_back(c.x[1]);
    g_out.cv.notify_all();
    c.x[0] = 1;  // bool: appended
}

void GetReleasedAudioOutBuffer(CPU& c) {
    std::lock_guard lk(g_out.lock);
    if (g_out.released.empty()) {
        c.x[0] = 0;
        return;
    }
    c.x[0] = g_out.released.front();
    g_out.released.pop_front();
    if (g_out.released.empty()) g_out.event->signaled = false;
}

void ListAudioIns(CPU& c) { c.x[0] = 0; }
void audio_in_fail(CPU& c) { c.x[0] = (1u << 9) | 153u; }  // audio: not found
void zero(CPU& c) { c.x[0] = 0; }

// SDK 16 (Super Mario Bros. Wonder): the AudioRenderer. The sdk carries the renderer's server side too and can
// reach it in process ("direct function call") instead of over IPC to audren:u: hand out that one.
// Both return nn::sf::SharedPointer<IAudioRendererManager> through x8.
void CreateAudioRendererManagerByHipc(CPU& c) {
    static uint64_t dfc = find_symbol("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if (!dfc) fatal("CreateAudioRendererManagerByDfc not found");
    log("audio: AudioRenderer manager in process (DFC) instead of audren:u");
    call(c, dfc);
}

// SDK 21 (Wonder, Switch 2 Edition): CreateAudioRendererManager(bool dfc) picks one of the two; the parameter's mode
// (+0x2c == 1) decides, so the game asks for the IPC one (the aud:u service). Always in process, as for SDK 16.
void CreateAudioRendererManager(CPU& c) {
    static uint64_t real = find_symbol("_ZN2nn5audio26CreateAudioRendererManagerEb");
    if (!real) fatal("CreateAudioRendererManager not found");
    static bool once = (log("audio: CreateAudioRendererManager forced to the in-process (DFC) manager"), true);
    (void)once;
    c.x[0] = 1;
    call(c, real);
}

// The AudioRenderer, in process (DFC). On the console the ADSP renders one audio frame every 5 ms and signals the
// renderer's system event, which paces the game's sound thread. In process nothing does that on its own: the
// renderer object (AudioRendererManagerImpl::AudioRendererImpl) has ExecuteAudioRendererRendering, which generates
// the frame's commands and runs the sdk's own DSP command processor on the calling thread, and its render system
// (impl + 0x60) signals its own event, which has no kernel object in process. A guest thread of ours renders every
// 5 ms and signals the game's event itself, then sends the frame's final mix to the host device.
constexpr uint64_t kRendererVtable = 0xd04910;     // sdk: vtable of the sf object wrapping AudioRendererImpl
constexpr uint64_t kExecuteRendering = 0x20cac8;   // sdk: AudioRendererImpl::ExecuteAudioRendererRendering

// The in-process render system hands the client a readable handle of 0 for the renderer's event (there is no
// kernel event behind it), so the SystemEvent the game waits on is ours: an InterProcessEventType keeps its
// readable handle at +0x14 and its writable one at +0x18 (nn::os::detail::WaitInterProcessEvent, sdk+1305a0).
std::shared_ptr<EventState> g_audio_frame = std::make_shared<EventState>();

// sysmix "room for data" events (AcquireStreamDataEvent): each stream gets its own event, signalled with every
// rendered frame only while the stream holds it. After ReleaseStreamDataEvent / CloseStream it is never signalled
// again, as on the console: a stream thread still waiting on it then only times out. Sharing g_audio_frame woke
// such a thread after the game had already closed the event (a movie cut short by the online-play question on a new
// file), and its ResetSignal on the closed handle aborted the game with 0xe401.
struct StreamDataEvent {
    uint64_t sysev, ctx;
    std::shared_ptr<EventState> state;
};
std::mutex g_stream_events_lock;
std::vector<StreamDataEvent> g_stream_events;

void signal_stream_data_events() {
    std::lock_guard lk(g_stream_events_lock);
    for (auto& e : g_stream_events) e.state->signaled = true;
}

void forget_stream_data_events(uint64_t sysev, uint64_t ctx) {  // 0 matches any
    std::lock_guard lk(g_stream_events_lock);
    std::erase_if(g_stream_events, [&](const StreamDataEvent& e) {
        return (!sysev || e.sysev == sysev) && (!ctx || e.ctx == ctx);
    });
}

std::mutex g_renderers_lock;
std::vector<uint64_t> g_renderers;  // AudioRendererImpl*

uint64_t sdk_base() {
    static uint64_t base = [] {
        const LoadedModule* m = module_at(find_symbol("nvnBootstrapLoader"));
        return m ? m->base : 0;
    }();
    return base;
}

// A symbol of the sdk module by name (any binding), as an address; 0 if absent. SDK 21 (Wonder's Switch 2 Edition)
// keeps the renderer's vtable and ExecuteAudioRendererRendering as named symbols; SDK 16's addresses are the fallback.
uint64_t sdk_symbol(const char* prefix, const char* contains = nullptr) {
    const LoadedModule* m = module_at(sdk_base());
    if (!m) return 0;
    size_t n = std::strlen(prefix);
    for (size_t k = 0; k < m->info->symbol_count; k++) {
        const Symbol& s = m->info->symbols[k];
        if (s.imported || std::strncmp(s.name, prefix, n) != 0) continue;
        if (contains && !std::strstr(s.name, contains)) continue;
        return m->base + s.value;
    }
    return 0;
}

uint64_t renderer_vtable() {
    static uint64_t v = [] {
        uint64_t s = sdk_symbol("_ZTVN2nn2sf6detail38ObjectImplFactoryWithStatefulAllocatorINS0_13InterfaceInfoINS_5audio6"
                                "detail14IAudioRendererEE", "AudioRendererManagerImpl17AudioRendererImpl");
        log("audio: renderer vtable %s", s ? describe(s).c_str() : "(SDK 16 address)");
        return s ? s : sdk_base() + kRendererVtable;
    }();
    return v;
}

uint64_t execute_rendering() {
    static uint64_t f = [] {
        uint64_t s = sdk_symbol("_ZN2nn5audio6server24AudioRendererManagerImpl17AudioRendererImpl29ExecuteAudioRendererRenderingEv");
        return s ? s : sdk_base() + kExecuteRendering;
    }();
    return f;
}

// Where the render system keeps its execution mode and rendering device (the next word): AudioRenderSystem::
// ExecuteAudioRendererRendering starts by loading both (SDK 21: `ldp w8, w9, [x0, #0x48]`). SDK 16 kept them at
// +0x2d0, which in SDK 21 is the render system's mutex: writing there deadlocked StartAudioRenderer.
uint64_t mode_offset() {
    static uint64_t off = [] {
        uint64_t f = sdk_symbol("_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv");
        uint32_t w = f ? ld<uint32_t>(f) : 0;
        if ((w & 0xFFC00000) == 0x29400000 && ((w >> 5) & 31) == 0) {  // LDP Wt, Wt2, [X0, #imm7*4]
            int32_t imm = int32_t((w >> 15) & 0x7F);
            if (imm & 0x40) imm -= 0x80;
            log("audio: execution mode at render system +%#x (read from the sdk)", imm * 4);
            return uint64_t(imm * 4);
        }
        return uint64_t(0x2d0);
    }();
    return off;
}

// The AudioRendererImpl behind an AudioRendererHandle: an sf object whose vtable pointer is that one, within two
// pointer hops of the handle.
uint64_t find_renderer(uint64_t handle) {
    uint64_t vptr = renderer_vtable() + 0x10;
    auto valid = [](uint64_t p) { return p >= kModuleBase && p < kKernelBase + kKernelSize && (p & 7) == 0; };
    for (uint64_t a = 0; a < 0x40; a += 8) {
        uint64_t p = ld<uint64_t>(handle + a);
        if (!valid(p)) continue;
        if (ld<uint64_t>(p) == vptr) return p + 0x10;
        for (uint64_t b = 0; b < 0x40; b += 8) {
            uint64_t q = ld<uint64_t>(p + b);
            if (valid(q) && ld<uint64_t>(q) == vptr) return q + 0x10;
        }
    }
    return 0;
}

// ---- the renderer's final mix to the host device ----
// The command list ends with a DeviceSink command (id 19) naming the final mix's buffers, but the sdk's DSP command
// processor skips it: on the console the ADSP's sink stage hands those buffers to the output device. So
// CommandListProcessor::Process is wrapped: after the real one has run the list, the sink's input mix buffers (s32
// samples in the PCM16 range, one buffer per channel) are summed into g_render.mix; the render thread converts each
// 5 ms frame to s16 stereo and queues it on the device. Command header: {u32 magic 0xcafebabe; u8 enabled; s8 id;
// s16 size; u32 estimated time}. DeviceSink (CommandBuffer::Generate<DeviceSink>, sdk+211e10): +0x128 the mix
// buffers (or the upsampler's), +0x130 input count, +0x134 s16 buffer index per input.
constexpr uint8_t kCmdDownMix6chTo2ch = 17, kCmdDeviceSink = 19;
constexpr int kFrame = 240;          // samples per 5 ms at 48 kHz
constexpr int kBlocks = 64;          // 320 ms of output blocks
constexpr uint32_t kMaxQueued = 12;  // 60 ms in the device queue: past that, frames are dropped
constexpr size_t kPrime = 4;         // after an underrun, 20 ms are gathered before playing again
// Rendering (5 ms timer) and the device keep the same rate, so the queue stays at whatever depth the start left it:
// 10-11 blocks (50-55 ms) in run 137, audio ~40 ms behind the picture. Above this target, wholly silent blocks are
// dropped (no click: nothing is heard either way) until the queue is back at it. NX_AUDIO_TARGET_MS overrides (30).
uint32_t target_queued() {
    static const uint32_t t = [] {
        const char* e = std::getenv("NX_AUDIO_TARGET_MS");
        int ms = e ? std::atoi(e) : 30;
        return uint32_t(std::clamp(ms / 5, 2, int(kMaxQueued)));
    }();
    return t;
}

struct RenderOut {
    std::mutex lock;
    int32_t mix[kFrame * 2] = {};
    bool mixed = false;
    int16_t blocks[kBlocks][kFrame * 2];
    std::vector<int> free, held;
    bool priming = true;
    // statistics since the last log, and the optional dump (NX_AUDIO_DUMP=file.wav: NX_AUDIO_DUMP_SECONDS (10) from
    // the first sound, or from NX_AUDIO_DUMP_FROM seconds of rendering on)
    uint64_t frames = 0, dropped = 0, underruns = 0, silent = 0, submitted = 0, trimmed = 0, log_ns = 0;
    uint32_t log_queued = 0;
    // the current run of silent blocks (§5zd9): when it began, and the longest the game went without an audio update
    // meanwhile (its sound thread stalled, or not)
    uint64_t silence_ns = 0, silence_gap_ns = 0;
    double sum_sq = 0;
    int peak = 0;
    std::vector<int16_t> dump;
    RenderOut() {
        for (int k = kBlocks - 1; k >= 0; k--) free.push_back(k);
    }
};
RenderOut g_render;

bool render_owns(void* ctx) {
    auto p = static_cast<const uint8_t*>(ctx), b = reinterpret_cast<const uint8_t*>(g_render.blocks);
    return p >= b && p < b + sizeof g_render.blocks;
}

void render_block_done(void* ctx) {
    std::lock_guard lk(g_render.lock);
    g_render.free.push_back(int((static_cast<int16_t*>(ctx) - g_render.blocks[0]) / (kFrame * 2)));
}

// Two command header formats. SDK 16 (Wonder): {u32 magic 0xcafebabe; u8 enabled; u8 id; s16 size; ...}, the device
// sink is id 19 (name +0x20, buffers +0x128, input count +0x130, s16 buffer index per input +0x134), 6->2 downmix id 17.
// SDK 21 (Wonder, Switch 2 Edition): no magic, {u8 enabled; u8 id; u16 size; u32}, the device sink is id 20 (0x140 bytes,
// "MainAudioOut" at +0x18: every field 8 bytes earlier). Read from a live list (run 135, §5t).
struct CmdFormat {
    bool v2;
    uint32_t enabled, id, size;  // offsets in the header
    int32_t shift;               // the sink's fields against SDK 16's
    uint8_t sink, downmix;       // ids (downmix 0: not known)
};
constexpr CmdFormat kCmdV1{false, 4, 5, 6, 0, kCmdDeviceSink, kCmdDownMix6chTo2ch};
constexpr CmdFormat kCmdV2{true, 0, 1, 2, -8, 20, 0};

void capture_device_sink(uint64_t proc) {  // proc: nn::audio::dsp::CommandListProcessor after Process
    uint64_t cmd = ld<uint64_t>(proc) + 0x20;  // the list header, then the commands (+8 is the cursor: at the end)
    uint32_t count = ld<uint32_t>(proc + 0x1c), samples = ld<uint32_t>(proc + 0x20), rate = ld<uint32_t>(proc + 0x28);
    uint32_t frames = rate == 48000 ? samples : kFrame;  // other rates: the upsampler's 48 kHz buffers
    const CmdFormat& f = ld<uint32_t>(cmd) == 0xcafebabe ? kCmdV1 : kCmdV2;
    bool downmixed = false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = f.v2 ? ld<uint16_t>(cmd + f.size) : uint32_t(ld<int16_t>(cmd + f.size));
        if (size < 8 || size > 0x10000 || (!f.v2 && ld<uint32_t>(cmd) != 0xcafebabe)) break;  // not a command
        uint64_t at = cmd;
        cmd += size;
        if (!ld<uint8_t>(at + f.enabled)) continue;
        uint8_t id = ld<uint8_t>(at + f.id);
        if (f.downmix && id == f.downmix) downmixed = true;
        if (id != f.sink) continue;
        uint64_t sink = at + int64_t(f.shift);  // so that the SDK 16 offsets below apply
        uint64_t buffers = ld<uint64_t>(sink + 0x128);
        uint32_t inputs = ld<uint32_t>(sink + 0x130);
        static int logged = 0;
        if (logged < 3) {
            logged++;
            const int32_t* b0 = buffers && inputs ? reinterpret_cast<const int32_t*>(
                buffers + uint64_t(ld<int16_t>(sink + 0x134)) * frames * 4) : nullptr;
            log("audio: device sink '%.16s' (SDK %s format), %u inputs (buffer indices %d %d), %u samples at %u Hz%s; "
                "first samples %d %d %d %d", reinterpret_cast<const char*>(sink + 0x20), f.v2 ? "21" : "16", inputs,
                ld<int16_t>(sink + 0x134), ld<int16_t>(sink + 0x136), samples, rate,
                downmixed ? " (downmixed to stereo)" : "", b0 ? b0[0] : 0, b0 ? b0[1] : 0, b0 ? b0[60] : 0,
                b0 ? b0[120] : 0);
        }
        if (!buffers || !inputs || inputs > 24 || frames != kFrame) continue;
        auto ch = [&](uint32_t k) {
            return reinterpret_cast<const int32_t*>(buffers + uint64_t(ld<int16_t>(sink + 0x134 + 2 * k)) * frames * 4);
        };
        const int32_t* l = ch(0);
        const int32_t* r = inputs > 1 ? ch(1) : l;
        // Six channels without the sdk's own downmix (FL FR FC LFE RL RR): fold them to stereo here.
        const int32_t *c = nullptr, *rl = nullptr, *rr = nullptr;
        if (inputs >= 6 && !downmixed) c = ch(2), rl = ch(4), rr = ch(5);
        std::lock_guard lk(g_render.lock);
        for (int s = 0; s < kFrame; s++) {
            int64_t a = l[s], b = r[s];
            if (c) {
                a += (int64_t(c[s]) + rl[s]) * 707 / 1000;
                b += (int64_t(c[s]) + rr[s]) * 707 / 1000;
            }
            g_render.mix[2 * s] += int32_t(std::clamp<int64_t>(a, INT32_MIN, INT32_MAX));
            g_render.mix[2 * s + 1] += int32_t(std::clamp<int64_t>(b, INT32_MIN, INT32_MAX));
        }
        g_render.mixed = true;
    }
}

// AudioRenderSystem::Update (the game's RequestUpdate) takes the render system's mutex, but GenerateCommand runs
// without it: on the console both run on the audio system's side, one after the other. Here the game's sound thread
// updates while our render thread generates, and an effect being updated mid-generation crashed
// (EffectContext::UpdateStateByDspShared on a null effect). Both are serialized by this lock.
std::mutex g_render_system;

// One update per rendered frame, as on the console: an update copies the game's parameters and a new effect's
// "initialize" status rides on exactly one of them. Our render thread signals the game's audio event every 5 ms
// whether or not it rendered since the last update, so a sound thread that fell behind updated twice in a row, the
// second update overwrote the status, and the next render applied a LightLimiter whose state was never initialized
// (null pointers in ApplyLightLimiterEffect, an intermittent crash at startup; docs/nvn/DEBUG.md #28). An update
// now waits (at most 1 s, outside the lock) until a render ran since the previous one.
std::mutex g_pace_lock;
std::condition_variable g_pace_cv;
uint64_t g_renders = 0, g_renders_at_update = ~0ull;
std::atomic<uint64_t> g_last_update_ns{0};  // the game's last AudioRenderSystem::Update (render_submit's silences)

void AudioRenderSystemUpdate(CPU& c) {
    static uint64_t real = find_symbol("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    {
        std::unique_lock pk(g_pace_lock);
        static uint64_t waited = 0;
        if (g_renders == g_renders_at_update && waited++ < 5) log("audio: update waits for a render");
        // 1 s, not 50 ms: at boot, with every thread loading, a render took longer and the second update went through
        // (the limiter crash again, once in ~25 Windows runs). The render thread runs every 5 ms: 1 s only bounds a stall.
        if (!g_pace_cv.wait_for(pk, std::chrono::seconds(1), [] { return g_renders != g_renders_at_update; }))
            log("audio: no render for 1 s since the last update: updating anyway");
    }
    std::lock_guard lk(g_render_system);
    call(c, real);
    // Counted after the update, under the render lock (renders count themselves under it too): a render that began
    // before this update and ended after it no longer passes for one that used it (the limiter crash, DEBUG-2 #74).
    std::lock_guard pk(g_pace_lock);
    g_renders_at_update = g_renders;
    g_last_update_ns.store(plat::monotonic_ns(), std::memory_order_relaxed);
}

void CommandListProcessorProcess(CPU& c) {
    static uint64_t real = find_symbol("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    uint64_t proc = c.x[0];
    call(c, real);
    capture_device_sink(proc);
}

void write_wav(const char* path, const std::vector<int16_t>& pcm) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f), u32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(2), u32(48000), u32(48000 * 4), u16(4), u16(16);
    std::fwrite("data", 1, 4, f), u32(bytes), std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    log("audio: wrote %s (%zu frames)", path, pcm.size() / 2);
}

// Called by the render thread after each 5 ms frame.
void render_submit() {
    static const bool device = device_open();
    static const char* dump_path = std::getenv("NX_AUDIO_DUMP");
    std::lock_guard lk(g_render.lock);
    RenderOut& o = g_render;
    if (!o.mixed) return;
    o.mixed = false;
    int16_t pcm[kFrame * 2];
    bool any = false;
    for (int s = 0; s < kFrame * 2; s++) {
        int v = std::clamp(o.mix[s], -32768, 32767);
        pcm[s] = int16_t(v);
        o.mix[s] = 0;
        o.peak = std::max(o.peak, std::abs(v));
        o.sum_sq += double(v) * v;
        any |= v != 0;
    }
    o.silent += !any;
    // Silences one by one (§5zd9): a run of silent blocks of 250 ms or more, with when it ended (the event's time) and
    // the longest the game went without updating its audio meanwhile. A collection report sets them against the
    // hitches: a silence inside a stall is not the game's choice, even with the device never running dry.
    if (const uint64_t now_ns = plat::monotonic_ns(); !any) {
        if (!o.silence_ns) o.silence_ns = now_ns, o.silence_gap_ns = 0;
        const uint64_t last = g_last_update_ns.load(std::memory_order_relaxed);
        if (last && now_ns > last) o.silence_gap_ns = std::max(o.silence_gap_ns, now_ns - last);
    } else if (o.silence_ns) {
        const uint64_t len = now_ns - o.silence_ns;
        if (len >= 250'000'000ull) {
            collect::event("audio", "silence\t%.2f s\tgame audio updates paused up to %.0f ms", double(len) / 1e9,
                           double(o.silence_gap_ns) / 1e6);
            static int logged = 0;
            if (len >= 500'000'000ull && logged++ < 200)
                log("audio: %.2f s of silence just ended; the game's audio updates paused up to %.0f ms meanwhile",
                    double(len) / 1e9, double(o.silence_gap_ns) / 1e6);
        }
        o.silence_ns = 0;
    }
    static const uint64_t dump_from = std::getenv("NX_AUDIO_DUMP_FROM") ? uint64_t(std::atof(std::getenv("NX_AUDIO_DUMP_FROM")) * 200) : 0;
    static const size_t dump_size = size_t(48000 * 2 * (std::getenv("NX_AUDIO_DUMP_SECONDS") ? std::atof(std::getenv("NX_AUDIO_DUMP_SECONDS")) : 10.0));
    static uint64_t all_frames = 0;
    all_frames++;
    if (dump_path && o.dump.size() < dump_size && all_frames >= dump_from && (dump_from || any || !o.dump.empty())) {
        o.dump.insert(o.dump.end(), pcm, pcm + kFrame * 2);
        if (o.dump.size() >= dump_size) write_wav(dump_path, o.dump);
    }
    if (!o.log_ns) o.log_ns = plat::monotonic_ns();
    if (++o.frames % 2000 == 0) {
        // 2000 frames are 10 s of sound: the wall time they took, and how many the device played meanwhile (submitted
        // minus the growth of its queue). Both near 10 s / 2000: rendering and the device keep real time.
        uint64_t now = plat::monotonic_ns();
        uint32_t queued = device ? plat::audio_queued() : 0;
        int64_t played = int64_t(o.submitted) - (int64_t(queued) - int64_t(o.log_queued));
        log("audio: mix out: peak %d, rms %.0f, %llu of 2000 frames silent, %llu dropped, %llu silent trimmed, "
            "%llu underruns, queue %u; 2000 frames in %.3f s, device played %lld",
            o.peak, std::sqrt(o.sum_sq / (2000.0 * kFrame * 2)), (unsigned long long)o.silent,
            (unsigned long long)o.dropped, (unsigned long long)o.trimmed, (unsigned long long)o.underruns, queued,
            double(now - o.log_ns) / 1e9, (long long)played);
        collect::event("audio", "%llu of 2000 frames silent\t%llu underruns\t%llu dropped\tqueue %u\t%.3f s", (unsigned long long)o.silent, (unsigned long long)o.underruns,
                       (unsigned long long)o.dropped, queued, double(now - o.log_ns) / 1e9);
        o.peak = 0, o.sum_sq = 0, o.silent = o.dropped = o.underruns = o.submitted = o.trimmed = 0;
        o.log_ns = now, o.log_queued = queued;
    }
    if (!device) return;
    uint32_t queued = plat::audio_queued();
    if (queued + o.held.size() >= kMaxQueued || o.free.empty()) {  // the device fell behind: keep latency bounded
        o.dropped++;
        return;
    }
    if (!any && !o.priming && queued + o.held.size() > target_queued()) {  // a silent block above the target depth
        o.trimmed++;
        return;
    }
    if (queued == 0 && !o.priming) {
        o.priming = true, o.underruns++;
        collect::event("audio", "underrun");  // the device ran dry (the moment, to set against the hitches)
    }
    int k = o.free.back();
    o.free.pop_back();
    std::memcpy(o.blocks[k], pcm, sizeof pcm);
    o.held.push_back(k);
    if (o.priming && o.held.size() < kPrime) return;
    o.priming = false;
    for (int h : o.held) plat::audio_submit(o.blocks[h], sizeof pcm, o.blocks[h]);
    o.submitted += o.held.size();
    o.held.clear();
}

void audio_render_thread(CPU& c) {
    log("audio: render thread running");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += 5'000'000;
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        std::vector<uint64_t> list;
        {
            std::lock_guard lk(g_renderers_lock);
            list = g_renderers;
        }
        static uint64_t frames = 0, busy_ns = 0, last_rc = 0;
        uint64_t t0 = plat::monotonic_ns();
        for (uint64_t impl : list) {
            uint64_t saved[31];
            std::memcpy(saved, c.x, sizeof saved);
            c.x[0] = impl;
            std::lock_guard lk(g_render_system);
            call(c, execute_rendering());
            last_rc = c.x[0];
            std::memcpy(c.x, saved, sizeof saved);
            std::lock_guard pk(g_pace_lock);  // counted under the render lock (AudioRenderSystemUpdate)
            g_renders++;
            g_pace_cv.notify_all();
        }
        busy_ns += plat::monotonic_ns() - t0;
        render_submit();
        if (++frames % 2000 == 0)
            log("audio: %llu frames rendered, %.1f us each, last result %llx", (unsigned long long)frames,
                double(busy_ns) / 1000.0 / 2000.0, (unsigned long long)last_rc), busy_ns = 0;
        g_audio_frame->signaled = true;
        signal_stream_data_events();
        notify_all();
    }
}

void start_render_thread(CPU& c) {
    uint64_t create = find_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    uint64_t start = find_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    if (!create || !start) {
        log("audio: nn::os::CreateThread/StartThread not found, no render thread");
        return;
    }
    uint64_t thread = kernel_alloc(0x1000), stack = kernel_alloc(0x40000);
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = thread, c.x[1] = hle_address("__audio_render_thread"), c.x[2] = 0, c.x[3] = stack, c.x[4] = 0x40000;
    c.x[5] = 16;  // high priority, like the audio system's own threads
    call(c, create);
    uint64_t rc = c.x[0];
    c.x[0] = thread;
    if (rc == 0) call(c, start);
    std::memcpy(c.x, saved, sizeof saved);
    if (rc) log("audio: CreateThread failed (%llx)", (unsigned long long)rc);
}

// OpenAudioRenderer(AudioRendererHandle*, os::SystemEvent*, const AudioRendererParameter&, void* work, size_t)
void OpenAudioRenderer(CPU& c) {
    static uint64_t real = find_symbol(
        "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    uint64_t handle = c.x[0], sysev = c.x[1];
    call(c, real);
    if (c.x[0] == 0 && ld<uint32_t>(sysev + 0x14) == 0) {
        auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
        r->state = w->state = g_audio_frame;
        st<uint32_t>(sysev + 0x14, handle_add(r));
        st<uint32_t>(sysev + 0x18, handle_add(w));
    }
    uint64_t rc = c.x[0];
    uint64_t impl = rc == 0 ? find_renderer(handle) : 0;
    log("audio: renderer opened (result %llx), impl %llx", (unsigned long long)rc, (unsigned long long)impl);
    if (!impl) return;
    // ExecuteAudioRendererRendering only renders in the manual execution mode on the CPU rendering device
    // (AudioRenderSystem::GetExecutionMode / GetRenderingDevice == 1, at render system + 0x2d0 / + 0x2d4); the game
    // opens the renderer in the automatic mode, where the ADSP renders on its own. Our 5 ms thread is that ADSP, so
    // the renderer is switched to manual/CPU; otherwise every call failed with 0x40299 and nothing was ever rendered
    // (streamed music never advanced, and the opening cutscene waited for it forever).
    uint64_t system = impl + 0x60, mode = system + mode_offset();
    log("audio: execution mode %u, rendering device %u -> 1, 1", ld<uint32_t>(mode), ld<uint32_t>(mode + 4));
    st<uint32_t>(mode, 1);
    st<uint32_t>(mode + 4, 1);
    {
        std::lock_guard lk(g_renderers_lock);
        g_renderers.push_back(impl);
    }
    static bool started = false;
    if (!started) {
        started = true;
        start_render_thread(c);
    }
    c.x[0] = rc;
}

// The audio device (output) side: one stereo output that never changes.
constexpr const char* kOutputName = "AudioTvOutput";

void AcquireAudioDeviceNotificationForOutput(CPU& c) {  // (os::SystemEvent*): an event that is never signaled
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    r->state = w->state = std::make_shared<EventState>();
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEvent's SystemEventType
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = 0;
}

void ListAudioOutputDeviceName(CPU& c) {  // int (AudioDeviceName* names, int count): 256 chars each
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 1;
}

void GetActiveAudioOutputDeviceName(CPU& c) {  // (AudioDeviceName*)
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 0;
}

void two(CPU& c) { c.x[0] = 2; }


// ---- SDK 21 (Wonder, Switch 2 Edition): nn::audio::sysmix as a null sink -------------------------------------------
// The in-process renderer sends its output to a system-mixer stream (sysmix), which on the console needs the audio
// device manager service (auddmg:u); without it every stream call fails with 0xc8099 and OpenAudioRenderer aborts.
// The PC takes the final mix from the renderer itself (above), so the streams accept everything and play nothing.
// The sdk calls these directly (BL), so they are listed in games/wonder2/hle_internal.txt (recomp --hle-list).
void sysmix_ok(CPU& c) { c.x[0] = 0; }
void sysmix_count_arg(CPU& c) {  // (int* out, ctx, ..., int frames): all of them
    if (c.x[0]) st<int32_t>(c.x[0], int32_t(c.x[3]));
    c.x[0] = 0;
}
void sysmix_count_zero(CPU& c) {  // (int* out, ...): none
    if (c.x[0]) st<int32_t>(c.x[0], 0);
    c.x[0] = 0;
}
void sysmix_write(CPU& c) {  // WriteStream(int* written, ctx, const void* data, int frames)
    static int logged = 0;
    if (logged < 6 && c.x[2] && int32_t(c.x[3]) > 0) {
        logged++;
        const int16_t* h = reinterpret_cast<const int16_t*>(c.x[2]);
        const float* f = reinterpret_cast<const float*>(c.x[2]);
        log("audio: sysmix WriteStream ctx %llx, %d frames; first words s16 %d %d %d %d %d %d %d %d, f32 %g %g %g %g",
            (unsigned long long)c.x[1], int32_t(c.x[3]), h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], f[0], f[1],
            f[2], f[3]);
    }
    if (c.x[0]) st<int32_t>(c.x[0], int32_t(c.x[3]));
    c.x[0] = 0;
}
void sysmix_open(CPU& c) {  // OpenStream(ctx, const StreamFormat& format, int, int, flags)
    const uint32_t* f = reinterpret_cast<const uint32_t*>(c.x[1]);
    log("audio: sysmix OpenStream ctx %llx, format words %08x %08x %08x %08x, args %lld %lld %llx",
        (unsigned long long)c.x[0], f[0], f[1], f[2], f[3], (long long)c.x[2], (long long)c.x[3],
        (unsigned long long)c.x[4]);
    c.x[0] = 0;
}
void sysmix_prime(CPU& c) {  // Prime/PurgeStream(int* out, ctx, int frames)
    if (c.x[0]) st<int32_t>(c.x[0], int32_t(c.x[2]));
    c.x[0] = 0;
}
void sysmix_free_frames(CPU& c) {  // GetStreamFreeFrameCount(int* out, ctx): room for 5 ms at 48 kHz, many times over
    if (c.x[0]) st<int32_t>(c.x[0], 240 * 16);
    c.x[0] = 0;
}
void sysmix_gain(CPU& c) {  // GetStreamGain(float* out, ctx)
    if (c.x[0]) st<float>(c.x[0], 1.0f);
    c.x[0] = 0;
}
void sysmix_state(CPU& c) {  // GetStreamState(StreamState* out, ctx)
    if (c.x[0]) st<uint32_t>(c.x[0], 0);
    c.x[0] = 0;
}
void attach_event(CPU& c, std::shared_ptr<EventState> state) {  // x0: the SystemEvent's SystemEventType
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    r->state = w->state = std::move(state);
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = 0;
}
void sysmix_state_event(CPU& c) { attach_event(c, std::make_shared<EventState>()); }  // never changes
void sysmix_data_event(CPU& c) {  // AcquireStreamDataEvent(SystemEvent*, ctx, mode, int): "room for data", 5 ms
    auto state = std::make_shared<EventState>();
    {
        std::lock_guard lk(g_stream_events_lock);
        g_stream_events.push_back({c.x[0], c.x[1], state});
    }
    attach_event(c, std::move(state));
}
void sysmix_release_data_event(CPU& c) {  // ReleaseStreamDataEvent(SystemEvent*, ctx)
    forget_stream_data_events(c.x[0], c.x[1]);
    c.x[0] = 0;
}
void sysmix_close(CPU& c) {  // CloseStream(ctx) / DestroyStream(ctx): its events stop with it
    forget_stream_data_events(0, c.x[0]);
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", AcquireAudioDeviceNotificationForOutput);
NX_HLE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", ListAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", GetActiveAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", zero);
NX_HLE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", two);
NX_HLE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", InitializeAudioOutParameter);
NX_HLE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", OpenDefaultAudioOut);
NX_HLE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", StartAudioOut);
NX_HLE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", StopAudioOut);
NX_HLE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", CloseAudioOut);
NX_HLE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", SetAudioOutBufferInfo);
NX_HLE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", GetAudioOutBufferDataPointer);
NX_HLE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", AppendAudioOutBuffer);
NX_HLE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", GetReleasedAudioOutBuffer);
NX_HLE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", ListAudioIns);
NX_HLE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", audio_in_fail);
NX_HLE("_ZN2nn5audio26InitializeAudioInParameterEPNS0_16AudioInParameterE", zero);
NX_HLE("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv", CreateAudioRendererManagerByHipc);
NX_HLE("_ZN2nn5audio26CreateAudioRendererManagerEb", CreateAudioRendererManager);
NX_HLE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", OpenAudioRenderer);
NX_HLE("__audio_render_thread", audio_render_thread);
NX_HLE("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv", CommandListProcessorProcess);
NX_HLE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", AudioRenderSystemUpdate);

// SDK 21 sysmix (null sink): reached through the --hle-list entry check, the sdk calls them directly.
NX_HLE("_ZN2nn5audio6sysmix10InitializeEv", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix8FinalizeEv", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix12CreateStreamEPNS1_13StreamContextEjNS_4util17basic_string_viewIcNSt3__111char_traitsIcEEEENS4_10BitFlagSetILi32ENS1_24StreamCreationOptionFlagEEEPvm", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix13DestroyStreamEPNS1_13StreamContextE", sysmix_close);
NX_HLE("_ZN2nn5audio6sysmix10OpenStreamEPNS1_13StreamContextERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS1_20StreamOpenOptionFlagEEE", sysmix_open);
NX_HLE("_ZN2nn5audio6sysmix11CloseStreamEPNS1_13StreamContextE", sysmix_close);
NX_HLE("_ZN2nn5audio6sysmix13EngageStreamsEPKPNS1_13StreamContextEi", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix16DisengageStreamsEPKPNS1_13StreamContextEi", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix12RenameStreamEPNS1_13StreamContextENS_4util17basic_string_viewIcNSt3__111char_traitsIcEEEE", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix13SetStreamGainEfPKNS1_13StreamContextENS_8TimeSpanE", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix11ClockStreamEPNS1_13StreamContextENS_8TimeSpanE", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix19GetStreamStatisticsEPNS1_16StreamStatisticsEPNS1_13StreamContextEb", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix11WriteStreamEPiPNS1_13StreamContextEPKvi", sysmix_write);
NX_HLE("_ZN2nn5audio6sysmix10ReadStreamEPiPvPNS_8TimeSpanEPNS1_13StreamContextEi", sysmix_count_zero);
NX_HLE("_ZN2nn5audio6sysmix11PrimeStreamEPiPNS1_13StreamContextEi", sysmix_prime);
NX_HLE("_ZN2nn5audio6sysmix11PurgeStreamEPiPNS1_13StreamContextEi", sysmix_prime);
NX_HLE("_ZN2nn5audio6sysmix23GetStreamFreeFrameCountEPiPKNS1_13StreamContextE", sysmix_free_frames);
NX_HLE("_ZN2nn5audio6sysmix25GetStreamFilledFrameCountEPiPKNS1_13StreamContextE", sysmix_count_zero);
NX_HLE("_ZN2nn5audio6sysmix13GetStreamGainEPfPKNS1_13StreamContextE", sysmix_gain);
NX_HLE("_ZN2nn5audio6sysmix14GetStreamStateEPNS1_11StreamStateEPKNS1_13StreamContextE", sysmix_state);
NX_HLE("_ZN2nn5audio6sysmix29AcquireStreamStateChangeEventEPNS_2os11SystemEventEPKNS1_13StreamContextE", sysmix_state_event);
NX_HLE("_ZN2nn5audio6sysmix29ReleaseStreamStateChangeEventEPNS_2os11SystemEventEPKNS1_13StreamContextE", sysmix_ok);
NX_HLE("_ZN2nn5audio6sysmix22AcquireStreamDataEventEPNS_2os11SystemEventEPKNS1_13StreamContextENS1_26StreamDataNotificationModeEi", sysmix_data_event);
NX_HLE("_ZN2nn5audio6sysmix22ReleaseStreamDataEventEPNS_2os11SystemEventEPKNS1_13StreamContextE", sysmix_release_data_event);

}  // namespace nx
