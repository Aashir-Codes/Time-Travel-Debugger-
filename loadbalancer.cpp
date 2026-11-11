#include <iostream>
#include <string>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <thread>
#include <vector>
#include <mutex>
#include "helper.h"

using namespace std;
#ifdef _WIN32
const char *SERVER_PATH = "server.exe";
#else
const char *SERVER_PATH = "./server";
#endif
const int LB_PORT = 5000;
const int LISTEN_BACKLOG = 16;
const int MAX_PENDING_CLIENTS = 8;
const int STARTUP_MAX_ATTEMPTS = 50;
const int STARTUP_POLL_INTERVAL_MS = 100;
const int CLIENT_RECV_TIMEOUT_SEC = 10;
const int QUEUE_WAIT_LIMIT_SEC = 30;

string serverHost = "127.0.0.1";
int serverPort = 5001;

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
        Node(const T &v) : data(v), next(nullptr) {}
    };
    Node *front, *rear;
    int count;

public:
    Queue() : front(nullptr), rear(nullptr), count(0) {}
    ~Queue()
    {
        while (!isEmpty())
            dequeue();
    }
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;

    void enqueue(const T &val)
    {
        Node *n = new Node(val);
        if (rear == nullptr)
            front = rear = n;
        else
        {
            rear->next = n;
            rear = n;
        }
        count++;
    }
    T dequeue()
    {
        Node *old = front;
        T val = old->data;
        front = old->next;
        if (front == nullptr)
            rear = nullptr;
        delete old;
        count--;
        return val;
    }
    bool isEmpty() const { return count == 0; }
    int size() const { return count; }
};

struct ClientInfo
{
    sock_t sock = BAD_SOCK;
    string address;
    int port = 0;
    time_t arrivedAt = 0;
};

Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1;

bool sendError(sock_t sock, const string &message)
{
    return sendFramedMessage(sock, 1, message);
}

bool startServer(int port)
{
#ifdef _WIN32
    string command = string("start /B ") + SERVER_PATH + " " + to_string(port);
#else
    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
#endif
    return system(command.c_str()) == 0;
}

sock_t connectToServerWithWakeup()
{
    sock_t s = connectToHost(serverHost.c_str(), serverPort);
    if (s != BAD_SOCK)
        return s; // server is alive

    if (!startServer(serverPort))
        return BAD_SOCK;
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        sleepMs(STARTUP_POLL_INTERVAL_MS);
        s = connectToHost(serverHost.c_str(), serverPort);
        if (s != BAD_SOCK)
            return s;
    }
    return BAD_SOCK;
}

void relayOneClient(ClientInfo &c)
{
    sock_t serverSock = connectToServerWithWakeup();
    if (serverSock == BAD_SOCK)
    {
        sendError(c.sock, "backend server failed to start");
        return;
    }

    TrailerChecker upCheck;
    vector<char> in(IO_BUFFER_SIZE), safe(IO_BUFFER_SIZE);
    bool forwardOk = true;
    while (true)
    {
        int r = recv(c.sock, in.data(), (int)in.size(), 0);
        if (r < 0 && lastErrorWasInterrupt())
            continue;
        if (r <= 0)
            break;
        size_t n = upCheck.feed(in.data(), (size_t)r, safe.data());
        if (n > 0 && !sendAll(serverSock, safe.data(), n))
        {
            forwardOk = false;
            break;
        }
    }

    if (!forwardOk)
    {
        sendError(c.sock, "backend server failed to start");
        closeSock(serverSock);
        return;
    }
    if (!upCheck.verify())
    {
        shutdownWrite(serverSock);
        sendError(c.sock, "upload checksum mismatch");
        closeSock(serverSock);
        return;
    }

    sendAll(serverSock, (const char *)upCheck.trailer(), 4);
    shutdownWrite(serverSock);

    TrailerChecker downCheck;
    while (true)
    {
        int r = recv(serverSock, in.data(), (int)in.size(), 0);
        if (r < 0 && lastErrorWasInterrupt())
            continue;
        if (r <= 0)
            break;
        size_t n = downCheck.feed(in.data(), (size_t)r, safe.data());
        if (n > 0 && !sendAll(c.sock, safe.data(), n))
            break;
    }
    if (downCheck.verify())
        sendAll(c.sock, (const char *)downCheck.trailer(), 4);

    closeSock(serverSock);
}

void acceptLoop(sock_t listenSock)
{
    while (flag)
    {
        sockaddr_in addr;
        sock_t c = acceptClient(listenSock, &addr);
        if (c == BAD_SOCK)
        {
            if (!flag)
                break;
            continue;
        }
        setRecvTimeout(c, CLIENT_RECV_TIMEOUT_SEC);

        ClientInfo info;
        info.sock = c;
        char ip[64] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
        info.address = ip;
        info.port = ntohs(addr.sin_port);
        info.arrivedAt = time(nullptr);

        bool full;
        {
            lock_guard<mutex> lock(queueMutex);
            full = pendingClients.size() >= MAX_PENDING_CLIENTS;
            if (!full)
                pendingClients.enqueue(info);
        }

        if (full)
        {
            sendError(c, "server busy, try again later");
            shutdownWrite(c);
            setRecvTimeout(c, 1);
            char junk[256];
            while (recv(c, junk, sizeof junk, 0) > 0)
            {
            }
            closeSock(c);
        }
    }
}

void dispatcherLoop()
{
    while (flag)
    {
        ClientInfo c;
        bool haveClient = false;
        {

            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty())
            {
                c = pendingClients.dequeue();
                haveClient = true;
            }
        }
        if (!haveClient)
        {
            sleepMs(50);
            continue;
        }

        if (time(nullptr) - c.arrivedAt > QUEUE_WAIT_LIMIT_SEC)
        {

            shutdownWrite(c.sock);
            setRecvTimeout(c.sock, 1);
            char junk[256];
            while (recv(c.sock, junk, sizeof junk, 0) > 0)
            {
            }
            closeSock(c.sock);
            continue;
        }

        relayOneClient(c);
        closeSock(c.sock);
    }
}

void handler(int sg)
{
    (void)sg;
    flag = 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        cout << "Usage: ./load_balancer <server_ip> <server_port>" << endl;
        return 1;
    }
    serverHost = argv[1];
    serverPort = atoi(argv[2]);

    netInit();
#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
#else
    signal(SIGINT, handler);
#endif

    sock_t listenSock = createListenSocket(LB_PORT, LISTEN_BACKLOG);
    cout << "Load balancer on port " << LB_PORT << ", server at " << serverHost << ":" << serverPort << endl;

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    closeSock(listenSock);
    cout << "Load balancer stopped." << endl;
    return 0;
}