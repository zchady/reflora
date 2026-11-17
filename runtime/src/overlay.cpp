// The settings overlay (overlay.h): Dear ImGui over the present pass, fed by the window and the pad.
#include "overlay.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "nvn/nvn_vk.h"  // first: the runtime's Vulkan headers (platform types) before ImGui's back end includes them
#include "backends/imgui_impl_vulkan.h"
#include "hooks.h"
#include "imgui.h"
#include "platform.h"
#include "settings.h"

namespace nx::nvn {
VkQueue vk_queue();  // nvn_vk.cpp
uint32_t vk_queue_family();
}  // namespace nx::nvn

namespace nx::overlay {

namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_swallow{false};  // after closing: the game gets nothing until every button is released

// ---- input from the window and the pad (other threads) -> the present path ----------------------------------------
struct Event {
    enum Type { kKey, kMousePos, kMouseButton, kWheel, kFocus } type;
    int a = 0;
    bool down = false;
    float x = 0, y = 0;
};
std::mutex g_input_lock;
std::deque<Event> g_events;
plat::PadState g_pad;  // the last raw pad state (pad_filter)

void push(const Event& e) {
    std::lock_guard lk(g_input_lock);
    if (g_events.size() < 512) g_events.push_back(e);
}

// Minus + Plus held together toggles; the pad is the game's again once released (pad_filter, game thread).
bool pad_filter(plat::PadState& p) {
    static bool combo = false;
    {
        std::lock_guard lk(g_input_lock);
        g_pad = p;
    }
    const bool both = (p.buttons & plat::kMinus) && (p.buttons & plat::kPlus);
    if (both && !combo) toggle();
    combo = both;
    if (g_open.load()) return true;
    if (g_swallow.load()) {
        if (p.buttons == 0 && std::abs(p.lx) < 4000 && std::abs(p.ly) < 4000) g_swallow = false;
        else return true;
    }
    return false;
}

// ---- Vulkan back end ---------------------------------------------------------------------------------------------
// ImGui's back end submits its font upload itself: its vkQueueSubmit and vkQueueWaitIdle take the queue lock the
// runtime's own submits and presents take (another thread presents).
VKAPI_ATTR VkResult VKAPI_CALL locked_submit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    std::lock_guard lk(nvn::queue_lock());
    return vk::p_vkQueueSubmit(q, n, si, f);
}
VKAPI_ATTR VkResult VKAPI_CALL locked_wait_idle(VkQueue q) {
    std::lock_guard lk(nvn::queue_lock());
    return vk::p_vkQueueWaitIdle(q);
}
PFN_vkVoidFunction load_vk(const char* name, void*) {
    if (std::strcmp(name, "vkQueueSubmit") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&locked_submit);
    if (std::strcmp(name, "vkQueueWaitIdle") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&locked_wait_idle);
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    static auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(nvn::vk_instance(), "vkGetDeviceProcAddr"));
    PFN_vkVoidFunction f = gdpa ? gdpa(nvn::vk_device(), name) : nullptr;
    return f ? f : gipa(nvn::vk_instance(), name);
}

struct Backend {
    bool context = false, vulkan = false, failed = false;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    uint32_t font_height = 0;  // the swapchain height the font atlas was built for
    float scale = 1;
};
Backend g;

void build_fonts(uint32_t height) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g.scale = std::clamp(float(height) / 1080.0f, 0.75f, 3.0f);
    const float px = std::round(19.0f * g.scale);
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2010, 0x2027,
                                     0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    ImFont* f = nullptr;
    for (const char* path : {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}) {
        if (FILE* t = plat::fopen_utf8(path, "rb")) {
            std::fclose(t);
            f = io.Fonts->AddFontFromFileTTF(path, px, &cfg, ranges);
            if (f) break;
        }
    }
    if (!f) {  // ImGui's own (ASCII only)
        ImFontConfig d;
        d.SizePixels = std::round(13.0f * g.scale);
        io.Fonts->AddFontDefault(&d);
    }
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10, s.FrameRounding = 6, s.GrabRounding = 6, s.TabRounding = 6, s.ChildRounding = 8;
    s.PopupRounding = 6, s.ScrollbarRounding = 6;
    s.WindowPadding = ImVec2(18, 16), s.FramePadding = ImVec2(10, 6), s.ItemSpacing = ImVec2(10, 9);
    s.WindowBorderSize = 0, s.FrameBorderSize = 0;
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.30f, 0.78f, 0.47f, 1.0f), accent_dim(0.20f, 0.52f, 0.33f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.21f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.27f, 0.33f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
    c[ImGuiCol_ButtonHovered] = accent_dim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = accent_dim;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.62f, 0.40f, 1.0f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_SliderGrab] = accent, c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.86f, 0.30f, 1.0f);
    c[ImGuiCol_PlotLines] = accent, c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    s.ScaleAllSizes(g.scale);
    g.font_height = height;
}

