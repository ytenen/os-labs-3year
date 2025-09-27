#include "SigmaShell.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <algorithm>
#include <csignal>
#include <fstream>

using namespace std;

SigmaShell::BackgroundProcess::BackgroundProcess(pid_t p,
                                                 chrono::high_resolution_clock::time_point st)
        : pid(p), start_time(st) {}


SigmaShell::SigmaShell() : running(true), prompt("SigmaShell> ") {}


vector<string> SigmaShell::parseCommand(const string &input, bool &background) {
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


void SigmaShell::checkBackgroundProcesses() {
    vector<size_t> completed_indices;

    for (size_t i = 0; i < background_processes.size(); ++i) {
        auto& bg_process = background_processes[i];
        int status;
        pid_t result = waitpid(bg_process.pid, &status, WNOHANG);

        if (result > 0) {
            string time_file = "/tmp/sigma_time_" + to_string(bg_process.pid);
            ifstream file(time_file);
            long exact_duration_ms = 0;

            if (file) {
                file >> exact_duration_ms;
                file.close();
                remove(time_file.c_str());
            } else {
                auto end_time = chrono::high_resolution_clock::now();
                exact_duration_ms = chrono::duration_cast<chrono::milliseconds>(
                        end_time - bg_process.start_time).count();
            }

            cout << "\n[Фоновый процесс " << bg_process.pid << " набегался]";
            cout << " Время выполнения: " << exact_duration_ms << " мс";

            if (WIFEXITED(status) && WEXITSTATUS(status) <= 255) {
                cout << " Код команды: доступен в логах";
            }
            cout << '\n' << "[" << getCurrentDirectory() << "] " << prompt;
            cout.flush();

            completed_indices.push_back(i);
        }
        else if (result == -1) {
            completed_indices.push_back(i);
        }
    }

    // Удаляем завершенные процессы
    sort(completed_indices.rbegin(), completed_indices.rend());
    for (size_t index : completed_indices) {
        background_processes.erase(background_processes.begin() + index);
    }
}


void SigmaShell::executeCommand(const vector<string> &args, bool background) {
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


void SigmaShell::launchExternalCommand(const vector<string> &args, bool background) {
    if (background) {
        // ФОНОВЫЙ РЕЖИМ - используем двойной fork для точного измерения времени
        auto start_time = chrono::high_resolution_clock::now();

        pid_t wrapper_pid = fork();

        if (wrapper_pid == -1) {
            perror("fork");
            return;
        }

        if (wrapper_pid == 0) {
            // WRAPPER-ПРОЦЕСС (внутренний) - здесь измеряем время выполнения команды

            // Игнорируем сигналы от клавиатуры для фонового процесса
            signal(SIGINT, SIG_IGN);
            signal(SIGTSTP, SIG_IGN);

            pid_t cmd_pid = fork();

            if (cmd_pid == -1) {
                perror("fork");
                exit(EXIT_FAILURE);
            }

            if (cmd_pid == 0) {
                // КОМАНДНЫЙ ПРОЦЕСС (внутренний) - выполняем реальную команду
                vector<char*> argv;
                argv.reserve(args.size() + 1);
                for (const auto &arg: args) {
                    argv.emplace_back(const_cast<char*>(arg.c_str()));
                }
                argv.emplace_back(nullptr);

                execvp(argv[0], argv.data());

                // Если дошли сюда - execvp failed
                cerr << "Ошибка: команда '" << args[0] << "' не найдена" << '\n';
                exit(EXIT_FAILURE);
            } else {
                // WRAPPER-ПРОЦЕСС - ждем завершения команды и измеряем время
                int cmd_status;
                waitpid(cmd_pid, &cmd_status, 0);

                auto end_time = chrono::high_resolution_clock::now();
                auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - start_time);

                // Записываем время в файл
                string time_file = "/tmp/sigma_time_" + to_string(getpid());
                ofstream file(time_file);
                if (file) {
                    file << duration.count();
                    file.close();
                }

                exit(0);  // Всегда нормальный выход
            }
        } else {
            // РОДИТЕЛЬСКИЙ ПРОЦЕСС (основной shell)
            BackgroundProcess bg_process;
            bg_process.pid = wrapper_pid;
            bg_process.start_time = start_time;
            background_processes.push_back(bg_process);

            cout << "[Пустили процесс на фоне погонять PID: " << wrapper_pid << "]" << '\n';
            cout << "[" << getCurrentDirectory() << "] " << prompt;
            cout.flush();
        }

    } else {
        // ОБЫЧНЫЙ РЕЖИМ (не фоновый) - оставляем оригинальную логику
        auto start_time = chrono::high_resolution_clock::now();

        pid_t pid = fork();

        if (pid == -1) {
            perror("fork");
            return;
        }

        if (pid == 0) {
            // Дочерний процесс для обычного выполнения
            vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto &arg: args) {
                argv.emplace_back(const_cast<char*>(arg.c_str()));
            }
            argv.emplace_back(nullptr);

            execvp(argv[0], argv.data());

            // Если execvp вернул управление, значит произошла ошибка
            cerr << "Ошибка: команда '" << args[0] << "' не найдена" << '\n';
            exit(EXIT_FAILURE);
        } else {
            // Родительский процесс - ждем завершения
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


string SigmaShell::getCurrentDirectory() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        return string(cwd);
    }
    return "unknown";
}


void SigmaShell:: run() {
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
