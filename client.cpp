#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <sstream>
#include <map>

#pragma comment(lib, "ws2_32.lib")

const int BUFFER_SIZE = 16384;  // Размер буфера для приёма сообщений
const int PORT = 8888;          // Порт для подключения к серверу

std::atomic<bool> running(true);    // Флаг работы программы (потокобезопасный)
SOCKET sock = INVALID_SOCKET;       // Сокет для связи с сервером
std::string myUsername;             // Имя текущего пользователя
std::string currentChat = "";       // С кем сейчас ведётся диалог (пусто - не в диалоге)
std::map<std::string, int> unreadMessages;  // Счётчик непрочитанных сообщений: [отправитель] = количество

/**
 * Удаляет символы перевода строки (\n, \r) в конце строки
 */
static inline void trimCRLF(std::string& s) {
    size_t end = s.find_last_not_of("\n\r");
    if (end == std::string::npos) { s.clear(); return; }
    s.erase(end + 1);
}

/**
 * Удаляет пробелы в начале и конце строки
 */
static inline void trimSpaces(std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t");
    s = s.substr(start, end - start + 1);
}

/**
 * Разбирает и обрабатывает сообщения от сервера
 * Формат сообщений: "ТИП|Данные"
 */
void parseServerMessage(const std::string& msg) {
    size_t pos = msg.find('|');
    if (pos == std::string::npos) {
        // Сообщение без разделителя - просто выводим
        std::cout << msg << std::endl;
        return;
    }

    std::string type = msg.substr(0, pos);
    std::string data = msg.substr(pos + 1);

    // Обработка непрочитанных сообщений
    if (type == "UNREAD") {
        unreadMessages.clear();
        std::stringstream ss(data);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (item.empty()) continue;
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            std::string from = item.substr(0, colon);
            int count = 0;
            try { count = std::stoi(item.substr(colon + 1)); }
            catch (...) { count = 0; }
            unreadMessages[from] = count;
        }
    }
    // Обработка списка всех пользователей
    else if (type == "ALL_USERS") {
        std::cout << "\n=== ВСЕ ПОЛЬЗОВАТЕЛИ ===" << std::endl;
        std::stringstream ss(data);
        std::string user;
        while (std::getline(ss, user, ',')) {
            if (!user.empty() && user != myUsername) {
                int c = 0;
                auto it = unreadMessages.find(user);
                if (it != unreadMessages.end()) c = it->second;
                if (c > 0) std::cout << "  - " << user << " (+" << c << " новых)" << std::endl;
                else std::cout << "  - " << user << std::endl;
            }
        }
        std::cout << "=======================" << std::endl;
        std::cout << "> " << std::flush;
    }
    // Обработка списка онлайн пользователей
    else if (type == "ONLINE_USERS") {
        std::cout << "\n=== ПОЛЬЗОВАТЕЛИ ОНЛАЙН ===" << std::endl;
        std::stringstream ss(data);
        std::string user;
        while (std::getline(ss, user, ',')) {
            if (!user.empty() && user != myUsername) {
                int c = 0;
                auto it = unreadMessages.find(user);
                if (it != unreadMessages.end()) c = it->second;
                if (c > 0) std::cout << "  - " << user << " (+" << c << " новых)" << std::endl;
                else std::cout << "  - " << user << std::endl;
            }
        }
        std::cout << "=========================" << std::endl;
        std::cout << "> " << std::flush;
    }
    // Обработка начала диалога
    else if (type == "CHAT") {
        std::cout << data << std::endl;
        if (!currentChat.empty()) unreadMessages[currentChat] = 0;
        std::cout << "> " << std::flush;
    }
    // Обработка нового сообщения
    else if (type == "MSG") {
        size_t sep = data.find('|');
        if (sep == std::string::npos) return;
        std::string from = data.substr(0, sep);
        std::string text = data.substr(sep + 1);

        if (currentChat == from) {
            // Если мы в диалоге с отправителем - показываем сообщение сразу
            std::cout << "\r[" << from << "]: " << text << std::endl;
            unreadMessages[from] = 0;
        }
        else {
            // Если не в диалоге - увеличиваем счётчик и показываем уведомление
            unreadMessages[from]++;
            std::cout << "\r[!] Новых сообщений от " << from << ": " << unreadMessages[from] << std::endl;
        }
        std::cout << "> " << std::flush;
    }
    // Обработка истории переписки
    else if (type == "HISTORY") {
        size_t sep = data.find('|');
        if (sep == std::string::npos) {
            std::cout << "> " << std::flush;
            return;
        }
        std::string chatWith = data.substr(0, sep);
        std::string history = data.substr(sep + 1);

        std::cout << "\n=== ИСТОРИЯ С " << chatWith << " ===" << std::endl;
        if (history.empty()) {
            std::cout << "Нет сообщений" << std::endl;
        }
        else {
            // Формат: отправитель|текст|отправитель|текст|...
            std::stringstream ss(history);
            std::string from, text;
            while (std::getline(ss, from, '|')) {
                if (std::getline(ss, text, '|')) {
                    if (from == myUsername) {
                        std::cout << "[Я]: " << text << std::endl;
                    }
                    else {
                        std::cout << "[" << from << "]: " << text << std::endl;
                    }
                }
            }
        }
        std::cout << "======================" << std::endl;
        unreadMessages[chatWith] = 0;
        std::cout << "> " << std::flush;
    }
    // Обработка ошибок
    else if (type == "ERROR") {
        std::cout << "[ОШИБКА] " << data << std::endl;
        std::cout << "> " << std::flush;
    }
    // Обработка выключения сервера
    else if (type == "SERVER_SHUTDOWN") {
        std::cout << "\n[!!!] СЕРВЕР ОСТАНОВЛЕН [!!!]" << std::endl;
        running = false;
    }
    // Всё остальное выводим как есть
    else {
        std::cout << data << std::endl;
        std::cout << "> " << std::flush;
    }
}

