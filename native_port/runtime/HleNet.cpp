// System Link ("direct connect") networking over a virtual network.
//
// The game uses the Xbox's statically linked Winsock/XNet library through a thunk table at 0x2A2E00-0x2A309C (every
// call there is __stdcall; see docs in the call map below). With CW_NET set, those thunks are replaced: the game gets
// virtual sockets on a virtual network, carried over ONE real UDP socket per game.
//
// - Each game has a virtual IPv4 address (10.x.y.z; CW_NET_IP to choose) that it sees as its own (XNetGetTitleXnAddr)
//   and that peers see as the sender. The game identifies peers by IP only, so every game needs a distinct one.
// - Virtual sockets keep the game's ports (1000 game, 1001 discovery, 1002 voice) and queue received packets; a real
//   datagram carries a header [magic, source IP, destination IP, source port, destination port] then the payload.
// - Broadcasts (255.255.255.255, used for discovery) go to every peer known so far, to the CW_NET_JOIN addresses, and
//   with CW_NET_LAN=1 also as a real LAN broadcast. Peers are learned from what arrives (virtual IP -> real address).
// - Xbox security (XNADDR/XNKID/XNKEY, XNetConnect) is reduced to "an XNADDR holds the virtual IP" and "connected".
//
// Settings:
//   CW_NET=1                 enable networking (otherwise the console is offline, as before)
//   CW_NET_PORT=3074         the real UDP port to use (the one to forward for internet play)
//   CW_NET_JOIN=host:port    where to look for games (comma-separated); the host's address for internet play
//   CW_NET_IP=10.0.0.2       this game's virtual IP (default: random 10.x.y.z)
//   CW_NET_LAN=1             also broadcast discovery on the local network
//   CW_NET_LOG=1             log every packet

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "Hle.h"

#include "Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace cw::hle {

