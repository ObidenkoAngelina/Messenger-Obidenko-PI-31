#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <csignal>

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
std::map<std::string, std::map<std::string, int>> unreadCounts;
std::set<std::string> knownUsers;

// ---------- helpers ----------
static inline void trimCRLF(std::string& s) {
    size_t end = s.find_last_not_of("\n\r");
    if (end == std::string::npos) { s.clear(); return; }
    s.erase(end + 1);
}

static inline void trimSpaces(std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t");
    s = s.substr(start, end - start + 1);
}

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

static std::string sanitizeForFilenameUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
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

// ---------- persistence ----------
static std::wstring getUnreadPath(const std::string& username) {
    std::string safe = sanitizeForFilenameUtf8(username);
    return L"logs\\unread_" + utf8ToWide(safe) + L".txt";
}

static std::wstring getPairHistoryPath(const std::string& a, const std::string& b) {
    std::string A = sanitizeForFilenameUtf8(a);
    std::string B = sanitizeForFilenameUtf8(b);
    if (A > B) std::swap(A, B);
    return L"logs\\history_" + utf8ToWide(A) + L"__" + utf8ToWide(B) + L".txt";
}

void writeServerLog(const std::string& msgUtf8) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::wstring wtime = utf8ToWide(getCurrentTime());
    std::wstring wmsg = utf8ToWide(msgUtf8);
    appendWideLineToFile(L"logs\\server.log", L"[" + wtime + L"] " + wmsg);
}

static void saveUnreadForUser(const std::string& username) {
    std::wstring path = getUnreadPath(username);
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w+, ccs=UTF-8");
    if (!f) {
        _wfopen_s(&f, path.c_str(), L"w+");
        if (!f) return;
    }

    for (const auto& p : unreadCounts[username]) {
        if (p.second <= 0) continue;
        std::wstring line = utf8ToWide(p.first + ":" + std::to_string(p.second));
        fputws(line.c_str(), f);
        fputws(L"\n", f);
    }
    fclose(f);
}

static void loadUnreadForUser(const std::string& username) {
    unreadCounts[username].clear();

    std::wstring path = getUnreadPath(username);
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8");
    if (!f) {
        _wfopen_s(&f, path.c_str(), L"r");
        if (!f) return;
    }

    wchar_t buf[1024];
    while (fgetws(buf, 1024, f)) {
        std::wstring wline(buf);
        while (!wline.empty() && (wline.back() == L'\n' || wline.back() == L'\r')) wline.pop_back();
        if (wline.empty()) continue;

        int len = WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), nullptr, 0, nullptr, nullptr);
        if (len <= 0) continue;
        std::string line(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), &line[0], len, nullptr, nullptr);

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string from = line.substr(0, colon);
        int cnt = 0;
        try { cnt = std::stoi(line.substr(colon + 1)); }
        catch (...) { cnt = 0; }
        if (!from.empty() && cnt > 0) unreadCounts[username][from] = cnt;
    }
    fclose(f);
}

static void appendMessageToPairHistory(const Message& m) {
    // Общий файл диалога
    std::wstring path = getPairHistoryPath(m.from, m.to);
    std::string line = m.time + "|" + m.from + "|" + m.to + "|" + m.text;
    appendWideLineToFile(path, utf8ToWide(line));

    // Личный файл отправителя
    std::wstring senderPath = L"logs\\chat_" + utf8ToWide(sanitizeForFilenameUtf8(m.from)) + L".txt";
    std::string senderLine = "[" + m.time + "] [-> " + m.to + "]: " + m.text;
    appendWideLineToFile(senderPath, utf8ToWide(senderLine));

    // Личный файл получателя
    std::wstring receiverPath = L"logs\\chat_" + utf8ToWide(sanitizeForFilenameUtf8(m.to)) + L".txt";
    std::string receiverLine = "[" + m.time + "] [" + m.from + " ->]: " + m.text;
    appendWideLineToFile(receiverPath, utf8ToWide(receiverLine));
}

static std::string loadPairHistoryForProtocol(const std::string& viewer, const std::string& with) {
    std::wstring path = getPairHistoryPath(viewer, with);
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8");
    if (!f) {
        _wfopen_s(&f, path.c_str(), L"r");
        if (!f) return "";
    }

    std::string out;
    wchar_t buf[4096];
    while (fgetws(buf, 4096, f)) {
        std::wstring wline(buf);
        while (!wline.empty() && (wline.back() == L'\n' || wline.back() == L'\r')) wline.pop_back();
        if (wline.empty()) continue;

        int len = WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), nullptr, 0, nullptr, nullptr);
        if (len <= 0) continue;
        std::string line(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), &line[0], len, nullptr, nullptr);

        size_t p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) continue;
        size_t p3 = line.find('|', p2 + 1);
        if (p3 == std::string::npos) continue;

        std::string from = line.substr(p1 + 1, p2 - (p1 + 1));
        std::string text = line.substr(p3 + 1);

        out += from + "|" + text + "|";
    }
    fclose(f);
    return out;
}

