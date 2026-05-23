#define _WINSOCK_DEPRECATED_NO_WARNINGS  // Отключаем предупреждения об устаревших функциях (inet_ntoa и др.)

// Подключаем необходимые заголовочные файлы
#include <iostream>     // Для ввода/вывода (cout, cerr)
#include <string>       // Для работы со строками (std::string)
#include <thread>       // Для создания потоков (каждый клиент в своём потоке)
#include <vector>       // Для хранения списка клиентов (динамический массив)
#include <map>          // Для ассоциативных массивов (имя -> сокет, имя -> счетчик)
#include <mutex>        // Для мьютексов (синхронизация потоков)
#include <algorithm>    // Для алгоритмов (std::find_if, std::remove_if)
#include <cstring>      // Для работы с C-строками (memset)
#include <sstream>      // Для строковых потоков (формирование сообщений)
#include <fstream>      // Для работы с файлами (логирование)
#include <iomanip>      // Для форматирования вывода (std::setw, std::setfill)
#include <chrono>       // Для работы со временем (std::chrono)
#include <ctime>        // Для работы с временем (std::time_t)
#include <csignal>      // Для обработки сигналов (Ctrl+C)
#include <winsock2.h>   // Для работы с сокетами в Windows
#include <ws2tcpip.h>   // Для дополнительных функций сокетов (inet_pton, inet_ntop)

#pragma comment(lib, "ws2_32.lib")  // Подключаем библиотеку сокетов

// Константы
const int PORT = 8888;              // Порт, на котором сервер слушает подключения
const int BUFFER_SIZE = 16384;      // Размер буфера для приёма сообщений (16 КБ)

// Глобальные переменные для логирования
std::ofstream server_log;           // Файловый поток для лога сервера
std::mutex log_mutex;               // Мьютекс для защиты файла лога
bool server_running = true;         // Флаг работы сервера
SOCKET server_socket_global;        // Глобальный сокет сервера (для обработки сигналов)

/**
 * Записывает сообщение в файл лога сервера с временной меткой
 * @param message - текст сообщения для записи
 */
void writeServerLog(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);  // Блокируем мьютекс для потокобезопасности
    if (server_log.is_open()) {
        // Получаем текущее время
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &time_t);  // Разбираем время на компоненты

        // Форматируем время в строку "ЧЧ:ММ:СС"
        std::stringstream ss;
        ss << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_sec;

        // Записываем в файл: [время] сообщение
        server_log << "[" << ss.str() << "] " << message << std::endl;
        server_log.flush();  // Принудительно сохраняем на диск
    }
}

/**
 * Сохраняет сообщение в личный файл лога пользователя
 * @param username - имя пользователя
 * @param from - отправитель
 * @param to - получатель
 * @param text - текст сообщения
 * @param time - время отправки
 */
void saveMessageToFile(const std::string& username, const std::string& from, const std::string& to, const std::string& text, const std::string& time) {
    std::string filename = "logs/chat_" + username + ".txt";  // Файл: chat_ИмяПользователя.txt
    std::ofstream file(filename, std::ios::app);  // Открываем в режиме добавления
    if (file.is_open()) {
        if (from == username) {
            // Если пользователь отправитель - помечаем исходящее сообщение
            file << "[" << time << "] [-> " << to << "]: " << text << std::endl;
        }
        else {
            // Если пользователь получатель - помечаем входящее сообщение
            file << "[" << time << "] [" << from << " ->]: " << text << std::endl;
        }
        file.close();
    }
}

/**
 * Структура для хранения информации о подключённом клиенте
 */
struct Client {
    SOCKET socket;          // Сокет клиента (канал связи)
    std::string username;   // Имя пользователя
    std::string currentChat; // С кем сейчас ведёт диалог
    std::string ip;         // IP-адрес клиента
};

/**
 * Структура для хранения сообщения
 */
struct Message {
    std::string from;       // Отправитель
    std::string to;         // Получатель
    std::string text;       // Текст сообщения
    std::string time;       // Время отправки
    bool isRead;            // Прочитано ли сообщение
};

