#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <sstream>
#include <map>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

const int BUFFER_SIZE = 16384;
const int PORT = 8888;

std::atomic<bool> running(true);
SOCKET sock;
std::string myUsername;
std::string currentChat = "";
std::map<std::string, int> unreadMessages;

static inline void trimCRLF(std::string& s) {
    size_t end = s.find_last_not_of("\n\r");
    if (end == std::string::npos) {
        s.clear();
        return;
    }
    s.erase(end + 1);
}

void parseServerMessage(const std::string& msg) {
    size_t pos = msg.find('|');
    if (pos == std::string::npos) {
        std::cout << msg << std::endl;
        return;
    }

    std::string type = msg.substr(0, pos);
    std::string data = msg.substr(pos + 1);

    if (type == "WELCOME") {
        // Приветствие - не выводим
    }
    else if (type == "HELP") {
        // Справка - не выводим
    }
    else if (type == "UNREAD") {
        unreadMessages.clear();
        std::stringstream ss(data);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    std::string from = item.substr(0, colon);
                    int count = std::stoi(item.substr(colon + 1));
                    unreadMessages[from] = count;
                }
            }
        }
    }
    else if (type == "USERS") {
        std::cout << "\n=== Пользователи онлайн ===" << std::endl;
        std::stringstream ss(data);
        std::string user;
        while (std::getline(ss, user, ',')) {
            if (!user.empty() && user != myUsername) {
                if (unreadMessages[user] > 0) {
                    std::cout << "  - " << user << " (+" << unreadMessages[user] << " новых)" << std::endl;
                }
                else {
                    std::cout << "  - " << user << std::endl;
                }
            }
        }
        std::cout << "==========================" << std::endl;
        std::cout << "> " << std::flush;
    }
    else if (type == "CHAT") {
        std::cout << data << std::endl;
        if (!currentChat.empty()) {
            unreadMessages[currentChat] = 0;
        }
        std::cout << "> " << std::flush;
    }
    else if (type == "MSG") {
        size_t sep = data.find('|');
        if (sep == std::string::npos) return;

        std::string from = data.substr(0, sep);
        std::string text = data.substr(sep + 1);

        if (currentChat == from) {
            std::cout << "\r[" << from << "]: " << text << std::endl;
            std::cout << "> " << std::flush;
        }
        else {
            unreadMessages[from]++;
            std::cout << "\r[!] Новых сообщений от " << from << ": " << unreadMessages[from] << std::endl;
            std::cout << "> " << std::flush;
        }
    }
    else if (type == "HISTORY") {
        size_t sep = data.find('|');
        if (sep == std::string::npos) {
            std::cout << "> " << std::flush;
            return;
        }
        std::string chatWith = data.substr(0, sep);
        std::string history = data.substr(sep + 1);

        std::cout << "\n=== История с " << chatWith << " ===" << std::endl;

        if (history.empty()) {
            std::cout << "Нет сообщений" << std::endl;
        }
        else {
            std::stringstream ss(history);
            std::string from;
            std::string text;
            while (std::getline(ss, from, '|')) {
                if (std::getline(ss, text, '|')) {
                    std::cout << "[" << from << "]: " << text << std::endl;
                }
            }
        }
        std::cout << "======================" << std::endl;
        unreadMessages[chatWith] = 0;
        std::cout << "> " << std::flush;
    }
    else if (type == "ERROR") {
        std::cout << "[ОШИБКА] " << data << std::endl;
        std::cout << "> " << std::flush;
    }
    else if (type == "CONNECTED") {
        // Игнорируем
    }
    else if (type == "SERVER_SHUTDOWN") {
        std::cout << "\n[!!!] СЕРВЕР ОСТАНОВЛЕН [!!!" << std::endl;
        std::cout << "[!!!] Нажмите Enter для выхода..." << std::endl;
        running = false;
    }
    else {
        std::cout << data << std::endl;
        std::cout << "> " << std::flush;
    }
}

void receiveMessages() {
    char buffer[BUFFER_SIZE];
    while (running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received <= 0) {
            if (running) {
                std::cout << "\n[!] Отключено от сервера." << std::endl;
            }
            running = false;
            break;
        }

        std::string message(buffer);
        trimCRLF(message);
        if (!message.empty()) {
            parseServerMessage(message);
        }
    }
}

bool initWinsock() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Ошибка Winsock" << std::endl;
        return false;
    }
    return true;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string server_ip;

    std::cout << "=== МЕССЕНДЖЕР ===" << std::endl;
    std::cout << "Введите IP сервера (localhost или IP): ";
    std::getline(std::cin, server_ip);

    if (server_ip.empty()) {
        server_ip = "127.0.0.1";
    }

    std::cout << "Введите ваше имя: ";
    std::getline(std::cin, myUsername);

    if (myUsername.empty()) {
        std::cerr << "Ошибка: имя не может быть пустым" << std::endl;
        return 1;
    }

    system("mkdir logs 2>nul");

    if (!initWinsock()) {
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (server_ip == "localhost") {
        server_ip = "127.0.0.1";
    }

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Ошибка: неверный IP" << std::endl;
        return 1;
    }

    std::cout << "Подключение..." << std::endl;

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Ошибка: не удалось подключиться" << std::endl;
        return 1;
    }

    send(sock, myUsername.c_str(), (int)myUsername.length(), 0);

    char response[256];
    memset(response, 0, 256);
    recv(sock, response, 255, 0);

    std::cout << "\n=== КОМАНДЫ ===" << std::endl;
    std::cout << "/users - список пользователей (с количеством новых сообщений)" << std::endl;
    std::cout << "/chat Имя - начать диалог" << std::endl;
    std::cout << "/quit - выход" << std::endl;
    std::cout << "==============" << std::endl;

    std::thread receiver(receiveMessages);

    std::string input;
    while (running) {
        std::cout << "> " << std::flush;
        std::getline(std::cin, input);

        if (!running) break;

        if (input.empty()) {
            continue;
        }

        if (input == "/quit") {
            send(sock, input.c_str(), (int)input.length(), 0);
            running = false;
            break;
        }
        else if (input == "/users") {
            currentChat = "";
            send(sock, input.c_str(), (int)input.length(), 0);
        }
        else if (input.substr(0, 5) == "/chat") {
            currentChat = input.substr(6);
            send(sock, input.c_str(), (int)input.length(), 0);
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

    if (receiver.joinable()) {
        receiver.join();
    }

    closesocket(sock);
    WSACleanup();

    std::cout << "Отключено." << std::endl;

    return 0;
}