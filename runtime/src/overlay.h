// The settings overlay (F1, or Minus + Plus held on the pad): Dear ImGui (runtime/third_party/imgui, MIT), drawn by the
// present pass into the swapchain image after the game image (hooks.h), so nothing the game renders or captures has
// it. Categories: performance (the runtime's real counters) and the options of settings.h, each marked as applying at
// once or at the next start. While it is open the game keeps running and gets a neutral pad.
//
// Threads: install() before the game starts; the event functions from the window thread; the pad filter from the game
// thread that polls the pad; ImGui itself only on the present path (under the GPU lock).
//
// NX_OVERLAY_TEST=<frame>[:<category>]: opens the overlay at that presented frame on that category (0 performance,
// then the settings' categories in order), for screenshots without input.
#pragma once
#include <cstdint>

namespace nx::overlay {

void install();  // settings callbacks and the hooks (hooks.h)
bool is_open();
void toggle();

// Keyboard and mouse from the window, used while the overlay is open.
enum class Key { kTab, kLeft, kRight, kUp, kDown, kPageUp, kPageDown, kHome, kEnd, kEnter, kEscape, kSpace, kBackspace,
                 kQ, kE };
void key_event(Key key, bool down);
void mouse_pos(float x, float y);  // pixels of the window's client area (= the swapchain image)
void mouse_button(int button, bool down);  // 0 left, 1 right
void mouse_wheel(float dy);
void focus(bool focused);

}  // namespace nx::overlay
