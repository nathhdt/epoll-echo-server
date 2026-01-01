#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <iostream>
#include <array>
#include <memory>

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <vector>
#include <string>
#include <algorithm>

constexpr size_t kConnectionBufferSize = 256;
constexpr bool kVerbose = true;

enum class ConnState { Connecting, Sending, Receiving, Dead };

struct Connection {
    ConnState state;
    int sock;
    size_t expectedSize;
    size_t bufferOffset;
    size_t bufferSize;
    std::array<char, kConnectionBufferSize + 1> buffer{};
    size_t repeatsLeft;
    std::string message;
    size_t id;
};

static const char* gClientMessage = "client%d";

inline void printErr(const std::string& msg) {
    std::cerr << msg << ": " << std::strerror(errno) << "\n";
}

std::string buildMessage(size_t id) {
    char buf[kConnectionBufferSize];
    std::snprintf(buf, sizeof(buf), gClientMessage, static_cast<int>(id));
    return std::string(buf);
}

void prepareSendBuffer(Connection& conn) {
    conn.message = buildMessage(conn.id);
    std::snprintf(conn.buffer.data(), conn.buffer.size(), "%s", conn.message.c_str());
    conn.bufferSize = conn.message.size();
    conn.bufferOffset = 0;
    conn.state = ConnState::Sending;
}

bool clientSend(Connection& conn);
bool clientRecv(Connection& conn);
bool resolveAddress(sockaddr_storage& sa, socklen_t& sa_len, const char* host, const char* port);
int connectNonBlocking(const sockaddr_storage& sa, socklen_t sa_len);
std::vector<Connection> setupConnections(const sockaddr_storage& servAddr, socklen_t sa_len, size_t numClients, size_t numRepeats);
void runClientLoop(std::vector<Connection>& connections);

int main(int argc, char* argv[]) {
    if (argc < 4 || argc > 6) {
        std::cerr << "Usage: " << argv[0] << " <server> <port> <num_clients> [<num_repeats>] [<message>]\n";
        return 1;
    }

    const char* serverAddr = argv[1];
    const char* serverPort = argv[2];
    size_t numClients = static_cast<size_t>(atoi(argv[3]));
    size_t numRepeats = (argc >= 5) ? static_cast<size_t>(atoi(argv[4])) : 1;
    if (argc >= 6) gClientMessage = argv[5];

    std::cout << "Simulating " << numClients << " clients.\n";

    sockaddr_storage servAddr{};
    socklen_t sa_len = 0;
    if (!resolveAddress(servAddr, sa_len, serverAddr, serverPort)) {
        std::cerr << "Failed to resolve address " << serverAddr << ":" << serverPort << "\n";
        return 1;
    }

    auto connections = setupConnections(servAddr, sa_len, numClients, numRepeats);
    if (connections.empty()) {
        std::cerr << "All connections failed.\n";
        return 1;
    }

    runClientLoop(connections);
    return 0;
}

std::vector<Connection> setupConnections(const sockaddr_storage& servAddr, socklen_t sa_len, size_t numClients, size_t numRepeats) {
    std::vector<Connection> connections;
    connections.reserve(numClients);

    for (size_t i = 0; i < numClients; ++i) {
        Connection conn{};
        conn.id = i;
        conn.sock = connectNonBlocking(servAddr, sa_len);
        conn.state = (conn.sock != -1) ? ConnState::Connecting : ConnState::Dead;
        conn.repeatsLeft = numRepeats - 1;

        if (conn.sock != -1) connections.push_back(conn);
        else if (kVerbose) std::cerr << "Connection " << i << " failed\n";
    }

    return connections;
}

void runClientLoop(std::vector<Connection>& connections) {
    size_t clientsAlive = connections.size();

    while (clientsAlive > 0) {
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        int maxfd = 0;

        for (auto& conn : connections) {
            if (conn.state == ConnState::Dead) continue;
            if (conn.state == ConnState::Connecting || conn.state == ConnState::Sending) FD_SET(conn.sock, &wset);
            if (conn.state == ConnState::Receiving) FD_SET(conn.sock, &rset);
            maxfd = std::max(maxfd, conn.sock);
        }

        if (select(maxfd + 1, &rset, &wset, nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        size_t finishedClients = 0;
        for (auto& conn : connections) {
            if (conn.state == ConnState::Dead) continue;

            bool keep = true;
            if (FD_ISSET(conn.sock, &wset)) keep = clientSend(conn);
            else if (FD_ISSET(conn.sock, &rset)) keep = clientRecv(conn);

            if (!keep) {
                if (kVerbose) std::cerr << "Client " << conn.id << " closing connection\n";
                close(conn.sock);
                conn.sock = -1;
                conn.state = ConnState::Dead;
                ++finishedClients;
            }
        }
        clientsAlive -= finishedClients;
    }
}

bool clientSend(Connection& conn) {
    if (conn.state == ConnState::Connecting) {
        prepareSendBuffer(conn);
    }

    ssize_t sent;
    do {
        sent = send(conn.sock, conn.buffer.data() + conn.bufferOffset, conn.bufferSize - conn.bufferOffset, MSG_NOSIGNAL);
    } while (sent == -1 && errno == EINTR);

    if (sent == -1) { printErr("send failed"); return false; }

    conn.bufferOffset += sent;
    if (conn.bufferOffset == conn.bufferSize) {
        conn.expectedSize = conn.bufferSize;
        conn.bufferOffset = conn.bufferSize = 0;
        conn.state = ConnState::Receiving;
    }
    return true;
}

bool clientRecv(Connection& conn) {
    ssize_t recvd;
    do {
        recvd = recv(conn.sock, conn.buffer.data() + conn.bufferOffset, conn.expectedSize - conn.bufferOffset, 0);
    } while (recvd == -1 && errno == EINTR);

    if (recvd <= 0) {
        if (recvd == 0) {
            if (kVerbose) std::cout << "Client " << conn.id << " disconnected by server\n";
        } else {
            printErr("recv failed");
        }
        return false;
    }

    conn.bufferOffset += recvd;
    if (conn.bufferOffset < conn.buffer.size()) conn.buffer[conn.bufferOffset] = '\0';

    if (conn.bufferOffset == conn.expectedSize) {
        if (conn.repeatsLeft > 0) {
            prepareSendBuffer(conn);
            --conn.repeatsLeft;
        } else return false;
    }

    if (kVerbose) std::cout << "Client " << conn.id << " received: " << conn.buffer.data() << "\n";
    return true;
}

bool resolveAddress(sockaddr_storage& sa, socklen_t& sa_len, const char* host, const char* port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results_ptr(nullptr, freeaddrinfo);
    addrinfo* raw_results = nullptr;

    int ret = getaddrinfo(host, port, &hints, &raw_results);
    if (ret != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(ret) << "\n";
        return false;
    }
    results_ptr.reset(raw_results);

    for (addrinfo* r = results_ptr.get(); r != nullptr; r = r->ai_next) {
        if (r->ai_family == AF_INET || r->ai_family == AF_INET6) {
            std::memcpy(&sa, r->ai_addr, r->ai_addrlen);
            sa_len = r->ai_addrlen;
            return true;
        }
    }
    return false;
}

int connectNonBlocking(const sockaddr_storage& sa, socklen_t sa_len) {
    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (connect(fd, reinterpret_cast<const sockaddr*>(&sa), sa_len) == -1 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return fd;
}