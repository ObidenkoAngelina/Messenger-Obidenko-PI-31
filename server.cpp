#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#define Sleep(x) usleep((x)*1000)
#endif

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

const int PORT = 8888;              // Порт для прослушивания подключений
const int BUFFER_SIZE = 16384;      // Размер буфера для приёма сообщений (16 КБ)

std::mutex log_mutex;               // Мьютекс для защиты файла логов
std::mutex clients_mutex;           // Мьютекс для защиты списка клиентов
bool server_running = true;         // Флаг работы сервера
SOCKET server_socket_global = INVALID_SOCKET; // Глобальный сокет для обработки Ctrl+C

// Структура для хранения информации о подключённом клиенте
struct Client {
    SOCKET socket = INVALID_SOCKET;  // Сокет клиента
    std::string username;            // Имя пользователя
    std::string currentChat;         // С кем сейчас ведёт диалог (имя собеседника)
    std::string ip;                  // IP-адрес клиента
};

// Структура для хранения сообщения
struct Message {
    std::string from, to, text, time;  // Отправитель, получатель, текст, время отправки
};

// Глобальные контейнеры для хранения данных
std::vector<Client> clients;                                    // Список всех активных клиентов
std::map<std::string, SOCKET> user_sockets;                     // Соответствие: имя пользователя -> его сокет
std::map<std::string, std::map<std::string, int>> unreadCounts; // Счётчики непрочитанных сообщений: [получатель][отправитель] = количество
std::set<std::string> knownUsers;                               // Множество всех известных пользователей (кто когда-либо заходил)

// ---------- Вспомогательные функции ----------

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
 * Возвращает текущее время в формате "ЧЧ:ММ:СС"
 * Использует localtime_s для Windows, localtime_r для Linux
 */
std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::stringstream ss;
    ss << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_sec;
    return ss.str();
}

/**
 * Отправляет сообщение клиенту через сокет
 * @return true - успешно, false - ошибка
 */
bool sendToClient(SOCKET client_socket, const std::string& message) {
    return send(client_socket, message.c_str(), (int)message.length(), 0) != SOCKET_ERROR;
}

/**
 * Добавляет строку в конец файла (создаёт файл, если его нет)
 */
void appendLineToFile(const std::string& path, const std::string& line) {
    std::ofstream file(path, std::ios::app);
    if (file.is_open()) {
        file << line << std::endl;
        file.close();
    }
}

/**
 * Очищает имя файла от запрещённых символов (/, \, :, *, ?, ", <, >, |)
 * Оставляет русские буквы и другие допустимые символы
 */
std::string sanitizeForFilename(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (ch < 32) continue;
        switch (ch) {
        case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
            out.push_back('_'); break;
        default:
            out.push_back(ch); break;
        }
    }
    if (out.empty()) out = "user";
    return out;
}

/**
 * Записывает событие в лог-файл сервера с временной меткой
 */
void writeServerLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    appendLineToFile("logs/server.log", "[" + getCurrentTime() + "] " + msg);
}

// ---------- Функции работы с файлами (сохранение/загрузка данных) ----------

/**
 * Возвращает путь к файлу, где хранятся непрочитанные сообщения пользователя
 */
static std::string getUnreadPath(const std::string& username) {
    return "logs/unread_" + sanitizeForFilename(username) + ".txt";
}

/**
 * Возвращает путь к файлу, где хранится история переписки между двумя пользователями
 * Имена сортируются, чтобы файл был один на пару (Анна__Борис = Борис__Анна)
 */
static std::string getPairHistoryPath(const std::string& a, const std::string& b) {
    std::string A = sanitizeForFilename(a);
    std::string B = sanitizeForFilename(b);
    if (A > B) std::swap(A, B);
    return "logs/history_" + A + "__" + B + ".txt";
}

/**
 * Сохраняет счётчики непрочитанных сообщений пользователя в файл
 */
static void saveUnreadForUser(const std::string& username) {
    std::string path = getUnreadPath(username);
    std::ofstream file(path);
    if (!file.is_open()) return;

    for (const auto& p : unreadCounts[username]) {
        if (p.second > 0) {
            file << p.first << ":" << p.second << std::endl;
        }
    }
    file.close();
}

/**
 * Загружает счётчики непрочитанных сообщений пользователя из файла
 */
