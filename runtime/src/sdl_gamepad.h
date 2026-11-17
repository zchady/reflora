#pragma once

#include <algorithm>
#include <SDL3/SDL_gamepad.h>
#include "platform.h"

namespace nx::plat {

inline void read_sdl_gamepad(SDL_Gamepad* pad, PadState& p) {
    // SDL names buttons by position (south/east/...); Nintendo's A is the east button, B the south one.
    struct Map {
        SDL_GamepadButton b;
        uint64_t bit;
    };
    static const Map map[] = {
        {SDL_GAMEPAD_BUTTON_EAST, kA},           {SDL_GAMEPAD_BUTTON_SOUTH, kB},
        {SDL_GAMEPAD_BUTTON_NORTH, kX},          {SDL_GAMEPAD_BUTTON_WEST, kY},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
        {SDL_GAMEPAD_BUTTON_START, kPlus},       {SDL_GAMEPAD_BUTTON_BACK, kMinus},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kStickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kStickR},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
    };
    for (const Map& m : map)
        if (SDL_GetGamepadButton(pad, m.b)) p.buttons |= m.bit;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) p.buttons |= kZL;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) p.buttons |= kZR;
    auto axis = [&](SDL_GamepadAxis a, bool invert) {
        int v = SDL_GetGamepadAxis(pad, a);
        if (v > -7849 && v < 7849) return 0;
        v = std::clamp(v, -kStickMax, kStickMax);
        return invert ? -v : v;
    };
    p.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
    p.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);  // SDL: down is positive; Horizon: up is positive
    p.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
    p.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
}

}  // namespace nx::plat
