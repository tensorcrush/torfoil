#include "vpn/manager.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util/clock.hpp"
#include "util/log.hpp"

#include "net/wg/wireguard.hpp"
#include "util/bytes.hpp"

namespace vpn {

namespace {

const char kConfigPath[] = "sdmc:/torfoil/vpn.cfg";

std::string trim_line(const std::string& s) {
    std::string out = s;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

}  // namespace

Manager::~Manager() {
    if (worker_.joinable()) worker_.join();
}

void Manager::load() {
    std::FILE* fp = std::fopen(kConfigPath, "rb");
    if (!fp) return;

    std::lock_guard<std::mutex> lock(mutex_);
    char line[512];
    while (std::fgets(line, sizeof(line), fp)) {
        const std::string text = trim_line(line);
        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = text.substr(0, eq);
        const std::string value = text.substr(eq + 1);

        if (key == "account") account_number_ = value;
        else if (key == "device") device_id_ = value;
        else if (key == "country") preferred_country_ = value;
        else if (key == "ip") assigned_ip_ = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "key") has_key_ = util::from_hex(value, private_key_, sizeof(private_key_));
    }
    std::fclose(fp);
}

void Manager::save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::FILE* fp = std::fopen(kConfigPath, "wb");
    if (!fp) return;

    // La clé privée reste en clair sur la SD : la console n'offre pas de coffre
    // accessible au homebrew. Elle ne vaut que pour ce device Mullvad, qui se
    // révoque depuis le site en cas de perte de la carte.
    std::fprintf(fp, "account=%s\n", account_number_.c_str());
    std::fprintf(fp, "device=%s\n", device_id_.c_str());
    std::fprintf(fp, "country=%s\n", preferred_country_.c_str());
    std::fprintf(fp, "ip=%u\n", assigned_ip_);
    if (has_key_) {
        std::fprintf(fp, "key=%s\n", util::to_hex(private_key_, sizeof(private_key_)).c_str());
    }
    std::fclose(fp);
}

bool Manager::has_account() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return account_number_.size() == 16;
}

std::string Manager::account_masked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (account_number_.size() != 16) return "non renseigné";
    return "•••• •••• •••• " + account_number_.substr(12);
}

void Manager::set_account(const std::string& number) {
    std::string digits;
    for (const char c : number) {
        if (c >= '0' && c <= '9') digits.push_back(c);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        account_number_ = digits;
        // Changer de compte invalide l'appareil enregistré précédemment.
        device_id_.clear();
        assigned_ip_ = 0;
        has_key_ = false;
    }
    save();
}

State Manager::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string Manager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string Manager::relay_label() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return relay_label_;
}

uint64_t Manager::bytes_sent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tunnel_ ? tunnel_->bytes_sent() : 0;
}

uint64_t Manager::bytes_received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tunnel_ ? tunnel_->bytes_received() : 0;
}

std::vector<std::string> Manager::countries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const mullvad::Relay& relay : relays_) {
        const std::string label = relay.country.empty() ? relay.country_code : relay.country;
        if (std::find(out.begin(), out.end(), label) == out.end()) out.push_back(label);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Manager::set_preferred_country(const std::string& code) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preferred_country_ = code;
    }
    save();
}

std::string Manager::preferred_country() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return preferred_country_;
}

void Manager::set_status(State state, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == state && status_ == message) return;
        state_ = state;
        status_ = message;
    }
    util::log_line("vpn : " + message);
}

void Manager::connect(bt::Session& session) {
    if (busy_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();

    set_status(State::Working, "préparation…");
    worker_ = std::thread([this, &session] { worker(&session); });
}

void Manager::disconnect(bt::Session& session) {
    if (busy_.load()) return;
    if (worker_.joinable()) worker_.join();

    std::shared_ptr<wg::Tunnel> tunnel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tunnel = tunnel_;
        tunnel_.reset();
        relay_label_.clear();
    }

    // On repasse en direct AVANT d'arrêter le tunnel, sinon la session se
    // retrouverait un instant avec un transport mort.
    session.set_transport(net::make_bsd_transport());
    if (tunnel) tunnel->stop();

    set_status(State::Disconnected, "déconnecté");
}

void Manager::update(bt::Session& session) {
    if (busy_.load()) return;

    std::shared_ptr<wg::Tunnel> tunnel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Connected) return;
        tunnel = tunnel_;
    }
    if (!tunnel) return;

    const wg::TunnelState ts = tunnel->state();
    if (ts == wg::TunnelState::Up || ts == wg::TunnelState::Handshaking) return;

    // Le tunnel est mort. Le trafic ne repart PAS en clair pour autant : on
    // laisse le transport tunnelé en place, il refuse désormais toute socket.
    // C'est exactement ce qu'on veut d'un killswitch — mais il faut le dire.
    (void)session;
    set_status(State::Failed, "tunnel interrompu — aucun trafic ne sort (A pour reconnecter)");
}