bool init_vulkan(VkRenderPass rp, uint32_t images) {
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load_vk)) return false;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = nvn::vk_instance();
    ii.PhysicalDevice = nvn::vk_physical();
    ii.Device = nvn::vk_device();
    ii.QueueFamily = nvn::vk_queue_family();
    ii.Queue = nvn::vk_queue();
    ii.RenderPass = rp;
    ii.MinImageCount = 2;
    // Its vertex buffers rotate per frame: more than the frames the runtime keeps in flight.
    ii.ImageCount = std::max(images, 6u);
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.DescriptorPoolSize = 16;
    if (!ImGui_ImplVulkan_Init(&ii)) return false;
    g.render_pass = rp;
    return true;
}

// ---- what the pages show ------------------------------------------------------------------------------------------
std::vector<uint64_t> g_presents;  // present times, newest last (present path)
uint64_t g_present_count = 0;

struct FrameStats {
    double fps = 0, avg_ms = 0, max_ms = 0;
    float history[240] = {};
    int count = 0;
};
FrameStats frame_stats() {
    FrameStats s;
    const size_t n = g_presents.size();
    if (n < 2) return s;
    const uint64_t now = g_presents.back();
    size_t in_second = 0;
    double sum = 0, mx = 0;
    for (size_t k = n - 1; k > 0; k--) {
        const double ms = double(g_presents[k] - g_presents[k - 1]) / 1e6;
        if (now - g_presents[k] <= 1'000'000'000ull) in_second++, sum += ms, mx = std::max(mx, ms);
    }
    s.fps = in_second ? 1000.0 * double(in_second) / sum : 0;
    s.avg_ms = in_second ? sum / double(in_second) : 0;
    s.max_ms = mx;
    const size_t first = n > 241 ? n - 241 : 0;
    for (size_t k = first + 1; k < n; k++) s.history[s.count++] = float(double(g_presents[k] - g_presents[k - 1]) / 1e6);
    return s;
}

std::vector<std::string> categories() {
    std::vector<std::string> v = {"Desempenho"};
    for (const settings::Option& o : settings::options())
        if (std::find(v.begin(), v.end(), o.category) == v.end()) v.push_back(o.category);
    return v;
}

int g_category = 0;

void badge(const settings::Option& o) {
    ImGui::SameLine();
    if (settings::locked(o.key)) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1), "[definido por %s]", o.env.c_str());
    } else if (settings::pending_restart(o.key)) {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1), "[vale ao reiniciar]");
    } else {
        ImGui::TextDisabled(o.apply == settings::Apply::kNow ? "[imediato]" : "[requer reiniciar]");
    }
}