static void loadUnreadForUser(const std::string& username) {
    unreadCounts[username].clear();
    std::string path = getUnreadPath(username);
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string from = line.substr(0, colon);
        int cnt = 0;
        try { cnt = std::stoi(line.substr(colon + 1)); }
        catch (...) { cnt = 0; }
        if (!from.empty() && cnt > 0) unreadCounts[username][from] = cnt;
    }
    file.close();
}

/**
 * Сохраняет сообщение в файлы:
 * - общий файл диалога (для истории)
 * - личный файл отправителя
 * - личный файл получателя
 */
static void appendMessageToPairHistory(const Message& m) {
    // Общий файл диалога (для быстрой загрузки истории)
    std::string path = getPairHistoryPath(m.from, m.to);
    std::string line = m.time + "|" + m.from + "|" + m.to + "" + m.text;
    appendLineToFile(path, line);

    // Личный файл отправителя
    std::string senderPath = "logs/chat_" + sanitizeForFilename(m.from) + ".txt";
    // Личный файл получателя
    std::string receiverPath = "logs/chat_" + sanitizeForFilename(m.to) + ".txt";

    appendLineToFile(senderPath, "[" + m.time + "] [-> " + m.to + "]: " + m.text);
    appendLineToFile(receiverPath, "[" + m.time + "] [" + m.from + " ->]: " + m.text);
}

/**
 * Загружает историю переписки для протокола (в формате from|text|from|text|...)
 */
static std::string loadPairHistoryForProtocol(const std::string& viewer, const std::string& with) {
    std::string path = getPairHistoryPath(viewer, with);
    std::ifstream file(path);
    if (!file.is_open()) return "";

    std::string out;
    std::string line;
    while (std::getline(file, line)) {
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
    file.close();
    return out;
}

/**
 * Загружает список всех известных пользователей из файлов в папке logs
 */
void loadAllKnownUsersFromFiles() {
    knownUsers.clear();

#ifdef _WIN32
    // Windows: используем команду dir для получения списка файлов
    system("dir /b logs\\chat_*.txt > logs\\temp.txt 2>nul");
    std::ifstream filelist("logs/temp.txt");
    std::string filename;
    while (std::getline(filelist, filename)) {
        if (filename.empty()) continue;
        if (filename.rfind("chat_", 0) == 0) {
            std::string name = filename.substr(5);
            size_t dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);
            if (!name.empty()) knownUsers.insert(name);
        }
    }
    filelist.close();
    system("del logs\\temp.txt 2>nul");
#else
    // Linux: используем opendir/readdir для чтения папки
    DIR* dir = opendir("logs");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.rfind("chat_", 0) == 0) {
                std::string username = name.substr(5);
                size_t dot = username.rfind('.');
                if (dot != std::string::npos) username = username.substr(0, dot);
                if (!username.empty()) knownUsers.insert(username);
            }
        }
        closedir(dir);
    }
#endif

    std::cout << "[!] Загружено известных пользователей: " << knownUsers.size() << std::endl;
    writeServerLog("ЗАГРУЗКА ИСТОРИИ | Известных пользователей: " + std::to_string(knownUsers.size()));
}

// ---------- Обработчики протокола ----------

/**
 * Отправляет клиенту список непрочитанных сообщений в формате "UNREAD|имя:количество,имя:количество"
 */
void sendUnreadCountsToClient(SOCKET client_socket, const std::string& username) {
    std::stringstream ss;
    bool hasUnread = false;
    for (const auto& pair : unreadCounts[username]) {
        if (pair.second > 0) {
            if (hasUnread) ss << ",";
            ss << pair.first << ":" << pair.second;
            hasUnread = true;
        }
    }
    if (hasUnread) {
        sendToClient(client_socket, "UNREAD|" + ss.str() + "\n");
    }
}

/**
 * Отправляет клиенту историю переписки с указанным пользователем
 * Формат: "HISTORY|собеседник|отправитель|текст|отправитель|текст|..."
 */
void sendChatHistory(SOCKET client_socket, const std::string& username, const std::string& with) {
    unreadCounts[username][with] = 0;
    saveUnreadForUser(username);

    std::string history = loadPairHistoryForProtocol(username, with);
    std::stringstream ss;
    ss << "HISTORY|" << with << "|" << history;
    sendToClient(client_socket, ss.str() + "\n");
    sendUnreadCountsToClient(client_socket, username);
}

/**
 * Устанавливает текущий диалог для клиента (потокобезопасно)
 */
