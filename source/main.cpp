/*
 * Bluetooth Manager — Ultrahand (libultrahand) overlay for the Nintendo Switch.
 *
 *  - Pair new controllers (SYNC mode)
 *  - Search for and pair Bluetooth headphones / speakers (13.0.0+)
 *  - View paired devices and their connection status, disconnect and forget
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
// UI helpers
// =========================================================================
namespace {

    u64 nowMs() {
        return armTicksToNs(armGetSystemTick()) / 1'000'000ULL;
    }

    /// Throttles Bluetooth service calls made from update()
    struct Ticker {
        u64 last = 0;
        bool due(u64 intervalMs) {
            const u64 n = nowMs();
            if (last == 0 || n - last >= intervalMs) { last = n; return true; }
            return false;
        }
    };

    /// Two-press confirmation (A to arm, A again to execute)
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

    /// Grey description text below items. Use '\n' for line breaks.
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

    /// Screen shown when the Bluetooth service could not be opened
    tsl::elm::Element* makeErrorFrame(const std::string& title) {
        auto* frame = new tsl::elm::OverlayFrame(title, "Bluetooth Manager v" APP_VERSION);
        auto* list  = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("Error"));
        list->addItem(new tsl::elm::ListItem("Bluetooth service", bt::resultToString(bt::initResult())));
        addDescription(list, hosversionBefore(13, 0, 0)
            ? "This overlay requires firmware 13.0.0 or later."
            : "Could not open the btm / btm:sys services.\nRestart the console and try again.");
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

    const char* connectedLabel(bool connected) { return connected ? "Connected" : "Disconnected"; }

} // namespace

// =========================================================================
// Paired device details
// =========================================================================
class DeviceGui : public tsl::Gui {
public:
    explicit DeviceGui(bt::Device dev) : m_dev(std::move(dev)) {}

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(m_dev.name, bt::typeToString(m_dev.type));
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Information"));
        list->addItem(new tsl::elm::ListItem("Type", bt::typeToString(m_dev.type)));
        list->addItem(new tsl::elm::ListItem("Address", bt::addrToString(m_dev.addr)));
        if (m_dev.type == bt::DevType::Gamepad && (m_dev.vid || m_dev.pid)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04X:%04X", m_dev.vid, m_dev.pid);
            list->addItem(new tsl::elm::ListItem("VID:PID", buf));
        }
        m_stateItem = new tsl::elm::ListItem("Status", connectedLabel(m_dev.connected));
        list->addItem(m_stateItem);

        list->addItem(new tsl::elm::CategoryHeader("Actions"));

        if (m_dev.type == bt::DevType::Audio) {
            auto* connectItem = new tsl::elm::ListItem("Connect");
            connectItem->setClickListener([this](u64 keys) {
                if (!(keys & KEY_A)) return false;
                if (m_dev.connected) { notify("Already connected"); return true; }
                const Result rc = bt::connectAudio(m_dev.addr);
                notify(R_SUCCEEDED(rc) ? "Connecting to " + m_dev.name + "…"
                                       : "Failed to connect (" + bt::resultToString(rc) + ")");
                return true;
            });
            list->addItem(connectItem);
        }

        auto* disconnectItem = new tsl::elm::ListItem("Disconnect");
        disconnectItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (!m_dev.connected) { notify("Device is not connected"); return true; }
            const Result rc = bt::disconnect(m_dev);
            notify(R_SUCCEEDED(rc) ? m_dev.name + " disconnected"
                                   : "Failed to disconnect (" + bt::resultToString(rc) + ")");
            return true;
        });
        list->addItem(disconnectItem);

        m_forgetItem = new tsl::elm::ListItem("Forget device");
        m_forgetItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (!m_confirm.press()) {
                m_forgetItem->setValue("Press  to confirm");
                return true;
            }
            m_forgetItem->setValue("");
            const Result rc = bt::forget(m_dev);
            if (R_SUCCEEDED(rc)) {
                notify(m_dev.name + " forgotten");
                m_gone = true;
                tsl::goBack();
            } else {
                notify("Failed to forget (" + bt::resultToString(rc) + ")");
            }
            return true;
        });
        list->addItem(m_forgetItem);

        addDescription(list, m_dev.type == bt::DevType::Gamepad
            ? "Joy-Con re-pair automatically when\nattached to the console."
            : "To use a forgotten device again, pair it\nagain from \"Headphones / speakers\".");

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
        // No longer paired (removed elsewhere)
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
// Paired device list
// =========================================================================
class DevicesGui : public tsl::Gui {
public:
    DevicesGui() { m_rc = bt::getPaired(m_devs); }

    tsl::elm::Element* createUI() override {
        if (!bt::isReady()) return makeErrorFrame("Devices");

        auto* frame = new tsl::elm::OverlayFrame("Devices", "Paired with this console");
        auto* list  = new tsl::elm::List();

        if (R_FAILED(m_rc)) {
            list->addItem(new tsl::elm::CategoryHeader("Error"));
            list->addItem(new tsl::elm::ListItem("Read devices", bt::resultToString(m_rc)));
            frame->setContent(list);
            return frame;
        }

        // Key hint: A = connect / disconnect, Y = details
        list->addItem(new tsl::elm::CategoryHeader(
            "\uE0E0 Connect / disconnect " + ult::DIVIDER_SYMBOL + " \uE0E3 Details"));

        addSection(list, "Controllers", bt::DevType::Gamepad, "No controllers paired");
        addSection(list, "Audio",       bt::DevType::Audio,   "No audio devices paired");

        // Devices with other profiles (rare)
        bool hasOther = false;
        for (auto& d : m_devs) if (d.type == bt::DevType::Other) { hasOther = true; break; }
        if (hasOther) addSection(list, "Other", bt::DevType::Other, "");

        list->addItem(new tsl::elm::CategoryHeader("Actions"));
        m_clearItem = new tsl::elm::ListItem("Forget all controllers");
        m_clearItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (!m_confirm.press()) {
                m_clearItem->setValue("Press  to confirm");
                return true;
            }
            m_clearItem->setValue("");
            const Result rc = bt::clearGamepadDatabase();
            notify(R_SUCCEEDED(rc) ? "All controllers forgotten"
                                   : "Failed (" + bt::resultToString(rc) + ")");
            if (R_SUCCEEDED(rc)) tsl::swapTo<DevicesGui>();
            return true;
        });
        list->addItem(m_clearItem);
        addDescription(list, "Removes all wireless controllers. Joy-Con\nre-pair when attached to the console.");

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (m_confirm.expired()) { m_confirm.reset(); if (m_clearItem) m_clearItem->setValue(""); }
        if (!bt::isReady() || !m_tick.due(1000)) return;

        std::vector<bt::Device> now;
        if (R_FAILED(bt::getPaired(now))) return;

        // The list changed (new or forgotten device): rebuild the screen
        if (!sameAddressSet(now, m_devs)) {
            tsl::swapTo<DevicesGui>();
            return;
        }
        // Only the connection status changed: update values in place
        for (const auto& d : now) {
            auto it = m_items.find(addrKey(d.addr));
            if (it == m_items.end()) continue;
            for (auto& old : m_devs) {
                if (bt::sameAddr(old.addr, d.addr) && old.connected != d.connected) {
                    old.connected = d.connected;
                    it->second->setValue(d.connected ? "Connected" : "");
                }
            }
        }

        // Pending audio connection started with A: report the result
        if (!m_pendingKey.empty()) {
            const bt::Device* dev = findDevice(m_pendingKey);
            if (!dev) {
                m_pendingKey.clear();
            } else if (dev->connected) {
                notify(dev->name + " connected");
                m_pendingKey.clear();
            } else if (nowMs() - m_pendingAt > 20000) {
                notify("Could not connect to " + dev->name);
                if (auto it = m_items.find(m_pendingKey); it != m_items.end())
                    it->second->setValue("");
                m_pendingKey.clear();
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
            auto* item = new tsl::elm::ListItem(d.name, d.connected ? "Connected" : "");
            const std::string key = addrKey(d.addr);
            item->setClickListener([this, item, key](u64 keys) {
                if (keys & KEY_A) {
                    toggleConnection(key);
                    return true;
                }
                if (keys & KEY_Y) {
                    const bt::Device* dev = findDevice(key);
                    if (!dev) return true;
                    item->triggerClickAnimation();
                    triggerSettingsFeedback();
                    tsl::changeTo<DeviceGui>(*dev);
                    return true;
                }
                return false;
            });
            m_items[key] = item;
            list->addItem(item);
        }
    }

    bt::Device* findDevice(const std::string& key) {
        for (auto& d : m_devs) if (addrKey(d.addr) == key) return &d;
        return nullptr;
    }

    /// A: connect (audio) or disconnect the device under the cursor
    void toggleConnection(const std::string& key) {
        bt::Device* dev = findDevice(key);
        if (!dev) return;
        auto it = m_items.find(key);
        tsl::elm::ListItem* item = it != m_items.end() ? it->second : nullptr;

        if (dev->connected) {
            const Result rc = bt::disconnect(*dev);
            notify(R_SUCCEEDED(rc) ? dev->name + " disconnected"
                                   : "Failed to disconnect (" + bt::resultToString(rc) + ")");
            return;
        }

        switch (dev->type) {
            case bt::DevType::Audio: {
                if (!m_pendingKey.empty()) { notify("A connection is already in progress"); return; }
                const Result rc = bt::connectAudio(dev->addr);
                if (R_FAILED(rc)) {
                    notify("Failed to connect (" + bt::resultToString(rc) + ")");
                    return;
                }
                m_pendingKey = key;
                m_pendingAt  = nowMs();
                if (item) item->setValue("Connecting…");
                break;
            }
            case bt::DevType::Gamepad:
                // The console cannot initiate a controller connection: the controller must
                // connect to the console itself (any button press wakes it up).
                notify("Press any button on the controller to connect it");
                break;
            default:
                notify("Not supported for this device");
                break;
        }
    }

    std::vector<bt::Device> m_devs;
    std::unordered_map<std::string, tsl::elm::ListItem*> m_items;
    tsl::elm::ListItem* m_clearItem = nullptr;
    std::string m_pendingKey;      // audio device being connected with A
    u64         m_pendingAt = 0;
    Result  m_rc = 0;
    Confirm m_confirm;
    Ticker  m_tick;
};

// =========================================================================
// Pair controller (SYNC mode)
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
        if (!bt::isReady()) return makeErrorFrame("Pair controller");

        auto* frame = new tsl::elm::OverlayFrame("Pair controller", "Pairing mode");
        m_list = new tsl::elm::List();

        m_list->addItem(new tsl::elm::CategoryHeader("Status"));
        m_statusItem = new tsl::elm::ListItem("Pairing",
            R_SUCCEEDED(m_rc) ? "Active" : "Error " + bt::resultToString(m_rc));
        m_list->addItem(m_statusItem);

        addDescription(m_list,
            "Hold the SYNC button on the controller.\n"
            "Joy-Con: button between SL and SR.\n"
            "Pro Controller: next to the USB-C port.");

        auto* restart = new tsl::elm::ListItem("Restart pairing");
        restart->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            bt::cancelGamepadPairing();
            m_rc = bt::startGamepadPairing();
            m_statusItem->setValue(R_SUCCEEDED(m_rc) ? "Active" : "Error " + bt::resultToString(m_rc));
            return true;
        });
        m_list->addItem(restart);

        auto* stop = new tsl::elm::ListItem("Stop pairing");
        stop->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            bt::cancelGamepadPairing();
            m_statusItem->setValue("Stopped");
            return true;
        });
        m_list->addItem(stop);

        addDescription(m_list,
            "Other controllers (PS4/PS5/Xbox…) require\n"
            "the MissionControl sysmodule.");

        frame->setContent(m_list);
        return frame;
    }

    void update() override {
        if (!bt::isReady() || !m_tick.due(500)) return;

        bool active = false;
        if (R_SUCCEEDED(bt::isGamepadPairing(active)))
            m_statusItem->setValue(active ? "Active" : "Finished");

        std::vector<bt::Device> devs;
        if (R_FAILED(bt::getPaired(devs))) return;
        for (const auto& d : devs) {
            if (d.type != bt::DevType::Gamepad || isKnown(d.addr)) continue;
            m_known.push_back(d.addr);

            if (!m_newHeader) {
                m_list->addItem(new tsl::elm::CategoryHeader("New controllers"));
                m_newHeader = true;
            }
            m_list->addItem(new tsl::elm::ListItem(d.name, "Paired"));
            notify(d.name + " paired");
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
// Pair audio device (13.0.0+)
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
        if (!bt::isReady()) return makeErrorFrame("Pair audio");

        auto* frame = new tsl::elm::OverlayFrame("Pair audio", "Headphones and speakers");
        m_list = new tsl::elm::List();

        m_list->addItem(new tsl::elm::CategoryHeader("Status"));
        m_statusItem = new tsl::elm::ListItem("Search",
            R_SUCCEEDED(m_rc) ? "Searching…" : "Error " + bt::resultToString(m_rc));
        m_list->addItem(m_statusItem);

        auto* rescan = new tsl::elm::ListItem("Search again");
        rescan->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (m_connecting) { notify("Wait for the connection to finish"); return true; }
            bt::stopAudioDiscovery();
            m_rc = bt::startAudioDiscovery();
            m_statusItem->setValue(R_SUCCEEDED(m_rc) ? "Searching…" : "Error " + bt::resultToString(m_rc));
            return true;
        });
        m_list->addItem(rescan);

        addDescription(m_list,
            "Put the device in pairing mode.\n"
            "While audio is connected, only 2 wireless\n"
            "controllers work and local play is unavailable.");

        m_list->addItem(new tsl::elm::CategoryHeader("Devices found"));

        frame->setContent(m_list);
        return frame;
    }

    void update() override {
        if (!bt::isReady() || !m_tick.due(500)) return;

        if (m_connecting) { pollConnection(); return; }

        bool discovering = false;
        if (R_SUCCEEDED(bt::isDiscoveringAudio(discovering)) && R_SUCCEEDED(m_rc))
            m_statusItem->setValue(discovering ? "Searching…" : "Search finished");

        std::vector<bt::Device> found;
        if (R_FAILED(bt::getDiscoveredAudio(found))) return;
        for (const auto& d : found) {
            const std::string key = addrKey(d.addr);
            if (m_items.count(key)) continue;

            auto* item = new tsl::elm::ListItem(d.name, isPaired(d.addr) ? "Paired" : "");
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
        if (m_connecting) { notify("A connection is already in progress"); return; }

        bt::stopAudioDiscovery();
        const Result rc = bt::connectAudio(dev.addr);
        if (R_FAILED(rc)) {
            notify("Failed to connect (" + bt::resultToString(rc) + ")");
            return;
        }
        m_connecting   = true;
        m_target       = dev;
        m_connectStart = nowMs();
        m_statusItem->setValue("Connecting…");
        if (auto it = m_items.find(addrKey(dev.addr)); it != m_items.end())
            it->second->setValue("Connecting…");
    }

    void pollConnection() {
        bool busy = false;
        bt::isConnectingAudio(busy);
        const u64 elapsed = nowMs() - m_connectStart;
        // Give btm some time to flag the connection as "in progress";
        // after 30 s we stop waiting (safety limit)
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
            notify(m_target.name + " connected");
            m_statusItem->setValue("Connected");
            if (it != m_items.end()) it->second->setValue("Connected");
        } else {
            notify("Could not connect to " + m_target.name);
            m_statusItem->setValue("Failed");
            if (it != m_items.end()) it->second->setValue(isPaired(m_target.addr) ? "Paired" : "");
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
// Main menu
// =========================================================================
class MainGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        if (!bt::isReady()) return makeErrorFrame("Bluetooth");

        auto* frame = new tsl::elm::OverlayFrame("Bluetooth", "Bluetooth Manager v" APP_VERSION);
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Status"));
        m_radioItem = new tsl::elm::ListItem("Bluetooth radio", "…");
        m_radioItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            toggleRadio();
            return true;
        });
        list->addItem(m_radioItem);
        addDescription(list, "\uE0E0 turns only Bluetooth on/off.\nAirplane mode and Wi-Fi are not changed.");

        list->addItem(new tsl::elm::CategoryHeader("Pair"));

        auto* pairPad = new tsl::elm::ListItem("Controller", "");
        pairPad->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<PairGamepadGui>();
            return true;
        });
        list->addItem(pairPad);

        auto* pairAudio = new tsl::elm::ListItem("Headphones / speakers", "");
        pairAudio->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<PairAudioGui>();
            return true;
        });
        list->addItem(pairAudio);

        list->addItem(new tsl::elm::CategoryHeader("Manage"));
        m_pairedItem = new tsl::elm::ListItem("Paired devices", "…");
        m_pairedItem->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<DevicesGui>();
            return true;
        });
        list->addItem(m_pairedItem);

        addDescription(list, "Connect, disconnect or forget devices.");

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (m_radioConfirm.expired()) { m_radioConfirm.reset(); m_tick.last = 0; }
        if (!bt::isReady() || !m_tick.due(1000)) return;

        bool on = false;
        if (!m_radioConfirm.armed && R_SUCCEEDED(bt::isRadioOn(on))) {
            m_radioOn = on;
            m_radioItem->setValue(on ? "On" : "Off");
        }

        std::vector<bt::Device> devs;
        if (R_SUCCEEDED(bt::getPaired(devs))) {
            size_t connected = 0;
            for (auto& d : devs) if (d.connected) connected++;
            m_pairedItem->setValue(std::to_string(devs.size()) + " (" +
                                   std::to_string(connected) + " connected)");
        }
    }

private:
    void toggleRadio() {
        bool on = false;
        if (R_FAILED(bt::isRadioOn(on))) on = m_radioOn;

        if (!on) {
            const Result rc = bt::setRadio(true);
            notify(R_SUCCEEDED(rc) ? "Bluetooth turned on"
                                   : "Failed to turn on Bluetooth (" + bt::resultToString(rc) + ")");
            m_tick.last = 0;   // refresh status on the next frame
            return;
        }

        // Turning off: always ask for confirmation, with a stronger warning when
        // every connected controller is wireless (input would be lost).
        if (!m_radioConfirm.press()) {
            m_radioItem->setValue(bt::hasNonBluetoothInput()
                ? "Press \uE0E0 to turn off"
                : "No wired input! \uE0E0 again to confirm");
            return;
        }
        const Result rc = bt::setRadio(false);
        notify(R_SUCCEEDED(rc) ? "Bluetooth turned off"
                               : "Failed to turn off Bluetooth (" + bt::resultToString(rc) + ")");
        m_tick.last = 0;
    }

    tsl::elm::ListItem* m_radioItem  = nullptr;
    tsl::elm::ListItem* m_pairedItem = nullptr;
    bool    m_radioOn = false;
    Confirm m_radioConfirm;
    Ticker  m_tick;
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
