/*
 * Bluetooth Manager — overlay para Ultrahand (libultrahand) na Nintendo Switch.
 *
 *  - Emparelhar novos comandos (modo SYNC)
 *  - Procurar e emparelhar auscultadores / colunas Bluetooth (13.0.0+)
 *  - Ver dispositivos emparelhados, estado de ligação, desligar e esquecer
 */
#define TESLA_INIT_IMPL
#if USE_EXCEPTION_WRAP
#include <exception_wrap.hpp>
#endif
#include <tesla.hpp>

#include <string>
#include <vector>
#include <unordered_map>

#include "bt_manager.hpp"

#ifndef APP_VERSION
#define APP_VERSION "1.0.0"
#endif

// =========================================================================
// Utilitários de UI
// =========================================================================
namespace {

    u64 nowMs() {
        return armTicksToNs(armGetSystemTick()) / 1'000'000ULL;
    }

    /// Limita a frequência de chamadas ao serviço Bluetooth dentro de update()
    struct Ticker {
        u64 last = 0;
        bool due(u64 intervalMs) {
            const u64 n = nowMs();
            if (last == 0 || n - last >= intervalMs) { last = n; return true; }
            return false;
        }
    };

    /// Confirmação em dois toques (A para armar, A outra vez para executar)
    struct Confirm {
        bool armed = false;
        u64  at    = 0;
        bool press() {
            if (armed && nowMs() - at < 3000) { armed = false; return true; }
            armed = true; at = nowMs();
            return false;
        }
        bool expired() { return armed && nowMs() - at >= 3000; }
        void reset() { armed = false; }
    };

    void notify(const std::string& msg) {
        if (tsl::notification)
            tsl::notification->showNow(ult::NOTIFY_HEADER + msg, 22);
    }

    std::string addrKey(const BtdrvAddress& a) { return bt::addrToString(a); }

    /// Texto descritivo (cinzento) por baixo dos itens. Usar '\n' para quebrar linhas.
    void addDescription(tsl::elm::List* list, const std::string& text) {
        size_t lines = 1;
        for (char c : text) if (c == '\n') lines++;
        const u16 height = static_cast<u16>(18 + lines * 20);
        list->addItem(new tsl::elm::CustomDrawer(
            [text](tsl::gfx::Renderer* r, s32 x, s32 y, s32, s32) {
                r->drawString(text, false, x + 20, y + 22, 15,
                              r->a(tsl::style::color::ColorDescription));
            }), height);
    }

    /// Ecrã usado quando o serviço Bluetooth não pôde ser aberto
    tsl::elm::Element* makeErrorFrame(const std::string& title) {
        auto* frame = new tsl::elm::OverlayFrame(title, "Bluetooth Manager v" APP_VERSION);
        auto* list  = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("Erro"));
        list->addItem(new tsl::elm::ListItem("Serviço Bluetooth", bt::resultToString(bt::initResult())));
        addDescription(list, hosversionBefore(13, 0, 0)
            ? "Este overlay requer o firmware 13.0.0 ou superior."
            : "Não foi possível abrir os serviços btm / btm:sys.\nReinicia a consola e tenta novamente.");
        frame->setContent(list);
        return frame;
    }

    bool sameAddressSet(const std::vector<bt::Device>& a, const std::vector<bt::Device>& b) {
        if (a.size() != b.size()) return false;
        for (const auto& x : a) {
            bool found = false;
            for (const auto& y : b) if (bt::sameAddr(x.addr, y.addr)) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }

    const char* connectedLabel(bool connected) { return connected ? "Ligado" : "Desligado"; }

} // namespace

// =========================================================================
// Detalhe de um dispositivo emparelhado
// =========================================================================
class DeviceGui : public tsl::Gui {
public:
    explicit DeviceGui(bt::Device dev) : m_dev(std::move(dev)) {}

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(m_dev.name, bt::typeToString(m_dev.type));
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Informação"));
        list->addItem(new tsl::elm::ListItem("Tipo", bt::typeToString(m_dev.type)));
        list->addItem(new tsl::elm::ListItem("Endereço", bt::addrToString(m_dev.addr)));
        if (m_dev.type == bt::DevType::Gamepad && (m_dev.vid || m_dev.pid)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04X:%04X", m_dev.vid, m_dev.pid);
            list->addItem(new tsl::elm::ListItem("VID:PID", buf));
        }
        m_stateItem = new tsl::elm::ListItem("Estado", connectedLabel(m_dev.connected));
        list->addItem(m_stateItem);

        list->addItem(new tsl::elm::CategoryHeader("Ações"));