/**
 * Функция приёма сообщений от сервера (работает в отдельном потоке)
 */
void receiveMessages() {
    char buffer[BUFFER_SIZE];
    while (running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            if (running) std::cout << "\n[!] Отключено от сервера." << std::endl;
            running = false;
            break;
        }
        std::string message(buffer);
        trimCRLF(message);
        if (!message.empty()) parseServerMessage(message);
    }
}

/**
 * Инициализация Winsock (обязательно для Windows)
 */
bool initWinsock() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Ошибка Winsock" << std::endl;
        return false;
    }
    return true;
}

/**
 * Главная функция - точка входа
 */
int main() {
    // Устанавливаем кодировку UTF-8 для консоли Windows
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string server_ip;

    std::cout << "=== МЕССЕНДЖЕР (Windows) ===" << std::endl;
    std::cout << "Введите IP сервера (localhost или IP): ";
    std::getline(std::cin, server_ip);
    if (server_ip.empty()) server_ip = "127.0.0.1";
    if (server_ip == "localhost") server_ip = "127.0.0.1";

    std::cout << "Введите ваше имя: ";
    std::getline(std::cin, myUsername);
    if (myUsername.empty()) {
        std::cerr << "Ошибка: имя не может быть пустым" << std::endl;
        return 1;
    }

    // Создаём папку для логов
    system("mkdir logs 2>nul");

    if (!initWinsock()) return 1;

    // Создаём сокет
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        WSACleanup();
        return 1;
    }

    // Настраиваем адрес сервера
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Ошибка: неверный IP" << std::endl;
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "Подключение..." << std::endl;

    // Подключаемся к серверу
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Ошибка: не удалось подключиться" << std::endl;
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Отправляем имя пользователя
    send(sock, myUsername.c_str(), (int)myUsername.length(), 0);

    // Ждём ответ сервера (игнорируем, это просто подтверждение)
    char response[256]{};
    recv(sock, response, 255, 0);

    // Выводим список команд
    std::cout << "\n=== КОМАНДЫ ===" << std::endl;
    std::cout << "/online_users - список онлайн пользователей" << std::endl;
    std::cout << "/all_users - список ВСЕХ пользователей" << std::endl;
    std::cout << "/chat Имя - начать диалог" << std::endl;
    std::cout << "/quit - выход" << std::endl;
    std::cout << "==============" << std::endl;

    // Запускаем поток для приёма сообщений
    std::thread receiver(receiveMessages);

    // Основной цикл отправки сообщений
    std::string input;
    while (running) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, input)) break;
        if (!running) break;
        if (input.empty()) continue;

        if (input == "/quit") {
            send(sock, input.c_str(), (int)input.length(), 0);
            running = false;
            break;
        }
        else if (input == "/online_users") {
            currentChat = "";
            send(sock, input.c_str(), (int)input.length(), 0);
        }
        else if (input == "/all_users") {
            currentChat = "";
            send(sock, input.c_str(), (int)input.length(), 0);
        }
        else if (input.rfind("/chat", 0) == 0) {
            std::string who = (input.size() > 6) ? input.substr(6) : "";
            trimSpaces(who);
            currentChat = who;
            send(sock, ("/chat " + who).c_str(), (int)(6 + who.size()), 0);
        }
        else {
            if (currentChat.empty()) {
                std::cout << "[ОШИБКА] Вы не в диалоге. Используйте /chat Имя" << std::endl;
            }
            else {
                send(sock, input.c_str(), (int)input.length(), 0);
            }
        }
    }

    if (receiver.joinable()) receiver.join();
    closesocket(sock);
    WSACleanup();
    std::cout << "Отключено." << std::endl;
    return 0;
}