void option_row(const settings::Option& o) {
    ImGui::PushID(o.key.c_str());
    const bool lock = settings::locked(o.key);
    if (lock) ImGui::BeginDisabled();
    const std::string v = settings::get(o.key);
    ImGui::TextUnformatted(o.label.c_str());
    badge(o);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    switch (o.kind) {
        case settings::Kind::kBool: {
            bool b = o.key == "fullscreen" ? plat::window_fullscreen() : v == "1";
            if (ImGui::Checkbox(b ? "Ligado" : "Desligado", &b)) settings::set(o.key, b ? "1" : "0");
            break;
        }
        case settings::Kind::kFloat: {
            float f = float(std::atof(v.c_str()));
            int pct = int(std::lround(f * 100));
            if (ImGui::SliderInt("##v", &pct, int(o.min * 100), int(o.max * 100), "%d%%")) {
                char b[32];
                std::snprintf(b, sizeof b, "%.2f", pct / 100.0);
                settings::set(o.key, b);
            }
            break;
        }
        case settings::Kind::kChoice: {
            const char* current = v.c_str();
            for (const settings::Choice& c : o.choices)
                if (c.value == v) current = c.label.c_str();
            if (ImGui::BeginCombo("##v", current)) {
                for (const settings::Choice& c : o.choices) {
                    const bool sel = c.value == v;
                    if (ImGui::Selectable(c.label.c_str(), sel)) settings::set(o.key, c.value);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        }
    }
    if (lock) ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", o.help.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PopID();
}

void stat_row(const char* label, const char* fmt, ...) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

void performance_page(uint32_t sw, uint32_t sh) {
    const FrameStats f = frame_stats();
    const hooks::PerfSnapshot p = hooks::perf_snapshot();
    ImGui::Text("%.1f fps", f.fps);
    ImGui::SameLine();
    ImGui::TextDisabled("  quadro médio %.2f ms, pior %.2f ms (último segundo)", f.avg_ms, f.max_ms);
    if (f.count > 1)
        ImGui::PlotLines("##frames", f.history, f.count, 0, "intervalo entre quadros (ms), últimos 4 s", 0.0f, 50.0f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 90 * g.scale));
    ImGui::Spacing();
    stat_row("Imagem do jogo", "%u x %u (resolução dinâmica do jogo)", p.game_w, p.game_h);
    stat_row("Janela", "%u x %u, %s", sw, sh, plat::window_fullscreen() ? "tela cheia" : "janela");
    uint64_t vb = 0, period = 0;
    if (plat::monitor_vblank(vb, period) && period)
        stat_row("Monitor", "%.2f Hz, apresentação %s", 1e9 / double(period), p.present_mode);
    else
        stat_row("Apresentação", "%s", p.present_mode);
    stat_row("Replay (CPU)", "%.2f ms por quadro, %.1f envios", p.replay_ms, p.submits);
    stat_row("Espera da GPU", "%.2f ms por quadro", p.fence_ms);
    stat_row("Envio e apresentação", "%.2f ms + %.2f ms", p.encode_ms, p.present_ms);
    stat_row("Uploads", "%.1f MB por quadro", p.upload_mb);
    stat_row("Memória do processo", "%.2f GB", double(plat::memory_footprint()) / double(1ull << 30));
    ImGui::TextDisabled("Médias da janela de 60 quadros do runtime (as mesmas do log).");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    for (const settings::Option& o : settings::options())
        if (o.category == "Desempenho") option_row(o);
}

void settings_page(const std::string& category) {
    for (const settings::Option& o : settings::options())
        if (o.category == category) option_row(o);
}

void menu(uint32_t sw, uint32_t sh) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    const ImVec2 size(std::min(io.DisplaySize.x * 0.62f, 1180.0f * g.scale), io.DisplaySize.y * 0.74f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.30f, 0.78f, 0.47f, 1), "Super Mario Bros. Wonder");
    ImGui::SameLine();
    ImGui::TextDisabled("  Configurações");
    ImGui::Separator();
    ImGui::Spacing();

    const std::vector<std::string> cats = categories();
    g_category = std::clamp(g_category, 0, int(cats.size()) - 1);
    const float side = 220 * g.scale, footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##cats", ImVec2(side, -footer), ImGuiChildFlags_None);
    for (int k = 0; k < int(cats.size()); k++)
        if (ImGui::Selectable(cats[k].c_str(), g_category == k, 0, ImVec2(0, 34 * g.scale))) {
            if (std::getenv("NX_OVERLAY_TRACE")) log("overlay: category %d selected (mouse or nav)", k);
            g_category = k;
        }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_None);
    ImGui::Spacing();
    if (g_category == 0)
        performance_page(sw, sh);
    else
        settings_page(cats[size_t(g_category)]);
    ImGui::EndChild();

    ImGui::Separator();
    bool pending = false;
    for (const settings::Option& o : settings::options()) pending |= settings::pending_restart(o.key);
    if (pending)
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1), "Há mudanças que só valem depois de reiniciar o jogo.");
    else
        ImGui::TextDisabled("Salvo em %s", settings::file_path().c_str());
    ImGui::TextDisabled("F1 ou Minus + Plus: fechar   |   Q / E ou L / R: categoria   |   o jogo continua rodando");
    ImGui::SameLine(ImGui::GetWindowWidth() - 150 * g.scale);
    if (ImGui::Button("Continuar", ImVec2(130 * g.scale, 0))) toggle();
    ImGui::End();
}

