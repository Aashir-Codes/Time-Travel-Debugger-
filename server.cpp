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
const int32_t MAX_RECORD_BYTES = 1024 * 1024;

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

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
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
    ~Timeline()
    {
        TimelineNode *cur = head;
        while (cur != nullptr)
        {
            TimelineNode *next = cur->next;
            delete cur->data;
            delete cur;
            cur = next;
        }
    }
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
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
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
        string keyword = firstWord(line);

        if (keyword == "func")
        {
            if (!st.isEmpty())
            {

                Rdr.close();
                return false;
            }
            st.push(keyword);
        }
        else if (keyword == "func_end")
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
bool readResolveRecord(FILE *f, int64_t &outOffsetField, string &outText)
{
    int32_t size = 0;

    if (fread(&outOffsetField, sizeof(int64_t), 1, f) != 1)
        return false;
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
        return false;
    if (size < 0 || size > MAX_RECORD_BYTES)
        return false;

    outText.assign((size_t)size, '\0');
    if (size > 0 && fread(&outText[0], 1, (size_t)size, f) != (size_t)size)
        return false;

    return true;
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
    if (maxTokens > MAX_TOKENS)
        maxTokens = MAX_TOKENS;

    string words[MAX_TOKENS];
    int32_t n = splitWords(line, words, maxTokens);

    for (int32_t i = 0; i < n; i++)
    {
        if (i == 0)
            tokens[i].type = KEYWORD;
        else if (i == 1)
            tokens[i].type = IDENTIFIER;
        else
            tokens[i].type = PARAM;

        tokens[i].text = words[i];
    }
    return n;

    /*
    SO WE ARE GOING TO TOKENISE THE A RECORD | ROW OR A LINE OF CODE INTO THREE DATATYPES
    * Keyword like set etc
    * identifier the value which will hold the value at the end
    * Paraem the rest of the stuff is the parameter

    */
}

Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot *snap = new Snapshot;
    snap->stackDepth = callStack.snapshot_into(snap->callStack, MAX_STACK_DEPTH);
    return snap;
    // basically copies the line and pushes into the stack
}
Variable *findVariable(Frame &f, const string &name)
{
    for (int32_t i = 0; i < f.argc; i++)
    {
        if (f.argv[i].name == name)
            return &f.argv[i];
    }
    for (int32_t i = 0; i < f.localCount; i++)
    {
        if (f.locals[i].name == name)
            return &f.locals[i];
    }
    return nullptr;
}
Variable *findOrCreateLocal(Frame &f, const string &name)
{
    Variable *v = findVariable(f, name);
    if (v != nullptr)
        return v;

    if (f.localCount == MAX_VARS_PER_FRAME)
        throw std::overflow_error("Too many variables in one frame!");

    f.locals[f.localCount].name = name;
    f.locals[f.localCount].value = 0;
    f.localCount++;
    return &f.locals[f.localCount - 1];
}
bool isNumber(const string &s)
{
    if (s.empty())
        return false;

    size_t start = 0;
    if (s[0] == '-' || s[0] == '+')
        start = 1;
    if (start == s.size())
        return false;

    for (size_t i = start; i < s.size(); i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

int32_t readOperand(Frame &f, const string &text)
{
    if (isNumber(text))
        return stoi(text);

    Variable *v = findVariable(f, text);
    if (v == nullptr)
        throw std::runtime_error("Unknown variable: " + text);
    return v->value;
}
void executeSet(Frame &f, Token tokens[], int32_t n)
{
    if (n < 3)
        throw std::runtime_error("set needs a name and a value");

    int32_t value = readOperand(f, tokens[2].text);
    Variable *target = findOrCreateLocal(f, tokens[1].text);
    target->value = value;
}

void executeMath(Frame &f, Token tokens[], int32_t n)
{
    if (n < 3)
        throw std::runtime_error(tokens[0].text + " needs two operands");

    Variable *target = findVariable(f, tokens[1].text);
    if (target == nullptr)
        throw std::runtime_error("Unknown variable: " + tokens[1].text);

    int32_t rhs = readOperand(f, tokens[2].text);
    const string &op = tokens[0].text;

    if (op == "add")
        target->value = target->value + rhs;
    else if (op == "sub")
        target->value = target->value - rhs;
    else if (op == "mul")
        target->value = target->value * rhs;
    else if (op == "div")
    {
        if (rhs == 0)
            throw std::runtime_error("Division by zero");
        target->value = target->value / rhs;
    }
}
void executeCall(FILE *f, Stack<Frame> &callStack, Token tokens[], int32_t n,
                 int64_t callRecordStart, int64_t targetOffset)
{
    if (n < 2)
        throw std::runtime_error("call needs a function name");

    Frame &caller = callStack.peek();
    int32_t argCount = n - 2;

    if (fseek(f, (long)targetOffset, SEEK_SET) != 0)
        throw std::runtime_error("Cannot jump to function in resolve.bin");

    int64_t ignoredField;
    string headerText;
    if (!readResolveRecord(f, ignoredField, headerText))
        throw std::runtime_error("Corrupted function header in resolve.bin");

    Token header[MAX_TOKENS];
    int32_t hn = tokenizeLine(headerText, header, MAX_TOKENS);
    if (hn < 2 || header[0].text != "func")
        throw std::runtime_error("Call target is not a function header");

    int32_t paramCount = hn - 2;
    if (paramCount != argCount)
        throw std::runtime_error("Wrong number of arguments for " + header[1].text);

    Frame callee;
    callee.func_name = header[1].text;
    callee.argc = paramCount;
    callee.returnLine = (int32_t)callRecordStart;
    for (int32_t i = 0; i < paramCount; i++)
    {
        callee.argv[i].name = header[2 + i].text;
        callee.argv[i].value = readOperand(caller, tokens[2 + i].text);
    }

    callStack.push(callee);
}

void executeReturn(FILE *f, Stack<Frame> &callStack)
{

    Frame finished = callStack.pop();

    if (callStack.isEmpty())
        return;

    Frame &caller = callStack.peek();

    if (fseek(f, (long)finished.returnLine, SEEK_SET) != 0)
        throw std::runtime_error("Cannot jump back to caller");

    int64_t ignoredField;
    string callText;
    if (!readResolveRecord(f, ignoredField, callText))
        throw std::runtime_error("Corrupted call record in resolve.bin");

    Token tokens[MAX_TOKENS];
    tokenizeLine(callText, tokens, MAX_TOKENS);

    for (int32_t i = 0; i < finished.argc; i++)
    {
        const string &argText = tokens[2 + i].text;
        if (isNumber(argText))
            continue;
        Variable *v = findVariable(caller, argText);
        if (v != nullptr)
            v->value = finished.argv[i].value;
    }
}

bool executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE *f = fopen(resolveBinPath, "rb"); // "rb": binary mode, required on Windows
    if (!f)
    {
        cout << "Could not open resolve.bin for execution!" << endl;
        return false;
    }

    Stack<Frame> callStack;
    try
    {
        // Enter main: read its header and create its frame
        if (fseek(f, (long)mainOffset, SEEK_SET) != 0)
            throw std::runtime_error("Cannot jump to main");

        int64_t field;
        string text;
        if (!readResolveRecord(f, field, text))
            throw std::runtime_error("Corrupted main header in resolve.bin");

        Token header[MAX_TOKENS];
        int32_t hn = tokenizeLine(text, header, MAX_TOKENS);
        Frame mainFrame;
        mainFrame.func_name = "main";
        mainFrame.argc = hn > 2 ? hn - 2 : 0;
        for (int32_t i = 0; i < mainFrame.argc; i++)
            mainFrame.argv[i].name = header[2 + i].text;
        mainFrame.returnLine = -1; // nobody called main
        callStack.push(mainFrame);

        // Run until the stack is empty
        while (!callStack.isEmpty())
        {
            int64_t recordStart = ftell(f);
            if (!readResolveRecord(f, field, text))
                throw std::runtime_error("Unexpected end of resolve.bin");

            Token tokens[MAX_TOKENS];
            int32_t n = tokenizeLine(text, tokens, MAX_TOKENS);
            const string &kw = tokens[0].text;

            if (kw == "set")
                executeSet(callStack.peek(), tokens, n);
            else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div")
                executeMath(callStack.peek(), tokens, n);
            else if (kw == "call")
                executeCall(f, callStack, tokens, n, recordStart, field);
            else if (kw == "func_end")
                executeReturn(f, callStack);
            else
                throw std::runtime_error("Unknown or misplaced instruction: " + kw);

            timeline.record(buildSnapshot(callStack));
        }
    }
    catch (const std::exception &e)
    {
        cout << "Execution error: " << e.what() << endl;
        fclose(f);
        return false;
    }

    fclose(f);
    return true;
}

void writeString(FILE *f, const string &s)
{
    int32_t len = (int32_t)s.size();
    fwrite(&len, sizeof(int32_t), 1, f);
    fwrite(s.data(), 1, len, f);
}

void writeVariable(FILE *f, const Variable &v)
{
    writeString(f, v.name);
    fwrite(&v.value, sizeof(int32_t), 1, f);
}

void writeFrame(FILE *f, const Frame &fr)
{
    writeString(f, fr.func_name);
    fwrite(&fr.argc, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.argc; i++)
        writeVariable(f, fr.argv[i]);

    fwrite(&fr.returnLine, sizeof(int32_t), 1, f);
    fwrite(&fr.localCount, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.localCount; i++)
        writeVariable(f, fr.locals[i]);
}

void writeSnapshot(FILE *f, const Snapshot &s)
{
    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < s.stackDepth; i++)
        writeFrame(f, s.callStack[i]);
}
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    FILE *f = fopen(tdbgPath, "wb");
    if (!f)
        throw runtime_error("Could not create .tdbg file");

    TTDBHeader placeholder = {{'T', 'T', 'D', 'B'}, 1, 0, 0};
    writeHeader(f, placeholder);

    vector<int64_t> index;
    index.reserve(timeline.getStepCount());

    if (timeline.getStepCount() > 0)
    {
        for (TimelineNode *n = timeline.begin(); n != nullptr; n = n->next)
        {
            index.push_back((int64_t)ftell(f)); // position BEFORE writing
            writeSnapshot(f, *n->data);
        }
    }

    int64_t indexOffset = (int64_t)ftell(f);
    for (int64_t off : index)
        fwrite(&off, sizeof(int64_t), 1, f);

    TTDBHeader real = {{'T', 'T', 'D', 'B'}, 1, (int32_t)index.size(), indexOffset};
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        throw runtime_error("Could not return to header");
    }
    writeHeader(f, real);

    fclose(f);
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

    int64_t mainOffset = resolveProgram(argv[1], "resolve.bin");
    if (mainOffset < 0)
        return 1;

    Timeline timeline;
    if (!executeProgram("resolve.bin", mainOffset, timeline))
        return 1;
    try
    {
        writeTdbg(timeline, "session.tdbg");
    }
    catch (const std::exception &e)
    {
        cout << "Could not write .tdbg: " << e.what() << endl;
        return 1;
    }
    cout << "session.tdbg written" << endl;
    return 0;
}