// Глобальные данные для работы с клиентами и сообщениями
std::vector<Client> clients;                                    // Список всех клиентов
std::map<std::string, SOCKET> user_sockets;                     // Соответствие: имя -> сокет
std::map<std::string, std::vector<Message>> messages;           // Сообщения по пользователям
std::map<std::string, std::map<std::string, int>> unreadCounts; // Счетчики непрочитанных: [получатель][отправитель]
std::mutex clients_mutex;                                       // Мьютекс для защиты списка клиентов

/**
 * Получает текущее время в отформатированном виде
 * @return строка с временем в формате "ЧЧ:ММ:СС"
 */
std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time_t);
    std::stringstream ss;
    ss << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_sec;
    return ss.str();
}

/**
 * Отправляет сообщение конкретному клиенту
 * @param client_socket - сокет клиента
 * @param message - текст сообщения
 * @return true - успешно, false - ошибка
 */
bool sendToClient(SOCKET client_socket, const std::string& message) {
    int sent = send(client_socket, message.c_str(), (int)message.length(), 0);
    return sent != SOCKET_ERROR;
}

/**
 * Отправляет клиенту список непрочитанных сообщений
 * @param client_socket - сокет клиента
 * @param username - имя пользователя
 */
void sendUnreadCounts(SOCKET client_socket, const std::string& username) {
    std::stringstream ss;
    ss << "UNREAD|";  // Тип сообщения - UNREAD
    for (const auto& pair : unreadCounts[username]) {
        if (pair.second > 0) {
            ss << pair.first << ":" << pair.second << ",";  // Формат: "Анна:3,Борис:1"
        }
    }
    sendToClient(client_socket, ss.str() + "\n");
}

/**
 * Отправляет историю переписки с указанным пользователем
 * @param client_socket - сокет клиента
 * @param username - имя пользователя
 * @param with - с кем нужна история
 */
void sendChatHistory(SOCKET client_socket, const std::string& username, const std::string& with) {
    // Обнуляем счетчик непрочитанных для этого отправителя
    unreadCounts[username][with] = 0;

    std::stringstream ss;
    ss << "HISTORY|" << with << "|";  // Тип сообщения - HISTORY

    // Собираем все сообщения между username и with
    for (const auto& msg : messages[username]) {
        if ((msg.from == with || msg.to == with)) {
            ss << msg.from << "|" << msg.text << "|";  // Формат: "отправитель|текст|"
        }
    }

    sendToClient(client_socket, ss.str() + "\n");
}

/**
 * Удаляет клиента из списков и логирует отключение
 * @param username - имя пользователя
 * @param ip - IP-адрес
 */
void removeClient(const std::string& username, const std::string& ip) {
    std::lock_guard<std::mutex> lock(clients_mutex);

    auto it = std::find_if(clients.begin(), clients.end(),
        [&username](const Client& c) { return c.username == username; });

    if (it != clients.end()) {
        clients.erase(it);          // Удаляем из вектора клиентов
        user_sockets.erase(username); // Удаляем из карты сокетов

        // Логируем отключение
        std::string logMsg = "ОТКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + ip;
        std::cout << logMsg << std::endl;
        writeServerLog(logMsg);
    }
}

/**
 * Функция обработки клиента (работает в отдельном потоке для КАЖДОГО клиента)
 * @param client - структура с информацией о клиенте
 */
