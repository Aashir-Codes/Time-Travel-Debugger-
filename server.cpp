/*
    I want this project to compile on Windows, Linux and Mac Os
    so I will add the libraries manually
*/

// ------------------------------- STANDARD LIBRARIES -----------------------------

#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstdio>
#include <exception>

//----------------------------------- Windows only ---------------------------------
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else // Mac and Linux
#include <unistd.h>
#include <sys/socket.h>
#endif

using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;              // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                  //  apply as SO_RCVTIMEO so a deadclient can't hang the server forever

template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;

        Node(const T &val) : data(val), next(nullptr) {}
    };
    Node *top;
    int32_t count;

public:
    Stack() : top(nullptr), count(0) {}
    void push(const T &val)
    {
        if (count == MAX_STACK_DEPTH)
        {
            throw(std::overflow_error("Maximum deapth Reached!"));
        }
        Node *newNode = new Node(val);
        count++;
        if (top == nullptr)
        {
            top = newNode;
            newNode->next = nullptr;
            return;
        }

        newNode->next = top;
        top = newNode;
    }
    T pop()
    {
        if (isEmpty())
        {
            throw(std::underflow_error("Stack is Empty!"));
        }

        Node *temp = top;
        top = top->next;
        T val = temp->data;
        delete temp;
        count--;
        return val;
    }
    T &peek()
    {
        if (count == 0)
        {
            throw(std::underflow_error("Stack is Empty!"));
        }

        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t i = 0;
        Node *cur = top;
        while (cur != nullptr && i < maxLen)
        {
            out[i++] = cur->data;
            cur = cur->next;
        }
        return i;
    }
};

struct Snapshot;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;

    TimelineNode(Snapshot *_data)
    {
        data = _data;
        next = prev = nullptr;
    }
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        TimelineNode *newerecord = new TimelineNode(s);
        if (head == nullptr)
        {
            head = tail = newerecord;
            stepCount++;
            return;
        }

        newerecord->prev = tail;
        tail->next = newerecord;
        tail = newerecord;
        stepCount++;
    }
    TimelineNode *begin()
    {
        if (head == nullptr)
        {
            throw(std::underflow_error("Timeline List is Empty"));
        }
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
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
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
}

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

bool readSourceLine(ifstream &in, string &out)
{
    if (!in)
        return false;

    string line;
    while (getline(in, line))
    {

        if (!line.empty())
        {
            out = line;
            return true;
        }
    }
    return false;
}
string firstWord(const string &line)
{

    string ans = "";

    for (auto ch : line)
    {
        if (ch == ' ')
        {
            return ans;
        }

        ans += ch;
    }

    return line;
}

string secondWord(const string &line)
{
    size_t first = line.find(' ');
    if (first == string::npos)
        return "";
    size_t st = first + 1;
    size_t second = line.find(' ', st);

    if (second == string::npos)
        return line.substr(st);

    return line.substr(st, second - st);
}
bool validateProgram(const char *sourcePath)
{

    ifstream Rdr(sourcePath);
    if (!Rdr)
    {
        std::cout << "File not opened! " << std::endl;
        return false;
    }
    Stack<string> st;
    string line;
    while (getline(Rdr, line))
    {
        string KEYWORD = firstWord(line);

        if (KEYWORD == "func")
        {
            if (!st.isEmpty())
            {

                Rdr.close();
                return false;
            }
            st.push(KEYWORD);
        }
        else if (KEYWORD == "func_end")
        {
            if (st.isEmpty())
            {

                Rdr.close();
                return false;
            }
            st.pop();
        }
    }

    Rdr.close();

    return st.isEmpty();
}

int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("/home/aashir/Time-Travel-Debugger-/tests/invalid_stray_end.bin"))
    {
        cout << "File is invalid!" << endl;
        return 1;
    }
    cout << "file is valid" << endl;

    //     int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    //
    //     Timeline timeline;
    //     executeProgram("resolve.bin", mainOffset, timeline);
    //
    //     writeTdbg(timeline, "session.tdbg");

    return 0;
}