// Transport « en clair » : sockets POSIX telles que fournies par libnx.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

#include "net/transport.hpp"

namespace net {

namespace {

int set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

sockaddr_in to_sockaddr(const Endpoint& ep) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep.port);
    addr.sin_addr.s_addr = htonl(ep.ipv4);
    return addr;
}

class BsdTcpSocket : public TcpSocket {
public:
    ~BsdTcpSocket() override { close(); }

    bool start_connect(const Endpoint& ep) override {
        close();
        fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ < 0) return false;

        set_nonblocking(fd_);

        // Nagle nuit au protocole peer wire : beaucoup de petits messages de
        // contrôle dont la latence compte plus que le remplissage des segments.
        const int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        const sockaddr_in addr = to_sockaddr(ep);
        const int rc = ::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) {
            connected_ = true;
            return true;
        }
        if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN) return true;

        close();
        return false;
    }

    bool finish_connect() override {
        if (connected_) return true;
        if (fd_ < 0) return false;

        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close();
            return false;
        }
        connected_ = true;
        return true;
    }

    int send(const uint8_t* data, size_t len) override {
        if (fd_ < 0) return kError;
        const ssize_t n = ::send(fd_, data, len, 0);
        if (n > 0) return static_cast<int>(n);
        if (n == 0) return kWouldBlock;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return kWouldBlock;
        return kError;
    }

    int recv(uint8_t* data, size_t len) override {
        if (fd_ < 0) return kError;
        const ssize_t n = ::recv(fd_, data, len, 0);
        if (n > 0) return static_cast<int>(n);
        if (n == 0) return kClosed;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return kWouldBlock;
        return kError;
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

    bool is_open() const override { return fd_ >= 0; }
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    bool connected_ = false;
};

class BsdUdpSocket : public UdpSocket {
public:
    ~BsdUdpSocket() override { close(); }

    bool open() override {
        close();
        fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) return false;
        set_nonblocking(fd_);
        return true;
    }

    int send_to(const Endpoint& ep, const uint8_t* data, size_t len) override {
        if (fd_ < 0) return kError;
        const sockaddr_in addr = to_sockaddr(ep);
        const ssize_t n = ::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(&addr),
                                   sizeof(addr));
        if (n >= 0) return static_cast<int>(n);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return kWouldBlock;
        return kError;
    }

    int recv_from(Endpoint& from, uint8_t* data, size_t len) override {
        if (fd_ < 0) return kError;
        sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);
        const ssize_t n =
            ::recvfrom(fd_, data, len, 0, reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (n >= 0) {
            from.ipv4 = ntohl(addr.sin_addr.s_addr);
            from.port = ntohs(addr.sin_port);
            return static_cast<int>(n);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return kWouldBlock;
        return kError;
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool is_open() const override { return fd_ >= 0; }
    int fd() const { return fd_; }

private:
    int fd_ = -1;
};

class BsdTransport : public Transport {
public:
    const char* name() const override { return "direct (sans VPN)"; }
    bool ready() const override { return true; }

    std::unique_ptr<TcpSocket> tcp() override { return std::make_unique<BsdTcpSocket>(); }
    std::unique_ptr<UdpSocket> udp() override { return std::make_unique<BsdUdpSocket>(); }

    bool resolve(const std::string& host, uint32_t& ipv4_out) override {
        // Déjà une adresse littérale ?
        in_addr literal{};
        if (inet_pton(AF_INET, host.c_str(), &literal) == 1) {
            ipv4_out = ntohl(literal.s_addr);
            return true;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;

        const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
        ipv4_out = ntohl(addr->sin_addr.s_addr);
        freeaddrinfo(res);
        return ipv4_out != 0;
    }

    int poll(PollItem* items, size_t count, int timeout_ms) override {
        if (count == 0) return 0;

        std::vector<pollfd> fds(count);
        for (size_t i = 0; i < count; ++i) {
            int fd = -1;
            if (items[i].tcp) fd = static_cast<BsdTcpSocket*>(items[i].tcp)->fd();
            else if (items[i].udp) fd = static_cast<BsdUdpSocket*>(items[i].udp)->fd();

            fds[i].fd = fd;
            fds[i].events = 0;
            if (items[i].want_read) fds[i].events |= POLLIN;
            if (items[i].want_write) fds[i].events |= POLLOUT;
            fds[i].revents = 0;

            items[i].can_read = items[i].can_write = items[i].has_error = false;
        }

        const int rc = ::poll(fds.data(), static_cast<nfds_t>(count), timeout_ms);
        if (rc <= 0) return rc;

        for (size_t i = 0; i < count; ++i) {
            items[i].can_read = (fds[i].revents & POLLIN) != 0;
            items[i].can_write = (fds[i].revents & POLLOUT) != 0;
            items[i].has_error = (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        }
        return rc;
    }
};

}  // namespace

std::string Endpoint::to_string() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ipv4 >> 24) & 0xff, (ipv4 >> 16) & 0xff,
                  (ipv4 >> 8) & 0xff, ipv4 & 0xff, port);
    return buf;
}

std::unique_ptr<Transport> make_bsd_transport() {
    return std::make_unique<BsdTransport>();
}

}  // namespace net
