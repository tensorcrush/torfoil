#include "net/wg/tunnel.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "lwip/def.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "net/wg/wireguard.hpp"
#include "util/clock.hpp"

// sys_now() : lwIP en mode NO_SYS réclame une horloge en millisecondes.
extern "C" u32_t sys_now(void) {
    return static_cast<u32_t>(util::now_ms());
}

namespace wg {

namespace {

constexpr size_t kMaxDatagram = 2048;
constexpr uint16_t kTunnelMtu = 1420;
// Datagrammes lus par passage. Assez haut pour absorber une rafale complète
// (~360 Ko), assez bas pour ne pas monopoliser la boucle moteur.
constexpr int kDrainBudget = 256;

class WireGuardTunnel;
WireGuardTunnel* g_active = nullptr;

// Conversion entre l'ordre hôte utilisé par net::Endpoint et lwIP.
ip4_addr_t to_lwip(uint32_t host_order) {
    ip4_addr_t addr;
    addr.addr = lwip_htonl(host_order);
    return addr;
}

uint32_t from_lwip(const ip4_addr_t* addr) {
    return lwip_ntohl(addr->addr);
}

// -------------------------------------------------------------------------
// Socket TCP au-dessus de l'API brute lwIP
// -------------------------------------------------------------------------

class LwipTcpSocket : public net::TcpSocket {
public:
    explicit LwipTcpSocket(WireGuardTunnel* owner) : owner_(owner) {}
    ~LwipTcpSocket() override { close(); }

    bool start_connect(const net::Endpoint& ep) override;
    bool finish_connect() override;
    int send(const uint8_t* data, size_t len) override;
    int recv(uint8_t* data, size_t len) override;
    void close() override;
    bool is_open() const override { return pcb_ != nullptr || !rx_.empty(); }

    bool ready_read() const { return rx_pos_ < rx_.size() || remote_closed_ || failed_; }
    bool ready_write() const { return connected_ && pcb_ != nullptr; }
    bool failed() const { return failed_; }

private:
    static err_t on_connected(void* arg, struct tcp_pcb* pcb, err_t err);
    static err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err);
    static err_t on_sent(void* arg, struct tcp_pcb* pcb, u16_t len);
    static void on_error(void* arg, err_t err);

    void detach();

    WireGuardTunnel* owner_;
    struct tcp_pcb* pcb_ = nullptr;
    std::vector<uint8_t> rx_;
    size_t rx_pos_ = 0;
    bool connected_ = false;
    bool remote_closed_ = false;
    bool failed_ = false;
};

// -------------------------------------------------------------------------
// Socket UDP au-dessus de l'API brute lwIP
// -------------------------------------------------------------------------

class LwipUdpSocket : public net::UdpSocket {
public:
    explicit LwipUdpSocket(WireGuardTunnel* owner) : owner_(owner) {}
    ~LwipUdpSocket() override { close(); }

    bool open() override;
    int send_to(const net::Endpoint& ep, const uint8_t* data, size_t len) override;
    int recv_from(net::Endpoint& from, uint8_t* data, size_t len) override;
    void close() override;
    bool is_open() const override { return pcb_ != nullptr; }

    bool ready_read() const { return !queue_.empty(); }

private:
    struct Datagram {
        net::Endpoint from;
        std::vector<uint8_t> payload;
    };

    static void on_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr,
                        u16_t port);

    WireGuardTunnel* owner_;
    struct udp_pcb* pcb_ = nullptr;
    std::deque<Datagram> queue_;
};

// -------------------------------------------------------------------------
// Le tunnel
// -------------------------------------------------------------------------

class WireGuardTunnel : public Tunnel {
public:
    explicit WireGuardTunnel(const TunnelConfig& config) : config_(config) {}
    ~WireGuardTunnel() override { stop(); }

    bool start(std::string* err) override;
    void stop() override;

    // --- net::Transport ---
    const char* name() const override { return "Mullvad (WireGuard)"; }
    bool ready() const override { return state_ == TunnelState::Up; }
    std::unique_ptr<net::TcpSocket> tcp() override;
    std::unique_ptr<net::UdpSocket> udp() override;
    bool resolve(const std::string& host, uint32_t& ipv4_out) override;
    int poll(net::PollItem* items, size_t count, int timeout_ms) override;
    std::string public_address() const override { return config_.server_label; }

