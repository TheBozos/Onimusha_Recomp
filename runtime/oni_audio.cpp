// Onimusha's sound modules: Sony's modular synthesizer and Capcom's SNDDRVCD, statically compiled from the player's
// disc (tools/oni_bozo_audio.py) and run by BozoEngine's native IRX host. The game reaches them through SIF RPC
// (CpCall_IOPSndDriver, servers 0x77777777/0x77777778) exactly as on the console's IOP; no IOP is modelled.
#include "oni_audio_modules.h"
#include "oni_port.h"
#include "bozo/libs/libraries.h"
#include "bozo/state.h"
#include <cctype>
#include <cstring>
#include <span>
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>

namespace oni
{
    namespace
    {
        std::unique_ptr<bozo::native::irx::CompiledLibraries> sound;
    }

    void audioState(bozo::state::Archive &a) { sound->serialize(a); }
    std::string audioUnsaveable() { return sound ? sound->unsaveable() : std::string(); }

    void installAudio(bozo::libs::Libraries &libraries, uint8_t *eeRam)
    {
        sound = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, kOniAudioHeapStart);
        addOniAudioImages(*sound);
        libraries.audioProvider = [](uint64_t cycle) { return sound->advanceTo(cycle); };
        libraries.sif.transfer = [](uint32_t address, const uint8_t *bytes, uint32_t count)
        { sound->writeBuffer(address, bytes, count); };
        libraries.sif.allocateBuffer = [](uint32_t bytes) { return sound->allocateBuffer(bytes); };
        libraries.sif.freeBuffer = [](uint32_t address) { return sound->freeBuffer(address); };
        libraries.sif.resetProvider = [] { sound->reset(); };
        // The EE service SNDDRVCD binds (0x77777779): the game's _CpEEComRecieveThread serves it with _CpDivideEECom
        // 0x1944b0. Command 0x1xxxx asks the EE to send EE memory to the driver (CpSifDmaTransEEToIOP 0x1940e0 with the
        // request's words 2-6: EE source, IOP destination, size, attribute, mode); the reply is the transfer's id,
        // -1 when the size is 0xffff1 or more. The other commands do nothing and reply 0 (as _CpDivideEECom).
        libraries.sif.eeServers[0x77777779u] = [eeRam](uint32_t command, std::span<const uint8_t> in, std::span<uint8_t> out)
        {
            int32_t result = 0;
            if ((command & 0xffff0000u) == 0x10000u)
            {
                if (in.size() < 0x1c)
                    throw std::runtime_error("SNDDRVCD's EE transfer request is too short");
                uint32_t word[7];
                std::memcpy(word, in.data(), sizeof(word));
                const uint32_t source = word[2] & 0x1ffffffu, destination = word[3], size = word[4];
                if (size >= 0xffff1u)
                    result = -1;
                else
                {
                    if (uint64_t(source) + size > 0x2000000u)
                        throw std::runtime_error("SNDDRVCD's EE transfer source is outside EE RAM");
                    sound->writeBuffer(destination, eeRam + source, size);
                    result = 1;
                }
            }
            if (out.size() >= 4)
                std::memcpy(out.data(), &result, 4);
        };
        // SNDDRVCD's start stays resident only with its two arguments (Snd_module_set passes "0x1FFFFF" and
        // " 0x01FFFF", parsed with strtol): sceSifLoadModule's arguments go to the module as argv.
        libraries.sif.moduleLoaderWithArguments = [](const std::string &path, const std::vector<std::string> &arguments)
        {
            auto name = path.substr(path.find_last_of("/\\") + 1);
            if (const auto version = name.find(';'); version != std::string::npos)
                name.resize(version);
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            // Answered at their EE APIs by BozoEngine's native pad and memory-card libraries.
            if (name == "SIO2MAN.IRX" || name == "PADMAN.IRX" || name == "MCMAN.IRX" || name == "MCSERV.IRX")
                return 1;
            return sound->startImage(name + ";1", arguments);
        };
    }
} // namespace oni