        if (m_dev.type == bt::DevType::Audio) {
            auto* connectItem = new tsl::elm::ListItem("Ligar");
            connectItem->setClickListener([this](u64 keys) {
                if (!(keys & KEY_A)) return false;
                if (m_dev.connected) { notify("Já está ligado"); return true; }
                const Result rc = bt::connectAudio(m_dev.addr);
                notify(R_SUCCEEDED(rc) ? "A ligar a " + m_dev.name + "…"
                                       : "Falha ao ligar (" + bt::resultToString(rc) + ")");
                return true;
            });
            list->addItem(connectItem);
        }

        auto* disconnectItem = new tsl::elm::ListItem("Desligar");
        disconnectItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (!m_dev.connected) { notify("O dispositivo não está ligado"); return true; }
            const Result rc = bt::disconnect(m_dev);
            notify(R_SUCCEEDED(rc) ? m_dev.name + " desligado"
                                   : "Falha ao desligar (" + bt::resultToString(rc) + ")");
            return true;
        });
        list->addItem(disconnectItem);

        m_forgetItem = new tsl::elm::ListItem("Esquecer dispositivo");
        m_forgetItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (!m_confirm.press()) {
                m_forgetItem->setValue("Prime  para confirmar");
                return true;
            }
            m_forgetItem->setValue("");
            const Result rc = bt::forget(m_dev);
            if (R_SUCCEEDED(rc)) {
                notify(m_dev.name + " foi esquecido");
                m_gone = true;
                tsl::goBack();
            } else {
                notify("Falha ao esquecer (" + bt::resultToString(rc) + ")");
            }
            return true;
        });
        list->addItem(m_forgetItem);

        addDescription(list, m_dev.type == bt::DevType::Gamepad
            ? "Os Joy-Con voltam a emparelhar sozinhos\nquando encaixados na consola."
            : "Para voltar a usar um dispositivo esquecido,\nemparelha-o de novo em \"Áudio\".");

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (m_confirm.expired()) { m_confirm.reset(); m_forgetItem->setValue(""); }
        if (m_gone || !m_tick.due(1000)) return;

        std::vector<bt::Device> devs;
        if (R_FAILED(bt::getPaired(devs))) return;
        for (const auto& d : devs) {
            if (bt::sameAddr(d.addr, m_dev.addr)) {
                if (d.connected != m_dev.connected) {
                    m_dev.connected = d.connected;
                    m_stateItem->setValue(connectedLabel(m_dev.connected));
                }
                return;
            }
        }
        // Deixou de estar emparelhado (removido noutro sítio)
        m_gone = true;
        tsl::goBack();
    }

private:
    bt::Device m_dev;
    tsl::elm::ListItem* m_stateItem  = nullptr;
    tsl::elm::ListItem* m_forgetItem = nullptr;
    Confirm m_confirm;
    Ticker  m_tick;
    bool    m_gone = false;
};

// =========================================================================
// Lista de dispositivos emparelhados
// =========================================================================
class DevicesGui : public tsl::Gui {
public:
    DevicesGui() { m_rc = bt::getPaired(m_devs); }

    tsl::elm::Element* createUI() override {
        if (!bt::isReady()) return makeErrorFrame("Dispositivos");

        auto* frame = new tsl::elm::OverlayFrame("Dispositivos", "Emparelhados nesta consola");
        auto* list  = new tsl::elm::List();

        if (R_FAILED(m_rc)) {
            list->addItem(new tsl::elm::CategoryHeader("Erro"));
            list->addItem(new tsl::elm::ListItem("Ler dispositivos", bt::resultToString(m_rc)));
            frame->setContent(list);
            return frame;
        }

        addSection(list, "Comandos", bt::DevType::Gamepad, "Nenhum comando emparelhado");
        addSection(list, "Áudio",    bt::DevType::Audio,   "Nenhum dispositivo de áudio");

        // Dispositivos de outros perfis (raro)
        bool hasOther = false;
        for (auto& d : m_devs) if (d.type == bt::DevType::Other) { hasOther = true; break; }
        if (hasOther) addSection(list, "Outros", bt::DevType::Other, "");

        list->addItem(new tsl::elm::CategoryHeader("Ações"));
        m_clearItem = new tsl::elm::ListItem("Esquecer todos os comandos");
        m_clearItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (!m_confirm.press()) {
                m_clearItem->setValue("Prime  para confirmar");
                return true;
            }
            m_clearItem->setValue("");
            const Result rc = bt::clearGamepadDatabase();
            notify(R_SUCCEEDED(rc) ? "Todos os comandos foram esquecidos"
                                   : "Falha (" + bt::resultToString(rc) + ")");
            if (R_SUCCEEDED(rc)) tsl::swapTo<DevicesGui>();
            return true;
        });
        list->addItem(m_clearItem);
        addDescription(list, "Remove todos os comandos sem fios. Os Joy-Con\nvoltam a emparelhar quando encaixados.");

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (m_confirm.expired()) { m_confirm.reset(); if (m_clearItem) m_clearItem->setValue(""); }
        if (!bt::isReady() || !m_tick.due(1000)) return;

        std::vector<bt::Device> now;
        if (R_FAILED(bt::getPaired(now))) return;

        // A lista mudou (novo dispositivo, dispositivo esquecido): reconstruir o ecrã
        if (!sameAddressSet(now, m_devs)) {
            tsl::swapTo<DevicesGui>();
            return;
        }
        // Só o estado de ligação mudou: atualizar os valores no sítio
        for (const auto& d : now) {
            auto it = m_items.find(addrKey(d.addr));
            if (it == m_items.end()) continue;
            for (auto& old : m_devs) {
                if (bt::sameAddr(old.addr, d.addr) && old.connected != d.connected) {
                    old.connected = d.connected;
                    it->second->setValue(d.connected ? "Ligado" : "");
                }
            }
        }
    }

