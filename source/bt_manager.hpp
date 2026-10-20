#pragma once
/*
 * bt_manager.hpp — thin layer over libnx's btm / btm:sys services.
 *
 * The Switch firmware officially supports only two device types:
 *   - Controllers (HID profile): Joy-Con, Pro Controller and similar
 *     (other controllers only work with the MissionControl sysmodule)
 *   - Audio (Audio profile, firmware 13.0.0+): headphones, speakers, etc.
 */
#include <switch.h>
#include <string>
#include <vector>

namespace bt {

    enum class DevType { Gamepad, Audio, Other };

    struct Device {
        BtdrvAddress addr{};
        std::string  name;
        DevType      type = DevType::Other;
        bool         connected = false;
        u16          vid = 0;
        u16          pid = 0;
    };

    // Services
    Result init();
    void   exit();
    bool   isReady();
    Result initResult();

    // Helpers
    bool        sameAddr(const BtdrvAddress& a, const BtdrvAddress& b);
    std::string addrToString(const BtdrvAddress& a);
    std::string resultToString(Result rc);
    std::string typeToString(DevType t);
    bool        audioSupported();   // 13.0.0+

    // Status
    Result isRadioOn(bool& out);

    // Paired devices (with connection status)
    Result getPaired(std::vector<Device>& out);

    // Controller pairing
    Result startGamepadPairing();
    Result cancelGamepadPairing();
    Result isGamepadPairing(bool& out);
    Result clearGamepadDatabase();

    // Audio discovery / connection
    Result startAudioDiscovery();
    Result stopAudioDiscovery();
    Result isDiscoveringAudio(bool& out);
    Result getDiscoveredAudio(std::vector<Device>& out);
    Result connectAudio(const BtdrvAddress& addr);
    Result isConnectingAudio(bool& out);

    // Management
    Result disconnect(const Device& dev);
    Result forget(const Device& dev);

} // namespace bt
