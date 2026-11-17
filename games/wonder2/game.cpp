// Super Mario Bros. Wonder – Nintendo Switch 2 Edition + Meetup in Bellabel Park: program 040015100B514001
// (program index 1 of application 040015100B514000), NACP version 1.2.1, NintendoSDK 21.4.1 with NVN 2.
// Modules main, sdk and subsdk0 (nn::movie); its add-on 010015100B515001 ("aocup") carries the Switch 2 data.
// Findings: WONDER_SWITCH2_PROGRESS.md. Data: private/wonder2 (exefs from tools/nup.py, analysis/*.bin from
// tools/nso2bin.py, nup.txt naming the package so the RomFS is read from it).
#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"wonder2", "Super Mario Bros. Wonder - Nintendo Switch 2 Edition", 0x040015100B514001ull,
                                "io.github.belmantegu.wonder2recomp", "private/wonder2",
                                0x1000000 /* main.npdm META+0x14: 16 MiB system resource */,
                                0x264000000 /* 9.5625 GiB: nninitStartup asks for a 5.05 GiB heap */,
                                0x3F /* main.npdm ThreadInfo: cores 0-5 */,
                                31250000 /* the Switch 2 system counter (the SDK's ConvertToTick constant) */};

void game::after_init() {}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
