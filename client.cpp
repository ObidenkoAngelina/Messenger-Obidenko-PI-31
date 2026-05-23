#include <iostream>     // Для ввода/вывода (cout, cin, cerr)
#include <string>       // Для работы со строками (std::string)
#include <thread>       // Для создания потоков (многопоточность)
#include <atomic>       // Для атомарных переменных (потокобезопасные)
#include <cstring>      // Для работы с C-строками (memset)
#include <winsock2.h>   // Для работы с сокетами в Windows
#include <ws2tcpip.h>   // Для дополнительных функций сокетов (inet_pton и др.)

#pragma comment(lib, "ws2_32.lib")  // Подключаем библиотеку сокетов

#include "logger.h"     // Наш логгер для записи событий в файл

// Константы
const int BUFFER_SIZE = 4096;   // Размер буфера для приёма сообщений
const int PORT = 8888;          // Порт для подключения к серверу

// Глобальные переменные
std::atomic<bool> running(true);    // Флаг работы программы (атомарный для потоков)
Logger* client_logger = nullptr;    // Указатель на логгер (будет создан в main)

/**
 * Функция для приёма сообщений от сервера
 * Работает в отдельном потоке, чтобы не блокировать ввод пользователя
 * @param sock - сокет для общения с сервером
 */
void receiveMessages(SOCKET sock) {
    char buffer[BUFFER_SIZE];   // Буфер для входящих сообщений

    // Цикл работает, пока программа активна
    while (running) {
        // Очищаем буфер перед приёмом
        memset(buffer, 0, BUFFER_SIZE);

        // Принимаем сообщение от сервера
        // recv() ждёт данные, может заблокировать поток
        int bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        // Если соединение разорвано или ошибка
        if (bytes_received <= 0) {
            if (running) {
                std::cout << "\nОтключено от сервера." << std::endl;
                client_logger->info("Отключено от сервера");
            }
            running = false;  // Останавливаем программу
            break;
        }

        // Преобразуем полученные данные в строку
        std::string message(buffer);
        // Удаляем символы перевода строки в конце
        message.erase(message.find_last_not_of("\n\r") + 1);

        // Выводим сообщение, очищая текущую строку ввода
        // \r - возврат каретки, \033[K - очистка до конца строки
        std::cout << "\r\033[K" << message << std::endl;
        std::cout << "> " << std::flush;  // Выводим приглашение к вводу

        // Логируем полученное сообщение в зависимости от типа
        if (message.find("[Личное") != std::string::npos) {
            client_logger->info("Получено личное: " + message);  // Личное сообщение
        }
        else if (message.find("***") != std::string::npos) {
            client_logger->info("Системное сообщение: " + message);  // Системное (вход/выход)
        }
        else {
            client_logger->info("Получено: " + message);  // Обычное сообщение
        }
    }
}

/**
 * Инициализация Winsock (только для Windows)
 * @return true - успех, false - ошибка
 */
bool initWinsock() {
    WSADATA wsaData;
    // WSAStartup - обязательный вызов для работы с сокетами в Windows
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Ошибка инициализации Winsock" << std::endl;
        return false;
    }
    return true;
}

//Главная функция программы - точка входа