    // --- Tunnel ---
    TunnelState state() const override { return state_; }
    std::string status_text() const override;
    uint64_t bytes_sent() const override { return bytes_sent_; }
    uint64_t bytes_received() const override { return bytes_received_; }
    uint64_t last_handshake_age_ms() const override {
        return last_handshake_ms_ == 0 ? 0 : util::now_ms() - last_handshake_ms_;
    }

    std::recursive_mutex& mutex() { return mutex_; }

    // Appelé par lwIP quand un paquet IP doit sortir : on l'encapsule.
    err_t output_packet(struct pbuf* p);

private:
    void pump(int timeout_ms);
    void send_handshake(uint64_t now_ms);
    void handle_datagram(const uint8_t* data, size_t len, uint64_t now_ms);
    void send_keepalive(uint64_t now_ms);
    bool open_udp_socket(std::string* err);

    static err_t netif_init_cb(struct netif* nif);
    static err_t netif_output_cb(struct netif* nif, struct pbuf* p, const ip4_addr_t* dst);
    static void dns_found_cb(const char* name, const ip_addr_t* addr, void* arg);

    TunnelConfig config_;
    Peer peer_;
    struct netif netif_ {};
    bool netif_added_ = false;
    bool lwip_started_ = false;

    int udp_fd_ = -1;
    sockaddr_in endpoint_{};

    std::recursive_mutex mutex_;
    TunnelState state_ = TunnelState::Down;
    std::string error_;

    uint64_t bytes_sent_ = 0;
    uint64_t bytes_received_ = 0;
    uint64_t tx_dropped_ = 0;
    uint64_t tx_errors_ = 0;
    uint64_t rx_saturated_ = 0;
    uint64_t last_handshake_ms_ = 0;
    uint64_t handshake_started_ms_ = 0;

    // Résolution DNS en cours (une à la fois, protégée par le verrou).
    struct {
        bool pending = false;
        bool done = false;
        bool ok = false;
        uint32_t result = 0;
    } dns_;
};

// -------------------------------------------------------------------------

err_t LwipTcpSocket::on_connected(void* arg, struct tcp_pcb* pcb, err_t err) {
    auto* self = static_cast<LwipTcpSocket*>(arg);
    if (!self) return ERR_OK;
    if (err != ERR_OK) {
        self->failed_ = true;
        return ERR_OK;
    }
    self->connected_ = true;
    return ERR_OK;
}

err_t LwipTcpSocket::on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    auto* self = static_cast<LwipTcpSocket*>(arg);
    if (!self) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        self->failed_ = true;
        if (p) pbuf_free(p);
        return ERR_OK;
    }

    if (p == nullptr) {
        // Fermeture propre côté distant.
        self->remote_closed_ = true;
        return ERR_OK;
    }

    const size_t before = self->rx_.size();
    self->rx_.resize(before + p->tot_len);
    pbuf_copy_partial(p, self->rx_.data() + before, p->tot_len, 0);

    // La fenêtre TCP n'est rendue qu'au moment où l'application consomme :
    // sinon un pair rapide ferait gonfler notre tampon sans limite.
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

err_t LwipTcpSocket::on_sent(void* arg, struct tcp_pcb* pcb, u16_t len) {
    return ERR_OK;
}

void LwipTcpSocket::on_error(void* arg, err_t err) {
    auto* self = static_cast<LwipTcpSocket*>(arg);
    if (!self) return;
    // lwIP a déjà libéré le pcb : surtout ne pas y toucher.
    self->pcb_ = nullptr;
    self->failed_ = true;
    self->connected_ = false;
}

bool LwipTcpSocket::start_connect(const net::Endpoint& ep) {
    close();

    pcb_ = tcp_new();
    if (!pcb_) {
        // lwIP ne renseigne pas errno ; le moteur, lui, s'en sert pour
        // distinguer « ce pair ne répond pas » de « plus de socket disponible ».
        errno = ENOBUFS;
        return false;
    }

    tcp_arg(pcb_, this);
    tcp_err(pcb_, &LwipTcpSocket::on_error);
    tcp_recv(pcb_, &LwipTcpSocket::on_recv);
    tcp_sent(pcb_, &LwipTcpSocket::on_sent);

    const ip4_addr_t dst = to_lwip(ep.ipv4);
    ip_addr_t target;
    ip_addr_copy_from_ip4(target, dst);

    const err_t rc = tcp_connect(pcb_, &target, ep.port, &LwipTcpSocket::on_connected);
    if (rc != ERR_OK) {
        errno = (rc == ERR_MEM) ? ENOBUFS : ECONNREFUSED;
        detach();
        return false;
    }
    return true;
}

