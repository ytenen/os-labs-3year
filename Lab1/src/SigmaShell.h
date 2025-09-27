#ifndef SIGMASHELL_H
#define SIGMASHELL_H

#include <string>
#include <vector>
#include <chrono>

using namespace std;

class SigmaShell {
private:
    bool running;
    string prompt;

    struct BackgroundProcess {
        pid_t pid;
        chrono::high_resolution_clock::time_point start_time;
        BackgroundProcess(pid_t p = 0,
                          chrono::high_resolution_clock::time_point st
                          = chrono::high_resolution_clock::now());
    };

    vector<BackgroundProcess> background_processes;

    // Вспомогательные методы
    static vector<string> parseCommand(const string &input, bool &background);
    void checkBackgroundProcesses();
    void executeCommand(const vector<string> &args, bool background);
    void launchExternalCommand(const vector<string> &args, bool background);
    static string getCurrentDirectory();

public:
    SigmaShell();
    void run();
};

#endif