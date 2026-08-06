// Pilotage du VPN : compte Mullvad, clés, choix du relais, montée du tunnel.
//
// Tout se fait sur un thread dédié — l'API Mullvad et la poignée de main
// prennent plusieurs secondes, et l'IHM doit rester vivante.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bt/session.hpp"
#include "net/mullvad.hpp"
#include "net/wg/tunnel.hpp"

namespace vpn {

enum class State { Disconnected, Working, Connected, Failed };

class Manager {
public:
    ~Manager();

    void load();
    void save() const;

    bool has_account() const;
    std::string account_masked() const;
    void set_account(const std::string& number);

    // Lance la connexion en tâche de fond. Le transport de la session est
    // remplacé une fois le tunnel monté.
    void connect(bt::Session& session);
    void disconnect(bt::Session& session);
    bool busy() const { return busy_.load(); }

    // À appeler régulièrement depuis l'IHM : surveille la santé du tunnel.
    // Sans ça, un tunnel tombé laisse l'écran afficher « connecté » pendant que
    // plus rien ne passe — le pire des deux mondes pour comprendre ce qui arrive.
    void update(bt::Session& session);

    State state() const;
    std::string status() const;
    std::string relay_label() const;
    uint64_t bytes_sent() const;
    uint64_t bytes_received() const;

    // Pays disponibles, alimentés après une première récupération des relais.
    std::vector<std::string> countries() const;
    void set_preferred_country(const std::string& code);
    std::string preferred_country() const;

private:
    void worker(bt::Session* session);
    void set_status(State state, const std::string& message);

    mutable std::mutex mutex_;
    State state_ = State::Disconnected;
    std::string status_;
    std::string relay_label_;

    std::string account_number_;
    std::string device_id_;
    uint8_t private_key_[32]{};
    bool has_key_ = false;
    uint32_t assigned_ip_ = 0;
    std::string preferred_country_;

    std::vector<mullvad::Relay> relays_;
    std::shared_ptr<wg::Tunnel> tunnel_;

    std::thread worker_;
    std::atomic<bool> busy_{false};
};

}  // namespace vpn