static void setClientCurrentChatLocked(const std::string& username, const std::string& target) {
    for (auto& c : clients) {
        if (c.username == username) { c.currentChat = target; return; }
    }
}

/**
 * Возвращает текущий диалог клиента (потокобезопасно)
 */
static std::string getClientCurrentChatLocked(const std::string& username) {
    for (const auto& c : clients) {
        if (c.username == username) return c.currentChat;
    }
    return "";
}

/**
 * Отправляет всем клиентам уведомление о выключении сервера
 */
void notifyAllClientsServerShutdown() {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const auto& client : clients) sendToClient(client.socket, "SERVER_SHUTDOWN\n");
}

/**
 * Удаляет клиента из всех списков и логирует отключение
 */
void removeClient(const std::string& username, const std::string& ip) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = std::find_if(clients.begin(), clients.end(),
        [&username](const Client& c) { return c.username == username; });
    if (it != clients.end()) clients.erase(it);
    user_sockets.erase(username);
    writeServerLog("ОТКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + ip);
}

/**
 * Проверяет, можно ли начать диалог с пользователем (онлайн или известен)
 */
static bool canChatWithLocked(const std::string& target) {
    if (user_sockets.find(target) != user_sockets.end()) return true;
    if (knownUsers.find(target) != knownUsers.end()) return true;
    return false;
}

/**
 * Главная функция обработки клиента (работает в отдельном потоке для каждого клиента)
 */
void handleClient(Client clientCopy) {
    char buffer[BUFFER_SIZE];
    std::string username = clientCopy.username;

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        knownUsers.insert(username);
    }

    loadUnreadForUser(username);
    writeServerLog("ПОДКЛЮЧЕНИЕ | Пользователь: " + username + " | IP: " + clientCopy.ip);

    // Отправляем непрочитанные только при подключении
    sendUnreadCountsToClient(clientCopy.socket, username);

    // Отправляем список всех известных пользователей
    std::stringstream allUsers;
    allUsers << "ALL_USERS|";
    bool first = true;
    for (const auto& u : knownUsers) {
        if (u != username) {
            if (!first) allUsers << ",";
            allUsers << u;
            first = false;
        }
    }
    sendToClient(clientCopy.socket, allUsers.str() + "\n");

    // Основной цикл обработки сообщений от клиента
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

        // Обработка команд от клиента
        if (message == "/quit") {
            removeClient(username, clientCopy.ip);
            break;
        }
        else if (message == "/online_users") {
            // Закрываем текущий диалог
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                setClientCurrentChatLocked(username, "");
            }

            // Формируем список онлайн пользователей
            std::stringstream ss;
            ss << "ONLINE_USERS|";
            bool firstUser = true;
            for (const auto& c : clients) {
                if (c.username != username) {
                    if (!firstUser) ss << ",";
                    ss << c.username;
                    firstUser = false;
                }
            }
            sendToClient(clientCopy.socket, ss.str() + "\n");
        }
        else if (message == "/all_users") {
            // Закрываем текущий диалог
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                setClientCurrentChatLocked(username, "");
            }

            // Формируем список всех известных пользователей
            std::stringstream ss;
            ss << "ALL_USERS|";
            bool firstUser = true;
            for (const auto& u : knownUsers) {
                if (u != username) {
                    if (!firstUser) ss << ",";
                    ss << u;
                    firstUser = false;
                }
            }
            sendToClient(clientCopy.socket, ss.str() + "\n");
        }
        else if (message.rfind("/chat", 0) == 0) {
            // Команда начала диалога: /chat Имя
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
            // Обычное текстовое сообщение
            std::string chatWith;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                chatWith = getClientCurrentChatLocked(username);
            }

            if (chatWith.empty()) {
                sendToClient(clientCopy.socket, "ERROR|Вы не в диалоге. Используйте /chat Имя\n");
                continue;
            }

            // Создаём структуру сообщения с текущим временем
            Message msg{ username, chatWith, message, getCurrentTime() };
            appendMessageToPairHistory(msg);  // Сохраняем в файлы

            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                knownUsers.insert(chatWith);
            }

            // Проверяем, онлайн ли получатель
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
                // Получатель онлайн
                bool recipientIsReadingNow = (recipientCurrentChat == username);
                if (!recipientIsReadingNow) {
                    unreadCounts[chatWith][username]++;
                    saveUnreadForUser(chatWith);
                }
                else {
                    unreadCounts[chatWith][username] = 0;
                    saveUnreadForUser(chatWith);
                }

                // Отправляем сообщение получателю
                sendToClient(recipientSocket, "MSG|" + username + "|" + message + "\n");
                sendUnreadCountsToClient(recipientSocket, chatWith);
            }
            else {
                // Получатель офлайн - просто увеличиваем счётчик непрочитанных
                unreadCounts[chatWith][username]++;
                saveUnreadForUser(chatWith);
            }

            writeServerLog("СООБЩЕНИЕ | От: " + username + " | Кому: " + chatWith + " | Текст: " + message);
        }
    }

    closesocket(clientCopy.socket);
}