bool LwipTcpSocket::finish_connect() {
    if (failed_) return false;
    return connected_;
}

int LwipTcpSocket::send(const uint8_t* data, size_t len) {
    if (failed_ || !pcb_) return net::kError;
    if (!connected_) return net::kWouldBlock;

    const u16_t room = tcp_sndbuf(pcb_);
    if (room == 0) return net::kWouldBlock;

    const size_t take = len < room ? len : room;
    const err_t rc = tcp_write(pcb_, data, static_cast<u16_t>(take), TCP_WRITE_FLAG_COPY);
    if (rc == ERR_MEM) return net::kWouldBlock;
    if (rc != ERR_OK) {
        failed_ = true;
        return net::kError;
    }

    tcp_output(pcb_);
    return static_cast<int>(take);
}

int LwipTcpSocket::recv(uint8_t* data, size_t len) {
    const size_t available = rx_.size() - rx_pos_;
    if (available > 0) {
        const size_t take = len < available ? len : available;
        std::memcpy(data, rx_.data() + rx_pos_, take);
        rx_pos_ += take;

        if (rx_pos_ == rx_.size()) {
            rx_.clear();
            rx_pos_ = 0;
        } else if (rx_pos_ > 64 * 1024) {
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(rx_pos_));
            rx_pos_ = 0;
        }
        return static_cast<int>(take);
    }

    if (remote_closed_) return net::kClosed;
    if (failed_) return net::kError;
    return net::kWouldBlock;
}

void LwipTcpSocket::detach() {
    if (!pcb_) return;
    tcp_arg(pcb_, nullptr);
    tcp_recv(pcb_, nullptr);
    tcp_sent(pcb_, nullptr);
    tcp_err(pcb_, nullptr);
    pcb_ = nullptr;
}

void LwipTcpSocket::close() {
    if (pcb_) {
        struct tcp_pcb* pcb = pcb_;
        detach();
        // tcp_close peut échouer faute de mémoire ; l'abandon est alors la seule
        // issue pour ne pas fuir le pcb.
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    rx_.clear();
    rx_pos_ = 0;
    connected_ = false;
    remote_closed_ = false;
}

// -------------------------------------------------------------------------

void LwipUdpSocket::on_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr,
                            u16_t port) {
    auto* self = static_cast<LwipUdpSocket*>(arg);
    if (!self || !p) {
        if (p) pbuf_free(p);
        return;
    }

    // Un tracker bavard ne doit pas pouvoir faire enfler la file sans fin.
    if (self->queue_.size() < 64 && p->tot_len <= kMaxDatagram) {
        Datagram datagram;
        datagram.from.ipv4 = from_lwip(ip_2_ip4(addr));
        datagram.from.port = port;
        datagram.payload.resize(p->tot_len);
        pbuf_copy_partial(p, datagram.payload.data(), p->tot_len, 0);
        self->queue_.push_back(std::move(datagram));
    }
    pbuf_free(p);
}

bool LwipUdpSocket::open() {
    close();
    pcb_ = udp_new();
    if (!pcb_) return false;

    if (udp_bind(pcb_, IP4_ADDR_ANY, 0) != ERR_OK) {
        udp_remove(pcb_);
        pcb_ = nullptr;
        return false;
    }
    udp_recv(pcb_, &LwipUdpSocket::on_recv, this);
    return true;
}

int LwipUdpSocket::send_to(const net::Endpoint& ep, const uint8_t* data, size_t len) {
    if (!pcb_) return net::kError;

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(len), PBUF_RAM);
    if (!p) return net::kWouldBlock;
    std::memcpy(p->payload, data, len);

    const ip4_addr_t dst = to_lwip(ep.ipv4);
    ip_addr_t target;
    ip_addr_copy_from_ip4(target, dst);

    const err_t rc = udp_sendto(pcb_, p, &target, ep.port);
    pbuf_free(p);

    if (rc != ERR_OK) return net::kError;
    return static_cast<int>(len);
}

int LwipUdpSocket::recv_from(net::Endpoint& from, uint8_t* data, size_t len) {
    if (queue_.empty()) return net::kWouldBlock;

    Datagram datagram = std::move(queue_.front());
    queue_.pop_front();

    const size_t take = len < datagram.payload.size() ? len : datagram.payload.size();
    std::memcpy(data, datagram.payload.data(), take);
    from = datagram.from;
    return static_cast<int>(take);
}

