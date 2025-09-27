#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <algorithm>
#include <signal.h>

using namespace std;

class SigmaShell {
private:
    bool running;
    string prompt;

    struct BackgroundProcess {
        pid_t pid;
        chrono::high_resolution_clock::time_point start_time;
    };
    vector<BackgroundProcess> background_processes;  // Заменяем старый вектор

    // Разделение строки на аргументы
    static vector<string> parseCommand(const string &input, bool &background) {
        vector<string> args;
        stringstream ss(input);
        string token;
        background = false;

        while (ss >> token) {
            // Проверка на фоновый режим (должен быть последним)
            if (token == "&" && args.empty()) {
                // Только & - игнорируем
                continue;
            }
            else if (token == "&") {
                // & после команды - фоновый режим
                background = true;
                break;
            }

            // Обработка кавычек
            if (token.front() == '"' && token.back() != '"') {
                string temp = token.substr(1);
                while (ss >> token) {
                    if (token == "&") {
                        background = true;
                        break;
                    }
                    temp += " " + token;
                    if (token.back() == '"') {
                        temp.pop_back();
                        break;
                    }
                }
                args.push_back(temp);
            } else {
                // Удаление кавычек если они есть
                if (token.front() == '"' && token.back() == '"') {
                    token = token.substr(1, token.length() - 2);
                }
                args.push_back(token);
            }
        }

        return args;
    }

    // Проверка завершения фоновых процессов
// Проверка завершения фоновых процессов
    void checkBackgroundProcesses() {
        vector<size_t> completed_indices;

        for (size_t i = 0; i < background_processes.size(); ++i) {
            auto& bg_process = background_processes[i];
            int status;
            pid_t result = waitpid(bg_process.pid, &status, WNOHANG);

            if (result > 0) {
                // Процесс завершился - вычисляем время выполнения
                auto end_time = chrono::high_resolution_clock::now();
                auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - bg_process.start_time);

                cout << "\n[Фоновый процесс " << bg_process.pid << " набегался]";
                cout << " Время выполнения: " << duration.count() << " мс";

                if (WIFEXITED(status)) {
                    cout << " Код: " << WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    cout << " Сигнал: " << WTERMSIG(status);
                }
                cout << '\n' << "[" << getCurrentDirectory() << "] " << prompt;
                cout.flush();

                completed_indices.push_back(i);
            }
            else if (result == -1) {
                // Ошибка или процесс не существует
                completed_indices.push_back(i);
            }
        }

        // Удаляем завершенные процессы из списка (с конца чтобы индексы не сдвигались)
        sort(completed_indices.rbegin(), completed_indices.rend());
        for (size_t index : completed_indices) {
            background_processes.erase(background_processes.begin() + index);
        }
    }
    // Выполнение команды
    void executeCommand(const vector<string> &args, bool background) {
        if (args.empty()) return;

        // Встроенная команда выхода
        if (args[0] == "exit") {
            // Перед выходом ждем завершения всех фоновых процессов
            if (!background_processes.empty()) {
                cout << "Ща на фоне процессы докрутятся..." << '\n';
                for (auto& bg_process : background_processes) {
                    waitpid(bg_process.pid, nullptr, 0);

                    // Выводим время выполнения для завершенных процессов
                    auto end_time = chrono::high_resolution_clock::now();
                    auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - bg_process.start_time);
                    cout << "[Процесс " << bg_process.pid << "] Время выполнения: " << duration.count() << " мс" << '\n';
                }
            }
            running = false;
            cout << "Бывай старичок)" << '\n';
            return;
        }

        // Встроенная команда смены директории
        if (args[0] == "cd") {
            if (args.size() < 2) {
                cerr << "cd: ожидается аргумент" << '\n';
            } else {
                if (chdir(args[1].c_str()) != 0) {
                    perror("cd");
                }
            }
            return;
        }

        // Встроенная команда помощи
        if (args[0] == "help") {
            cout << "Доступные команды:" << '\n';
            cout << "  <команда> [аргументы] - выполнить команду" << '\n';
            cout << "  <команда> [аргументы] & - выполнить в фоне" << '\n';
            cout << "  cd <директория> - сменить директорию" << '\n';
            cout << "  exit - выход из shell" << '\n';
            cout << "  help - показать эту справку" << '\n';
            return;
        }

        // Запуск внешней команды
        launchExternalCommand(args, background);
    }

    void launchExternalCommand(const vector<string> &args, bool background) {
        auto start_time = chrono::high_resolution_clock::now();

        pid_t pid = fork();

        if (pid == -1) {
            perror("fork");
            return;
        }

        if (pid == 0) {
            // Дочерний процесс

            // Для фоновых процессов отключаем сигналы от клавиатуры
            if (background) {
                signal(SIGINT, SIG_IGN);   // Игнорировать Ctrl+C
                signal(SIGTSTP, SIG_IGN);  // Игнорировать Ctrl+Z
            }

            vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const auto &arg: args) {
                argv.emplace_back(const_cast<char *>(arg.c_str()));
            }
            argv.emplace_back(nullptr);

            execvp(argv[0], argv.data());

            // Если execvp вернул управление, значит произошла ошибка
            cerr << "Ошибка: команда '" << args[0] << "' не найдена" << '\n';
            exit(EXIT_FAILURE);
        } else {
            // Родительский процесс
            if (background) {
                // Фоновый режим - не ждем завершения
                BackgroundProcess bg_process;
                bg_process.pid = pid;
                bg_process.start_time = start_time;  // Сохраняем время старта
                background_processes.push_back(bg_process);

                cout << "[Пустили процесс на фоне погонять PID: " << pid << "]" << '\n';
                cout << "[" << getCurrentDirectory() << "] " << prompt;
                cout.flush();
            } else {
                // Обычный режим - ждем завершения
                int status;
                waitpid(pid, &status, 0);

                auto end_time = chrono::high_resolution_clock::now();
                auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - start_time);

                cout << "Время выполнения: " << duration.count() << " мс" << '\n';

                if (WIFEXITED(status)) {
                    cout << "Код завершения: " << WEXITSTATUS(status) << '\n';
                } else if (WIFSIGNALED(status)) {
                    cout << "Процесс завершен сигналом: " << WTERMSIG(status) << '\n';
                }
            }
        }
    }

    // Получение текущей директории для промпта
    static string getCurrentDirectory() {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            return string(cwd);
        }
        return "unknown";
    }

public:
    SigmaShell(): running(true), prompt("SigmaShell> ") {}

    void run() {
        string input;

        cout << "Приветствую в SigmaShell!" << '\n';
        cout << "Пиши 'help' для списка команд, 'exit' для выхода" << '\n';
        cout << "Используй '&' в конце команды для фонового выполнения" << '\n';

        while (running) {
            // Проверяем завершение фоновых процессов
            checkBackgroundProcesses();

            // Динамический промпт с текущей директорией
            cout << "[" << getCurrentDirectory() << "] " << prompt;

            if (!getline(cin, input)) {
                break; // EOF (Ctrl+D)
            }

            // Пропуск пустых строк
            if (input.empty()) {
                continue;
            }

            bool background = false;
            auto args = parseCommand(input, background);
            executeCommand(args, background);
        }

        // Завершаем все фоновые процессы при выходе
        for (auto& bg_process : background_processes) {
            kill(bg_process.pid, SIGTERM);
        }
    }
};

int main() {
    SigmaShell shell;
    shell.run();
    return 0;
}