#include <iostream>
#include <fstream>
#include <string>
using namespace std;

int main()
{
    ifstream in("tests/valid.bin", ios::binary);
    if (!in)
    {
        cout << "Could not open the file\n";
        return 1;
    }

    string line;
    int lineNo = 0;
    while (getline(in, line))
    {
        lineNo++;
        cout << lineNo << ": " << line << "\n";
    }
    return 0;
}