// Загрузка всех известных пользователей из файлов при старте
void loadAllKnownUsersFromFiles() {
    system("dir /b logs\\chat_*.txt > logs\\temp_users.txt 2>nul");
    std::ifstream filelist("logs/temp_users.txt");
    std::string filename;

    while (std::getline(filelist, filename)) {
        if (filename.empty()) continue;
        // Файл вида "chat_Имя.txt"
        std::string name = filename;
        if (name.rfind("chat_", 0) == 0) {
            name = name.substr(5); // убираем "chat_"
            size_t dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);
            if (!name.empty()) knownUsers.insert(name);
        }
    }
    filelist.close();
    system("del logs\\temp_users.txt 2>nul");

    // Также сканируем history файлы
    system("dir /b logs\\history_*.txt > logs\\temp_history.txt 2>nul");
    std::ifstream histfile("logs/temp_history.txt");
    while (std::getline(histfile, filename)) {
        if (filename.empty()) continue;
        // Файл вида "history_Анна__Борис.txt"
        if (filename.rfind("history_", 0) == 0) {
            std::string names = filename.substr(8);
            size_t sep = names.find("__");
            if (sep != std::string::npos) {
                std::string user1 = names.substr(0, sep);
                std::string user2 = names.substr(sep + 2);
                size_t dot = user1.rfind('.');
                if (dot != std::string::npos) user1 = user1.substr(0, dot);
                dot = user2.rfind('.');
                if (dot != std::string::npos) user2 = user2.substr(0, dot);
                if (!user1.empty()) knownUsers.insert(user1);
                if (!user2.empty()) knownUsers.insert(user2);
            }
        }
    }
    histfile.close();
    system("del logs\\temp_history.txt 2>nul");

    std::cout << "[!] Загружено известных пользователей: " << knownUsers.size() << std::endl;
    writeServerLog("ЗАГРУЗКА ИСТОРИИ | Известных пользователей: " + std::to_string(knownUsers.size()));
}

// ---------- protocol handlers ----------
void sendUnreadCountsToClient(SOCKET client_socket, const std::string& username) {
    std::stringstream ss;
    ss << "UNREAD|";
    for (const auto& pair : unreadCounts[username]) {
        if (pair.second > 0) ss << pair.first << ":" << pair.second << ",";
    }
    sendToClient(client_socket, ss.str() + "\n");
}

void sendChatHistory(SOCKET client_socket, const std::string& username, const std::string& with) {
    unreadCounts[username][with] = 0;
    saveUnreadForUser(username);

    std::string history = loadPairHistoryForProtocol(username, with);
    std::stringstream ss;
    ss << "HISTORY|" << with << "|" << history;
    sendToClient(client_socket, ss.str() + "\n");
    sendUnreadCountsToClient(client_socket, username);
}

static void setClientCurrentChatLocked(const std::string& username, const std::string& target) {
    for (auto& c : clients) {
        if (c.username == username) { c.currentChat = target; return; }
    }
}

static std::string getClientCurrentChatLocked(const std::string& username) {
    for (const auto& c : clients) {
        if (c.username == username) return c.currentChat;
    }
    return "";
}

void notifyAllClientsServerShutdown() {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const auto& client : clients) sendToClient(client.socket, "SERVER_SHUTDOWN\n");
}

void removeClient(const std::string& username, const std::string& ip) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = std::find_if(clients.begin(), clients.end(),
        [&username](const Client& c) { return c.username == username; });
    if (it != clients.end()) clients.erase(it);
    user_sockets.erase(username);
    writeServerLog("ОТКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + ip);
}

static bool canChatWithLocked(const std::string& target) {
    if (user_sockets.find(target) != user_sockets.end()) return true;
    if (knownUsers.find(target) != knownUsers.end()) return true;
    return false;
}

