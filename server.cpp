#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <csignal>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

const int PORT = 8888;
const int BUFFER_SIZE = 16384;

std::mutex log_mutex;
std::mutex clients_mutex;
bool server_running = true;
SOCKET server_socket_global = INVALID_SOCKET;

struct Client {
    SOCKET socket = INVALID_SOCKET;
    std::string username;
    std::string currentChat;
    std::string ip;
};

struct Message {
    std::string from, to, text, time;
};

std::vector<Client> clients;
std::map<std::string, SOCKET> user_sockets;
std::map<std::string, std::vector<Message>> messages;
std::map<std::string, std::map<std::string, int>> unreadCounts;

// --- UTF-8 конвертация ---
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring ws;
    ws.resize(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], wlen);
    return ws;
}

static void appendWideLineToFile(const std::wstring& path, const std::wstring& line) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"a+, ccs=UTF-8");
    if (!f) {
        _wfopen_s(&f, path.c_str(), L"a+");
        if (!f) return;
    }
    fputws(line.c_str(), f);
    fputws(L"\n", f);
    fclose(f);
}

static std::string sanitizeForFilenameUtf8(const std::string& usernameUtf8) {
    std::string out;
    out.reserve(usernameUtf8.size());
    for (unsigned char ch : usernameUtf8) {
        if (ch < 32) continue;
        switch (ch) {
        case '\\': case '/': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
            out.push_back('_'); break;
        default:
            out.push_back((char)ch); break;
        }
    }
    if (out.empty()) out = "user";
    return out;
}

std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::stringstream ss;
    ss << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_sec;
    return ss.str();
}

bool sendToClient(SOCKET client_socket, const std::string& message) {
    return send(client_socket, message.c_str(), (int)message.length(), 0) != SOCKET_ERROR;
}

void sendUnreadCounts(SOCKET client_socket, const std::string& username) {
    std::stringstream ss;
    ss << "UNREAD|";
    for (const auto& pair : unreadCounts[username]) {
        if (pair.second > 0) {
            ss << pair.first << ":" << pair.second << ",";
        }
    }
    sendToClient(client_socket, ss.str() + "\n");
}

void sendChatHistory(SOCKET client_socket, const std::string& username, const std::string& with) {
    unreadCounts[username][with] = 0;

    std::stringstream ss;
    ss << "HISTORY|" << with << "|";
    for (const auto& msg : messages[username]) {
        if (msg.from == with || msg.to == with) {
            ss << msg.from << "|" << msg.text << "|";
        }
    }
    sendToClient(client_socket, ss.str() + "\n");
    sendUnreadCounts(client_socket, username);
}

static void setClientCurrentChatLocked(const std::string& username, const std::string& target) {
    for (auto& c : clients) {
        if (c.username == username) {
            c.currentChat = target;
            return;
        }
    }
}

static std::string getClientCurrentChatLocked(const std::string& username) {
    for (const auto& c : clients) {
        if (c.username == username) return c.currentChat;
    }
    return "";
}

void writeServerLog(const std::string& messageUtf8) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::wstring wtime = utf8ToWide(getCurrentTime());
    std::wstring wmsg = utf8ToWide(messageUtf8);
    appendWideLineToFile(L"logs\\server.log", L"[" + wtime + L"] " + wmsg);
}

void saveMessageToFile(const std::string& usernameUtf8,
    const std::string& fromUtf8,
    const std::string& toUtf8,
    const std::string& textUtf8,
    const std::string& timeUtf8) {
    std::string safeUser = sanitizeForFilenameUtf8(usernameUtf8);
    std::wstring path = L"logs\\chat_" + utf8ToWide(safeUser) + L".txt";

    std::wstring wtime = utf8ToWide(timeUtf8);
    std::wstring wfrom = utf8ToWide(fromUtf8);
    std::wstring wto = utf8ToWide(toUtf8);
    std::wstring wtext = utf8ToWide(textUtf8);

    std::wstring line;
    if (fromUtf8 == usernameUtf8)
        line = L"[" + wtime + L"] [-> " + wto + L"]: " + wtext;
    else
        line = L"[" + wtime + L"] [" + wfrom + L" ->]: " + wtext;

    appendWideLineToFile(path, line);
}

void notifyAllClientsServerShutdown() {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const auto& client : clients) {
        sendToClient(client.socket, "SERVER_SHUTDOWN\n");
    }
}

