#pragma once
// helper.h - shared by server.cpp, load_balancer.cpp and client.cpp
// Works on Windows (MinGW / MSVC) and Linux. NO "using namespace std" in a header.

#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cerrno>
#include <cstdlib>

// ======================= PORTABILITY LAYER =======================
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#else
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // Windows (and macOS) do not have it; they do not raise SIGPIPE the same way
#endif

// Call once at the start of main() in every program.
inline void netInit()
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#else
    signal(SIGPIPE, SIG_IGN); // a dead peer must not kill us
#endif
}
inline void closeSock(sock_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
inline void shutdownWrite(sock_t s) // "I am done sending, but I can still read"
{
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
}
inline void sleepMs(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}
inline bool lastErrorWasInterrupt()
{
#ifdef _WIN32
    return false;
#else
    return errno == EINTR;
#endif
}
// TCP socket that is not inherited by child processes (SOCK_CLOEXEC on Linux; Windows does not need it)
inline sock_t makeTcpSocket()
{
#if defined(SOCK_CLOEXEC)
    return socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    return socket(AF_INET, SOCK_STREAM, 0);
#endif
}
// Receive timeout so a stalled peer cannot freeze us forever
inline void setRecvTimeout(sock_t s, int seconds)
{
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

// ======================= SHARED CONSTANTS =======================
const int IO_BUFFER_SIZE = 64 * 1024;

// ======================= SHARED STRUCTS =======================
struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    std::string targetFuncName;
};

// ======================= SHARED LIMITS AND SNAPSHOT TYPES =======================
// Used by server.cpp and client.cpp. The .tdbg layout depends on these, so they live here once.
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;

struct Variable
{
    std::string name;
    int32_t value;
};
struct Frame
{
    std::string func_name;
    int32_t argc = 0;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine = -1;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount = 0;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ======================= SOCKET SEND / RECEIVE =======================
// send() may send fewer bytes than asked, so loop until all n are gone.
inline bool sendAll(sock_t sock, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        int r = (int)send(sock, buf + sent, (int)(n - sent), MSG_NOSIGNAL);
        if (r <= 0)
        {
            if (r < 0 && lastErrorWasInterrupt())
                continue;
            return false;
        }
        sent += (size_t)r;
    }
    return true;
}
// recv() may return fewer bytes than asked, so loop until exactly n have arrived.
inline bool recvAll(sock_t sock, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        int r = (int)recv(sock, buf + got, (int)(n - got), 0);
        if (r <= 0)
        {
            if (r < 0 && lastErrorWasInterrupt())
                continue;
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

// ======================= CRC-32 =======================
// Keep the running register between chunks; apply the final XOR only when you finish.
const uint32_t CRC_INIT = 0xFFFFFFFFu;

inline uint32_t crc32Update(uint32_t reg, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        reg ^= (unsigned char)data[i];
        for (int b = 0; b < 8; b++)
            reg = (reg & 1u) ? ((reg >> 1) ^ 0xEDB88320u) : (reg >> 1);
    }
    return reg;
}
inline uint32_t crc32Finish(uint32_t reg) { return reg ^ 0xFFFFFFFFu; }

// The 4 checksum bytes on the wire are LITTLE-ENDIAN. Decided once, here, for all three programs.
inline void crcEncode(uint32_t crc, unsigned char out[4])
{
    out[0] = (unsigned char)(crc & 0xFF);
    out[1] = (unsigned char)((crc >> 8) & 0xFF);
    out[2] = (unsigned char)((crc >> 16) & 0xFF);
    out[3] = (unsigned char)((crc >> 24) & 0xFF);
}
inline uint32_t crcDecode(const unsigned char in[4])
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

// ======================= TrailerChecker =======================
// Message on the wire:  [status 1][body ...][CRC 4]
// The receiver cannot know which 4 bytes are the CRC until the stream ends,
// so it always holds back the newest 4 bytes.
class TrailerChecker
{
    uint32_t reg;          // CRC register over everything already released
    unsigned char held[4]; // the newest 4 bytes, kept back
    size_t heldCount;      // how many of those 4 are filled

public:
    TrailerChecker() : reg(CRC_INIT), heldCount(0) { std::memset(held, 0, sizeof held); }

    // Give it a chunk (in, n). It copies the bytes that are now SAFE to use into `out`
    // and returns how many. `out` must be a different buffer, at least n bytes big.
    // The very first released byte of a message is the status byte.
    size_t feed(const char *in, size_t n, char *out)
    {
        size_t total = heldCount + n;
        if (total <= 4) // still inside the reserve, release nothing
        {
            std::memcpy(held + heldCount, in, n);
            heldCount = total;
            return 0;
        }
        size_t release = total - 4;
        for (size_t i = 0; i < release; i++) // held bytes come first, then the new chunk
            out[i] = (i < heldCount) ? (char)held[i] : in[i - heldCount];
        reg = crc32Update(reg, out, release);

        unsigned char nh[4]; // the last 4 of (held + in) become the new reserve
        for (size_t j = 0; j < 4; j++)
        {
            size_t idx = release + j;
            nh[j] = (idx < heldCount) ? held[idx] : (unsigned char)in[idx - heldCount];
        }
        std::memcpy(held, nh, 4);
        heldCount = 4;
        return release;
    }
    // True only if 4 bytes are held AND they equal the CRC of everything released.
    bool verify() const
    {
        return heldCount == 4 && crcDecode(held) == crc32Finish(reg);
    }
    // The held 4 bytes. The load balancer forwards them ONLY if verify() is true.
    const unsigned char *trailer() const { return held; }
};
// ======================= ADDED FOR PHASE 2 =======================
// Paste this at the END of helper.h.
// ALSO add  #include <cstdlib>  to the include list at the top of helper.h.

// Word splitting, moved here from server.cpp because the client needs it too.
inline bool isWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline int32_t splitWords(const std::string &line, std::string out[], int32_t maxOut)
{
    int32_t count = 0;
    size_t pos = 0;
    while (count < maxOut)
    {
        while (pos < line.size() && isWhitespace(line[pos]))
            pos++;
        if (pos == line.size())
            break;
        size_t start = pos;
        while (pos < line.size() && !isWhitespace(line[pos]))
            pos++;
        out[count] = line.substr(start, pos - start);
        count++;
    }
    return count;
}

// Streaming writer for one framed message: [status][body...][CRC 4].
// Call write() for each piece, then finish() once. CRC covers everything written.
class FrameWriter
{
    sock_t sock;
    uint32_t reg;

public:
    explicit FrameWriter(sock_t s) : sock(s), reg(CRC_INIT) {}

    bool write(const char *data, size_t n)
    {
        if (n == 0)
            return true;
        reg = crc32Update(reg, data, n);
        return sendAll(sock, data, n);
    }
    bool finish()
    {
        unsigned char t[4];
        crcEncode(crc32Finish(reg), t);
        return sendAll(sock, (const char *)t, 4);
    }
};

// A whole short message in one call: used for every error reply.
inline bool sendFramedMessage(sock_t sock, char status, const std::string &body)
{
    FrameWriter w(sock);
    char st = status;
    return w.write(&st, 1) && w.write(body.data(), body.size()) && w.finish();
}

// Listening socket on any address. Exits the program if it cannot be created.
inline sock_t createListenSocket(int port, int backlog)
{
    sock_t s = makeTcpSocket();
    if (s == BAD_SOCK)
    {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        exit(1);
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);

    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (sockaddr *)&addr, sizeof addr) < 0)
    {
        fprintf(stderr, "bind() to port %d failed: %s\n", port, strerror(errno));
        exit(1);
    }
    if (listen(s, backlog) < 0)
    {
        fprintf(stderr, "listen() failed: %s\n", strerror(errno));
        exit(1);
    }
    return s;
}

// Connect to host:port. Returns BAD_SOCK on failure (and closes the socket, so nothing leaks).
inline sock_t connectToHost(const char *host, int port)
{
    sock_t s = makeTcpSocket();
    if (s == BAD_SOCK)
        return BAD_SOCK;
    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        connect(s, (sockaddr *)&addr, sizeof addr) < 0)
    {
        closeSock(s);
        return BAD_SOCK;
    }
    return s;
}

// accept() a client so that child processes (the server, started with system()) do NOT
// inherit the client socket. Without this, the server keeps the client connection open
// and the client never sees the end of the reply. peer may be nullptr.
inline sock_t acceptClient(sock_t listenSock, sockaddr_in *peer)
{
    socklen_t len = sizeof(sockaddr_in);
    sockaddr *addr = (sockaddr *)peer;
    socklen_t *lenp = peer ? &len : nullptr;
#ifdef __linux__
    return accept4(listenSock, addr, lenp, SOCK_CLOEXEC);
#else
    return accept(listenSock, addr, lenp);
#endif
}