void Manager::worker(bt::Session* session) {
    std::string account_number;
    std::string device_id;
    std::string country;
    uint8_t private_key[32];
    bool has_key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        account_number = account_number_;
        device_id = device_id_;
        country = preferred_country_;
        std::memcpy(private_key, private_key_, sizeof(private_key));
        has_key = has_key_;
    }

    auto finish = [&](State state, const std::string& message) {
        set_status(state, message);
        busy_.store(false);
    };

    if (account_number.size() != 16) {
        finish(State::Failed, "numéro de compte Mullvad manquant");
        return;
    }

    // Les appels d'API partent en clair : c'est le seul moyen d'obtenir de quoi
    // monter le tunnel. Rien de sensible n'y transite hormis le numéro de compte.
    auto api_transport = net::make_bsd_transport();

    set_status(State::Working, "authentification…");
    mullvad::Account account;
    std::string err;
    if (!mullvad::login(*api_transport, account_number, account, &err)) {
        finish(State::Failed, err);
        return;
    }

    if (!has_key) {
        wg::generate_private_key(private_key);
        has_key = true;
        device_id.clear();
    }

    uint8_t public_key[32];
    wg::derive_public_key(public_key, private_key);

    uint32_t assigned_ip = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        assigned_ip = assigned_ip_;
    }

    // Un appareil enregistré peut avoir été supprimé depuis le site Mullvad, ou
    // évincé par la limite de cinq. On ne peut pas le deviner : la poignée de
    // main échouerait sans un mot, indéfiniment, puisque la clé serait devenue
    // inconnue du relais. On vérifie donc qu'il existe encore, et on en recrée
    // un sinon — c'est la différence entre « ça ne marche plus » et « ça se
    // répare tout seul ».
    if (!device_id.empty() && assigned_ip != 0) {
        bool still_there = false;
        if (mullvad::device_exists(*api_transport, account, device_id, still_there, nullptr) &&
            !still_there) {
            device_id.clear();
            assigned_ip = 0;
            util::log_line("vpn : appareil révoqué côté Mullvad, réenregistrement");
        }
    }

    if (device_id.empty() || assigned_ip == 0) {
        set_status(State::Working, "enregistrement de l'appareil…");
        // Clé neuve : réutiliser celle d'un appareil disparu ne servirait à rien.
        wg::generate_private_key(private_key);
        wg::derive_public_key(public_key, private_key);

        mullvad::Device device;
        if (!mullvad::create_device(*api_transport, account, wg::key_to_base64(public_key), device,
                                    &err)) {
            finish(State::Failed, err);
            return;
        }
        device_id = device.id;
        assigned_ip = device.ipv4;
    }

    set_status(State::Working, "récupération des relais…");
    std::vector<mullvad::Relay> relays;
    if (!mullvad::list_relays(*api_transport, relays, &err)) {
        finish(State::Failed, err);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        relays_ = relays;
        device_id_ = device_id;
        assigned_ip_ = assigned_ip;
        std::memcpy(private_key_, private_key, sizeof(private_key_));
        has_key_ = has_key;
    }
    save();

    // Choix du relais : le pays préféré s'il est disponible, sinon le premier.
    const mullvad::Relay* chosen = nullptr;
    if (!country.empty()) {
        for (const mullvad::Relay& relay : relays) {
            const std::string label = relay.country.empty() ? relay.country_code : relay.country;
            if (label == country) {
                chosen = &relay;
                break;
            }
        }
    }
    if (!chosen) chosen = &relays.front();

    set_status(State::Working, "tunnel vers " + chosen->hostname + "…");

    wg::TunnelConfig config;
    config.endpoint_ip = chosen->ipv4;
    config.endpoint_port = chosen->port;
    std::memcpy(config.server_public, chosen->public_key, 32);
    config.server_label = chosen->hostname;
    std::memcpy(config.private_key, private_key, 32);
    config.assigned_ip = assigned_ip;
    config.netmask = 0xffffffffu;
    config.dns_ip = mullvad::kDnsServer;

    auto tunnel = wg::make_tunnel(config, &err);
    if (!tunnel) {
        finish(State::Failed, err);
        return;
    }

    // Attente de la poignée de main : le transport n'est basculé qu'une fois le
    // tunnel réellement debout, jamais avant.
    const uint64_t deadline = util::now_ms() + 15000;
    while (util::now_ms() < deadline && tunnel->state() != wg::TunnelState::Up) {
        net::PollItem dummy;
        tunnel->poll(&dummy, 0, 100);
    }

    if (tunnel->state() != wg::TunnelState::Up) {
        tunnel->stop();
        finish(State::Failed, "le relais n'a pas répondu (poignée de main)");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tunnel_ = tunnel;
        relay_label_ = chosen->hostname;
    }

    if (session) session->set_transport(tunnel);
    finish(State::Connected, "connecté");
}

}  // namespace vpn
