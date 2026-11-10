#pragma once
// helper.h - shared by server.cpp, load_balancer.cpp and client.cpp
// Works on Windows (MinGW / MSVC) and Linux. NO "using namespace std" in a header.

#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cerrno>

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