void LwipUdpSocket::close() {
    if (pcb_) {
        udp_recv(pcb_, nullptr, nullptr);
        udp_remove(pcb_);
        pcb_ = nullptr;
    }
    queue_.clear();
}

// -------------------------------------------------------------------------

err_t WireGuardTunnel::netif_init_cb(struct netif* nif) {
    nif->name[0] = 'w';
    nif->name[1] = 'g';
    nif->output = &WireGuardTunnel::netif_output_cb;
    nif->mtu = kTunnelMtu;
    // Liaison point à point : ni diffusion, ni ARP.
    nif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

err_t WireGuardTunnel::netif_output_cb(struct netif* nif, struct pbuf* p, const ip4_addr_t* dst) {
    auto* self = static_cast<WireGuardTunnel*>(nif->state);
    if (!self) return ERR_IF;
    return self->output_packet(p);
}

err_t WireGuardTunnel::output_packet(struct pbuf* p) {
    if (state_ != TunnelState::Up) return ERR_CONN;
    if (p->tot_len > kTunnelMtu) return ERR_MEM;

    uint8_t packet[kTunnelMtu];
    pbuf_copy_partial(p, packet, p->tot_len, 0);

    uint8_t wire[kTunnelMtu + 64];
    const int written = peer_.encapsulate(packet, p->tot_len, wire, sizeof(wire), util::now_ms());
    if (written < 0) {
        state_ = TunnelState::Handshaking;
        return ERR_CONN;
    }

    // La socket est non bloquante : sous charge, elle refuse temporairement.
    // Abandonner à la première tentative revient à jeter un segment TCP déjà
    // chiffré — et le compteur WireGuard, lui, a déjà avancé. Le pair verra un
    // trou, retransmettra, et TCP lira une congestion là où il n'y avait qu'un
    // tampon momentanément plein.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const ssize_t sent = ::sendto(udp_fd_, wire, static_cast<size_t>(written), 0,
                                      reinterpret_cast<const sockaddr*>(&endpoint_),
                                      sizeof(endpoint_));
        if (sent >= 0) {
            bytes_sent_ += static_cast<uint64_t>(sent);
            return ERR_OK;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
            ++tx_errors_;
            return ERR_IF;
        }

        struct pollfd out;
        out.fd = udp_fd_;
        out.events = POLLOUT;
        out.revents = 0;
        ::poll(&out, 1, 4);
    }

    // Toujours plein : on rend la main à lwIP, qui gardera le segment en file et
    // le réémettra. ERR_MEM, pas ERR_IF : ce n'est pas l'interface qui est morte.
    ++tx_dropped_;
    return ERR_MEM;
}

bool WireGuardTunnel::open_udp_socket(std::string* err) {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_fd_ < 0) {
        if (err) *err = "socket UDP système indisponible";
        return false;
    }

    const int flags = fcntl(udp_fd_, F_GETFL, 0);
    fcntl(udp_fd_, F_SETFL, flags | O_NONBLOCK);

    std::memset(&endpoint_, 0, sizeof(endpoint_));
    endpoint_.sin_family = AF_INET;
    endpoint_.sin_port = htons(config_.endpoint_port);
    endpoint_.sin_addr.s_addr = htonl(config_.endpoint_ip);
    return true;
}

bool WireGuardTunnel::start(std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (g_active != nullptr && g_active != this) {
        if (err) *err = "un tunnel est déjà actif";
        return false;
    }
    if (config_.endpoint_ip == 0 || config_.assigned_ip == 0) {
        if (err) *err = "configuration de tunnel incomplète";
        return false;
    }

    if (!open_udp_socket(err)) return false;

    peer_.configure(config_.private_key, config_.server_public, nullptr);

    if (!lwip_started_) {
        lwip_init();
        lwip_started_ = true;
    }

    const ip4_addr_t addr = to_lwip(config_.assigned_ip);
    const ip4_addr_t mask = to_lwip(config_.netmask);
    const ip4_addr_t gateway = to_lwip(0);

    if (!netif_add(&netif_, &addr, &mask, &gateway, this, &WireGuardTunnel::netif_init_cb,
                   &ip_input)) {
        if (err) *err = "création de l'interface réseau impossible";
        ::close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }
    netif_added_ = true;
    netif_set_default(&netif_);
    netif_set_up(&netif_);
    netif_set_link_up(&netif_);

    if (config_.dns_ip != 0) {
        const ip4_addr_t dns4 = to_lwip(config_.dns_ip);
        ip_addr_t dns;
        ip_addr_copy_from_ip4(dns, dns4);
        dns_setserver(0, &dns);
    }

    g_active = this;
    state_ = TunnelState::Handshaking;
    handshake_started_ms_ = util::now_ms();
    send_handshake(handshake_started_ms_);
    return true;
}

