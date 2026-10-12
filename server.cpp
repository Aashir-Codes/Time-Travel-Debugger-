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
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;

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
    ~Stack()
    {
        while (!isEmpty())
        {
            pop();
        }
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

struct FuncEntry // BAsically  a dictionery with name and at what offset it is located
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
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (isSkippable(line))
            continue;
        if (!line.empty())
        {
            out = line;
            return true;
        }
    }
    return false;
}

bool isWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int32_t splitWords(const string &line, string out[], int32_t maxOut)
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

bool isSkippable(const string &line)
{
    string w[1];
    int32_t n = splitWords(line, w, 1);
    return n == 0 || w[0].rfind("//", 0) == 0;
}

string firstWord(const string &line)
{
    string words[1];
    if (splitWords(line, words, 1) == 1)
        return words[0];
    return "";
}

string secondWord(const string &line)
{
    string words[2];
    if (splitWords(line, words, 2) == 2)
        return words[1];
    return "";
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
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (isSkippable(line))
            continue;
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
    int64_t RecordStart = ftell(f);
    int32_t size = text.size();

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, size, f);
    return RecordStart;

    /*

     * SO BASICALLY THE FORMULA WAS TO WRITE THE  8 BYTES AT CURRENT OFFSET (BASICALLY LIKE
         THE VTABLE IN POLYMORPHISM WE STORED THE ADDRESS IN THE FUNCTION ARRAY WE ARE STORING THE ADDRESS OF EACH FUNCTION)
         SO THAT WE CAN PATCH THE CALLS OF THAT SPECIFIC FUCNTION WITH THE ADDRESS OF THAT FUNCTION
     + 4 BYTES AT SIZE AND THE RAW TEXT WITHOUT THE \n null character

    */
}

int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t OF; // OFFSET FIELD
    int32_t size;

    fread(&OF, sizeof(int64_t), 1, f);
    fread(&size, sizeof(int32_t), 1, f);

    char *buffer = new char[size + 1];
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    outText = string(buffer);
    delete[] buffer;
    /*
    SO what is this function doing in sane words it is a function that is taking a line(empty line ig) and the file which is already opened
    it first reads the offset value where exactly is the address this is written
    then thesize pf the char [] without the \0
    then it eeads that  into the string with \0 returs the address of where it was written in the file
    */
    return OF;
}

int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    ifstream src(sourcePath);
    if (!src)
    {
        cout << "Could not open source file!" << endl;
        return -1;
    }
    FILE *out = fopen(resolveBinPath, "wb"); // we are reading it in wb so it can be opened in windows and linux both because windows silently corrrupt the file on reading
    if (!out)
    {
        cout << "Could not open resolve.bin!" << endl;
        return -1;
    }

    FuncEntry funcArray[MAX_FUNCS]; // Using the constraints that were provided as global vars
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES]; // Max numeber of patches
    int32_t patchCount = 0;

    int64_t currentOffset = 0;
    string line;

    while (getline(src, line))
    {
        if (!line.empty() && line.back() == '\r') // for windows cause getline ignores \r
            line.pop_back();
        if (isSkippable(line))
            continue;
        string kw = firstWord(line);

        if (kw == "func")
        {
            string name = secondWord(line);
            if (name.empty() || funcCount == MAX_FUNCS)
            {
                cout << "Bad or too many functions" << endl;
                fclose(out);
                return -1;
            }

            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = currentOffset;
            for (int32_t j = 0; j < funcCount; j++)
            {
                if (funcArray[j].funcName == name)
                {
                    cout << "Duplicate function: " << name << endl;
                    fclose(out);
                    return -1;
                }
            }
            funcCount++;
        }
        else if (kw == "call")
        {
            if (patchCount == MAX_PATCHES)
            {
                cout << "Too many call instructions" << endl;
                fclose(out);
                return -1;
            }
            patches[patchCount].byteOffsetOfOffsetField = currentOffset;
            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
        }

        writeResolveRecord(out, currentOffset, line);
        currentOffset += 8 + 4 + (int64_t)line.size();
    }

    src.close();

    for (int32_t i = 0; i < patchCount; i++)
    {

        int64_t targetOffset = -1;
        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetOffset == -1)
        {
            cout << "Undefined function: " << patches[i].targetFuncName << endl;
            fclose(out);
            return -1;
        }

        fseek(out, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, out);
    }

    int64_t mainOffset = -1;
    for (int32_t i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }
    }

    fclose(out);

    if (mainOffset == -1)
    {
        cout << "No main function found!" << endl;
        return -1;
    }

    return mainOffset;
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
/*
Instead of Hard Coding PATH I AM GOING TO TAKE PATH AT COMPILE TIME AND ALSO THIS RESEMBLES THE KWARG, ARGS CONCEPT IN PYTHON BTW
*/
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        cout << "Usage: ttdb <source.bin>" << endl;
        return 1;
    }
    if (!validateProgram(argv[1]))
    {
        cout << "File is invalid!" << endl;
        return 1;
    }
    cout << "file is valid" << endl;
    return 0;
}