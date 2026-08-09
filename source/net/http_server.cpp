#include "net/http_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

#include "util/clock.hpp"
#include "util/log.hpp"

namespace net {

namespace {

// Une requête depuis un téléphone du même Wi-Fi arrive en quelques
// millisecondes. Ces délais servent uniquement à ne pas rester accroché à un
// client qui s'est éteint au milieu d'un envoi.
constexpr int kHeaderTimeoutMs = 8000;
constexpr int kBodyTimeoutMs = 30000;
constexpr int kSendTimeoutMs = 15000;

bool wait_readable(int fd, int timeout_ms) {
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    const int rc = ::poll(&p, 1, timeout_ms);
    return rc > 0 && (p.revents & POLLIN) != 0;
}

bool wait_writable(int fd, int timeout_ms) {
    pollfd p{};
    p.fd = fd;
    p.events = POLLOUT;
    const int rc = ::poll(&p, 1, timeout_ms);
    return rc > 0 && (p.revents & POLLOUT) != 0;
}

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 303: return "See Other";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

}  // namespace

bool is_private_ipv4(uint32_t addr) {
    const uint32_t a = (addr >> 24) & 0xff;
    const uint32_t b = (addr >> 16) & 0xff;
    if (a == 10) return true;
    if (a == 127) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;  // lien-local, si le DHCP a échoué
    return false;
}

uint32_t HttpServer::local_ipv4() {
    // gethostid() renvoie l'adresse de l'interface courante de la console, dans
    // l'ordre réseau. C'est le chemin le plus court : pas de service nifm à
    // ouvrir, et il vaut aussi sur PC pour les tests.
    const long id = ::gethostid();
    if (id == 0) return 0;
    const uint32_t host_order = ntohl(static_cast<uint32_t>(id));
    // 127.0.0.1 signifie « pas de réseau » ici : l'annoncer au téléphone
    // enverrait l'utilisateur vers une page qui ne s'ouvrira jamais.
    if (((host_order >> 24) & 0xff) == 127) return 0;
    return host_order;
}

std::string HttpServer::url() const {
    const uint32_t ip = address_ != 0 ? address_ : local_ipv4();
    if (ip == 0 || port_ == 0) return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "http://%u.%u.%u.%u:%u/", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                  (ip >> 8) & 0xff, ip & 0xff, port_);
    return buf;
}

std::string HttpServer::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(uint16_t port, HttpHandler handler, std::string* err) {
    if (running_.load()) return true;
    handler_ = std::move(handler);

    for (int offset = 0; offset < 8; ++offset) {
        const int candidate = static_cast<int>(port) + offset;
        if (candidate > 65535) break;

        const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            if (err) *err = "socket indisponible pour le serveur local";
            return false;
        }

        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(candidate));

        if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd, 4) != 0) {
            ::close(fd);
            continue;
        }

        listen_fd_ = fd;
        port_ = static_cast<uint16_t>(candidate);
        break;
    }

    if (listen_fd_ < 0) {
        if (err) *err = "aucun port libre pour le serveur local";
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this] { serve(); });
    util::log_fmt("import téléphone : serveur local en écoute sur le port %u", port_);
    return true;
}

void HttpServer::stop() {
    running_.store(false);

    // On réveille le poll de la boucle sans fermer le descripteur : la
    // fermeture appartient au thread qui s'en sert. Un descripteur fermé sous
    // les pieds d'un accept() peut être réattribué à une autre socket entre
    // les deux, et l'accept se mettrait alors à écouter une connexion qui ne
    // le regarde pas.
    if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    port_ = 0;
}

void HttpServer::serve() {
    const int listen_fd = listen_fd_;

    while (running_.load()) {
        if (!wait_readable(listen_fd, 300)) continue;
        if (!running_.load()) break;

        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;  // socket d'écoute close ou en erreur : on sort
        }

        const uint32_t peer_ip = ntohl(peer.sin_addr.s_addr);
        if (!is_private_ipv4(peer_ip)) {
            // Rien à négocier : une connexion venue d'ailleurs que du réseau
            // local n'a aucune raison d'être ici.
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                last_error_ = "connexion refusée (adresse hors réseau local)";
            }
            ::close(fd);
            continue;
        }

        handle_client(fd, peer_ip);
        ::close(fd);
    }
}

void HttpServer::handle_client(int fd, uint32_t peer_ip) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::string raw;
    HttpRequest request;
    size_t body_offset = 0;

    // --- en-têtes
    {
        const uint64_t deadline = util::now_ms() + kHeaderTimeoutMs;
        char chunk[4096];
        while (true) {
            bool complete = false;
            if (parse_http_headers(raw, request, &body_offset, &complete)) break;
            if (complete) return;  // malformée : on ferme sans répondre

            const uint64_t now = util::now_ms();
            if (now >= deadline) return;
            if (!wait_readable(fd, static_cast<int>(deadline - now))) return;

            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            raw.append(chunk, static_cast<size_t>(n));
        }
    }

    // --- corps
    const long long declared = request.content_length();
    HttpReply reply;
    bool overflow = false;

    if (declared > 0) {
        if (static_cast<size_t>(declared) > kMaxBody) {
            overflow = true;
        } else {
            request.body = raw.substr(body_offset);
            const size_t want = static_cast<size_t>(declared);

            const uint64_t deadline = util::now_ms() + kBodyTimeoutMs;
            char chunk[16 * 1024];
            while (request.body.size() < want) {
                const uint64_t now = util::now_ms();
                if (now >= deadline) return;
                if (!wait_readable(fd, static_cast<int>(deadline - now))) return;

                const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
                if (n <= 0) return;
                request.body.append(chunk, static_cast<size_t>(n));
            }
            request.body.resize(want);
        }
    }

    if (overflow) {
        reply.status = 413;
        reply.content_type = "text/plain; charset=utf-8";
        reply.body = "fichier trop volumineux";
    } else if (handler_) {
        reply = handler_(request);
        served_.fetch_add(1);
    } else {
        reply.status = 503;
        reply.body = "serveur non initialisé";
    }

    // --- réponse
    char head[512];
    const int head_len = std::snprintf(
        head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        reply.status, status_text(reply.status), reply.content_type.c_str(), reply.body.size(),
        reply.extra_headers.c_str());
    if (head_len <= 0) return;

    std::string out(head, static_cast<size_t>(head_len));
    out += reply.body;

    size_t sent = 0;
    const uint64_t deadline = util::now_ms() + kSendTimeoutMs;
    while (sent < out.size()) {
        const uint64_t now = util::now_ms();
        if (now >= deadline) break;
        if (!wait_writable(fd, static_cast<int>(deadline - now))) break;

        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        break;
    }

    (void)peer_ip;
}

}  // namespace net
