#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

constexpr char kDefaultPorts[] = "8090,8091";
constexpr int kDefaultMinimumConnections = 100;
// 单台客户端、单网卡能建多少条 TCP 连接，通常先被 Windows 动态端口范围
// （默认 49152~65535，共 16384 个）卡住。探测目标设 20000 已超过它，
// 因此测试正常情况下会在"客户端端口耗尽"处自动停下，而不是停在服务端限制。
constexpr int kDefaultMaximumConnections = 20000;
constexpr int kDefaultHoldMilliseconds = 1000;

int ReadPositiveEnvironmentValue(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }

    try {
        const long parsed_value = std::stol(value);
        if (parsed_value > 0 && parsed_value <= (std::numeric_limits<int>::max)()) {
            return static_cast<int>(parsed_value);
        }
    } catch (const std::exception&) {
    }

    std::cerr << "Ignoring invalid " << name << "=" << value
              << "; using " << default_value << std::endl;
    return default_value;
}

std::string ReadEnvironmentValue(const char* name, const char* default_value) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? default_value : value;
}

#ifdef _WIN32
std::string DescribeSocketError(int error_code) {
    switch (error_code) {
        case WSAECONNREFUSED:
            return "server refused the connection (service down, backlog full, or firewall)";
        case WSAETIMEDOUT:
            return "connection timed out (server too slow to accept, or network issue)";
        case WSAECONNRESET:
            return "connection reset by peer while connecting";
        case WSAEADDRINUSE:
            return "local port already in use (ephemeral port exhaustion or TIME_WAIT reuse)";
        case WSAEADDRNOTAVAIL:
            return "no local address/port available (client ephemeral port range exhausted)";
        case WSAENOBUFS:
            return "no buffer space available (client or server resource exhaustion)";
        case WSAEMFILE:
            return "too many open files/handles in this process";
        case WSAENETUNREACH:
            return "network unreachable";
        case WSAEHOSTUNREACH:
            return "host unreachable";
        default:
            return "socket error code " + std::to_string(error_code);
    }
}
#endif

std::vector<int> ReadPortList() {
    const std::string ports = ReadEnvironmentValue("CHAT_CONNECTION_PORTS", kDefaultPorts);
    std::vector<int> result;
    std::size_t start = 0;

    while (start < ports.size()) {
        const std::size_t end = ports.find(',', start);
        const std::string value = ports.substr(start, end - start);
        try {
            const long port = std::stol(value);
            if (port > 0 && port <= 65535) {
                result.push_back(static_cast<int>(port));
            }
        } catch (const std::exception&) {
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return result;
}

#ifdef _WIN32

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        result_ = WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~WinsockRuntime() {
        if (result_ == 0) {
            WSACleanup();
        }
    }

    int result() const {
        return result_;
    }

private:
    int result_ = 0;
};

class Socket {
public:
    explicit Socket(SOCKET handle) : handle_(handle) {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_SOCKET;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = other.handle_;
            other.handle_ = INVALID_SOCKET;
        }
        return *this;
    }

    ~Socket() {
        Close();
    }

private:
    void Close() {
        if (handle_ != INVALID_SOCKET) {
            // 用 SO_LINGER + 0 超时主动发 RST 关闭，而不是走 FIN 四次挥手，
            // 避免测试结束后留下大量 TIME_WAIT 连接，占住动态端口导致不能连续复跑。
            linger abortive_linger{1, 0};
            setsockopt(handle_, SOL_SOCKET, SO_LINGER,
                       reinterpret_cast<const char*>(&abortive_linger),
                       static_cast<int>(sizeof(abortive_linger)));
            closesocket(handle_);
            handle_ = INVALID_SOCKET;
        }
    }

    SOCKET handle_ = INVALID_SOCKET;
};

SOCKET ConnectToServer(const std::string& host, int port, int& last_error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const int lookup_result = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
    if (lookup_result != 0) {
        last_error = lookup_result;
        return INVALID_SOCKET;
    }

    SOCKET connected_socket = INVALID_SOCKET;
    last_error = 0;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        SOCKET socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_handle == INVALID_SOCKET) {
            last_error = WSAGetLastError();
            continue;
        }

        if (connect(socket_handle, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            connected_socket = socket_handle;
            break;
        }

        last_error = WSAGetLastError();
        closesocket(socket_handle);
    }

    freeaddrinfo(addresses);
    return connected_socket;
}

#endif

