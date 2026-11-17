// Player settings (the settings overlay, overlay.cpp): one registry of the options the runtime really supports, kept in
// <data root>/settings.ini. Most options are the runtime's own NX_* variables: settings::load() runs before anything
// reads them and sets each one from the file, unless the variable was already set outside (the environment wins, and
// the overlay shows the option locked). An option either applies at once (a callback registered with on_change) or at
// the next start (pending_restart says so until then). New categories (resolution, controls, graphics) are new rows
// in options(); a category without options is not shown.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace nx::settings {

enum class Apply { kNow, kRestart };
enum class Kind { kBool, kFloat, kChoice };

struct Choice {
    std::string value, label;
};

struct Option {
    std::string key, category, label, help;
    Kind kind = Kind::kBool;
    Apply apply = Apply::kRestart;
    std::string def;  // the default, as stored ("1"/"0" for booleans)
    // The variable it sets ("" for the overlay's own options). Booleans set env_on / env_off ("" = unset).
    std::string env, env_on = "1", env_off;
    float min = 0, max = 1, step = 0.05f;  // kFloat
    std::vector<Choice> choices;           // kChoice
};

void load();  // once, first thing in main (before the memory guard reads NX_MEM_LIMIT_GB)
const std::vector<Option>& options();
const Option* find(const std::string& key);
std::string get(const std::string& key);
bool get_bool(const std::string& key);
float get_float(const std::string& key);
// Changes an option and saves the file. Immediate options call their callbacks; restart options are pending until the
// next start. False for an unknown key, a locked option or an invalid value.
bool set(const std::string& key, const std::string& value);
bool locked(const std::string& key);           // its variable was set outside: the environment decides
bool pending_restart(const std::string& key);  // differs from what this run started with
std::string file_path();
void on_change(const std::string& key, std::function<void(const std::string&)> fn);

}  // namespace nx::settings
