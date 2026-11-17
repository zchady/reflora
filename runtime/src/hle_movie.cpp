// movie::VideoDecoder (subsdk0, nn::movie of SDK 21) on the CPU. On the console the class drives the video engine
// (NVDEC) through the nv driver; its Initialize aborts in nv::InitCheck in process. Wonder's Switch 2 Edition plays
// WebM movies (VP9 960x540 + Vorbis) with it: the demuxer (nn::movie::MediaExtractor) and the Vorbis decoder
// (movie::AudioDecoder) are the SDK's own CPU code and run as recompiled; only the video decoder is replaced, here,
// with libvpx (movie_vpx.cpp).
//
// What the game does with the class (main+0x2293500..0x2298100, read from its code):
//   C1, Initialize(mem, size, mime, output format 3, mode 1), Configure(properties, codec config, size)
//   frames: VideoFrame objects of its own (0x90 bytes), SetPixelStorageType(1), SetBuffer(width * height * 3 / 2),
//           then RegisterVideoFrame(frames, count); GetInputReadyEvent / GetOutputReadyEvent (SystemEventType**)
//   feeding: DecodeAsync(nn::movie::Buffer*) per sample from the extractor; 0 = taken, anything else = later (it
//           waits on the input event); buffer flag 0x8 = end of stream
//   output:  AcquireVideoFrame(&frame) until it gives null; per frame GetEndOfStreamState, GetTimeStamp, GetBuffer
//           and the filled sizes of planes 0 and 1, which it copies in one memcpy into a texture: NV12, Y then
//           interleaved UV, rows of `width` bytes; ReleaseVideoFrame gives the frame back
//   Start / Stop / Flush (seek), Finalize, D1.
// VideoFrame fields (subsdk0 accessors): +0x08 buffer, +0x28 + 8 * plane filled size, +0x48 buffer size,
// +0x78 time stamp (nn::TimeSpan), +0x84 pixel storage type, +0x88 end of stream (u8).
// nn::movie::Buffer: +0x00 data, +0x08 capacity, +0x10 size, +0x18 time stamp, +0x20 flags.
//
// NX_MOVIE=vpx (default): decode with libvpx. NX_MOVIE=skip: EXPERIMENTAL BYPASS, no picture is decoded and the
// first sample ends the movie (for testing past movies only; it is not movie support). NX_MOVIE=sdk: the SDK's own
// class (aborts in process). Without libvpx, vpx falls back to skip and says so in the log.
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "kernel.h"
#include "movie_vpx.h"