void handleClient(Client client) {
    char buffer[BUFFER_SIZE];
    std::string username = client.username;

    // Логируем подключение
    std::string logMsg = "ПОДКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + client.ip;
    std::cout << logMsg << std::endl;
    writeServerLog(logMsg);

    // Отправляем приветственные сообщения
    sendUnreadCounts(client.socket, username);  // Отправляем счетчики непрочитанных

    // Основной цикл обработки сообщений от клиента
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client.socket, buffer, BUFFER_SIZE - 1, 0);

        // Если клиент отключился или произошла ошибка
        if (bytes_received <= 0) {
            removeClient(username, client.ip);
            break;
        }

        std::string message(buffer);
        message.erase(message.find_last_not_of("\n\r") + 1);  // Удаляем символы перевода строки

        // --- ОБРАБОТКА КОМАНД ---

        // Команда выхода
        if (message == "/quit") {
            removeClient(username, client.ip);
            break;
        }
        // Команда получения списка пользователей
        else if (message == "/users") {
            std::stringstream ss;
            ss << "USERS|";
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (const auto& c : clients) {
                if (c.username != username) {
                    ss << c.username << ",";
                }
            }
            sendToClient(client.socket, ss.str() + "\n");
        }
        // Команда начала диалога /chat Имя
        else if (message.substr(0, 5) == "/chat") {
            std::string target = message.substr(6);
            if (!target.empty()) {
                bool userExists = false;
                {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    userExists = (user_sockets.find(target) != user_sockets.end());
                }

                if (!userExists) {
                    sendToClient(client.socket, "ERROR|Пользователь " + target + " не найден\n");
                }
                else {
                    client.currentChat = target;  // Запоминаем собеседника
                    sendToClient(client.socket, "CHAT|Теперь вы общаетесь с " + target + "\n");
                    sendChatHistory(client.socket, username, target);  // Отправляем историю
                }
            }
        }
        // Обычное сообщение (не команда) - отправляем текущему собеседнику
        else {
            if (!client.currentChat.empty()) {
                // Проверяем, что собеседник всё ещё в сети
                bool recipientExists = false;
                {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    recipientExists = (user_sockets.find(client.currentChat) != user_sockets.end());
                }

                if (!recipientExists) {
                    sendToClient(client.socket, "ERROR|Пользователь " + client.currentChat + " больше не в сети\n");
                    client.currentChat = "";
                }
                else {
                    std::string time = getCurrentTime();

                    // Создаём структуру сообщения
                    Message msg;
                    msg.from = username;
                    msg.to = client.currentChat;
                    msg.text = message;
                    msg.time = time;
                    msg.isRead = false;

                    // Сохраняем сообщение для обоих участников
                    messages[username].push_back(msg);
                    messages[client.currentChat].push_back(msg);

                    // Увеличиваем счетчик непрочитанных для получателя
                    unreadCounts[client.currentChat][username]++;

                    // Сохраняем в файлы логов
                    saveMessageToFile(username, username, client.currentChat, message, time);
                    saveMessageToFile(client.currentChat, username, client.currentChat, message, time);

                    // Логируем на сервере факт отправки сообщения
                    std::string logMsg = "СООБЩЕНИЕ | От: " + username + " | Кому: " + client.currentChat + " | Текст: " + message;
                    writeServerLog(logMsg);

                    // Отправляем сообщение получателю
                    std::string sendMsg = "MSG|" + username + "|" + message;
                    auto it = user_sockets.find(client.currentChat);
                    if (it != user_sockets.end()) {
                        sendToClient(it->second, sendMsg + "\n");
                        sendUnreadCounts(it->second, client.currentChat);  // Обновляем счетчики
                    }
                }
            }
            else {
                sendToClient(client.socket, "ERROR|Вы не в диалоге. Используйте /chat Имя\n");
            }
        }
    }

    closesocket(client.socket);  // Закрываем сокет клиента
}

/**
 * Инициализация Winsock (обязательно для Windows)
 * @return true - успех, false - ошибка
 */
bool initWinsock() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
    return true;
}

/**
 * Обработчик сигнала Ctrl+C
 * @param signum - номер сигнала (SIGINT)
 */
void signalHandler(int signum) {
    std::cout << "\n[!] Остановка сервера..." << std::endl;
    std::string stopTime = getCurrentTime();
    std::cout << "[!] Сервер остановлен в " << stopTime << std::endl;

    // Записываем в лог остановку сервера
    writeServerLog("========== СЕРВЕР ОСТАНАВЛИВАЕТСЯ ========== | Время: " + stopTime);
    writeServerLog("========== СЕРВЕР ОСТАНОВЛЕН ========== | Время: " + stopTime);

    // Закрываем файл лога
    if (server_log.is_open()) {
        server_log.close();
    }

    // Закрываем сокет сервера
    if (server_socket_global != INVALID_SOCKET) {
        closesocket(server_socket_global);
    }
    WSACleanup();  // Очищаем Winsock

    exit(0);  // Завершаем программу
}

/**
 * Главная функция - точка входа в программу
 */
