#include <algorithm>
#include <vector>
#include <iostream>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>

constexpr int kServerPort = 9666;
constexpr int kServerBacklog = 128;
constexpr size_t kTransferBufferSize = 64;
constexpr int kMaxEvents = 1000;
constexpr bool kVerbose = true;
constexpr bool kNonBlocking = true;

enum class ConnState { Receiving, Sending };

struct ConnectionData {
    int sock{-1};
    ConnState state{ConnState::Receiving};
    size_t bufferOffset{0};
    size_t bufferSize{0};
    std::array<char, kTransferBufferSize + 1> buffer{};
};

static bool processClientRecv(ConnectionData &cd);
static bool processClientSend(ConnectionData &cd);
static bool setSocketNonblocking(int fd);
static bool isInvalidConnection(const ConnectionData &cd);
static int setupServerSocket(short port);

int main(int argc, char *argv[]) {
    int serverPort = (argc == 2) ? atoi(argv[1]) : kServerPort;

    if (kVerbose) std::cout << "Attempting to bind to port " << serverPort << "\n";

    int listenfd = setupServerSocket(serverPort);
    if (listenfd == -1) return 1;

    std::vector<ConnectionData> connections;
    std::array<epoll_event, kMaxEvents> epEvents;

    int epollfd = epoll_create1(0);
    if (epollfd == -1) { perror("epoll_create1"); return 1; }

    epoll_event event{};
    event.data.fd = listenfd;
    event.events = EPOLLIN;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &event) == -1) {
        perror("epoll_ctl ADD listenfd");
        return 1;
    }

    while (true) {
        int eventCount = epoll_wait(epollfd, epEvents.data(), kMaxEvents, -1);
        if (eventCount == -1) { perror("epoll_wait"); break; }

        for (int i = 0; i < eventCount; ++i) {
            int fd = epEvents[i].data.fd;

            if (fd == listenfd) {
                sockaddr_in clientAddr{};
                socklen_t addrLen = sizeof(clientAddr);
                int clientfd = accept(listenfd, reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
                if (clientfd == -1) { perror("accept"); continue; }

                if (kVerbose) {
                    char addrStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
                    std::cout << "New connection from " << addrStr << ":" << ntohs(clientAddr.sin_port) << " -> fd " << clientfd << "\n";
                }

                if (kNonBlocking && !setSocketNonblocking(clientfd)) { close(clientfd); continue; }

                ConnectionData conn{};
                conn.sock = clientfd;
                connections.push_back(conn);

                event.data.fd = clientfd;
                event.events = EPOLLIN;
                epoll_ctl(epollfd, EPOLL_CTL_ADD, clientfd, &event);

            } else {
                auto it = std::find_if(connections.begin(), connections.end(), [fd](const ConnectionData &c) { return c.sock == fd; });
                if (it == connections.end()) continue;

                ConnectionData &cd = *it;
                bool keep = true;

                if ((epEvents[i].events & EPOLLIN) && cd.state == ConnState::Receiving)
                    keep = processClientRecv(cd);
                else if ((epEvents[i].events & EPOLLOUT) && cd.state == ConnState::Sending)
                    keep = processClientSend(cd);

                if (!keep) {
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, cd.sock, nullptr);
                    close(cd.sock);
                    cd.sock = -1;
                    continue;
                }

                event.data.fd = cd.sock;
                event.events = (cd.state == ConnState::Receiving) ? EPOLLIN : EPOLLOUT;
                epoll_ctl(epollfd, EPOLL_CTL_MOD, cd.sock, &event);
            }
        }
        
        connections.erase(std::remove_if(connections.begin(), connections.end(), isInvalidConnection), connections.end());
    }

    close(listenfd);
    close(epollfd);
    return 0;
}

static bool processClientRecv(ConnectionData &cd) {
    ssize_t ret = recv(cd.sock, cd.buffer.data(), kTransferBufferSize, 0);
    if (ret <= 0) return false;

    cd.bufferSize = static_cast<size_t>(ret);
    cd.bufferOffset = 0;
    cd.buffer[cd.bufferSize] = '\0';
    cd.state = ConnState::Sending;
    return true;
}

static bool processClientSend(ConnectionData &cd) {
    ssize_t ret = send(cd.sock, cd.buffer.data() + cd.bufferOffset, cd.bufferSize - cd.bufferOffset, MSG_NOSIGNAL);
    if (ret == -1) return false;

    cd.bufferOffset += static_cast<size_t>(ret);
    if (cd.bufferOffset == cd.bufferSize) {
        cd.bufferOffset = cd.bufferSize = 0;
        cd.state = ConnState::Receiving;
    }
    return true;
}

static int setupServerSocket(short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in servAddr{};
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr *>(&servAddr), sizeof(servAddr)) == -1) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, kServerBacklog) == -1) { perror("listen"); close(fd); return -1; }

    if (kNonBlocking && !setSocketNonblocking(fd)) { close(fd); return -1; }

    if (kVerbose) {
        char addrStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &servAddr.sin_addr, addrStr, sizeof(addrStr));
        std::cout << "Server listening on " << addrStr << ":" << ntohs(servAddr.sin_port) << "\n";
    }

    return fd;
}

static bool setSocketNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) { perror("fcntl F_GETFL"); return false; }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) { perror("fcntl F_SETFL"); return false; }
    return true;
}

static bool isInvalidConnection(const ConnectionData &cd) { return cd.sock == -1; }