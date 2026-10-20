#include "bt_manager.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace bt {

    namespace {
        bool   g_btmReady    = false;
        bool   g_btmsysReady = false;
        Result g_initRc      = 0;

        // Service limits (switchbrew): these requests use fixed-size "pointer buffers";
        // if the buffer is larger than the server accepts, btm closes the session
        // (error 2001-0123 / KernelError_ConnectionClosed).
        constexpr size_t MaxDeviceInfo      = 10;  // GetDeviceInfo:      0x13C * 10
        constexpr size_t MaxDeviceCondition = 8;   // GetDeviceCondition: 0x148 * 8
        constexpr size_t MaxAudioDevices    = 8;   // BtmAudioDevice:     0xFF  * 8

        const Result ConnectionClosed = MAKERESULT(Module_Kernel, KernelError_ConnectionClosed);

        /// If the service closed the session, reopen it for subsequent requests.
        Result recover(Result rc) {
            if (rc != ConnectionClosed) return rc;
            if (g_btmReady)    { btmExit();    g_btmReady    = R_SUCCEEDED(btmInitialize()); }
            if (g_btmsysReady) { btmsysExit(); g_btmsysReady = R_SUCCEEDED(btmsysInitialize()); }
            return rc;
        }

        std::string safeName(const char* raw, size_t maxLen, const BtdrvAddress& addr) {
            const void* end = std::memchr(raw, '\0', maxLen);
            const size_t len = end ? static_cast<const char*>(end) - raw : maxLen;
            std::string s(raw, len);
            // Trim trailing spaces
            while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
                s.pop_back();
            return s.empty() ? addrToString(addr) : s;
        }

        DevType profileToType(u32 profile) {
            switch (profile) {
                case BtmProfile_Hid:   return DevType::Gamepad;
                case BtmProfile_Audio: return DevType::Audio;
                default:               return DevType::Other;
            }
        }
    }

    // ------------------------------------------------------------------ services

    Result init() {
        if (hosversionBefore(13, 0, 0)) {
            g_initRc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
            return g_initRc;
        }

        Result rc = btmInitialize();
        if (R_SUCCEEDED(rc)) g_btmReady = true;
        else g_initRc = rc;

        rc = btmsysInitialize();
        if (R_SUCCEEDED(rc)) g_btmsysReady = true;
        else if (R_SUCCEEDED(g_initRc)) g_initRc = rc;

        return g_initRc;
    }

    void exit() {
        if (g_btmsysReady) {
            // Never leave pairing / discovery mode active on exit
            bool active = false;
            if (R_SUCCEEDED(btmsysIsGamepadPairingStarted(&active)) && active)
                btmsysCancelGamepadPairing();
            active = false;
            if (R_SUCCEEDED(btmsysIsDiscoveryingAudioDevice(&active)) && active)
                btmsysStopAudioDeviceDiscovery();

            btmsysExit();
            g_btmsysReady = false;
        }
        if (g_btmReady) {
            btmExit();
            g_btmReady = false;
        }
    }

    bool   isReady()    { return g_btmReady && g_btmsysReady; }
    Result initResult() { return g_initRc; }

    // ------------------------------------------------------------- helpers

    bool sameAddr(const BtdrvAddress& a, const BtdrvAddress& b) {
        return std::memcmp(a.address, b.address, sizeof(a.address)) == 0;
    }

    std::string addrToString(const BtdrvAddress& a) {
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      a.address[0], a.address[1], a.address[2],
                      a.address[3], a.address[4], a.address[5]);
        return buf;
    }

    std::string resultToString(Result rc) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04u-%04u",
                      2000 + R_MODULE(rc), R_DESCRIPTION(rc));
        return buf;
    }

    std::string typeToString(DevType t) {
        switch (t) {
            case DevType::Gamepad: return "Controller";
            case DevType::Audio:   return "Audio";
            default:               return "Other";
        }
    }

    bool audioSupported() { return hosversionAtLeast(13, 0, 0); }

    // ------------------------------------------------------------------ status

    Result isRadioOn(bool& out) {
        out = false;
        if (!g_btmsysReady) return g_initRc;
        return btmsysGetRadioOnOff(&out);
    }

    Result getPaired(std::vector<Device>& out) {
        out.clear();
        if (!g_btmReady) return g_initRc;

        std::vector<BtmDeviceInfoV13> infos(MaxDeviceInfo);
        s32 total = 0;
        Result rc = btmGetDeviceInfo(BtmProfile_None, infos.data(), infos.size(), &total);
        if (R_FAILED(rc)) return recover(rc);
        total = std::clamp<s32>(total, 0, (s32)infos.size());

        // Connection status (currently connected devices)
        std::vector<BtmConnectedDeviceV13> conds(MaxDeviceCondition);
        s32 connectedTotal = 0;
        if (Result crc = btmGetDeviceCondition(BtmProfile_None, conds.data(), conds.size(), &connectedTotal); R_FAILED(crc)) {
            recover(crc);
            connectedTotal = 0;
        }
        connectedTotal = std::clamp<s32>(connectedTotal, 0, (s32)conds.size());

        out.reserve(total);
        for (s32 i = 0; i < total; i++) {
            const BtmDeviceInfoV13& info = infos[i];
            Device d;
            d.addr = info.addr;
            d.type = profileToType(info.profile);
            d.name = safeName(info.name, sizeof(info.name), info.addr);
            if (d.type == DevType::Gamepad) {
                d.vid = info.profile_info.hid_device_info.vid;
                d.pid = info.profile_info.hid_device_info.pid;
            }
            for (s32 j = 0; j < connectedTotal; j++) {
                if (sameAddr(conds[j].address, info.addr)) { d.connected = true; break; }
            }
            out.push_back(std::move(d));
        }

        // Audio: btm:sys's "connected" list is the most reliable for this profile
        if (g_btmsysReady) {
            std::vector<BtmAudioDevice> audio(MaxAudioDevices);
            s32 audioTotal = 0;
            Result arc = btmsysGetConnectedAudioDevices(audio.data(), audio.size(), &audioTotal);
            if (R_FAILED(arc)) recover(arc);
            else {
                audioTotal = std::clamp<s32>(audioTotal, 0, (s32)audio.size());
                for (auto& d : out) {
                    if (d.type != DevType::Audio) continue;
                    for (s32 j = 0; j < audioTotal; j++)
                        if (sameAddr(audio[j].addr, d.addr)) { d.connected = true; break; }
                }
            }
        }
        return 0;
    }

    // ----------------------------------------------------- controller pairing

    Result startGamepadPairing() {
        if (!g_btmsysReady) return g_initRc;
        return btmsysStartGamepadPairing();
    }

    Result cancelGamepadPairing() {
        if (!g_btmsysReady) return g_initRc;
        return btmsysCancelGamepadPairing();
    }

    Result isGamepadPairing(bool& out) {
        out = false;
        if (!g_btmsysReady) return g_initRc;
        return btmsysIsGamepadPairingStarted(&out);
    }

    Result clearGamepadDatabase() {
        if (!g_btmsysReady) return g_initRc;
        return btmsysClearGamepadPairingDatabase();
    }

    // ------------------------------------------------------------------- audio

    Result startAudioDiscovery() {
        if (!g_btmsysReady) return g_initRc;
        return btmsysStartAudioDeviceDiscovery();
    }

    Result stopAudioDiscovery() {
        if (!g_btmsysReady) return g_initRc;
        return btmsysStopAudioDeviceDiscovery();
    }

    Result isDiscoveringAudio(bool& out) {
        out = false;
        if (!g_btmsysReady) return g_initRc;
        return btmsysIsDiscoveryingAudioDevice(&out);
    }

    Result getDiscoveredAudio(std::vector<Device>& out) {
        out.clear();
        if (!g_btmsysReady) return g_initRc;

        std::vector<BtmAudioDevice> found(MaxAudioDevices);
        s32 total = 0;
        Result rc = btmsysGetDiscoveredAudioDevice(found.data(), found.size(), &total);
        if (R_FAILED(rc)) return recover(rc);
        total = std::clamp<s32>(total, 0, (s32)found.size());

        for (s32 i = 0; i < total; i++) {
            Device d;
            d.addr = found[i].addr;
            d.name = safeName(found[i].name, sizeof(found[i].name), found[i].addr);
            d.type = DevType::Audio;
            out.push_back(std::move(d));
        }
        return 0;
    }

    Result connectAudio(const BtdrvAddress& addr) {
        if (!g_btmsysReady) return g_initRc;
        return btmsysConnectAudioDevice(addr);
    }

    Result isConnectingAudio(bool& out) {
        out = false;
        if (!g_btmsysReady) return g_initRc;
        return btmsysIsConnectingAudioDevice(&out);
    }

    // ----------------------------------------------------------------- management

    Result disconnect(const Device& dev) {
        if (!isReady()) return g_initRc;
        if (dev.type == DevType::Audio)
            return btmsysDisconnectAudioDevice(dev.addr);
        return btmHidDisconnect(dev.addr);
    }

    Result forget(const Device& dev) {
        if (!isReady()) return g_initRc;
        if (dev.type == DevType::Audio)
            return btmsysRemoveAudioDevicePairing(dev.addr);

        // Controller: disconnect first (if connected), then delete the record
        if (dev.connected) {
            btmHidDisconnect(dev.addr);
            svcSleepThread(300'000'000ULL); // 300 ms for btm to process
        }
        return btmRemoveDeviceInfo(dev.addr);
    }

} // namespace bt