TEST(ConnectionCapacityTest, HoldsConfiguredNumberOfTcpConnections) {
#ifndef _WIN32
    GTEST_SKIP() << "This test currently uses the Windows socket API used by the project development environment.";
#else
    WinsockRuntime winsock;
    ASSERT_EQ(winsock.result(), 0) << "WSAStartup failed with error " << winsock.result();

    const std::string host = ReadEnvironmentValue("CHAT_CONNECTION_HOST", "127.0.0.1");
    const std::vector<int> ports = ReadPortList();
    const int minimum_connections = ReadPositiveEnvironmentValue(
        "CHAT_CONNECTION_MIN", kDefaultMinimumConnections);
    const int maximum_connections = ReadPositiveEnvironmentValue(
        "CHAT_CONNECTION_MAX", kDefaultMaximumConnections);
    const int hold_milliseconds = ReadPositiveEnvironmentValue(
        "CHAT_CONNECTION_HOLD_MS", kDefaultHoldMilliseconds);

    ASSERT_GE(maximum_connections, minimum_connections)
        << "CHAT_CONNECTION_MAX must be greater than or equal to CHAT_CONNECTION_MIN";
    ASSERT_FALSE(ports.empty())
        << "CHAT_CONNECTION_PORTS must contain at least one valid TCP port";

    struct ServerProbe {
        int port;
        int first_error = 0;
        bool accepting_connections = true;
        std::vector<Socket> connections;
    };

    std::vector<ServerProbe> probes;
    probes.reserve(ports.size());
    for (int port : ports) {
        probes.push_back({port});
        probes.back().connections.reserve(static_cast<std::size_t>(maximum_connections));
    }

    for (int attempt = 0; attempt < maximum_connections; ++attempt) {
        for (ServerProbe& probe : probes) {
            if (!probe.accepting_connections) {
                continue;
            }

            SOCKET socket_handle = ConnectToServer(host, probe.port, probe.first_error);
            if (socket_handle == INVALID_SOCKET) {
                probe.accepting_connections = false;
                continue;
            }

            probe.connections.emplace_back(socket_handle);
        }

        if ((attempt + 1) % 100 == 0) {
            std::cout << "Completed connection round " << attempt + 1 << std::endl;
        }
    }

    bool every_server_refused_connection = true;
    for (const ServerProbe& probe : probes) {
        every_server_refused_connection = every_server_refused_connection
            && probe.connections.empty() && probe.first_error == WSAECONNREFUSED;
    }

    std::cout << "Each server is probed one connection at a time until the first failure "
              << "or until " << maximum_connections << " per server (probe ceiling)."
              << std::endl;

    if (every_server_refused_connection) {
        GTEST_SKIP() << "No ChatServer is listening on " << host
                     << "; start ChatServer1 and ChatServer2 before running this integration test.";
    }

    for (const ServerProbe& probe : probes) {
        std::cout << "Connection capacity result for " << host << ':' << probe.port << ": "
                  << probe.connections.size() << " successful connections"
                  << " (minimum=" << minimum_connections << ", maximum probe="
                  << maximum_connections << ')';
        if (static_cast<int>(probe.connections.size()) < maximum_connections) {
            std::cout << ", first socket error=" << probe.first_error << " ("
                      << DescribeSocketError(probe.first_error) << ')';
        } else {
            std::cout << ", probe ceiling reached; this is the client-side test ceiling, "
                      << "not necessarily the ChatServer limit. Increase CHAT_CONNECTION_MAX "
                      << "or run clients from more machines to continue";
        }
        std::cout << std::endl;
    }

    bool client_port_limit_hit = false;
    for (const ServerProbe& probe : probes) {
        client_port_limit_hit = client_port_limit_hit
            || probe.first_error == WSAEADDRINUSE
            || probe.first_error == WSAEADDRNOTAVAIL
            || probe.first_error == WSAENOBUFS;
    }
    if (client_port_limit_hit) {
        std::cout << "Note: the failure error indicates this single client ran out of local "
                  << "TCP ports before the server stopped accepting. Query the current range "
                  << "with: netsh int ipv4 show dynamicport tcp" << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(hold_milliseconds));

    for (const ServerProbe& probe : probes) {
        EXPECT_GE(static_cast<int>(probe.connections.size()), minimum_connections)
            << "ChatServer on port " << probe.port
            << " accepted fewer TCP connections than CHAT_CONNECTION_MIN";
    }
#endif
}

} // namespace
