#define _WINSOCK_DEPRECATED_NO_WARNINGS  // Отключаем предупреждения об устаревших функциях (inet_ntoa и др.)

#include <iostream>     // Для ввода/вывода (cout, cin, cerr)
#include <string>       // Для работы со строками (std::string)
#include <thread>       // Для создания потоков (приём сообщений в отдельном потоке)
#include <atomic>       // Для атомарных переменных (потокобезопасный флаг)
#include <cstring>      // Для работы с C-строками (memset)
#include <sstream>      // Для строковых потоков (разбор сообщений от сервера)
#include <map>          // Для ассоциативного массива (счетчик непрочитанных сообщений)
#include <winsock2.h>   // Для работы с сокетами в Windows
#include <ws2tcpip.h>   // Для дополнительных функций сокетов (inet_pton)

#pragma comment(lib, "ws2_32.lib")  // Подключаем библиотеку сокетов

const int BUFFER_SIZE = 16384;  // Размер буфера для приёма сообщений (16 КБ)
const int PORT = 8888;          // Порт для подключения к серверу

std::atomic<bool> running(true);    // Флаг работы программы (потокобезопасный)
SOCKET sock;                        // Сокет для связи с сервером
std::string myUsername;             // Имя текущего пользователя
std::string currentChat = "";       // С кем сейчас ведётся диалог (пусто - не в диалоге)
std::map<std::string, int> unreadMessages;  // Счетчик непрочитанных сообщений: [отправитель] = количество

/**
 * Функция разбора и обработки сообщений от сервера
 * @param msg - сообщение от сервера в формате "ТИП|Данные"
 */
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
        // std::cout << data << std::endl;
    }
    else if (type == "HELP") {
        // Справка - не выводим
        // std::cout << "[СПРАВКА] " << data << std::endl;
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
                std::cout << "  - " << user << std::endl;  // Убрали (+X новых)
            }
        }
        std::cout << "==========================" << std::endl;
        std::cout << "> " << std::flush;
    }
    else if (type == "CHAT") {
        std::cout << data << std::endl;
        unreadMessages[currentChat] = 0;
        std::cout << "> " << std::flush;
    }
    else if (type == "MSG") {
        size_t sep = data.find('|');
        std::string from = data.substr(0, sep);
        std::string text = data.substr(sep + 1);

        if (currentChat == from) {
            // Если в диалоге - показываем сообщение сразу
            std::cout << "\r[" << from << "]: " << text << std::endl;
            std::cout << "> " << std::flush;
        }
        else {
            // Если не в диалоге - увеличиваем счетчик и показываем уведомление
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
        std::stringstream ss(history);
        std::string from;
        std::string text;
        while (std::getline(ss, from, '|')) {
            if (std::getline(ss, text, '|')) {
                std::cout << "[" << from << "]: " << text << std::endl;
            }
        }
        std::cout << "======================" << std::endl;
        std::cout << "> " << std::flush;
    }
    else if (type == "ERROR") {
        std::cout << "[ОШИБКА] " << data << std::endl;
        std::cout << "> " << std::flush;
    }
    else if (type == "CONNECTED") {
        // Игнорируем
    }
    else {
        std::cout << data << std::endl;
        std::cout << "> " << std::flush;
    }
}

/**
 * Функция приёма сообщений от сервера
 * Работает в отдельном потоке, чтобы не блокировать ввод пользователя
 */
void receiveMessages() {
    char buffer[BUFFER_SIZE];

    while (running) {
        // Очищаем буфер перед приёмом
        memset(buffer, 0, BUFFER_SIZE);
        // Принимаем сообщение от сервера (блокирующий вызов)
        int bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        // Если соединение разорвано или ошибка
        if (bytes_received <= 0) {
            if (running) {
                std::cout << "\n[!] Отключено от сервера." << std::endl;
            }
            running = false;
            break;
        }

        // Преобразуем полученные данные в строку
        std::string message(buffer);
        message.erase(message.find_last_not_of("\n\r") + 1);  // Удаляем символы перевода строки

        // Обрабатываем сообщение от сервера
        parseServerMessage(message);
    }
}

/**
 * Инициализация Winsock (только для Windows)
 * @return true - успех, false - ошибка
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
 * Главная функция - точка входа в программу
 */
int main() {
    // Устанавливаем кодировку UTF-8 для корректного отображения русского языка
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string server_ip;

    // Приветствие и ввод параметров подключения
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

    // Создаём папку для логов (2>nul подавляет ошибку "папка существует")
    system("mkdir logs 2>nul");

    // Инициализируем Winsock
    if (!initWinsock()) {
        return 1;
    }

    // Создаём сокет
    // AF_INET - IPv4, SOCK_STREAM - TCP
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }

    // Настраиваем адрес сервера
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (server_ip == "localhost") {
        server_ip = "127.0.0.1";
    }

    // Преобразуем IP-адрес из строки в двоичный формат
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Ошибка: неверный IP" << std::endl;
        return 1;
    }

    std::cout << "Подключение..." << std::endl;

    // Подключаемся к серверу
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Ошибка: не удалось подключиться" << std::endl;
        return 1;
    }

    // Отправляем серверу своё имя пользователя
    send(sock, myUsername.c_str(), (int)myUsername.length(), 0);

    // Ждём ответ от сервера (игнорируем, так как это просто подтверждение)
    char response[256];
    memset(response, 0, 256);
    recv(sock, response, 255, 0);

    // Выводим список команд
    std::cout << "\n=== КОМАНДЫ ===" << std::endl;
    std::cout << "/users - список пользователей (с количеством новых сообщений)" << std::endl;
    std::cout << "/chat Имя - начать диалог" << std::endl;
    std::cout << "/quit - выход" << std::endl;
    std::cout << "==============" << std::endl;

    // Запускаем отдельный поток для приёма сообщений
    std::thread receiver(receiveMessages);
    
    // Основной цикл отправки сообщений
    std::string input;
    while (running) {
        std::cout << "> " << std::flush;
        std::getline(std::cin, input);

        if (!running) break;

        if (input.empty()) {
            continue;
        }

        // Обработка команды выхода
        if (input == "/quit") {
            send(sock, input.c_str(), (int)input.length(), 0);
            running = false;
            break;
        }
        // Обработка команды списка пользователей (при этом диалог закрывается)
        else if (input == "/users") {
            currentChat = "";  // Закрываем текущий диалог
            send(sock, input.c_str(), (int)input.length(), 0);
        }
        // Обработка команды начала диалога
        else if (input.substr(0, 5) == "/chat") {
            currentChat = input.substr(6);  // Запоминаем собеседника
            send(sock, input.c_str(), (int)input.length(), 0);
        }
        // Обычное сообщение - отправляем текущему собеседнику
        else {
            if (currentChat.empty()) {
                std::cout << "[ОШИБКА] Вы не в диалоге. Используйте /chat Имя" << std::endl;
            }
            else {
                send(sock, input.c_str(), (int)input.length(), 0);
            }
        }
    }

    // Дожидаемся завершения потока приёма сообщений
    if (receiver.joinable()) {
        receiver.join();
    }

    // Закрываем сокет и очищаем Winsock
    closesocket(sock);
    WSACleanup();

    std::cout << "Отключено." << std::endl;

    return 0;
}