namespace {

// Xbox Winsock/XNet thunks (the game calls these; each loads the XNet object and jumps to the library).
constexpr std::uint32_t kXNetStartup = 0x002A32A7;
constexpr std::uint32_t kWSAStartup = 0x002A32BE;
constexpr std::uint32_t kWSACleanup = 0x002A33B9;
constexpr std::uint32_t kXNetCleanup = 0x002A33AC;
constexpr std::uint32_t kWSAGetLastError = 0x002A3449;
constexpr std::uint32_t kSocket = 0x002A2F75;
constexpr std::uint32_t kCloseSocket = 0x002A2F80;
constexpr std::uint32_t kIoctlSocket = 0x002A2F96;
constexpr std::uint32_t kSetSockOpt = 0x002A2FA1;
constexpr std::uint32_t kBind = 0x002A2FB0;
constexpr std::uint32_t kSelect = 0x002A2FC6;
constexpr std::uint32_t kRecvFrom = 0x002A2FFE;
constexpr std::uint32_t kSendTo = 0x002A301C;
constexpr std::uint32_t kXNetRandom = 0x002A2ECF;
constexpr std::uint32_t kXNetCreateKey = 0x002A2EDA;
constexpr std::uint32_t kXNetRegisterKey = 0x002A2EE5;
constexpr std::uint32_t kXNetUnregisterKey = 0x002A2EF0;
constexpr std::uint32_t kXNetXnAddrToInAddr = 0x002A2EFB;
constexpr std::uint32_t kXNetInAddrToXnAddr = 0x002A3074;
constexpr std::uint32_t kXNetConnect = 0x002A308B;
constexpr std::uint32_t kXNetGetConnectStatus = 0x002A3096;
constexpr std::uint32_t kXNetQosListen = 0x002A2F34;
constexpr std::uint32_t kXNetQosLookup = 0x002A2F43;
constexpr std::uint32_t kXNetQosRelease = 0x002A2F5F;
constexpr std::uint32_t kXNetGetTitleXnAddr = 0x002A2F6A;
constexpr std::uint32_t kXNetGetEthernetLinkStatus = 0x0015E2E0;
constexpr std::uint32_t kXOnlineStartup = 0x002B57CC;  // HRESULT __stdcall(params*)

constexpr std::uint32_t kMagic = 0x314E5743;  // "CWN1"
constexpr std::uint32_t kBroadcast = 0xFFFFFFFF;
constexpr int kWouldBlock = 10035;  // WSAEWOULDBLOCK
constexpr int kNotSocket = 10038;   // WSAENOTSOCK
constexpr int kMaxDatagram = 1500;

#pragma pack(push, 1)
struct Header {
    std::uint32_t magic;
    std::uint32_t sourceIp;  // network byte order, as the game sees it
    std::uint32_t destinationIp;
    std::uint16_t sourcePort;  // network byte order
    std::uint16_t destinationPort;
};
#pragma pack(pop)

struct XnAddr {
    std::uint32_t ina;
    std::uint32_t inaOnline;
    std::uint16_t portOnline;
    std::uint8_t ethernet[6];
    std::uint8_t online[20];
};
static_assert(sizeof(XnAddr) == 0x24);

struct QosInfo {
    std::uint8_t flags;
    std::uint8_t reserved;
    std::uint16_t probesSent;
    std::uint16_t probesReceived;
    std::uint16_t dataSize;
    std::uint8_t* data;
    std::uint16_t rttMin;
    std::uint16_t rttMedian;
    std::uint32_t upBitsPerSecond;
    std::uint32_t downBitsPerSecond;
};
static_assert(sizeof(QosInfo) == 0x18);

struct Datagram {
    std::uint32_t sourceIp;
    std::uint16_t sourcePort;
    std::vector<std::uint8_t> payload;
};

struct VirtualSocket {
    std::uint16_t port = 0;  // network byte order; 0 = not bound yet
    bool nonBlocking = false;
    std::deque<Datagram> received;
};

std::mutex g_mutex;
bool g_enabled = false;
bool g_log = false;
bool g_lan = false;
SOCKET g_real = INVALID_SOCKET;
std::uint16_t g_realPort = 0;           // host byte order
std::uint32_t g_ip = 0;                 // network byte order
std::map<std::uint32_t, sockaddr_in> g_peers;  // virtual IP -> real address
std::vector<sockaddr_in> g_seeds;       // CW_NET_JOIN addresses
std::map<std::uint32_t, VirtualSocket> g_sockets;  // handle -> socket
std::uint32_t g_nextHandle = 0x100;
std::uint16_t g_nextEphemeral = 50000;
thread_local int g_lastError = 0;

std::string ipText(std::uint32_t ip) {
    char text[32];
    const auto* b = reinterpret_cast<const std::uint8_t*>(&ip);
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

std::string addressText(const sockaddr_in& address) {
    return ipText(address.sin_addr.s_addr) + ":" + std::to_string(ntohs(address.sin_port));
}

bool parseAddress(const std::string& text, std::uint16_t defaultPort, sockaddr_in& out) {
    std::string host = text;
    std::uint16_t port = defaultPort;
    if (const std::size_t colon = text.rfind(':'); colon != std::string::npos) {
        host = text.substr(0, colon);
        port = static_cast<std::uint16_t>(std::atoi(text.c_str() + colon + 1));
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    out = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    out.sin_port = htons(port);
    freeaddrinfo(result);
    return true;
}

void sendReal(const sockaddr_in& to, const Header& header, const void* payload, int length) {
    std::uint8_t buffer[sizeof(Header) + kMaxDatagram];
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), payload, length);
    sendto(g_real, reinterpret_cast<const char*>(buffer), static_cast<int>(sizeof(header)) + length, 0,
        reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// Reads everything waiting on the real socket into the virtual sockets' queues. Caller holds g_mutex.
void pump() {
    std::uint8_t buffer[sizeof(Header) + kMaxDatagram];
    for (;;) {
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = recvfrom(g_real, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received < 0) {
            return;  // WSAEWOULDBLOCK (or a ICMP port-unreachable error from an earlier send); nothing more for now
        }
        if (received < static_cast<int>(sizeof(Header))) {
            continue;
        }
        Header header;
        std::memcpy(&header, buffer, sizeof(header));
        if (header.magic != kMagic || header.sourceIp == g_ip) {
            continue;
        }
        if (header.destinationIp != g_ip && header.destinationIp != kBroadcast) {
            continue;
        }
        auto& known = g_peers[header.sourceIp];
        if (known.sin_port != from.sin_port || known.sin_addr.s_addr != from.sin_addr.s_addr) {
            logf("net: peer %s is at %s", ipText(header.sourceIp).c_str(), addressText(from).c_str());
            known = from;
        }
        const int payloadLength = received - static_cast<int>(sizeof(Header));
        bool delivered = false;
        for (auto& [handle, socket] : g_sockets) {
            if (socket.port == header.destinationPort) {
                socket.received.push_back({header.sourceIp, header.sourcePort, std::vector<std::uint8_t>(buffer + sizeof(Header), buffer + received)});
                if (socket.received.size() > 512) {
                    socket.received.pop_front();
                }
                delivered = true;
                break;
            }
        }
        if (g_log) {
            logf("net: <- %s:%u to port %u, %d bytes%s", ipText(header.sourceIp).c_str(), ntohs(header.sourcePort), ntohs(header.destinationPort),
                payloadLength, delivered ? "" : " (no socket)");
        }
    }
}

VirtualSocket* find(std::uint32_t handle) {
    const auto it = g_sockets.find(handle);
    return it != g_sockets.end() ? &it->second : nullptr;
}

// --- XNet / Winsock replacements (all __stdcall, like the Xbox library) ---

int __stdcall xXNetStartup(const void*) {
    if (!g_enabled) {
        logf("network: XNetStartup -> WSASYSNOTREADY (offline; set CW_NET=1 for System Link)");
        return 10091;
    }
    logf("net: XNetStartup (virtual network, this game is %s, UDP port %u)", ipText(g_ip).c_str(), g_realPort);
    return 0;
}

int __stdcall xWSAStartup(WORD, void* data) {
    if (!g_enabled) {
        return 10091;
    }
    if (data != nullptr) {
        std::memset(data, 0, 0x190);
        *reinterpret_cast<WORD*>(data) = 0x0202;
        *(reinterpret_cast<WORD*>(data) + 1) = 0x0202;
    }
    return 0;
}

int __stdcall xCleanup() {
    return 0;
}

int __stdcall xWSAGetLastError() {
    return g_lastError;
}

std::uint32_t __stdcall xSocket(int af, int type, int protocol) {
    std::lock_guard lock(g_mutex);
    const std::uint32_t handle = g_nextHandle++;
    g_sockets[handle] = VirtualSocket{};
    logf("net: socket(%d, %d, %d) -> %u", af, type, protocol, handle);
    return handle;
}

int __stdcall xCloseSocket(std::uint32_t handle) {
    std::lock_guard lock(g_mutex);
    g_sockets.erase(handle);
    return 0;
}

int __stdcall xIoctlSocket(std::uint32_t handle, long command, u_long* argument) {
    std::lock_guard lock(g_mutex);
    VirtualSocket* socket = find(handle);
    if (socket == nullptr) {
        g_lastError = kNotSocket;
        return -1;
    }
    if (command == static_cast<long>(0x8004667E) && argument != nullptr) {  // FIONBIO
        socket->nonBlocking = *argument != 0;
    }
    return 0;
}

int __stdcall xSetSockOpt(std::uint32_t, int, int, const char*, int) {
    return 0;  // SO_BROADCAST / SO_REUSEADDR: always allowed on the virtual network
}

int __stdcall xBind(std::uint32_t handle, const sockaddr_in* address, int) {
    std::lock_guard lock(g_mutex);
    VirtualSocket* socket = find(handle);
    if (socket == nullptr) {
        g_lastError = kNotSocket;
        return -1;
    }
    socket->port = address != nullptr && address->sin_port != 0 ? address->sin_port : htons(g_nextEphemeral++);
    logf("net: socket %u bound to port %u", handle, ntohs(socket->port));
    return 0;
}

// The game only uses select to wait until a socket can send, with no timeout; virtual sockets always can.
int __stdcall xSelect(int, void* readSet, void* writeSet, void*, const void*) {
    return 1;
}

int __stdcall xRecvFrom(std::uint32_t handle, char* buffer, int length, int, sockaddr_in* from, int* fromLength) {
    for (;;) {
        {
            std::lock_guard lock(g_mutex);
            pump();
            VirtualSocket* socket = find(handle);
            if (socket == nullptr) {
                g_lastError = kNotSocket;
                return -1;
            }
            if (!socket->received.empty()) {
                Datagram datagram = std::move(socket->received.front());
                socket->received.pop_front();
                const int copied = std::min<int>(length, static_cast<int>(datagram.payload.size()));
                std::memcpy(buffer, datagram.payload.data(), copied);
                if (from != nullptr) {
                    std::memset(from, 0, sizeof(*from));
                    from->sin_family = AF_INET;
                    from->sin_addr.s_addr = datagram.sourceIp;
                    from->sin_port = datagram.sourcePort;
                }
                if (fromLength != nullptr) {
                    *fromLength = sizeof(sockaddr_in);
                }
                return copied;
            }
            if (socket->nonBlocking) {
                g_lastError = kWouldBlock;
                return -1;
            }
        }
        Sleep(1);
    }
}

int __stdcall xSendTo(std::uint32_t handle, const char* buffer, int length, int, const sockaddr_in* to, int) {
    std::lock_guard lock(g_mutex);
    pump();
    VirtualSocket* socket = find(handle);
    if (socket == nullptr) {
        g_lastError = kNotSocket;
        return -1;
    }
    if (socket->port == 0) {
        socket->port = htons(g_nextEphemeral++);
    }
    length = std::min(length, kMaxDatagram);
    const Header header{kMagic, g_ip, to->sin_addr.s_addr, socket->port, to->sin_port};
    if (to->sin_addr.s_addr == kBroadcast || to->sin_addr.s_addr == INADDR_ANY) {
        Header broadcast = header;
        broadcast.destinationIp = kBroadcast;
        std::vector<std::uint32_t> sentTo;
        for (const auto& [ip, address] : g_peers) {
            sendReal(address, broadcast, buffer, length);
        }
        for (const sockaddr_in& seed : g_seeds) {
            sendReal(seed, broadcast, buffer, length);
        }
        if (g_lan) {
            sockaddr_in lan{};
            lan.sin_family = AF_INET;
            lan.sin_addr.s_addr = INADDR_BROADCAST;
            lan.sin_port = htons(g_realPort);
            sendReal(lan, broadcast, buffer, length);
        }
        if (g_log) {
            logf("net: -> broadcast port %u, %d bytes (%zu peers, %zu join addresses)", ntohs(to->sin_port), length, g_peers.size(), g_seeds.size());
        }
        return length;
    }
    const auto peer = g_peers.find(to->sin_addr.s_addr);
    if (peer == g_peers.end()) {
        // Not heard from yet: try the join addresses (the host is usually one of them).
        for (const sockaddr_in& seed : g_seeds) {
            sendReal(seed, header, buffer, length);
        }
        if (g_log) {
            logf("net: -> %s:%u (unknown peer, via join addresses), %d bytes", ipText(to->sin_addr.s_addr).c_str(), ntohs(to->sin_port), length);
        }
        return length;
    }
    sendReal(peer->second, header, buffer, length);
    if (g_log) {
        logf("net: -> %s:%u, %d bytes", ipText(to->sin_addr.s_addr).c_str(), ntohs(to->sin_port), length);
    }
    return length;
}

int __stdcall xXNetRandom(std::uint8_t* buffer, unsigned count) {
    static std::mt19937 random{std::random_device{}()};
    for (unsigned i = 0; i < count; ++i) {
        buffer[i] = static_cast<std::uint8_t>(random());
    }
    return 0;
}

int __stdcall xXNetCreateKey(std::uint8_t* keyId, std::uint8_t* key) {
    xXNetRandom(keyId, 8);
    xXNetRandom(key, 16);
    return 0;
}

int __stdcall xXNetRegisterKey(const void*, const void*) {
    return 0;
}

int __stdcall xXNetUnregisterKey(const void*) {
    return 0;
}

// An XNADDR on the virtual network is just the game's virtual IP (secure-address mapping is the identity).
int __stdcall xXNetXnAddrToInAddr(const XnAddr* address, const void*, std::uint32_t* ina) {
    *ina = address->ina;
    return 0;
}

int __stdcall xXNetInAddrToXnAddr(std::uint32_t ina, XnAddr* address, std::uint8_t* keyId) {
    if (address != nullptr) {
        std::memset(address, 0, sizeof(*address));
        address->ina = ina;
        std::memcpy(address->ethernet, &ina, 4);
    }
    return 0;
}

int __stdcall xXNetConnect(std::uint32_t) {
    return 0;
}

int __stdcall xXNetGetConnectStatus(std::uint32_t) {
    return 2;  // XNET_CONNECT_STATUS_CONNECTED
}

int __stdcall xXNetQosListen(const void*, const void*, unsigned, unsigned, unsigned) {
    return 0;
}

// Lobby ping probes: report every target as reached with a nominal round trip.
int __stdcall xXNetQosLookup(unsigned count, const void**, const void**, const void**, unsigned, const void*, const void*, unsigned, unsigned, unsigned, HANDLE event,
    void** result) {
    const std::size_t size = 8 + sizeof(QosInfo) * (count > 0 ? count : 1);
    auto* qos = static_cast<std::uint8_t*>(std::calloc(1, size));
    reinterpret_cast<std::uint32_t*>(qos)[0] = count;  // cxnqos
    reinterpret_cast<std::uint32_t*>(qos)[1] = 0;      // cxnqosPending
    auto* infos = reinterpret_cast<QosInfo*>(qos + 8);
    for (unsigned i = 0; i < count; ++i) {
        infos[i].flags = 0x01 | 0x02;  // COMPLETE | TARGET_CONTACTED
        infos[i].probesSent = infos[i].probesReceived = 8;
        infos[i].rttMin = infos[i].rttMedian = 40;
        infos[i].upBitsPerSecond = infos[i].downBitsPerSecond = 10000000;
    }
    *result = qos;
    if (event != nullptr) {
        SetEvent(event);
    }
    return 0;
}

int __stdcall xXNetQosRelease(void* qos) {
    std::free(qos);
    return 0;
}

unsigned __stdcall xXNetGetTitleXnAddr(XnAddr* address) {
    std::memset(address, 0, sizeof(*address));
    address->ina = g_ip;
    std::memcpy(address->ethernet, &g_ip, 4);
    return 0x04 | 0x02;  // XNET_GET_XNADDR_ETHERNET | STATIC
}

// Xbox Live is not emulated: XOnlineStartup (called at boot once there is a network) fails, which leaves the global
// XOnline object null exactly as when the console is offline; System Link does not use it.
int __stdcall xXOnlineStartup(const void*) {
    logf("net: XOnlineStartup -> E_FAIL (Xbox Live is not available; System Link is)");
    return static_cast<int>(0x80004005);
}

unsigned __stdcall xXNetGetEthernetLinkStatus() {
    return g_enabled ? 0x01 | 0x02 | 0x08 : 0;  // ACTIVE | 100MBPS | FULL_DUPLEX
}

void configure() {
    const char* enabled = std::getenv("CW_NET");
    g_enabled = enabled != nullptr && std::strcmp(enabled, "0") != 0;
    if (!g_enabled) {
        return;
    }
    g_log = std::getenv("CW_NET_LOG") != nullptr;
    g_lan = std::getenv("CW_NET_LAN") != nullptr;
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    const char* portText = std::getenv("CW_NET_PORT");
    g_realPort = static_cast<std::uint16_t>(portText != nullptr ? std::atoi(portText) : 3074);
    g_real = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL broadcast = TRUE;
    setsockopt(g_real, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
    // Windows reports an ICMP "port unreachable" from an earlier send as a recvfrom error; ignore those.
    BOOL noReset = FALSE;
    DWORD returned = 0;
    WSAIoctl(g_real, _WSAIOW(IOC_VENDOR, 12), &noReset, sizeof(noReset), nullptr, 0, &returned, nullptr, nullptr);  // SIO_UDP_CONNRESET
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(g_realPort);
    if (bind(g_real, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        logf("net: cannot use UDP port %u (error %d); is another game using it? Set CW_NET_PORT", g_realPort, WSAGetLastError());
    }
    u_long nonBlocking = 1;
    ioctlsocket(g_real, FIONBIO, &nonBlocking);

    if (const char* ip = std::getenv("CW_NET_IP")) {
        inet_pton(AF_INET, ip, &g_ip);
    } else {
        std::mt19937 random{std::random_device{}()};
        const std::uint8_t bytes[4] = {10, static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(random()),
            static_cast<std::uint8_t>(2 + random() % 250)};
        std::memcpy(&g_ip, bytes, 4);
    }
    if (const char* join = std::getenv("CW_NET_JOIN")) {
        std::string list = join;
        std::size_t position = 0;
        while (position <= list.size()) {
            const std::size_t end = list.find(',', position) == std::string::npos ? list.size() : list.find(',', position);
            sockaddr_in address{};
            if (end > position && parseAddress(list.substr(position, end - position), 3074, address)) {
                g_seeds.push_back(address);
                logf("net: will look for games at %s", addressText(address).c_str());
            }
            position = end + 1;
        }
    }
    logf("net: virtual network on, this game is %s, real UDP port %u%s", ipText(g_ip).c_str(), g_realPort, g_lan ? ", LAN broadcast on" : "");
}

} // namespace

void installNetHooks() {
    configure();
    hookFunction(kXNetStartup, reinterpret_cast<const void*>(&xXNetStartup), "XNetStartup");
    hookFunction(kWSAStartup, reinterpret_cast<const void*>(&xWSAStartup), "WSAStartup");
    if (!g_enabled) {
        return;
    }
    hookFunction(kWSACleanup, reinterpret_cast<const void*>(&xCleanup), "WSACleanup");
    hookFunction(kXNetCleanup, reinterpret_cast<const void*>(&xCleanup), "XNetCleanup");
    hookFunction(kWSAGetLastError, reinterpret_cast<const void*>(&xWSAGetLastError), "WSAGetLastError");
    hookFunction(kSocket, reinterpret_cast<const void*>(&xSocket), "socket");
    hookFunction(kCloseSocket, reinterpret_cast<const void*>(&xCloseSocket), "closesocket");
    hookFunction(kIoctlSocket, reinterpret_cast<const void*>(&xIoctlSocket), "ioctlsocket");
    hookFunction(kSetSockOpt, reinterpret_cast<const void*>(&xSetSockOpt), "setsockopt");
    hookFunction(kBind, reinterpret_cast<const void*>(&xBind), "bind");
    hookFunction(kSelect, reinterpret_cast<const void*>(&xSelect), "select");
    hookFunction(kRecvFrom, reinterpret_cast<const void*>(&xRecvFrom), "recvfrom");
    hookFunction(kSendTo, reinterpret_cast<const void*>(&xSendTo), "sendto");
    hookFunction(kXNetRandom, reinterpret_cast<const void*>(&xXNetRandom), "XNetRandom");
    hookFunction(kXNetCreateKey, reinterpret_cast<const void*>(&xXNetCreateKey), "XNetCreateKey");
    hookFunction(kXNetRegisterKey, reinterpret_cast<const void*>(&xXNetRegisterKey), "XNetRegisterKey");
    hookFunction(kXNetUnregisterKey, reinterpret_cast<const void*>(&xXNetUnregisterKey), "XNetUnregisterKey");
    hookFunction(kXNetXnAddrToInAddr, reinterpret_cast<const void*>(&xXNetXnAddrToInAddr), "XNetXnAddrToInAddr");
    hookFunction(kXNetInAddrToXnAddr, reinterpret_cast<const void*>(&xXNetInAddrToXnAddr), "XNetInAddrToXnAddr");
    hookFunction(kXNetConnect, reinterpret_cast<const void*>(&xXNetConnect), "XNetConnect");
    hookFunction(kXNetGetConnectStatus, reinterpret_cast<const void*>(&xXNetGetConnectStatus), "XNetGetConnectStatus");
    hookFunction(kXNetQosListen, reinterpret_cast<const void*>(&xXNetQosListen), "XNetQosListen");
    hookFunction(kXNetQosLookup, reinterpret_cast<const void*>(&xXNetQosLookup), "XNetQosLookup");
    hookFunction(kXNetQosRelease, reinterpret_cast<const void*>(&xXNetQosRelease), "XNetQosRelease");
    hookFunction(kXNetGetTitleXnAddr, reinterpret_cast<const void*>(&xXNetGetTitleXnAddr), "XNetGetTitleXnAddr");
    hookFunction(kXNetGetEthernetLinkStatus, reinterpret_cast<const void*>(&xXNetGetEthernetLinkStatus), "XNetGetEthernetLinkStatus");
    hookFunction(kXOnlineStartup, reinterpret_cast<const void*>(&xXOnlineStartup), "XOnlineStartup");
}

} // namespace cw::hle