void mini_overlay() {
    const std::string mode = settings::get("perf_overlay");
    if (mode == "off") return;
    const FrameStats f = frame_stats();
    ImGui::SetNextWindowPos(ImVec2(12 * g.scale, 12 * g.scale));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##mini", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%.0f fps", f.fps);
    if (mode == "full") {
        const hooks::PerfSnapshot p = hooks::perf_snapshot();
        ImGui::Text("%.2f ms (pior %.2f)", f.avg_ms, f.max_ms);
        ImGui::Text("jogo %ux%u", p.game_w, p.game_h);
        ImGui::Text("replay %.2f ms", p.replay_ms);
        ImGui::Text("%.2f GB", double(plat::memory_footprint()) / double(1ull << 30));
    }
    ImGui::End();
}

ImGuiKey imgui_key(int k) {
    switch (Key(k)) {
        case Key::kTab: return ImGuiKey_Tab;
        case Key::kLeft: return ImGuiKey_LeftArrow;
        case Key::kRight: return ImGuiKey_RightArrow;
        case Key::kUp: return ImGuiKey_UpArrow;
        case Key::kDown: return ImGuiKey_DownArrow;
        case Key::kPageUp: return ImGuiKey_PageUp;
        case Key::kPageDown: return ImGuiKey_PageDown;
        case Key::kHome: return ImGuiKey_Home;
        case Key::kEnd: return ImGuiKey_End;
        case Key::kEnter: return ImGuiKey_Enter;
        case Key::kEscape: return ImGuiKey_Escape;
        case Key::kSpace: return ImGuiKey_Space;
        case Key::kBackspace: return ImGuiKey_Backspace;
        case Key::kQ: return ImGuiKey_Q;
        case Key::kE: return ImGuiKey_E;
    }
    return ImGuiKey_None;
}

// Pending window events and the pad, into ImGui's IO; category switching and closing.
void feed_input() {
    ImGuiIO& io = ImGui::GetIO();
    std::deque<Event> events;
    plat::PadState pad;
    {
        std::lock_guard lk(g_input_lock);
        events.swap(g_events);
        pad = g_pad;
    }
    const bool open = g_open.load();
    const int cats = int(categories().size());
    for (const Event& e : events) {
        if (!open && e.type != Event::kFocus) continue;
        switch (e.type) {
            case Event::kKey: {
                const ImGuiKey k = imgui_key(e.a);
                if (e.down && k == ImGuiKey_Q) g_category = (g_category + cats - 1) % cats;
                if (e.down && k == ImGuiKey_E) g_category = (g_category + 1) % cats;
                if (e.down && k == ImGuiKey_Escape && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) toggle();
                io.AddKeyEvent(k, e.down);
                break;
            }
            case Event::kMousePos: io.AddMousePosEvent(e.x, e.y); break;
            case Event::kMouseButton: io.AddMouseButtonEvent(e.a, e.down); break;
            case Event::kWheel: io.AddMouseWheelEvent(0, e.y); break;
            case Event::kFocus: io.AddFocusEvent(e.down); break;
        }
    }
    // The pad (Nintendo layout: A confirms, B goes back), as ImGui's gamepad keys; L / R switch the category.
    // Buttons already held when it opened (Minus + Plus, a held L) count only once released and pressed again.
    static uint64_t last = 0, held_at_open = 0;
    static bool was_open = false;
    if (open && !was_open) held_at_open = pad.buttons;
    was_open = open;
    held_at_open &= pad.buttons;  // released: free again
    const uint64_t b = open ? pad.buttons & ~held_at_open : 0;
    const uint64_t pressed = b & ~last;
    last = b;
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, b & plat::kA);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, b & plat::kB);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, b & plat::kX);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, b & plat::kY);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, b & plat::kLeft);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, b & plat::kRight);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, b & plat::kUp);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, b & plat::kDown);
    const float lx = open ? float(pad.lx) / float(plat::kStickMax) : 0, ly = open ? float(pad.ly) / float(plat::kStickMax) : 0;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < -0.3f, std::max(0.0f, -lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > 0.3f, std::max(0.0f, lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > 0.3f, std::max(0.0f, ly));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < -0.3f, std::max(0.0f, -ly));
    if (pressed & plat::kL) g_category = (g_category + cats - 1) % cats;
    if (pressed & plat::kR) g_category = (g_category + 1) % cats;
    if (std::getenv("NX_OVERLAY_TRACE") && (pressed || !events.empty()))
        log("overlay: pad %llx pressed %llx, %zu events, category %d", (unsigned long long)pad.buttons,
            (unsigned long long)pressed, events.size(), g_category);
    if ((pressed & plat::kB) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive()) toggle();
}