private:
    void addSection(tsl::elm::List* list, const std::string& title, bt::DevType type, const std::string& emptyText) {
        size_t count = 0;
        for (auto& d : m_devs) if (d.type == type) count++;
        list->addItem(new tsl::elm::CategoryHeader(title + " (" + std::to_string(count) + ")"));

        if (count == 0) {
            if (!emptyText.empty()) addDescription(list, emptyText);
            return;
        }
        for (const auto& d : m_devs) {
            if (d.type != type) continue;
            auto* item = new tsl::elm::ListItem(d.name, d.connected ? "Ligado" : "");
            const bt::Device copy = d;
            item->setClickListener([copy](u64 keys) {
                if (!(keys & KEY_A)) return false;
                tsl::changeTo<DeviceGui>(copy);
                return true;
            });
            m_items[addrKey(d.addr)] = item;
            list->addItem(item);
        }
    }

    std::vector<bt::Device> m_devs;
    std::unordered_map<std::string, tsl::elm::ListItem*> m_items;
    tsl::elm::ListItem* m_clearItem = nullptr;
    Result  m_rc = 0;
    Confirm m_confirm;
    Ticker  m_tick;
};

// =========================================================================
// Emparelhar comando (modo SYNC)
// =========================================================================
class PairGamepadGui : public tsl::Gui {
public:
    PairGamepadGui() {
        std::vector<bt::Device> devs;
        if (R_SUCCEEDED(bt::getPaired(devs)))
            for (auto& d : devs) m_known.push_back(d.addr);
        m_rc = bt::startGamepadPairing();
    }

    ~PairGamepadGui() override {
        bool active = false;
        if (R_SUCCEEDED(bt::isGamepadPairing(active)) && active)
            bt::cancelGamepadPairing();
    }

    tsl::elm::Element* createUI() override {
        if (!bt::isReady()) return makeErrorFrame("Emparelhar comando");

        auto* frame = new tsl::elm::OverlayFrame("Emparelhar comando", "Modo de emparelhamento");
        m_list = new tsl::elm::List();

        m_list->addItem(new tsl::elm::CategoryHeader("Estado"));
        m_statusItem = new tsl::elm::ListItem("Emparelhamento",
            R_SUCCEEDED(m_rc) ? "Ativo" : "Erro " + bt::resultToString(m_rc));
        m_list->addItem(m_statusItem);

        addDescription(m_list,
            "Mantém premido o botão SYNC do comando.\n"
            "Joy-Con: botão entre SL e SR.\n"
            "Pro Controller: junto à porta USB-C.");

        auto* restart = new tsl::elm::ListItem("Reiniciar emparelhamento");
        restart->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            bt::cancelGamepadPairing();
            m_rc = bt::startGamepadPairing();
            m_statusItem->setValue(R_SUCCEEDED(m_rc) ? "Ativo" : "Erro " + bt::resultToString(m_rc));
            return true;
        });
        m_list->addItem(restart);

        auto* stop = new tsl::elm::ListItem("Parar emparelhamento");
        stop->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            bt::cancelGamepadPairing();
            m_statusItem->setValue("Parado");
            return true;
        });
        m_list->addItem(stop);

        addDescription(m_list,
            "Outros comandos (PS4/PS5/Xbox…) requerem o\n"
            "sysmodule MissionControl instalado.");

        frame->setContent(m_list);
        return frame;
    }

    void update() override {
        if (!bt::isReady() || !m_tick.due(500)) return;

        bool active = false;
        if (R_SUCCEEDED(bt::isGamepadPairing(active)))
            m_statusItem->setValue(active ? "Ativo" : "Terminado");

        std::vector<bt::Device> devs;
        if (R_FAILED(bt::getPaired(devs))) return;
        for (const auto& d : devs) {
            if (d.type != bt::DevType::Gamepad || isKnown(d.addr)) continue;
            m_known.push_back(d.addr);

            if (!m_newHeader) {
                m_list->addItem(new tsl::elm::CategoryHeader("Novos comandos"));
                m_newHeader = true;
            }
            m_list->addItem(new tsl::elm::ListItem(d.name, "Emparelhado"));
            notify(d.name + " emparelhado");
        }
    }

