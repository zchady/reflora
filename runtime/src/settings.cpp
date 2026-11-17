// Player settings: the registry, settings.ini, and the NX_* variables behind the options (settings.h).
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include <nx/runtime.h>

#include "platform.h"

namespace nx::settings {

namespace {

// The options. Only what the runtime really does: language (am GetDesiredLanguage, svc_am.cpp), the window
// (platform), the audio voice (platform), the performance overlay (overlay.cpp), the display clock lock
// (nvn_queue.cpp) and the memory guard (memguard_*.cpp).
std::vector<Option> make_options() {
    std::vector<Option> v;
    {
        Option o;
        o.key = "language", o.category = "Jogo", o.label = "Idioma do jogo";
        o.help = "O idioma que o jogo recebe do sistema (nn::oe::GetDesiredLanguage). O jogo o lê ao iniciar.";
        o.kind = Kind::kChoice, o.apply = Apply::kRestart, o.env = "NX_LANGUAGE", o.def = "en-US";
        // The codes Wonder maps to its message folders (its table: ja .. th), as the NACP declares 18 of them.
        o.choices = {{"en-US", "Inglês (EUA)"},           {"en-GB", "Inglês (Reino Unido)"},
                     {"pt-BR", "Português (Brasil)"},     {"pt", "Português (Portugal)"},
                     {"es-419", "Espanhol (América Latina)"}, {"es", "Espanhol (Espanha)"},
                     {"fr", "Francês (França)"},          {"fr-CA", "Francês (Canadá)"},
                     {"de", "Alemão"},                    {"it", "Italiano"},
                     {"nl", "Holandês"},                  {"ru", "Russo"},
                     {"pl", "Polonês"},                   {"ja", "Japonês"},
                     {"ko", "Coreano"},                   {"zh-Hans", "Chinês simplificado"},
                     {"zh-Hant", "Chinês tradicional"},   {"th", "Tailandês"}};
        v.push_back(o);
    }
    {
        Option o;
        o.key = "fullscreen", o.category = "Tela", o.label = "Tela cheia";
        o.help = "Janela sem bordas cobrindo o monitor (também F11 ou Alt+Enter, que não são salvos).";
        o.kind = Kind::kBool, o.apply = Apply::kNow, o.env = "NX_FULLSCREEN", o.def = "0";
        v.push_back(o);
    }
    {
        Option o;
        o.key = "volume", o.category = "Áudio", o.label = "Volume";
        o.help = "Volume do jogo (só desta janela; o volume do Windows não muda).";
        o.kind = Kind::kFloat, o.apply = Apply::kNow, o.env = "NX_AUDIO_VOLUME", o.def = "1";
        o.min = 0, o.max = 1, o.step = 0.05f;
        v.push_back(o);
    }
    {
        Option o;
        o.key = "mute", o.category = "Áudio", o.label = "Sem áudio";
        o.help = "Não abre o dispositivo de áudio.";
        o.kind = Kind::kBool, o.apply = Apply::kRestart, o.env = "NX_MUTE", o.def = "0";
        v.push_back(o);
    }
    {
        Option o;
        o.key = "perf_overlay", o.category = "Desempenho", o.label = "Indicador de desempenho";
        o.help = "Um quadro pequeno no canto da tela com o fps, mesmo com o menu fechado.";
        o.kind = Kind::kChoice, o.apply = Apply::kNow, o.def = "off";
        o.choices = {{"off", "Desligado"}, {"fps", "Só fps"}, {"full", "Completo"}};
        v.push_back(o);
    }
    {
        Option o;
        o.key = "vsync_lock", o.category = "Avançado", o.label = "Sincronizar com o monitor";
        o.help = "Os quadros saem no meio de um refresh real do monitor (monitores de 60, 120, 180 Hz...). "
                 "Desligado: o relógio de 60 Hz de antes.";
        o.kind = Kind::kBool, o.apply = Apply::kRestart, o.env = "NX_VSYNC_LOCK", o.def = "1";
        o.env_on = "", o.env_off = "0";
        v.push_back(o);
    }
    {
        Option o;
        o.key = "mem_limit_gb", o.category = "Avançado", o.label = "Limite de memória";
        o.help = "Acima disso o jogo é encerrado com uma mensagem, antes de travar o computador.";
        o.kind = Kind::kChoice, o.apply = Apply::kRestart, o.env = "NX_MEM_LIMIT_GB", o.def = "20";
        o.choices = {{"16", "16 GB"}, {"20", "20 GB"}, {"24", "24 GB"}, {"32", "32 GB"}};
        v.push_back(o);
    }
    return v;
}

struct State {
    std::mutex lock;
    std::vector<Option> options = make_options();
    std::map<std::string, std::string> values, started;  // current, and what this run started with
    std::map<std::string, bool> locked;
    std::map<std::string, std::vector<std::function<void(const std::string&)>>> callbacks;
};

State& state() {
    static State s;
    return s;
}

bool valid(const Option& o, const std::string& v) {
    switch (o.kind) {
        case Kind::kBool: return v == "0" || v == "1";
        case Kind::kFloat: {
            char* end = nullptr;
            double d = std::strtod(v.c_str(), &end);
            return end && *end == 0 && d >= o.min - 1e-6 && d <= o.max + 1e-6;
        }
        case Kind::kChoice:
            for (const Choice& c : o.choices)
                if (c.value == v) return true;
            return false;
    }
    return false;
}

// The variable's value for an option value ("" unsets it).
std::string env_value(const Option& o, const std::string& v) {
    if (o.kind == Kind::kBool) return v == "1" ? o.env_on : o.env_off;
    return v;
}

// The option value a variable set outside means.
std::string from_env(const Option& o, const char* e) {
    if (o.kind == Kind::kBool) {
        if (!o.env_on.empty()) return std::string(e) == o.env_on ? "1" : "0";  // NX_MUTE=1, NX_FULLSCREEN=1
        return std::string(e) == o.env_off ? "0" : "1";                         // NX_VSYNC_LOCK=0
    }
    return e;
}

void save(State& s) {
    const std::string path = file_path();
    FILE* f = plat::fopen_utf8(path, "wb");
    if (!f) {
        log("settings: cannot write %s", path.c_str());
        return;
    }
    std::fprintf(f, "# Super Mario Bros. Wonder recomp: settings (the F1 menu writes this file)\n");
    for (const Option& o : s.options)
        if (!s.locked[o.key]) std::fprintf(f, "%s=%s\n", o.key.c_str(), s.values[o.key].c_str());
    std::fclose(f);
}

}  // namespace

std::string file_path() { return plat::data_root() + "/settings.ini"; }

void load() {
    State& s = state();
    std::lock_guard lk(s.lock);
    std::map<std::string, std::string> file;
    if (FILE* f = plat::fopen_utf8(file_path(), "rb")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l.empty() || l[0] == '#') continue;
            size_t eq = l.find('=');
            if (eq != std::string::npos) file[l.substr(0, eq)] = l.substr(eq + 1);
        }
        std::fclose(f);
    }
    for (const Option& o : s.options) {
        const char* e = o.env.empty() ? nullptr : std::getenv(o.env.c_str());
        std::string v = o.def;
        if (e) {  // set outside: it decides, the file keeps what it had
            s.locked[o.key] = true;
            v = from_env(o, e);
            if (!valid(o, v)) v = o.def;
        } else if (auto it = file.find(o.key); it != file.end() && valid(o, it->second)) {
            v = it->second;
        }
        s.values[o.key] = s.started[o.key] = v;
        if (!e && !o.env.empty()) {
            const std::string ev = env_value(o, v);
            plat::set_env(o.env.c_str(), ev.empty() ? nullptr : ev.c_str());
        }
    }
    if (!file.empty()) log("settings: %s (%zu values)", file_path().c_str(), file.size());
}

