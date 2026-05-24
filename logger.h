#ifndef LOGGER_H
#define LOGGER_H

// Подключаем необходимые заголовочные файлы
#include <iostream>    // ДОБАВИТЬ: для std::cerr
#include <string>      // Для работы со строками (std::string)
#include <fstream>     // Для работы с файлами (std::ofstream)
#include <mutex>       // Для мьютексов
#include <chrono>      // Для работы со временем
#include <iomanip>     // Для форматирования вывода
#include <sstream>     // Для строковых потоков
#include <windows.h>   // Для Windows-specific функций

#define SET_CONSOLE_UTF8() SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8)

// Класс для логирования событий в файл
class Logger {
private:
    std::string filename;
    std::ofstream file;
    std::mutex mtx;

    std::string getCurrentTime() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm;
        localtime_s(&tm, &time_t);

        std::stringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
            << "." << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

public:
    Logger(const std::string& fname) : filename(fname) {
        file.open(filename, std::ios::app);
        if (!file.is_open()) {
            std::cerr << "Не удалось открыть файл лога: " << filename << std::endl;
        }
    }

    ~Logger() {
        if (file.is_open()) {
            file.close();
        }
    }

    void log(const std::string& level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mtx);
        if (file.is_open()) {
            file << "[" << getCurrentTime() << "] [" << level << "] " << message << std::endl;
            file.flush();
        }
    }

    void info(const std::string& message) { log("INFO", message); }
    void error(const std::string& message) { log("ERROR", message); }
    void warning(const std::string& message) { log("WARNING", message); }
};

#endif // LOGGER_H