void handleClient(Client clientCopy) {
    char buffer[BUFFER_SIZE];
    std::string username = clientCopy.username;

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        knownUsers.insert(username);
    }

    loadUnreadForUser(username);
    writeServerLog("ПОДКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + clientCopy.ip);
    sendUnreadCountsToClient(clientCopy.socket, username);

    // Отправляем список всех известных пользователей
    std::stringstream allUsers;
    allUsers << "ALL_USERS|";
    for (const auto& u : knownUsers) {
        if (u != username) allUsers << u << ",";
    }
    sendToClient(clientCopy.socket, allUsers.str() + "\n");

    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(clientCopy.socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            removeClient(username, clientCopy.ip);
            break;
        }

        std::string message(buffer);
        trimCRLF(message);
        if (message.empty()) continue;

        if (message == "/quit") {
            removeClient(username, clientCopy.ip);
            break;
        }
        else if (message == "/online_users") {
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                setClientCurrentChatLocked(username, "");
            }

            std::stringstream ss;
            ss << "ONLINE_USERS|";
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (const auto& c : clients) {
                    if (c.username != username) ss << c.username << ",";
                }
            }
            sendToClient(clientCopy.socket, ss.str() + "\n");
            sendUnreadCountsToClient(clientCopy.socket, username);
        }
        else if (message == "/all_users") {
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                setClientCurrentChatLocked(username, ""); 
            }

            std::stringstream ss;
            ss << "ALL_USERS|";
            for (const auto& u : knownUsers) {
                if (u != username) ss << u << ",";
            }
            sendToClient(clientCopy.socket, ss.str() + "\n");
        }
        else if (message.rfind("/chat", 0) == 0) {
            std::string target = (message.size() > 6) ? message.substr(6) : "";
            trimSpaces(target);

            if (target.empty()) {
                sendToClient(clientCopy.socket, "ERROR|Используйте /chat Имя\n");
                continue;
            }
            if (target == username) {
                sendToClient(clientCopy.socket, "ERROR|Нельзя открыть чат с самим собой\n");
                continue;
            }

            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                ok = canChatWithLocked(target);
                if (ok) {
                    setClientCurrentChatLocked(username, target);
                    knownUsers.insert(target);
                }
            }

            if (!ok) {
                sendToClient(clientCopy.socket, "ERROR|Пользователь " + target + " не найден\n");
                continue;
            }

            sendToClient(clientCopy.socket, "CHAT|Теперь вы общаетесь с " + target + "\n");
            sendChatHistory(clientCopy.socket, username, target);
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

            Message msg{ username, chatWith, message, getCurrentTime() };
            appendMessageToPairHistory(msg);

            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                knownUsers.insert(chatWith);
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

            if (recipientSocket != INVALID_SOCKET) {
                bool recipientIsReadingNow = (recipientCurrentChat == username);
                if (!recipientIsReadingNow) {
                    unreadCounts[chatWith][username]++;
                    saveUnreadForUser(chatWith);
                }
                else {
                    unreadCounts[chatWith][username] = 0;
                    saveUnreadForUser(chatWith);
                }

                sendToClient(recipientSocket, "MSG|" + username + "|" + message + "\n");
                sendUnreadCountsToClient(recipientSocket, chatWith);
            }
            else {
                unreadCounts[chatWith][username]++;
                saveUnreadForUser(chatWith);
            }

            writeServerLog("СООБЩЕНИЕ | От: " + username + " | Кому: " + chatWith + " | Текст: " + message);
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
    writeServerLog("========== СЕРВЕР ОСТАНАВЛИВАЕТСЯ ========== | Время: " + stopTime);
    notifyAllClientsServerShutdown();
    Sleep(500);
    writeServerLog("========== СЕРВЕР ОСТАНОВЛЕН ========== | Время: " + stopTime);
    if (server_socket_global != INVALID_SOCKET) closesocket(server_socket_global);
    WSACleanup();
    exit(0);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    signal(SIGINT, signalHandler);
    system("mkdir logs 2>nul");

    // Загружаем всех известных пользователей из файлов
    loadAllKnownUsersFromFiles();

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
        trimCRLF(username);
        trimSpaces(username);

        if (username.empty()) {
            sendToClient(client_socket, "ERROR|Имя не может быть пустым\n");
            closesocket(client_socket);
            continue;
        }

        bool name_taken = false;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            name_taken = (user_sockets.find(username) != user_sockets.end());
        }

        if (name_taken) {
            sendToClient(client_socket, "ERROR|Имя уже занято\n");
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
            knownUsers.insert(username);
        }

        sendToClient(client_socket, "CONNECTED\n");

        std::thread client_thread(handleClient, new_client);
        client_thread.detach();
    }

    closesocket(server_socket_global);
    WSACleanup();
    return 0;
}