int main() {
    // Устанавливаем кодировку UTF-8 для корректного отображения русского языка
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Устанавливаем обработчик сигнала Ctrl+C
    signal(SIGINT, signalHandler);

    // Создаём папку для логов
    system("mkdir logs 2>nul");

    // Открываем файл лога сервера в режиме добавления
    server_log.open("logs/server.log", std::ios::app);

    // Выводим информацию о запуске
    std::string startTime = getCurrentTime();
    std::cout << "=== Сервер мессенджера ===" << std::endl;
    std::cout << "Запуск на порту " << PORT << "..." << std::endl;
    std::cout << "[!] Сервер запущен в " << startTime << std::endl;
    std::cout << "[!] Для остановки нажмите Ctrl+C" << std::endl;

    // Записываем запуск в лог
    writeServerLog("========== СЕРВЕР ЗАПУЩЕН ========== | Время: " + startTime);

    // Инициализируем Winsock
    if (!initWinsock()) {
        std::cerr << "Ошибка Winsock" << std::endl;
        writeServerLog("ОШИБКА | Не удалось инициализировать Winsock");
        server_log.close();
        return 1;
    }

    // --- СОЗДАНИЕ СОКЕТА ---
    server_socket_global = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_global == INVALID_SOCKET) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        writeServerLog("ОШИБКА | Не удалось создать сокет");
        server_log.close();
        return 1;
    }

    // --- НАСТРОЙКА АДРЕСА СЕРВЕРА ---
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;              // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;      // Слушаем все сетевые интерфейсы
    server_addr.sin_port = htons(PORT);            // Порт (htons - перевод в сетевой порядок байт)

    // --- ПРИВЯЗКА СОКЕТА К АДРЕСУ ---
    if (bind(server_socket_global, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Ошибка привязки" << std::endl;
        writeServerLog("ОШИБКА | Не удалось привязать сокет");
        server_log.close();
        return 1;
    }

    // --- НАЧАЛО ПРОСЛУШИВАНИЯ ---
    if (listen(server_socket_global, 10) == SOCKET_ERROR) {
        std::cerr << "Ошибка прослушивания" << std::endl;
        writeServerLog("ОШИБКА | Не удалось начать прослушивание");
        server_log.close();
        return 1;
    }

    std::cout << "Сервер запущен. Ожидание подключений..." << std::endl;
    writeServerLog("СЕРВЕР ЗАПУЩЕН | Порт: " + std::to_string(PORT));

    // --- ОСНОВНОЙ ЦИКЛ СЕРВЕРА (ПРИЁМ ПОДКЛЮЧЕНИЙ) ---
    while (server_running) {
        sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);

        // accept() - блокирующий вызов, ждёт новое подключение
        SOCKET client_socket = accept(server_socket_global, (struct sockaddr*)&client_addr, &addr_len);

        if (client_socket == INVALID_SOCKET) {
            continue;
        }

        // Получаем IP-адрес клиента
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        std::string ip_address(client_ip);

        // Получаем имя пользователя от клиента
        char username_buffer[256];
        memset(username_buffer, 0, 256);
        recv(client_socket, username_buffer, 255, 0);

        std::string username(username_buffer);
        username.erase(username.find_last_not_of("\n\r") + 1);

        // Проверяем, не занято ли имя
        bool name_taken = false;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (user_sockets.find(username) != user_sockets.end()) {
                name_taken = true;
            }
        }

        if (name_taken) {
            sendToClient(client_socket, "ERROR|Имя уже занято\n");
            writeServerLog("ОШИБКА | Попытка подключения с занятым именем: " + username + " | IP: " + ip_address);
            closesocket(client_socket);
            continue;
        }

        // Создаём структуру клиента
        Client new_client;
        new_client.socket = client_socket;
        new_client.username = username;
        new_client.currentChat = "";
        new_client.ip = ip_address;

        // Добавляем клиента в списки
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(new_client);
            user_sockets[username] = client_socket;
        }

        // Подтверждаем успешное подключение
        sendToClient(client_socket, "CONNECTED\n");

        // Запускаем поток для обработки клиента
        std::thread client_thread(handleClient, new_client);
        client_thread.detach();  // detach() - поток будет работать автономно
    }

    // --- ЗАВЕРШЕНИЕ РАБОТЫ (СЮДА НЕ ДОХОДИТ ПРИ НОРМАЛЬНОЙ РАБОТЕ) ---
    closesocket(server_socket_global);
    WSACleanup();

    writeServerLog("========== СЕРВЕР ОСТАНОВЛЕН ========== | Время: " + getCurrentTime());
    server_log.close();

    return 0;
}