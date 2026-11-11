// ======================= TIME-TRAVEL DEBUGGER - CLIENT =======================
// Build:  g++ -std=c++17 -Wall -Wextra client.cpp -o client
// Run:    ./client <lb_ip> <lb_port>        e.g.  ./client 127.0.0.1 5000
//
// The network is used ONLY by "start". After that, the .tdbg file is on disk and
// next / prv / jmp read it locally.

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include "helper.h"

using namespace std;

// ---- Constants ----
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg";           // the session currently in use
const char *TDBG_PART_FILE = "ttdb_last.tdbg.part"; // the download in progress
const int32_t MAX_JUMP = 1000000000;
const size_t MAX_ERROR_TEXT = 64 * 1024;  // cap on an error message from the server
const int32_t MAX_SNAPSHOT_STRING = 4096; // sanity limit when reading names
const int REPLY_TIMEOUT_SEC = 60;         // the queue wait is at most 30 s on the balancer
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// ---- Structs only the client needs (Variable, Frame, Snapshot, TTDBHeader are in helper.h) ----
struct TdbgReader
{
    FILE *file = nullptr; // the open .tdbg file
    TTDBHeader header{};
    vector<int64_t> index; // byte offset of every snapshot
};
struct DebugSession
{
    bool active = false; // false until a "start" has worked
    TdbgReader reader;
    int64_t currentStep = 0; // 0-based
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

// Send the source file as [status 0][source][CRC], then shut down our write side.
bool uploadSource(sock_t sock, const char *sourcePath)
{
    FILE *f = fopen(sourcePath, "rb");
    if (!f)
        return false;

    FrameWriter w(sock);
    char status = 0;
    bool ok = w.write(&status, 1);
    vector<char> buf(IO_BUFFER_SIZE);
    size_t n;
    while (ok && (n = fread(buf.data(), 1, buf.size(), f)) > 0)
        ok = w.write(buf.data(), n);
    fclose(f);

    ok = ok && w.finish();
    shutdownWrite(sock); // "I am done sending" (we can still read the reply)
    return ok;
}

// Read [status][payload][CRC]. Status 0: payload goes to outTdbgPath. Status 1: payload is the error text.
bool receiveResponse(sock_t sock, const char *outTdbgPath, string &outError)
{
    TrailerChecker check;
    vector<char> in(IO_BUFFER_SIZE), safe(IO_BUFFER_SIZE);
    bool gotStatus = false;
    char status = 0;
    FILE *out = nullptr;
    bool writeFailed = false;
    string errorText;

    while (true)
    {
        int r = recv(sock, in.data(), (int)in.size(), 0);
        if (r < 0 && lastErrorWasInterrupt())
            continue;
        if (r <= 0)
            break;

        size_t n = check.feed(in.data(), (size_t)r, safe.data());
        size_t from = 0;
        if (n > 0 && !gotStatus)
        {
            gotStatus = true;
            status = safe[0]; // the first released byte is the status
            from = 1;
            if (status == 0)
            {
                out = fopen(outTdbgPath, "wb");
                if (!out)
                    writeFailed = true;
            }
        }
        size_t body = n - from;
        if (body == 0)
            continue;
        if (status == 0 && out)
        {
            if (fwrite(safe.data() + from, 1, body, out) != body)
                writeFailed = true;
        }
        else if (status == 1 && errorText.size() < MAX_ERROR_TEXT)
        {
            errorText.append(safe.data() + from, body);
        }
    }
    if (out)
        fclose(out);

    if (!gotStatus)
    {
        outError = "no reply (server busy, or you waited too long in the queue)";
        return false;
    }
    if (!check.verify())
    {
        remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == 1)
    {
        outError = errorText;
        return false;
    }
    if (status != 0 || writeFailed)
    {
        remove(outTdbgPath);
        outError = "could not save the downloaded .tdbg file";
        return false;
    }
    return true;
}

// Whole download: connect, upload, receive into outTdbgPath.
bool requestDebugFile(const string &host, int port, const char *sourcePath,
                      const char *outTdbgPath, string &outError)
{
    FILE *probe = fopen(sourcePath, "rb"); // check before we bother the server
    if (!probe)
    {
        outError = string("could not open ") + sourcePath;
        return false;
    }
    fclose(probe);

    sock_t sock = connectToHost(host.c_str(), port);
    if (sock == BAD_SOCK)
    {
        outError = "could not reach the load balancer at " + host + ":" + to_string(port);
        return false;
    }
    setRecvTimeout(sock, REPLY_TIMEOUT_SEC);

    bool ok = uploadSource(sock, sourcePath);
    if (!ok)
        outError = "could not send the source file";
    else
        ok = receiveResponse(sock, outTdbgPath, outError);

    closeSock(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// Layout: header (20 bytes) | snapshot 0 | ... | snapshot N-1 | index (8 bytes per snapshot)
// =============================================================================================

void closeTdbg(TdbgReader &r);

// Read the header field by field: 4 + 4 + 4 + 8 = 20 bytes. Never sizeof(TTDBHeader): that is 24 in memory.
bool openTdbg(const char *path, TdbgReader &r)
{
    closeTdbg(r);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    TTDBHeader h;
    bool good = fread(h.magic, 1, 4, f) == 4 &&
                fread(&h.version, sizeof(int32_t), 1, f) == 1 &&
                fread(&h.stepCount, sizeof(int32_t), 1, f) == 1 &&
                fread(&h.indexOffset, sizeof(int64_t), 1, f) == 1;

    if (good)
    {
        fseek(f, 0, SEEK_END);
        int64_t fileSize = (int64_t)ftell(f);
        good = memcmp(h.magic, "TTDB", 4) == 0 &&
               h.version == 1 &&
               h.stepCount >= 0 &&
               h.indexOffset >= 20 &&
               h.indexOffset <= fileSize &&
               (fileSize - h.indexOffset) == 8 * (int64_t)h.stepCount;
    }
    if (good)
    {
        r.index.resize((size_t)h.stepCount);
        fseek(f, (long)h.indexOffset, SEEK_SET);
        if (h.stepCount > 0 && fread(r.index.data(), sizeof(int64_t), (size_t)h.stepCount, f) != (size_t)h.stepCount)
            good = false;
        // Every snapshot must start inside the snapshot area, and the offsets must increase.
        for (size_t i = 0; good && i < r.index.size(); i++)
        {
            if (r.index[i] < 20 || r.index[i] >= h.indexOffset)
                good = false;
            if (i > 0 && r.index[i] <= r.index[i - 1])
                good = false;
        }
    }
    if (!good)
    {
        fclose(f);
        vector<int64_t>().swap(r.index);
        return false;
    }

    r.file = f;
    r.header = h;
    return true;
}

void closeTdbg(TdbgReader &r)
{
    if (r.file)
        fclose(r.file);
    r.file = nullptr;
    vector<int64_t>().swap(r.index); // swap with an empty vector frees the memory
}

// Read helpers that mirror writeString / writeVariable / writeFrame / writeSnapshot in server.cpp.
static bool readI32(FILE *f, int32_t &v)
{
    return fread(&v, sizeof(int32_t), 1, f) == 1;
}
static bool readString(FILE *f, string &s)
{
    int32_t len;
    if (!readI32(f, len) || len < 0 || len > MAX_SNAPSHOT_STRING)
        return false;
    s.resize((size_t)len);
    return len == 0 || fread(&s[0], 1, (size_t)len, f) == (size_t)len;
}
static bool readVariable(FILE *f, Variable &v)
{
    return readString(f, v.name) && readI32(f, v.value);
}
static bool readFrame(FILE *f, Frame &fr)
{
    if (!readString(f, fr.func_name))
        return false;
    if (!readI32(f, fr.argc) || fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.argc; i++)
        if (!readVariable(f, fr.argv[i]))
            return false;
    if (!readI32(f, fr.returnLine))
        return false;
    if (!readI32(f, fr.localCount) || fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.localCount; i++)
        if (!readVariable(f, fr.locals[i]))
            return false;
    return true;
}

// Rebuild one snapshot. Returns nullptr if the step is out of range or the data is damaged.
// The caller owns the returned pointer.
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (!r.file || step < 0 || (size_t)step >= r.index.size())
        return nullptr;
    if (fseek(r.file, (long)r.index[(size_t)step], SEEK_SET) != 0)
        return nullptr;

    unique_ptr<Snapshot> snap(new Snapshot());
    int32_t depth;
    if (!readI32(r.file, depth) || depth < 0 || depth > MAX_STACK_DEPTH)
        return nullptr;
    for (int32_t i = 0; i < depth; i++)
        if (!readFrame(r.file, snap->callStack[i]))
            return nullptr;
    snap->stackDepth = depth;
    return snap.release();
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active = false;
    s.currentStep = 0;
    s.reader = TdbgReader();
}

void closeSession(DebugSession &s)
{
    if (s.active)
        closeTdbg(s.reader);
    s.active = false;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s);

bool showCurrent(DebugSession &s)
{
    unique_ptr<Snapshot> snap(readSnapshotAt(s.reader, (int32_t)s.currentStep));
    if (!snap)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    return true;
}

bool moveBy(DebugSession &s, int64_t delta)
{
    if (!s.active)
    {
        cout << "No program loaded yet. Run: start <file.bin>" << endl;
        return false;
    }
    int64_t total = s.reader.header.stepCount;
    int64_t target = s.currentStep + delta;
    if (target < 0 || target >= total)
    {
        cout << "You are at step " << s.currentStep + 1 << " of " << total
             << ". Valid steps are 1 to " << total << "." << endl;
        return false;
    }
    s.currentStep = target;
    return showCurrent(s);
}

bool cmdStart(DebugSession &s, const string &sourcePath)
{
    string err;
    if (!requestDebugFile(LB_HOST, LB_PORT, sourcePath.c_str(), TDBG_PART_FILE, err))
    {
        cout << err << endl;
        return false;
    }

    TdbgReader fresh;
    if (!openTdbg(TDBG_PART_FILE, fresh))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        remove(TDBG_PART_FILE);
        return false;
    }
    closeTdbg(fresh);
    closeSession(s);
    remove(TDBG_FILE);
    if (rename(TDBG_PART_FILE, TDBG_FILE) != 0 || !openTdbg(TDBG_FILE, s.reader))
    {
        cout << "could not save the downloaded session" << endl;
        remove(TDBG_PART_FILE);
        return false;
    }
    s.active = true;
    s.currentStep = 0;
    return showCurrent(s);
}

bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -(int64_t)n); }