void WireGuardTunnel::stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (netif_added_) {
        netif_set_down(&netif_);
        netif_remove(&netif_);
        netif_added_ = false;
    }
    if (udp_fd_ >= 0) {
        ::close(udp_fd_);
        udp_fd_ = -1;
    }
    if (g_active == this) g_active = nullptr;
    state_ = TunnelState::Down;
}

void WireGuardTunnel::send_handshake(uint64_t now_ms) {
    uint8_t initiation[kInitiationSize];

    // TAI64N réclame l'heure murale : c'est la seule horloge acceptable ici, le
    // serveur s'en sert pour rejeter les initiations rejouées.
    const uint64_t unix_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());

    if (!peer_.make_initiation(initiation, now_ms, unix_ns)) {
        state_ = TunnelState::Failed;
        error_ = peer_.last_error();
        return;
    }

    ::sendto(udp_fd_, initiation, sizeof(initiation), 0,
             reinterpret_cast<const sockaddr*>(&endpoint_), sizeof(endpoint_));
    peer_.note_handshake_sent(now_ms);
    handshake_started_ms_ = now_ms;
}

void WireGuardTunnel::send_keepalive(uint64_t now_ms) {
    uint8_t wire[64];
    const int written = peer_.encapsulate(nullptr, 0, wire, sizeof(wire), now_ms);
    if (written < 0) return;
    ::sendto(udp_fd_, wire, static_cast<size_t>(written), 0,
             reinterpret_cast<const sockaddr*>(&endpoint_), sizeof(endpoint_));
}

void WireGuardTunnel::handle_datagram(const uint8_t* data, size_t len, uint64_t now_ms) {
    if (len < 4) return;

    switch (data[0]) {
        case kTypeResponse:
            if (peer_.consume_response(data, len, now_ms)) {
                state_ = TunnelState::Up;
                last_handshake_ms_ = now_ms;
            }
            break;

        case kTypeData: {
            uint8_t packet[kTunnelMtu + 64];
            const int decoded = peer_.decapsulate(data, len, packet, sizeof(packet), now_ms);
            if (decoded < 0) return;
            if (decoded == 0) return;  // keepalive

            struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(decoded), PBUF_POOL);
            if (!p) return;
            pbuf_take(p, packet, static_cast<u16_t>(decoded));

            if (netif_.input(p, &netif_) != ERR_OK) pbuf_free(p);
            break;
        }

        case kTypeCookieReply:
            // Le serveur est sous charge : on retentera la poignée de main.
            break;

        default:
            break;
    }
}

void WireGuardTunnel::pump(int timeout_ms) {
    if (udp_fd_ < 0) return;

    const uint64_t now = util::now_ms();

    if (state_ != TunnelState::Up && peer_.needs_handshake(now)) {
        send_handshake(now);
    } else if (state_ == TunnelState::Up && peer_.needs_handshake(now)) {
        // Renouvellement périodique des clés.
        send_handshake(now);
    }

    struct pollfd fd;
    fd.fd = udp_fd_;
    fd.events = POLLIN;
    fd.revents = 0;

    if (::poll(&fd, 1, timeout_ms) > 0 && (fd.revents & POLLIN)) {
        uint8_t buffer[kMaxDatagram];
        // On vide la socket jusqu'à l'épuisement. L'ancien plafond de 32
        // datagrammes laissait le reste s'accumuler dans le tampon du pilote
        // pendant que la boucle moteur faisait autre chose ; passé sa capacité,
        // le pilote jette, et TCP prend ces pertes pour de la congestion. À
        // 1420 octets par datagramme, 32 par tour valaient 45 Ko de fenêtre pour
        // l'ensemble des pairs réunis.
        int drained = 0;
        for (; drained < kDrainBudget; ++drained) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const ssize_t n = ::recvfrom(udp_fd_, buffer, sizeof(buffer), 0,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) break;

            // On n'accepte que ce qui vient du relais configuré.
            if (from.sin_addr.s_addr != endpoint_.sin_addr.s_addr) continue;

            bytes_received_ += static_cast<uint64_t>(n);
            handle_datagram(buffer, static_cast<size_t>(n), util::now_ms());
        }
        // Budget épuisé : il reste du trafic en attente. On le note, c'est le
        // signe que la boucle moteur ne suit plus le rythme du réseau.
        if (drained >= kDrainBudget) ++rx_saturated_;
    }

    if (state_ == TunnelState::Up && peer_.needs_keepalive(util::now_ms())) {
        send_keepalive(util::now_ms());
    }

    sys_check_timeouts();
}