private:
    bool isKnown(const BtdrvAddress& a) const {
        for (auto& k : m_known) if (bt::sameAddr(k, a)) return true;
        return false;
    }

    std::vector<BtdrvAddress> m_known;
    tsl::elm::List*     m_list = nullptr;
    tsl::elm::ListItem* m_statusItem = nullptr;
    bool   m_newHeader = false;
    Result m_rc = 0;
    Ticker m_tick;
};

// =========================================================================
// Emparelhar dispositivo de áudio (13.0.0+)
// =========================================================================
class PairAudioGui : public tsl::Gui {
public:
    PairAudioGui() {
        std::vector<bt::Device> devs;
        if (R_SUCCEEDED(bt::getPaired(devs)))
            for (auto& d : devs) if (d.type == bt::DevType::Audio) m_paired.push_back(d.addr);
        m_rc = bt::startAudioDiscovery();
    }

    ~PairAudioGui() override {
        bool active = false;
        if (R_SUCCEEDED(bt::isDiscoveringAudio(active)) && active)
            bt::stopAudioDiscovery();
    }

    tsl::elm::Element* createUI() override {
        if (!bt::isReady()) return makeErrorFrame("Emparelhar áudio");

        auto* frame = new tsl::elm::OverlayFrame("Emparelhar áudio", "Auscultadores e colunas");
        m_list = new tsl::elm::List();

        m_list->addItem(new tsl::elm::CategoryHeader("Estado"));
        m_statusItem = new tsl::elm::ListItem("Pesquisa",
            R_SUCCEEDED(m_rc) ? "A procurar…" : "Erro " + bt::resultToString(m_rc));
        m_list->addItem(m_statusItem);

        auto* rescan = new tsl::elm::ListItem("Procurar novamente");
        rescan->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (m_connecting) { notify("Aguarda o fim da ligação"); return true; }
            bt::stopAudioDiscovery();
            m_rc = bt::startAudioDiscovery();
            m_statusItem->setValue(R_SUCCEEDED(m_rc) ? "A procurar…" : "Erro " + bt::resultToString(m_rc));
            return true;
        });
        m_list->addItem(rescan);

        addDescription(m_list,
            "Coloca o dispositivo em modo de emparelhamento.\n"
            "Com áudio ligado só podes usar 2 comandos\n"
            "sem fios e o modo local fica indisponível.");

        m_list->addItem(new tsl::elm::CategoryHeader("Dispositivos encontrados"));

        frame->setContent(m_list);
        return frame;
    }

    void update() override {
        if (!bt::isReady() || !m_tick.due(500)) return;

        if (m_connecting) { pollConnection(); return; }

        bool discovering = false;
        if (R_SUCCEEDED(bt::isDiscoveringAudio(discovering)) && R_SUCCEEDED(m_rc))
            m_statusItem->setValue(discovering ? "A procurar…" : "Pesquisa terminada");

        std::vector<bt::Device> found;
        if (R_FAILED(bt::getDiscoveredAudio(found))) return;
        for (const auto& d : found) {
            const std::string key = addrKey(d.addr);
            if (m_items.count(key)) continue;

            auto* item = new tsl::elm::ListItem(d.name, isPaired(d.addr) ? "Emparelhado" : "");
            const bt::Device copy = d;
            item->setClickListener([this, copy](u64 keys) {
                if (!(keys & KEY_A)) return false;
                beginConnect(copy);
                return true;
            });
            m_items[key] = item;
            m_list->addItem(item);
        }
    }

