#ifndef LOGGER_H
#define LOGGER_H

// Подключаем необходимые заголовочные файлы
#include <string>      // Для работы со строками (std::string)
#include <fstream>     // Для работы с файлами (std::ofstream - запись в файл)
#include <mutex>       // Для мьютексов (синхронизация потоков, чтобы логи не перемешивались)
#include <chrono>      // Для работы со временем (std::chrono - точное время)
#include <iomanip>     // Для форматирования вывода (std::put_time, std::setfill, std::setw)
#include <sstream>     // Для строковых потоков (формирование строки с датой/временем)
#include <windows.h>   // Для Windows-specific функций (SetConsoleOutputCP, SetConsoleCP)

#define SET_CONSOLE_UTF8() SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8)

//Класс для логирования событий в файл
class Logger {
private:
    std::string filename;      // Имя файла лога (путь к файлу)
    std::ofstream file;        // Файловый поток для записи
    std::mutex mtx;            // Мьютекс для потокобезопасности (защита от одновременной записи)

    //Получает текущее время в отформатированном виде
    std::string getCurrentTime() {
        // Получаем текущий момент времени
        auto now = std::chrono::system_clock::now();

        // Преобразуем в time_t (стандартный тип для времени в C++)
        auto time_t = std::chrono::system_clock::to_time_t(now);

        // Получаем миллисекунды (берём остаток от деления на 1000)
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // Структура для хранения разобранного времени (день, час, минута и т.д.)
        std::tm tm;
        // localtime_s - потокобезопасная версия localtime (разбивает timestamp на компоненты)
        localtime_s(&tm, &time_t);

        // Формируем строку с датой и временем
        std::stringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")  // ГГГГ-ММ-ДД ЧЧ:ММ:СС
            << "." << std::setfill('0') << std::setw(3) << ms.count();  // Добавляем миллисекунды
        return ss.str();
    }

public:
    //Конструктор - открывает файл лога для записи
    Logger(const std::string& fname) : filename(fname) {
        file.open(filename, std::ios::app);  // Открываем файл в режиме добавления
        if (!file.is_open()) {
            // Если не удалось открыть файл, выводим сообщение об ошибке
            std::cerr << "Не удалось открыть файл лога: " << filename << std::endl;
        }
    }

    /**
     * Деструктор - закрывает файл лога
     * Автоматически вызывается при уничтожении объекта
     */
    ~Logger() {
        if (file.is_open()) {
            file.close();  // Закрываем файл, сохраняя все буферизированные данные
        }
    }

    //Основная функция записи в лог
    void log(const std::string& level, const std::string& message) {
        // std::lock_guard - автоматическая блокировка/разблокировка мьютекса
        // При входе в блок мьютекс блокируется, при выходе (конец области видимости) - разблокируется
        std::lock_guard<std::mutex> lock(mtx);

        if (file.is_open()) {
            // Записываем в файл: [время] [уровень] сообщение
            file << "[" << getCurrentTime() << "] [" << level << "] " << message << std::endl;
            file.flush();  // Принудительно сбрасываем буфер на диск (чтобы лог сразу сохранился)
        }
    }

    /**
     * Логирование информационного сообщения (INFO)
     * Используется для обычных событий: подключения, отключения, обычные сообщения
     */
    void info(const std::string& message) { log("INFO", message); }

    /**
     * Логирование ошибки (ERROR)
     * Используется для критических ошибок: не удалось создать сокет, не удалось подключиться
     */
    void error(const std::string& message) { log("ERROR", message); }

    /**
     * Логирование предупреждения (WARNING)
     * Используется для не критичных проблем: пользователь не найден для личного сообщения
     */
    void warning(const std::string& message) { log("WARNING", message); }
};

#endif // LOGGER_H