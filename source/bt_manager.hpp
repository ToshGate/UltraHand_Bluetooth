#pragma once
/*
 * bt_manager.hpp — pequena camada sobre os serviços btm / btm:sys da libnx.
 *
 * O firmware da Switch só suporta oficialmente dois tipos de dispositivo:
 *   - Comandos (perfil HID): Joy-Con, Pro Controller e afins
 *     (outros comandos só funcionam com o sysmodule MissionControl)
 *   - Áudio (perfil Audio, firmware 13.0.0+): auscultadores, colunas, etc.
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

    // Serviços
    Result init();
    void   exit();
    bool   isReady();
    Result initResult();

    // Utilitários
    bool        sameAddr(const BtdrvAddress& a, const BtdrvAddress& b);
    std::string addrToString(const BtdrvAddress& a);
    std::string resultToString(Result rc);
    std::string typeToString(DevType t);
    bool        audioSupported();   // 13.0.0+

    // Estado
    Result isRadioOn(bool& out);

    // Dispositivos emparelhados (com estado de ligação)
    Result getPaired(std::vector<Device>& out);

    // Emparelhamento de comandos
    Result startGamepadPairing();
    Result cancelGamepadPairing();
    Result isGamepadPairing(bool& out);
    Result clearGamepadDatabase();

    // Descoberta / ligação de áudio
    Result startAudioDiscovery();
    Result stopAudioDiscovery();
    Result isDiscoveringAudio(bool& out);
    Result getDiscoveredAudio(std::vector<Device>& out);
    Result connectAudio(const BtdrvAddress& addr);
    Result isConnectingAudio(bool& out);

    // Gestão
    Result disconnect(const Device& dev);
    Result forget(const Device& dev);

} // namespace bt