const std::vector<Option>& options() { return state().options; }

const Option* find(const std::string& key) {
    for (const Option& o : state().options)
        if (o.key == key) return &o;
    return nullptr;
}

std::string get(const std::string& key) {
    State& s = state();
    std::lock_guard lk(s.lock);
    auto it = s.values.find(key);
    if (it != s.values.end()) return it->second;
    const Option* o = find(key);
    return o ? o->def : std::string();
}

bool get_bool(const std::string& key) { return get(key) == "1"; }
float get_float(const std::string& key) { return float(std::atof(get(key).c_str())); }

bool set(const std::string& key, const std::string& value) {
    State& s = state();
    const Option* o = find(key);
    std::vector<std::function<void(const std::string&)>> calls;
    {
        std::lock_guard lk(s.lock);
        if (!o || s.locked[key] || !valid(*o, value)) return false;
        if (s.values[key] == value) return true;
        s.values[key] = value;
        save(s);
        if (o->apply == Apply::kNow) {
            if (!o->env.empty()) {  // later readers of the variable see the new value too
                const std::string ev = env_value(*o, value);
                plat::set_env(o->env.c_str(), ev.empty() ? nullptr : ev.c_str());
            }
            calls = s.callbacks[key];
        }
    }
    for (auto& fn : calls) fn(value);
    return true;
}

bool locked(const std::string& key) {
    State& s = state();
    std::lock_guard lk(s.lock);
    return s.locked[key];
}

bool pending_restart(const std::string& key) {
    State& s = state();
    const Option* o = find(key);
    std::lock_guard lk(s.lock);
    return o && o->apply == Apply::kRestart && s.values[key] != s.started[key];
}

void on_change(const std::string& key, std::function<void(const std::string&)> fn) {
    State& s = state();
    std::lock_guard lk(s.lock);
    s.callbacks[key].push_back(std::move(fn));
}

}  // namespace nx::settings