std::unique_ptr<net::TcpSocket> WireGuardTunnel::tcp() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (state_ != TunnelState::Up) return nullptr;
    return std::make_unique<LwipTcpSocket>(this);
}

std::unique_ptr<net::UdpSocket> WireGuardTunnel::udp() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (state_ != TunnelState::Up) return nullptr;
    return std::make_unique<LwipUdpSocket>(this);
}

void WireGuardTunnel::dns_found_cb(const char* name, const ip_addr_t* addr, void* arg) {
    auto* self = static_cast<WireGuardTunnel*>(arg);
    if (!self) return;
    self->dns_.done = true;
    self->dns_.pending = false;
    self->dns_.ok = addr != nullptr;
    self->dns_.result = addr ? from_lwip(ip_2_ip4(addr)) : 0;
}

bool WireGuardTunnel::resolve(const std::string& host, uint32_t& ipv4_out) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (state_ != TunnelState::Up) return false;

    ip4_addr_t literal;
    if (ip4addr_aton(host.c_str(), &literal)) {
        ipv4_out = from_lwip(&literal);
        return true;
    }

    dns_ = {};
    ip_addr_t resolved;
    const err_t rc = dns_gethostbyname(host.c_str(), &resolved, &WireGuardTunnel::dns_found_cb,
                                       this);
    if (rc == ERR_OK) {
        ipv4_out = from_lwip(ip_2_ip4(&resolved));
        return true;
    }
    if (rc != ERR_INPROGRESS) return false;

    dns_.pending = true;
    const uint64_t deadline = util::now_ms() + 8000;
    while (!dns_.done && util::now_ms() < deadline) {
        pump(50);
    }

    if (!dns_.ok) return false;
    ipv4_out = dns_.result;
    return true;
}

int WireGuardTunnel::poll(net::PollItem* items, size_t count, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Le pompage est ce qui fait vivre lwIP : sans lui, rien n'avance.
    pump(timeout_ms > 50 ? 50 : timeout_ms);

    int ready = 0;
    for (size_t i = 0; i < count; ++i) {
        net::PollItem& item = items[i];
        item.can_read = item.can_write = item.has_error = false;

        if (item.tcp) {
            auto* sock = static_cast<LwipTcpSocket*>(item.tcp);
            if (item.want_read && sock->ready_read()) item.can_read = true;
            if (item.want_write && sock->ready_write()) item.can_write = true;
            if (sock->failed()) item.has_error = true;
        } else if (item.udp) {
            auto* sock = static_cast<LwipUdpSocket*>(item.udp);
            if (item.want_read && sock->ready_read()) item.can_read = true;
            if (item.want_write) item.can_write = true;  // UDP : toujours prêt
        }

        if (item.can_read || item.can_write || item.has_error) ++ready;
    }
    return ready;
}

std::string WireGuardTunnel::status_text() const {
    switch (state_) {
        case TunnelState::Down: return "déconnecté";
        case TunnelState::Handshaking: return "connexion en cours…";
        case TunnelState::Up: {
            std::string text = "connecté à " + config_.server_label;
            // Ces compteurs doivent rester visibles : ce sont eux qui disent
            // « le tunnel tient mais la console ne suit pas », le seul cas où un
            // débit ridicule n'a rien à voir avec les pairs.
            if (tx_dropped_ || tx_errors_ || rx_saturated_) {
                char note[96];
                std::snprintf(note, sizeof(note), " · %llu perdus, %llu saturations",
                              static_cast<unsigned long long>(tx_dropped_ + tx_errors_),
                              static_cast<unsigned long long>(rx_saturated_));
                text += note;
            }
            return text;
        }
        case TunnelState::Failed: return "échec : " + error_;
    }
    return "?";
}

}  // namespace

std::shared_ptr<Tunnel> make_tunnel(const TunnelConfig& config, std::string* err) {
    auto tunnel = std::make_shared<WireGuardTunnel>(config);
    if (!tunnel->start(err)) return nullptr;
    return tunnel;
}

}  // namespace wg