void removeClient(const std::string& username, const std::string& ip) {
    std::lock_guard<std::mutex> lock(clients_mutex);

    auto it = std::find_if(clients.begin(), clients.end(),
        [&username](const Client& c) { return c.username == username; });

    if (it != clients.end()) {
        clients.erase(it);
        user_sockets.erase(username);
        writeServerLog("ОТКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + ip);
    }
}

void handleClient(Client clientCopy) {
    char buffer[BUFFER_SIZE];
    std::string username = clientCopy.username;

    writeServerLog("ПОДКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + clientCopy.ip);
    sendUnreadCounts(clientCopy.socket, username);

    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(clientCopy.socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            removeClient(username, clientCopy.ip);
            break;
        }

        std::string message(buffer);
        message.erase(message.find_last_not_of("\n\r") + 1);
        if (message.empty()) continue;

        if (message == "/quit") {
            removeClient(username, clientCopy.ip);
            break;
        }
        else if (message == "/users") {
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                setClientCurrentChatLocked(username, "");
            }

            std::stringstream ss;
            ss << "USERS|";
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (const auto& c : clients) {
                    if (c.username != username) {
                        ss << c.username << ",";
                    }
                }
            }
            sendToClient(clientCopy.socket, ss.str() + "\n");
            sendUnreadCounts(clientCopy.socket, username);
        }
        else if (message.substr(0, 5) == "/chat") {
            std::string target = (message.size() > 6) ? message.substr(6) : "";

            target.erase(0, target.find_first_not_of(" \t"));
            target.erase(target.find_last_not_of(" \t") + 1);

            if (target.empty()) {
                sendToClient(clientCopy.socket, "ERROR|Используйте /chat Имя\n");
                continue;
            }

            bool userExists = false;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                userExists = (user_sockets.find(target) != user_sockets.end());
            }

            if (!userExists) {
                sendToClient(clientCopy.socket, "ERROR|Пользователь " + target + " не найден\n");
            }
            else {
                {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    setClientCurrentChatLocked(username, target);
                }
                sendToClient(clientCopy.socket, "CHAT|Теперь вы общаетесь с " + target + "\n");
                sendChatHistory(clientCopy.socket, username, target);
            }
        }
        else {
            std::string chatWith;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                chatWith = getClientCurrentChatLocked(username);
            }

            if (chatWith.empty()) {
                sendToClient(clientCopy.socket, "ERROR|Вы не в диалоге. Используйте /chat Имя\n");
                continue;
            }

            SOCKET recipientSocket = INVALID_SOCKET;
            std::string recipientCurrentChat;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                auto it = user_sockets.find(chatWith);
                if (it != user_sockets.end()) {
                    recipientSocket = it->second;
                    recipientCurrentChat = getClientCurrentChatLocked(chatWith);
                }
            }

            if (recipientSocket == INVALID_SOCKET) {
                sendToClient(clientCopy.socket, "ERROR|Пользователь " + chatWith + " больше не в сети\n");
                std::lock_guard<std::mutex> lock(clients_mutex);
                setClientCurrentChatLocked(username, "");
                continue;
            }

            std::string time = getCurrentTime();

            Message msg{ username, chatWith, message, time };
            messages[username].push_back(msg);
            messages[chatWith].push_back(msg);

            bool recipientIsReadingNow = (recipientCurrentChat == username);
            if (!recipientIsReadingNow) {
                unreadCounts[chatWith][username]++;
            }
            else {
                unreadCounts[chatWith][username] = 0;
            }

            saveMessageToFile(username, username, chatWith, message, time);
            saveMessageToFile(chatWith, username, chatWith, message, time);

            writeServerLog("СООБЩЕНИЕ | От: " + username + " | Кому: " + chatWith + " | Текст: " + message);

            sendToClient(recipientSocket, "MSG|" + username + "|" + message + "\n");
            sendUnreadCounts(recipientSocket, chatWith);
        }
    }

    closesocket(clientCopy.socket);
}

bool initWinsock() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
}

void signalHandler(int) {
    std::cout << "\n[!] Остановка сервера..." << std::endl;
    std::string stopTime = getCurrentTime();
    std::cout << "[!] Сервер остановлен в " << stopTime << std::endl;

    writeServerLog("========== СЕРВЕР ОСТАНАВЛИВАЕТСЯ ========== | Время: " + stopTime);

    // Уведомляем всех клиентов об остановке сервера
    notifyAllClientsServerShutdown();

    // Даем время на отправку уведомлений
    Sleep(1000);

    writeServerLog("========== СЕРВЕР ОСТАНОВЛЕН ========== | Время: " + stopTime);

    if (server_socket_global != INVALID_SOCKET) {
        closesocket(server_socket_global);
    }
    WSACleanup();
    exit(0);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    signal(SIGINT, signalHandler);

    system("mkdir logs 2>nul");

    writeServerLog("========== СЕРВЕР ЗАПУЩЕН ==========");

    if (!initWinsock()) {
        std::cerr << "Ошибка Winsock" << std::endl;
        return 1;
    }

    server_socket_global = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_global == INVALID_SOCKET) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket_global, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Ошибка привязки" << std::endl;
        closesocket(server_socket_global);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket_global, 10) == SOCKET_ERROR) {
        std::cerr << "Ошибка прослушивания" << std::endl;
        closesocket(server_socket_global);
        WSACleanup();
        return 1;
    }

    std::cout << "=== Сервер мессенджера ===" << std::endl;
    std::cout << "Запуск на порту " << PORT << "..." << std::endl;
    std::cout << "[!] Сервер запущен. Ожидание подключений..." << std::endl;
    std::cout << "[!] Для остановки нажмите Ctrl+C" << std::endl;

    while (server_running) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);

        SOCKET client_socket = accept(server_socket_global, (struct sockaddr*)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET) continue;

        char client_ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        std::string ip_address(client_ip);

        char username_buffer[256]{};
        recv(client_socket, username_buffer, 255, 0);

        std::string username(username_buffer);
        username.erase(username.find_last_not_of("\n\r") + 1);

        bool name_taken = false;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            name_taken = (user_sockets.find(username) != user_sockets.end());
        }

        if (name_taken || username.empty()) {
            sendToClient(client_socket, "ERROR|Имя уже занято или некорректно\n");
            closesocket(client_socket);
            continue;
        }

        Client new_client;
        new_client.socket = client_socket;
        new_client.username = username;
        new_client.currentChat = "";
        new_client.ip = ip_address;

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(new_client);
            user_sockets[username] = client_socket;
        }

        sendToClient(client_socket, "CONNECTED\n");

        std::thread client_thread(handleClient, new_client);
        client_thread.detach();
    }

    closesocket(server_socket_global);
    WSACleanup();
    return 0;
}