// The present pass's hook (hooks.h): lays out and records this frame's overlay.
void present_overlay(void* cbp, uint64_t rpv, uint32_t w, uint32_t h, uint32_t images) {
    const uint64_t now = plat::monotonic_ns();
    g_presents.push_back(now);
    if (g_presents.size() > 600) g_presents.erase(g_presents.begin(), g_presents.begin() + 300);
    g_present_count++;
    static const char* test = std::getenv("NX_OVERLAY_TEST");
    if (test && !g_open.load()) {
        static bool done = false;
        const uint64_t at = std::strtoull(test, nullptr, 10);
        if (!done && g_present_count >= at) {
            done = true;
            if (const char* c = std::strchr(test, ':')) g_category = std::atoi(c + 1);
            toggle();
        }
    }
    const bool open = g_open.load();
    if (!open && settings::get("perf_overlay") == "off") return;  // nothing to draw
    if (g.failed || !w || !h) return;
    auto cb = reinterpret_cast<VkCommandBuffer>(cbp);
    auto rp = reinterpret_cast<VkRenderPass>(rpv);
    if (!g.context) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr, io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.BackendPlatformName = "nx_overlay";
        g.context = true;
    }
    if (g.vulkan && (rp != g.render_pass || std::abs(int(h) - int(g.font_height)) > int(g.font_height) / 6)) {
        locked_wait_idle(nvn::vk_queue());  // nothing in flight uses the old pipeline or font
        ImGui_ImplVulkan_Shutdown();
        g.vulkan = false;
    }
    if (!g.vulkan) {
        build_fonts(h);
        if (!init_vulkan(rp, images) || !ImGui_ImplVulkan_CreateFontsTexture()) {
            g.failed = true;
            log("overlay: the Vulkan back end did not start: no overlay");
            return;
        }
        g.vulkan = true;
        log("overlay: started (%ux%u, font for %u px height)", w, h, h);
    }
    ImGuiIO& io = ImGui::GetIO();
    static uint64_t last = 0;
    io.DeltaTime = last ? std::clamp(float(double(now - last) / 1e9), 1e-4f, 0.25f) : 1.0f / 60.0f;
    last = now;
    io.DisplaySize = ImVec2(float(w), float(h));
    io.MouseDrawCursor = false;
    feed_input();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (g_open.load())
        menu(w, h);
    else
        mini_overlay();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
}

}  // namespace

bool overlay_wanted() { return g_open.load() || settings::get("perf_overlay") != "off" || std::getenv("NX_OVERLAY_TEST"); }

void install() {
    hooks::present_overlay = &present_overlay;
    hooks::overlay_wanted = &overlay_wanted;
    hooks::pad_filter = &pad_filter;
    settings::on_change("fullscreen", [](const std::string& v) { plat::window_set_fullscreen(v == "1"); });
    settings::on_change("volume", [](const std::string& v) { plat::audio_set_volume(float(std::atof(v.c_str()))); });
    log("overlay: F1 (or Minus + Plus) opens the settings; %s", settings::file_path().c_str());
}

bool is_open() { return g_open.load(); }

void toggle() {
    const bool was = g_open.exchange(!g_open.load());
    if (was) g_swallow = true;  // the keys that closed it are still down
}

void key_event(Key key, bool down) { push({Event::kKey, int(key), down}); }
void mouse_pos(float x, float y) {
    if (!g_open.load()) return;
    Event e{Event::kMousePos};
    e.x = x, e.y = y;
    push(e);
}
void mouse_button(int button, bool down) {
    if (g_open.load()) push({Event::kMouseButton, button, down});
}
void mouse_wheel(float dy) {
    if (!g_open.load()) return;
    Event e{Event::kWheel};
    e.y = dy;
    push(e);
}
void focus(bool focused) { push({Event::kFocus, 0, focused}); }

}  // namespace nx::overlay