static void printVars(const Variable *vars, int32_t count)
{
    for (int32_t i = 0; i < count; i++)
    {
        if (i > 0)
            cout << ", ";
        cout << vars[i].name << "=" << vars[i].value;
    }
}

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "step " << step << " of " << totalSteps << endl;
    if (s.stackDepth == 0)
    {
        cout << "  program finished" << endl;
        return;
    }
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame &fr = s.callStack[i];
        cout << (i == 0 ? "  -> " : "     ") << fr.func_name << "(";
        printVars(fr.argv, fr.argc);
        cout << ")";
        if (fr.localCount > 0)
        {
            cout << "  locals: ";
            printVars(fr.locals, fr.localCount);
        }
        cout << endl;
    }
}

void printHelp()
{
    cout << "Commands:" << endl;
    cout << "  start <file.bin>     send a program to the server and load its debug session" << endl;
    cout << "  next                 go forward one step" << endl;
    cout << "  prv                  go back one step" << endl;
    cout << "  jmp next <n>         go forward n steps" << endl;
    cout << "  jmp prv <n>          go back n steps" << endl;
    cout << "  help                 show this list" << endl;
    cout << "  quit                 exit" << endl;
}

static bool parseJump(const string &text, int32_t &out)
{
    if (text.empty())
        return false;
    char *end = nullptr;
    errno = 0;
    long v = strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE)
        return false;
    if (v < 1 || v > MAX_JUMP)
        return false;
    out = (int32_t)v;
    return true;
}

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    const int32_t MAX_WORDS = 5;
    string w[MAX_WORDS + 1];
    int32_t n = splitWords(line, w, MAX_WORDS + 1);
    if (n == 0)
        return false;

    int32_t first = (w[0] == "ttdb") ? 1 : 0;
    int32_t count = n - first;

    auto unknown = [&]() -> bool
    {
        cout << "unknown command" << endl;
        printHelp();
        return false;
    };
    if (n > MAX_WORDS || count <= 0)
        return unknown();

    const string &cmd = w[first];
    if (cmd == "start")
    {
        if (count != 2)
            return unknown();
        return cmdStart(s, w[first + 1]);
    }
    if (cmd == "next")
        return count == 1 ? cmdNext(s) : unknown();
    if (cmd == "prv")
        return count == 1 ? cmdPrv(s) : unknown();
    if (cmd == "jmp")
    {
        if (count != 3)
            return unknown();
        int32_t value;
        if (!parseJump(w[first + 2], value))
        {
            cout << "jmp needs a whole number from 1 to " << MAX_JUMP << endl;
            return false;
        }
        if (w[first + 1] == "next")
            return cmdJmpNext(s, value);
        if (w[first + 1] == "prv")
            return cmdJmpPrv(s, value);
        return unknown();
    }
    if (cmd == "help")
    {
        if (count != 1)
            return unknown();
        printHelp();
        return true;
    }
    if (cmd == "quit" || cmd == "exit")
    {
        if (count != 1)
            return unknown();
        quitRequested = true;
        return true;
    }
    return unknown();
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        cout << "Usage: ./client <lb_ip> <lb_port>" << endl;
        return 1;
    }
    LB_HOST = argv[1];
    LB_PORT = atoi(argv[2]);
    if (LB_PORT <= 0)
    {
        cout << "lb_port must be a positive number" << endl;
        return 1;
    }
    netInit();

    DebugSession session;
    initSession(session);

    cout << "Time-Travel Debugger client. Type help for the list of commands." << endl;
    printHelp();

    bool quitRequested = false;
    string line;
    while (!quitRequested)
    {
        cout << PROMPT << flush;
        if (!getline(cin, line))
            break;                                // end of input
        runCommand(session, line, quitRequested); // errors never leave the loop
    }

    closeSession(session);
    return 0;
}