namespace nx {

namespace {

constexpr uint32_t kBusy = (1u << 9) | 0x1F7u;  // DecodeAsync "not now" (any non-zero Result; the game waits)
constexpr uint32_t kBufferEos = 0x8;

enum class Mode { Vpx, Skip, Sdk };

Mode mode() {
    static const Mode m = [] {
        const char* e = std::getenv("NX_MOVIE");
        if (e && std::strcmp(e, "skip") == 0) return Mode::Skip;
        if (e && std::strcmp(e, "sdk") == 0) return Mode::Sdk;
        return Mode::Vpx;
    }();
    return m;
}

struct Event {
    uint64_t guest = 0;  // the SystemEventType handed to the game
    std::shared_ptr<EventState> state = std::make_shared<EventState>();
};

struct Decoder {
    std::mutex lock;
    Event input, output;
    std::vector<uint64_t> frames;          // registered VideoFrame*
    std::deque<uint64_t> free_frames, ready;
    movie::Vp9Decoder vpx;
    bool skip = false, eos_sent = false;
    uint64_t samples = 0, pictures = 0, errors = 0, dropped = 0;
};

std::mutex g_lock;
std::map<uint64_t, std::shared_ptr<Decoder>> g_decoders;

std::shared_ptr<Decoder> find(uint64_t obj) {
    std::lock_guard lk(g_lock);
    auto it = g_decoders.find(obj);
    return it == g_decoders.end() ? nullptr : it->second;
}

// A guest SystemEventType backed by a kernel event whose state we set (manual clear), as hle_audio.cpp does.
void make_event(CPU& c, Event& e) {
    e.guest = kernel_alloc(0x100);
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    r->state = w->state = e.state;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = e.guest, c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // manual clear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
}

// Under d.lock: event states follow the queues (input: a frame is free; output: a frame is ready).
void update_events(Decoder& d) {
    d.input.state->signaled = !d.free_frames.empty() && !d.eos_sent;
    d.output.state->signaled = !d.ready.empty();
    notify_all();
}

void fill_frame(uint64_t f, const movie::Picture* p, int64_t ts, bool eos, Decoder& d) {
    uint64_t buf = ld<uint64_t>(f + 0x08), size = ld<uint64_t>(f + 0x48);
    uint64_t y = 0, uv = 0;
    if (p) {
        if (movie::i420_to_nv12(*p, reinterpret_cast<uint8_t*>(buf), size)) {
            y = movie::nv12_y_size(p->width, p->height), uv = movie::nv12_uv_size(p->width, p->height);
        } else if (d.errors++ < 5) {
            log("movie: %ux%u picture does not fit a %llu-byte frame", p->width, p->height, (unsigned long long)size);
        }
    }
    for (int k = 0; k < 4; k++) st<uint64_t>(f + 0x28 + 8 * k, 0);
    st<uint64_t>(f + 0x28, y);
    st<uint64_t>(f + 0x30, uv);
    st<int64_t>(f + 0x78, ts);
    st<uint8_t>(f + 0x88, eos ? 1 : 0);
}

void C1(CPU& c) {  // movie::VideoDecoder::VideoDecoder(): a 0x50-byte object the game allocates
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x50);
}

void D1(CPU& c) {
    std::lock_guard lk(g_lock);
    g_decoders.erase(c.x[0]);
}

// Result Initialize(void* memory, size_t size, MediaMimeType, VideoDecoderOutputFormat, VideoCodecMode)
void Initialize(CPU& c) {
    uint64_t obj = c.x[0];
    auto d = std::make_shared<Decoder>();
    make_event(c, d->input);
    make_event(c, d->output);
    std::string err;
    d->skip = mode() == Mode::Skip;
    if (!d->skip && !d->vpx.open(&err)) {
        log("movie: VP9 decoder unavailable (%s): EXPERIMENTAL BYPASS, movies end at once", err.c_str());
        d->skip = true;
    }
    if (d->skip)
        log("movie: NX_MOVIE=skip or no libvpx: EXPERIMENTAL BYPASS, this movie is not decoded and ends at once");
    log("movie: video decoder %llx: mime %llu, output format %llu, mode %llu, %s", (unsigned long long)obj,
        (unsigned long long)c.x[3], (unsigned long long)c.x[4], (unsigned long long)c.x[5],
        d->skip ? "bypass" : "libvpx VP9 -> NV12");
    std::lock_guard lk(g_lock);
    g_decoders[obj] = d;
    c.x[0] = 0;
}

void ok(CPU& c) { c.x[0] = 0; }

void RegisterVideoFrame(CPU& c) {  // (VideoFrame** frames, int count)
    auto d = find(c.x[0]);
    if (d) {
        std::lock_guard lk(d->lock);
        int n = int(c.x[2]);
        for (int k = 0; k < n; k++) {
            uint64_t f = ld<uint64_t>(c.x[1] + 8 * uint64_t(k));
            if (!f) continue;
            d->frames.push_back(f);
            d->free_frames.push_back(f);
        }
        update_events(*d);
    }
    c.x[0] = 0;
}

void GetInputReadyEvent(CPU& c) {  // (SystemEventType** out)
    auto d = find(c.x[0]);
    if (d && c.x[1]) st<uint64_t>(c.x[1], d->input.guest);
    c.x[0] = d ? 0 : kBusy;
}

void GetOutputReadyEvent(CPU& c) {
    auto d = find(c.x[0]);
    if (d && c.x[1]) st<uint64_t>(c.x[1], d->output.guest);
    c.x[0] = d ? 0 : kBusy;
}

void Start(CPU& c) {
    if (auto d = find(c.x[0])) {
        std::lock_guard lk(d->lock);
        d->eos_sent = false;
        update_events(*d);
    }
    c.x[0] = 0;
}

// Flush (before a seek) and Stop: frames waiting for the game go back to the free list; a new decoder starts the
// stream from the next key frame.
void Flush(CPU& c) {
    if (auto d = find(c.x[0])) {
        std::lock_guard lk(d->lock);
        for (uint64_t f : d->ready) d->free_frames.push_back(f);
        d->ready.clear();
        d->eos_sent = false;
        if (!d->skip) {
            std::string err;
            d->vpx.open(&err);
        }
        update_events(*d);
    }
    c.x[0] = 0;
}

void Finalize(CPU& c) {
    if (auto d = find(c.x[0])) {
        std::lock_guard lk(d->lock);
        log("movie: video decoder %llx finished: %llu samples, %llu pictures, %llu errors, %llu dropped%s",
            (unsigned long long)c.x[0], (unsigned long long)d->samples, (unsigned long long)d->pictures,
            (unsigned long long)d->errors, (unsigned long long)d->dropped, d->skip ? " (bypass)" : "");
        d->vpx.close();
        d->free_frames.clear();
        d->ready.clear();
        update_events(*d);
    }
    c.x[0] = 0;
}

// Result DecodeAsync(const nn::movie::Buffer*): decoded right away, into a free frame.
void DecodeAsync(CPU& c) {
    auto d = find(c.x[0]);
    if (!d) {
        c.x[0] = kBusy;
        return;
    }
    uint64_t b = c.x[1];
    std::lock_guard lk(d->lock);
    if (d->free_frames.empty() || d->eos_sent) {
        update_events(*d);
        c.x[0] = kBusy;
        return;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(ld<uint64_t>(b));
    uint64_t size = ld<uint64_t>(b + 0x10);
    int64_t ts = ld<int64_t>(b + 0x18);
    uint32_t flags = ld<uint32_t>(b + 0x20);
    d->samples++;
    bool eos = (flags & kBufferEos) != 0 || d->skip;
    if (!d->skip && data && size) {
        std::string err;
        if (!d->vpx.decode(data, size, &err)) {
            if (d->errors++ < 5) log("movie: VP9 sample %llu: %s", (unsigned long long)d->samples, err.c_str());
        } else {
            movie::Picture p;
            while (d->vpx.next_picture(p)) {
                if (d->free_frames.empty()) {  // cannot happen with one picture per sample; counted if it does
                    d->dropped++;
                    continue;
                }
                uint64_t f = d->free_frames.front();
                d->free_frames.pop_front();
                fill_frame(f, &p, ts, false, *d);
                d->ready.push_back(f);
                d->pictures++;
            }
        }
    }
    if (eos) {
        if (d->free_frames.empty()) {  // the end-of-stream marker needs a frame: take this sample again later
            update_events(*d);
            c.x[0] = kBusy;
            d->samples--;
            return;
        }
        uint64_t f = d->free_frames.front();
        d->free_frames.pop_front();
        fill_frame(f, nullptr, ts, true, *d);
        d->ready.push_back(f);
        d->eos_sent = true;
    }
    update_events(*d);
    c.x[0] = 0;
}

void AcquireVideoFrame(CPU& c) {  // (VideoFrame** out): null when none is ready
    auto d = find(c.x[0]);
    uint64_t f = 0;
    if (d) {
        std::lock_guard lk(d->lock);
        if (!d->ready.empty()) {
            f = d->ready.front();
            d->ready.pop_front();
        }
        update_events(*d);
    }
    if (c.x[1]) st<uint64_t>(c.x[1], f);
    c.x[0] = 0;
}

void ReleaseVideoFrame(CPU& c) {  // (VideoFrame*)
    if (auto d = find(c.x[0])) {
        std::lock_guard lk(d->lock);
        uint64_t f = c.x[1];
        bool known = false;
        for (uint64_t g : d->frames) known |= g == f;
        if (known) d->free_frames.push_back(f);
        update_events(*d);
    }
    c.x[0] = 0;
}

struct Registrar {
    Registrar() {
        if (mode() == Mode::Sdk) return;
        static const struct {
            const char* name;
            HleFn fn;
        } table[] = {
            {"_ZN5movie12VideoDecoderC1Ev", C1},
            {"_ZN5movie12VideoDecoderD1Ev", D1},
            {"_ZN5movie12VideoDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24VideoDecoderOutputFormatENS_14VideoCodecModeE",
             Initialize},
            {"_ZN5movie12VideoDecoder9ConfigureEPKN2nn5movie13MediaPropertyEPKvm", ok},
            {"_ZN5movie12VideoDecoder18RegisterVideoFrameEPPNS_10VideoFrameEi", RegisterVideoFrame},
            {"_ZN5movie12VideoDecoder18GetInputReadyEventEPPN2nn2os15SystemEventTypeE", GetInputReadyEvent},
            {"_ZN5movie12VideoDecoder19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE", GetOutputReadyEvent},
            {"_ZN5movie12VideoDecoder5StartEv", Start},
            {"_ZN5movie12VideoDecoder4StopEv", Flush},
            {"_ZN5movie12VideoDecoder5FlushEv", Flush},
            {"_ZN5movie12VideoDecoder8FinalizeEv", Finalize},
            {"_ZN5movie12VideoDecoder11DecodeAsyncEPKN2nn5movie6BufferE", DecodeAsync},
            {"_ZN5movie12VideoDecoder17AcquireVideoFrameEPPNS_10VideoFrameE", AcquireVideoFrame},
            {"_ZN5movie12VideoDecoder17ReleaseVideoFrameEPNS_10VideoFrameE", ReleaseVideoFrame},
        };
        for (const auto& e : table) HleRegistrar(e.name, e.fn);  // registers into the loader's table
    }
};

const Registrar g_registrar;  // NX_MOVIE=sdk leaves the SDK's class in place

}  // namespace

}  // namespace nx