private:
    void beginConnect(const bt::Device& dev) {
        if (m_connecting) { notify("Já existe uma ligação em curso"); return; }

        bt::stopAudioDiscovery();
        const Result rc = bt::connectAudio(dev.addr);
        if (R_FAILED(rc)) {
            notify("Falha ao ligar (" + bt::resultToString(rc) + ")");
            return;
        }
        m_connecting   = true;
        m_target       = dev;
        m_connectStart = nowMs();
        m_statusItem->setValue("A ligar…");
        if (auto it = m_items.find(addrKey(dev.addr)); it != m_items.end())
            it->second->setValue("A ligar…");
    }

    void pollConnection() {
        bool busy = false;
        bt::isConnectingAudio(busy);
        const u64 elapsed = nowMs() - m_connectStart;
        // Dar ao btm algum tempo para marcar a ligação como "em curso";
        // ao fim de 30 s desistimos de esperar (limite de segurança)
        if (elapsed < 1500 || (busy && elapsed < 30000)) return;

        m_connecting = false;

        bool ok = false;
        std::vector<bt::Device> devs;
        if (R_SUCCEEDED(bt::getPaired(devs)))
            for (auto& d : devs)
                if (bt::sameAddr(d.addr, m_target.addr) && d.connected) { ok = true; break; }

        auto it = m_items.find(addrKey(m_target.addr));
        if (ok) {
            m_paired.push_back(m_target.addr);
            notify(m_target.name + " ligado");
            m_statusItem->setValue("Ligado");
            if (it != m_items.end()) it->second->setValue("Ligado");
        } else {
            notify("Não foi possível ligar a " + m_target.name);
            m_statusItem->setValue("Falhou");
            if (it != m_items.end()) it->second->setValue(isPaired(m_target.addr) ? "Emparelhado" : "");
        }
    }

    bool isPaired(const BtdrvAddress& a) const {
        for (auto& p : m_paired) if (bt::sameAddr(p, a)) return true;
        return false;
    }

    std::vector<BtdrvAddress> m_paired;
    std::unordered_map<std::string, tsl::elm::ListItem*> m_items;
    tsl::elm::List*     m_list = nullptr;
    tsl::elm::ListItem* m_statusItem = nullptr;
    bt::Device m_target;
    bool   m_connecting   = false;
    u64    m_connectStart = 0;
    Result m_rc = 0;
    Ticker m_tick;
};

// =========================================================================
// Menu principal
// =========================================================================
class MainGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        if (!bt::isReady()) return makeErrorFrame("Bluetooth");

        auto* frame = new tsl::elm::OverlayFrame("Bluetooth", "Bluetooth Manager v" APP_VERSION);
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Estado"));
        m_radioItem = new tsl::elm::ListItem("Rádio Bluetooth", "…");
        list->addItem(m_radioItem);

        list->addItem(new tsl::elm::CategoryHeader("Emparelhar"));

        auto* pairPad = new tsl::elm::ListItem("Comando", "");
        pairPad->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<PairGamepadGui>();
            return true;
        });
        list->addItem(pairPad);

        auto* pairAudio = new tsl::elm::ListItem("Auscultadores / colunas", "");
        pairAudio->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<PairAudioGui>();
            return true;
        });
        list->addItem(pairAudio);

        list->addItem(new tsl::elm::CategoryHeader("Gerir"));
        m_pairedItem = new tsl::elm::ListItem("Dispositivos emparelhados", "…");
        m_pairedItem->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<DevicesGui>();
            return true;
        });
        list->addItem(m_pairedItem);

        addDescription(list, "Ligar, desligar ou esquecer dispositivos.");

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (!bt::isReady() || !m_tick.due(1000)) return;

        bool on = false;
        if (R_SUCCEEDED(bt::isRadioOn(on)))
            m_radioItem->setValue(on ? "Ligado" : "Desligado (modo de voo)");

        std::vector<bt::Device> devs;
        if (R_SUCCEEDED(bt::getPaired(devs))) {
            size_t connected = 0;
            for (auto& d : devs) if (d.connected) connected++;
            m_pairedItem->setValue(std::to_string(devs.size()) + " (" +
                                   std::to_string(connected) + " ligados)");
        }
    }

private:
    tsl::elm::ListItem* m_radioItem  = nullptr;
    tsl::elm::ListItem* m_pairedItem = nullptr;
    Ticker m_tick;
};

// =========================================================================
// Overlay
// =========================================================================
class BluetoothOverlay : public tsl::Overlay {
public:
    void initServices() override { bt::init(); }
    void exitServices() override { bt::exit(); }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<BluetoothOverlay>(argc, argv);
}