int main() {
    // Устанавливаем кодировку UTF-8 для корректного отображения русского языка
    SET_CONSOLE_UTF8();

    std::string server_ip;  // IP-адрес сервера
    std::string username;   // Имя пользователя

    // Приветствие и ввод параметров подключения
    std::cout << "=== Клиент мессенджера ===" << std::endl;
    std::cout << "Введите IP-адрес сервера (localhost или IP): ";
    std::getline(std::cin, server_ip);

    // Если пользователь ничего не ввёл, используем localhost
    if (server_ip.empty()) {
        server_ip = "127.0.0.1";
    }

    std::cout << "Введите ваше имя пользователя: ";
    std::getline(std::cin, username);

    // Проверка, что имя не пустое
    if (username.empty()) {
        std::cerr << "Имя пользователя не может быть пустым!" << std::endl;
        return 1;
    }

    // Создаём папку logs, если её нет (2>nul подавляет ошибку "папка существует")
    system("mkdir logs 2>nul");

    // Создаём логгер для этого клиента (имя файла = client_ИмяПользователя.log)
    std::string log_filename = "logs/client_" + username + ".log";
    client_logger = new Logger(log_filename);
    client_logger->info("=== КЛИЕНТ ЗАПУЩЕН ===");

    // Инициализируем Winsock
    if (!initWinsock()) {
        delete client_logger;
        return 1;
    }

    // Создаём сокет для подключения к серверу
    // AF_INET - IPv4, SOCK_STREAM - TCP (надёжная передача)
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Не удалось создать сокет" << std::endl;
        client_logger->error("Не удалось создать сокет");
        delete client_logger;
        return 1;
    }

    // Настраиваем адрес сервера
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;      // IPv4
    server_addr.sin_port = htons(PORT);    // Порт (htons - переводит в сетевой порядок байт)

    // Если ввели "localhost", заменяем на 127.0.0.1
    if (server_ip == "localhost") {
        server_ip = "127.0.0.1";
    }

    // Преобразуем IP-адрес из строки в двоичный формат
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Неверный адрес или адрес не поддерживается" << std::endl;
        client_logger->error("Неверный IP сервера: " + server_ip);
        closesocket(sock);
        delete client_logger;
        return 1;
    }

    client_logger->info("Подключение к серверу " + server_ip + ":" + std::to_string(PORT));

    // Подключаемся к серверу
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Не удалось подключиться к серверу. Убедитесь, что сервер запущен." << std::endl;
        client_logger->error("Не удалось подключиться к серверу");
        closesocket(sock);
        delete client_logger;
        return 1;
    }

    // Отправляем серверу своё имя пользователя
    send(sock, username.c_str(), (int)username.length(), 0);

    // Ждём ответ от сервера (подтверждение или ошибка)
    char response[256];
    memset(response, 0, 256);
    recv(sock, response, 255, 0);

    // Если сервер вернул ошибку (имя уже занято и т.п.)
    if (std::string(response).find("ОШИБКА") != std::string::npos) {
        std::cout << "Ошибка подключения: " << response << std::endl;
        client_logger->error("Ошибка подключения: " + std::string(response));
        closesocket(sock);
        delete client_logger;
        return 1;
    }

    // Успешное подключение
    client_logger->info("Подключено к серверу как " + username);
    std::cout << "\nПодключено к серверу как '" << username << "'!" << std::endl;
    std::cout << "Вводите сообщения. Используйте /help для списка команд." << std::endl;
    std::cout << "Команды: /users, /msg <пользователь> <сообщение>, /quit" << std::endl;
    std::cout << "\n> " << std::flush;

    // Запускаем отдельный поток для приёма сообщений
    // Теперь программа может одновременно: Принимать сообщения (в этом потоке). Ждать ввод пользователя (в главном потоке)
    std::thread receiver(receiveMessages, sock);

    // Основной цикл для отправки сообщений
    std::string input;
    while (running) {
        std::getline(std::cin, input);  // Ждём ввод пользователя

        if (!running) break;  // Если программа завершается, выходим

        if (input.empty()) {
            std::cout << "> " << std::flush;  // Пустая строка - просто новый пригласитель
            continue;
        }

        // Обработка команды выхода
        if (input == "/quit") {
            send(sock, input.c_str(), (int)input.length(), 0);  // Уведомляем сервер
            client_logger->info("Пользователь запросил отключение");
            running = false;  // Останавливаем программу
            break;
        }
        // Обработка команды помощи
        else if (input == "/help") {
            std::cout << "Доступные команды:" << std::endl;
            std::cout << "  /users - Показать список активных пользователей" << std::endl;
            std::cout << "  /msg <пользователь> <сообщение> - Отправить личное сообщение" << std::endl;
            std::cout << "  /quit - Отключиться от сервера" << std::endl;
            std::cout << "  Любой другой текст - Отправить сообщение всем пользователям" << std::endl;
            std::cout << "> " << std::flush;
            continue;
        }

        // Отправляем сообщение на сервер
        send(sock, input.c_str(), (int)input.length(), 0);
        client_logger->info("Отправлено: " + input);

        // Если это не личное сообщение (не начинается с /msg), выводим приглашение на новую строку
        if (input.substr(0, 4) != "/msg") {
            std::cout << "> " << std::flush;
        }
    }

    // Дожидаемся завершения потока приёма сообщений
    if (receiver.joinable()) {
        receiver.join();
    }

    // Закрываем сокет и очищаем Winsock
    closesocket(sock);
    WSACleanup();

    // Завершаем логгирование
    client_logger->info("=== КЛИЕНТ ОСТАНОВЛЕН ===");
    delete client_logger;
    std::cout << "Отключено." << std::endl;

    return 0;
}