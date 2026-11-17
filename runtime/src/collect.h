// Collection sessions (development): normal play recorded for the pipeline catalog and for performance work
// (WONDER_SWITCH2_PROGRESS.md §5zd6). Off unless switched on, so ordinary players never have it:
//   <data root>/collect/ENABLED exists   (a file; the development shortcuts create and remove it; works when the
//                                         game is started by the REFLORA launcher, which passes no test variables)
//   NX_COLLECT=1 / NX_COLLECT=0          forces it on / off (tests). Runs without a window (the preparer's check
//                                         step, test harnesses) are left out unless NX_COLLECT=1.
// What a session writes, in <data root>/collect/ (never the official catalog):
//   pipelines_collected.bin        every pipeline the game made that the installed catalogs did not have (graphics
//                                  and compute; catalog format 3, one checked entry at a time, flushed: a crash
//                                  keeps every entry before it; no duplicates across sessions)
//   sessions/<date-time>.tsv       events, one line each, flushed as written: wall-clock time, seconds since the
//                                  session began, kind, details (pipelines compiled while playing and their cost,
//                                  hitches with where the time went, a summary every minute, missing shaders,
//                                  costly textures, audio windows and underruns)
//   shader_misses/                 the code and control of stages with no translated .spv (to translate them)
#pragma once

#include <string>

namespace nx::collect {

bool on();                // decided once, at the first call
const std::string& dir(); // <data root>/collect
// One event line: "<YYYY-MM-DD HH:MM:SS.mmm>\t<seconds>\t<kind>\t<printf text>". Thread-safe; does nothing when off.
void event(const char* kind, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace nx::collect
