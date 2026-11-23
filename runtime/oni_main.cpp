// Onimusha: Warlords on BozoEngine: the headless runner (oni-headless) and the window (onimusha).
// The headless runner prints what is not native yet: the end reason, unknown device accesses by site, library calls by
// name (ONI_CALL_STATS=1). Each stop is the next thing to name or answer.
#include "bozo/app/frame_pacer.h"
#include "bozo/audio/wav.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "oni_functions.h"
#include "oni_bot.h"
#include "oni_cheats.h"
#include "oni_vu1.h"
#include "oni_port.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>
#ifdef ONI_WITH_WINDOW
#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/window.h"
#endif

int main(int argc, char **argv)
{
    bozo::host::FreeOptions options;
    options.framePeriodCycles = 294912000 / 60;
    bool window = false;
#ifdef ONI_WITH_WINDOW
    window = true;
    bozo::app::setIdentity({"Onimusha: Warlords", "Onimusha_Recomp", "onimusha-recomp", "SLUS_200.18"});
    bozo::app::WindowOptions windowOptions;
    windowOptions.title = "Onimusha: Warlords - BozoEngine";
    windowOptions.width = 960;
    windowOptions.height = 720;
#endif
    options.elf = ONI_DEFAULT_ELF;
    options.iso = ONI_DEFAULT_ISO;
    options.frames = window ? 0 : 600;
    options.printGameOutput = true;
    // CpEEInit_RpcClient busy-waits (CpEEWait, no syscall) for Capcom's driver to register its RPC servers before
    // the first frame: the IOP modules must advance while no frame ends.
    options.advanceLibrariesWhileStalled = true;
    std::string wav, dumpRam;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
            options.frames = std::stoul(argv[++i]);
        else if (a == "--pad-script" && i + 1 < argc)
            options.padScript = argv[++i];
        else if (a == "--card-dir" && i + 1 < argc)
            options.cardDir = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            options.out = argv[++i];
        else if (a == "--elf" && i + 1 < argc)
            options.elf = argv[++i];
        else if (a == "--iso" && i + 1 < argc)
            options.iso = argv[++i];
        else if (a == "--wav" && i + 1 < argc)
            wav = argv[++i];
        else if (a == "--headless")
            window = false;
        else if (a == "--profile-io")
            options.profileIo = true;
        else if (a == "--quiet")
            options.printGameOutput = false;
        else if (a == "--dump-ram" && i + 1 < argc)
            dumpRam = argv[++i];
        else if (a == "--load-state" && i + 1 < argc)
            options.loadState = argv[++i];
        else if (a == "--save-state" && i + 1 < argc)
            options.saveState = argv[++i];
        else if (a == "--save-state-at" && i + 1 < argc)
            options.saveStateFrame = std::stoul(argv[++i]);
#ifdef ONI_WITH_WINDOW
        else if (a == "--no-vsync")
            windowOptions.vsync = false;
        else if (a == "--hidden") // no window on the desktop and no sound: captures from the offscreen picture
            windowOptions.hidden = true;
        else if (a == "--fullscreen")
            windowOptions.fullscreen = true;
        else if (a == "--capture-dir" && i + 1 < argc)
            windowOptions.captureDir = argv[++i];
        else if (a == "--capture-frame" && i + 1 < argc)
            windowOptions.captureFrames.push_back(std::stoull(argv[++i]));
#endif
        else
        {
            std::fprintf(stderr, "Usage: onimusha [--frames N] [--headless] [--elf FILE] [--iso FILE] [--card-dir DIR]\n"
                                 "                [--pad-script FILE] [--wav FILE] [--out FILE] [--profile-io] [--quiet]\n"
                                 "                [--dump-ram FILE] [--load-state FILE] [--save-state FILE --save-state-at N]\n"
                                 "                [--fullscreen] [--no-vsync] [--hidden]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    options.realTime = window;
#ifdef ONI_WITH_WINDOW
    // The memory cards of a played game live in the user folder (%APPDATA%\Onimusha_Recomp: mc0, mc1), so a save
    // outlives the session without --card-dir. Headless and scripted runs keep fresh cards in memory unless given
    // --card-dir, so a test never touches the player's saves; ONI_MEMORY_CARDS=1 keeps them in memory for a played
    // game too.
    if (window && options.padScript.empty() && options.cardDir.empty() && !std::getenv("ONI_MEMORY_CARDS") &&
        !bozo::app::userDirectory().empty())
    {
        options.cardDir = bozo::app::userDirectory().string();
        std::printf("[oni] memory cards in %s\n", options.cardDir.c_str());
    }
#endif
    options.gameTag = "SLUS-20018"; // a save state loads only into Onimusha
    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    oni::installAudio(host.libraries(), d.runtime().memory.ram());
    // libmpeg for Sony's ezmpeg player (movie.c): nothing written at Twisted Metal: Black's addresses, the SDK's results;
    // each picture held until its fields are due; the stream demuxed with the string callbacks (no Nodata input).
    host.libraries().mpegOptions = {.referenceLayout = false, .paced = true, .sdkNodata = false};
    d.setFunctions(oni_function_table, oni_function_count, oni_resume_table, oni_resume_count);
    bozo::port::Registry registry;
    oni::install(d.runtime(), registry);
    oni::installState(*host.stateSections());
    oni::vu1::setThreaded(window); // the models' chains on a thread of their own in real time
    std::string error;
    if (!host.setUp(d, oni_external_table, oni_external_count, &registry, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    const size_t modules = oni::registerModules(d.runtime());
    std::printf("[oni] %zu overlay modules registered\n", modules);

    bozo::app::FramePacer pacer;
    pacer.setPaced(window);
    // ONI_THREADS_AT=<frame>: every thread's state and pc at that frame (where a loop spins).
    const uint64_t threadsAt = std::getenv("ONI_THREADS_AT") ? std::strtoull(std::getenv("ONI_THREADS_AT"), nullptr, 0) : 0;
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        if (threadsAt && f == threadsAt)
            for (const auto &[id, t] : d.kernel().threads())
                std::printf("[oni:threads] %d state=%u wait=%u object=%u entry=%x pc=%x %s\n", id, unsigned(t->state),
                            t->waitReason, t->waitObject, t->entry, t->frames.empty() ? 0 : t->frames.front()->ctx.pc,
                            host.functionAt(t->frames.empty() ? 0 : t->frames.front()->ctx.pc).c_str());
        // ONI_SCE_TRACE=<n>: every n frames, the event scripts' tasks (10 at 0x28e860, 0x160 apart: +1 active, +4 the
        // bytecode pc; the opcode's handler from the table at 0x1f4060).
        static const uint64_t sceEvery = std::getenv("ONI_SCE_TRACE") ? std::strtoull(std::getenv("ONI_SCE_TRACE"), nullptr, 0) : 0;
        if (sceEvery && f % sceEvery == 0)
        {
            const uint8_t *ram = d.runtime().memory.ram();
            // cd_work 0x3dc5c0 (struct _CD_W): what Wsleeping waits on.
            constexpr uint32_t cd = 0x3dc5c0;
            std::printf("[oni:sce] frame %llu cd: read_flag %d r_no %u,%u,%u voice queue %u..%u bg_req %u bg_stck %u "
                        "stream flag %u read %u\n",
                        (unsigned long long)f, static_cast<int8_t>(ram[cd + 0x27]), ram[cd + 0x24], ram[cd + 0x25],
                        ram[cd + 0x26], ram[cd + 0x176], ram[cd + 0x177], ram[cd + 0xbd], ram[cd + 0xbe], ram[cd + 0xba],
                        ram[cd + 0xbb]);
            // gTransADS 0x3dbbc0: the sound driver's ADS (voice stream) status, its RPC reply (bit 0: busy).
            std::printf("[oni:sce] frame %llu ads:", (unsigned long long)f);
            for (int k = 0; k < 8; ++k)
            {
                uint32_t v;
                std::memcpy(&v, ram + 0x3dbbc0 + 4 * k, 4);
                std::printf(" %08x", v);
            }
            std::printf("\n");
            for (uint32_t i = 0; i < 10; ++i)
            {
                const uint32_t task = 0x28e860 + i * 0x160;
                if (!ram[task + 1])
                    continue;
                uint32_t pc, handler;
                std::memcpy(&pc, ram + task + 4, 4);
                const uint8_t op = ram[pc & 0x1ffffff];
                std::memcpy(&handler, ram + 0x1f4060 + op * 4, 4);
                std::printf("[oni:sce] frame %llu task %u pc %x op %02x %s bytes", (unsigned long long)f, i, pc, op,
                            host.functionAt(handler).c_str());
                for (int k = 0; k < 8; ++k)
                    std::printf(" %02x", ram[(pc + k) & 0x1ffffff]);
                std::printf("\n");
            }
        }
        // ONI_CUT_TRACE=1: the stage, room and cut (0x2875e0 / e2 / e4) and the cut's background movies (0x28287d) as
        // they change.
        static const bool cutTrace = std::getenv("ONI_CUT_TRACE") != nullptr;
        if (cutTrace)
        {
            const uint8_t *ram = d.runtime().memory.ram();
            int16_t place[3];
            std::memcpy(place, ram + 0x2875e0, sizeof place);
            const uint32_t key = static_cast<uint32_t>(place[0] & 0xff) << 24 | static_cast<uint32_t>(place[1] & 0xff) << 16 |
                                 static_cast<uint32_t>(place[2] & 0xff) << 8 | ram[0x28287d];
            static uint32_t last = ~0u;
            if (key != last)
                std::printf("[oni:cut] frame %llu stage %d room %d cut %d, %u background movies\n", (unsigned long long)f,
                            place[0], place[1], place[2], ram[0x28287d]);
            last = key;
        }
        // ONI_PLAYER_TRACE=<n>: every n frames, the player's position (_OCW 0x287ed0: Pos +0x70, Dir_y +0xc2), life,
        // the game mode and the place.
        static const uint64_t playerEvery = std::getenv("ONI_PLAYER_TRACE") ? std::strtoull(std::getenv("ONI_PLAYER_TRACE"), nullptr, 0) : 0;
        if (playerEvery && f % playerEvery == 0)
        {
            const uint8_t *ram = d.runtime().memory.ram();
            float pos[3];
            int16_t dir, life, place[3];
            std::memcpy(pos, ram + 0x287ed0 + 0x70, sizeof pos);
            std::memcpy(&dir, ram + 0x287ed0 + 0xc2, 2);
            std::memcpy(&life, ram + 0x287ed0 + 0x6d0, 2);
            std::memcpy(place, ram + 0x2875e0, sizeof place);
            std::printf("[oni:player] frame %llu place %d:%d:%d mode %u pos %.0f %.0f %.0f dir %d life %d\n", (unsigned long long)f,
                        place[0], place[1], place[2], ram[0x27de30 + 0x44b0], pos[0], pos[1], pos[2], dir, life);
        }
        // ONI_FADE_TRACE=1: fade.c's Fade_w (0x27c370: active, out, status, type, ...) and the game mode as they change.
        static const bool fadeTrace = std::getenv("ONI_FADE_TRACE") != nullptr;
        if (fadeTrace)
        {
            const uint8_t *ram = d.runtime().memory.ram();
            uint64_t key;
            std::memcpy(&key, ram + 0x27c370, 8);
            key ^= static_cast<uint64_t>(ram[0x27de30 + 0x44b0]) << 56;
            static uint64_t last = ~0ull;
            if (key != last)
                std::printf("[oni:fade] frame %llu mode %u fade %02x %02x %02x %02x %02x %02x %02x %02x\n", (unsigned long long)f,
                            ram[0x27de30 + 0x44b0], ram[0x27c370], ram[0x27c371], ram[0x27c372], ram[0x27c373],
                            ram[0x27c374], ram[0x27c375], ram[0x27c376], ram[0x27c377]);
            last = key;
        }
        oni::bot::frame(d.runtime().memory.ram(), f); // the story bot's next input (ONI_BOT)
        oni::cheats::apply(d.runtime().memory.ram()); // the frame boundary: the game thread
        pacer.frameEnd();
        // ONI_AUDIO_STATS: the frame rate every 120 frames, on stderr beside the sound device's lines.
        static const bool rateStats = std::getenv("ONI_AUDIO_STATS") != nullptr;
        if (rateStats && f % 120 == 0)
        {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[oni:rate] frame %llu: %.1f fps\n", (unsigned long long)f,
                         120.0 / std::chrono::duration<double>(now - last).count());
            last = now;
        }
        if (f % 60 == 0)
            std::printf("[oni] frame=%llu pc=%x cycles=%llu\n", (unsigned long long)f, ctx->pc,
                        (unsigned long long)d.now());
    };
    bozo::audio::WavWriter recording;
    if (!wav.empty() && !recording.open(wav))
    {
        std::fprintf(stderr, "cannot open WAV %s\n", wav.c_str());
        return 2;
    }
#ifdef ONI_WITH_WINDOW
    oni::cheats::registerMenu();
    bozo::app::DevicePad devices;
    // ONI_AUDIO_STATS=1: the sound device's queue, underruns and drops every 2 s on stderr.
    bozo::app::AudioOptions audioOptions;
    audioOptions.stats = std::getenv("ONI_AUDIO_STATS") != nullptr;
    bozo::app::AudioDevice audio(audioOptions);
    if (window && options.padScript.empty())
        host.libraries().pad.input = &devices;
#endif
    oni::bot::start(host); // ONI_BOT=<script>: the story bot as player 1's controller
    host.onAudio = [&](uint64_t, const int16_t *pcm, size_t count)
    {
        if (recording.isOpen())
            recording.write(pcm, count);
#ifdef ONI_WITH_WINDOW
        if (window)
            audio.push(pcm, count);
#endif
    };
    auto run = [&]()
    {
#ifdef ONI_WITH_WINDOW
        if (window)
            if (!windowOptions.hidden)
                audio.open();
#endif
        uint64_t frames = 0;
        try
        {
            frames = host.run();
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[oni] native failure: %s\n", e.what());
            return 1;
        }
        if (!dumpRam.empty())
            if (FILE *f = std::fopen(dumpRam.c_str(), "wb"))
            {
                std::fwrite(d.runtime().memory.ram(), 1, 0x2000000u, f);
                std::fclose(f);
            }
        std::printf("[oni] frames=%llu end=%s\n", (unsigned long long)frames, host.endReason().c_str());
        const auto &stats = host.stats();
        std::printf("[oni] compatibility: %llu register accesses, %llu unknown; %llu native VU0 calls\n",
                    (unsigned long long)stats.ioAccesses, (unsigned long long)stats.ioUnknown,
                    (unsigned long long)stats.vu0Calls);
        std::printf("[oni] calls: %llu kernel, %llu library, %llu port overrides\n",
                    (unsigned long long)stats.kernelCalls, (unsigned long long)stats.libraryCalls,
                    (unsigned long long)stats.overrideCalls);
        for (const auto &[name, count] : stats.unanswered)
            std::printf("[oni:unanswered] %10llu %s\n", (unsigned long long)count, name.c_str());
        if (std::getenv("ONI_CALL_STATS"))
        {
            std::vector<std::pair<uint64_t, std::string>> byName;
            for (const auto &[name, count] : stats.libraryByName)
                byName.emplace_back(count, name);
            std::sort(byName.rbegin(), byName.rend());
            for (const auto &[count, name] : byName)
                std::printf("[oni:lib] %10llu %s\n", (unsigned long long)count, name.c_str());
        }
        for (const auto &[site, counts] : stats.ioSites)
            std::printf("[oni:io] pc=%08x address=%08x reads=%llu writes=%llu %s\n", site.first, site.second,
                        (unsigned long long)counts[0], (unsigned long long)counts[1],
                        host.functionAt(site.first).c_str());
        return host.endReason().rfind("FAULT:", 0) == 0 ? 1 : 0;
    };
    int result;
#ifdef ONI_WITH_WINDOW
    if (window)
    {
        bozo::app::Game game;
        game.run = run;
        game.stop = [&] { host.requestStop(); };
        game.framesEnded = [&] { return host.framesEnded(); };
        game.saveState = [&](int slot) { host.requestSave(bozo::app::stateSlotPath(slot)); };
        game.loadState = [&](int slot) { host.requestLoad(bozo::app::stateSlotPath(slot)); };
        host.onStateMessage = [](bool, const std::string &message) { bozo::app::notify(message); };
        result = bozo::app::runWindow(windowOptions, game, &devices);
    }
    else
#endif
        result = run();
    recording.close();
    return result;
}