#ifdef _WIN32
/**
 * Инициализация Winsock (только для Windows)
 */
bool initWinsock() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
}
#endif

/**
 * Обработчик сигнала Ctrl+C
 * Отправляет всем клиентам сообщение о выключении сервера
 */
void signalHandler(int) {
    std::cout << "\n[!] Остановка сервера..." << std::endl;
    std::string stopTime = getCurrentTime();
    writeServerLog("========== СЕРВЕР ОСТАНАВЛИВАЕТСЯ ========== | Время: " + stopTime);
    notifyAllClientsServerShutdown();
    Sleep(500);  // Даём время на отправку сообщений
    writeServerLog("========== СЕРВЕР ОСТАНОВЛЕН ========== | Время: " + stopTime);
    if (server_socket_global != INVALID_SOCKET) closesocket(server_socket_global);
#ifdef _WIN32
    WSACleanup();
#endif
    exit(0);
}

/**
 * Главная функция - точка входа в программу
 */
int main() {
    // Устанавливаем кодировку UTF-8 для консоли (Windows) или локаль (Linux)
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    setlocale(LC_ALL, "ru_RU.UTF-8");
    std::cout.imbue(std::locale("ru_RU.UTF-8"));
#endif

    // Устанавливаем обработчик сигнала Ctrl+C
    signal(SIGINT, signalHandler);

    // Создаём папку для логов
#ifdef _WIN32
    system("mkdir logs 2>nul");
#else
    system("mkdir -p logs 2>/dev/null");
#endif

    // Загружаем список известных пользователей из файлов
    loadAllKnownUsersFromFiles();
    writeServerLog("========== СЕРВЕР ЗАПУЩЕН ==========");

    // Инициализируем сокеты (Winsock для Windows)
#ifdef _WIN32
    if (!initWinsock()) {
        std::cerr << "Ошибка Winsock" << std::endl;
        return 1;
    }
#endif

    // Создаём серверный сокет
    server_socket_global = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_global == INVALID_SOCKET) {
        std::cerr << "Ошибка создания сокета" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // Позволяем переиспользовать адрес (чтобы не ждать 2 минуты после перезапуска)
    int opt = 1;
    setsockopt(server_socket_global, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    // Настраиваем адрес сервера
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;              // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;      // Слушаем все сетевые интерфейсы
    server_addr.sin_port = htons(PORT);            // Порт 8888

    // Привязываем сокет к адресу
    if (bind(server_socket_global, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Ошибка привязки" << std::endl;
        closesocket(server_socket_global);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // Начинаем прослушивание (максимум 10 подключений в очереди)
    if (listen(server_socket_global, 10) == SOCKET_ERROR) {
        std::cerr << "Ошибка прослушивания" << std::endl;
        closesocket(server_socket_global);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "=== Сервер мессенджера ===" << std::endl;
    std::cout << "Запуск на порту " << PORT << "..." << std::endl;
    std::cout << "[!] Сервер запущен. Ожидание подключений..." << std::endl;
    std::cout << "[!] Для остановки нажмите Ctrl+C" << std::endl;

    // Основной цикл приёма подключений
    while (server_running) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Принимаем новое подключение
        SOCKET client_socket = accept(server_socket_global, (struct sockaddr*)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET) continue;

        // Получаем IP-адрес клиента
        char client_ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        std::string ip_address(client_ip);

        // Получаем имя пользователя от клиента
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

        // Проверяем, не занято ли имя
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
            knownUsers.insert(username);
        }

        // Отправляем подтверждение подключения
        sendToClient(client_socket, "CONNECTED\n");

        // Запускаем отдельный поток для обработки этого клиента
        std::thread client_thread(handleClient, new_client);
        client_thread.detach();  // Открепляем поток (будет работать автономно)
    }

    closesocket(server_socket_global);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}