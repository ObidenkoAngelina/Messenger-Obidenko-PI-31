#define _WINSOCK_DEPRECATED_NO_WARNINGS  // Отключаем предупреждения об устаревших функциях (inet_ntoa и др.)

#include <iostream>     // Для ввода/вывода (cout, cerr)
#include <string>       // Для работы со строками (std::string)
#include <thread>       // Для создания потоков (каждый клиент в своём потоке)
#include <vector>       // Для хранения списка клиентов (динамический массив)
#include <map>          // Для ассоциативного массива (имя -> сокет)
#include <mutex>        // Для мьютексов (синхронизация потоков)
#include <algorithm>    // Для алгоритмов (std::remove_if)
#include <cstring>      // Для работы с C-строками (memset)
#include <sstream>      // Для строковых потоков (формирование сообщений)
#include <winsock2.h>   // Для работы с сокетами в Windows
#include <ws2tcpip.h>   // Для дополнительных функций сокетов

#pragma comment(lib, "ws2_32.lib")  // Подключаем библиотеку сокетов

#include "logger.h"     // Наш логгер для записи событий в файл

// Константы
const int PORT = 8888;              // Порт, на котором сервер слушает подключения
const int BUFFER_SIZE = 4096;       // Размер буфера для приёма сообщений
const std::string LOG_FILE = "logs/server.log";  // Путь к файлу лога сервера

/**
 * Структура для хранения информации о подключённом клиенте
 */
struct Client {
    SOCKET socket;          // Сокет клиента (канал связи)
    std::string username;   // Имя пользователя
    std::string address;    // IP-адрес клиента
    int port;               // Порт клиента (с которого он подключился)
};

// Глобальные данные (доступны из всех потоков)
std::vector<Client> clients;                    // Список всех подключённых клиентов
std::map<std::string, SOCKET> user_sockets;     // Соответствие: имя пользователя -> его сокет
std::mutex clients_mutex;                       // Мьютекс для защиты списка клиентов (потокобезопасность)
Logger server_logger(LOG_FILE);                 // Логгер сервера (записывает события в файл)

//Отправляет сообщение конкретному клиенту
bool sendToClient(SOCKET client_socket, const std::string& message) {
    // send() отправляет данные через сокет
    int sent = send(client_socket, message.c_str(), (int)message.length(), 0);
    return sent != SOCKET_ERROR;  // SOCKET_ERROR = ошибка
}

//Рассылает сообщение ВСЕМ клиентам (широковещательная рассылка)
void broadcastMessage(const std::string& message, SOCKET exclude_socket = INVALID_SOCKET) {
    std::lock_guard<std::mutex> lock(clients_mutex);  // Блокируем мьютекс (автоматически разблокируется при выходе)
    for (const auto& client : clients) {              // Проходим по всем клиентам
        if (client.socket != exclude_socket) {        // Если это не исключённый клиент
            sendToClient(client.socket, message);     // Отправляем ему сообщение
        }
    }
}

//Отправляет личное сообщение от одного пользователя другому
bool sendPrivateMessage(const std::string& from_user, const std::string& to_user, const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex);  // Защищаем доступ к карте пользователей

    auto it = user_sockets.find(to_user);  // Ищем получателя в карте
    if (it == user_sockets.end()) {
        return false;  // Пользователь не найден в чате
    }

    // Форматируем сообщение с пометкой "Личное"
    std::string formatted_msg = "[Личное от " + from_user + "]: " + message;
    return sendToClient(it->second, formatted_msg);  // Отправляем личное сообщение
}

//Формирует строку со списком всех активных пользователей
std::string getUserList() {
    std::lock_guard<std::mutex> lock(clients_mutex);  // Защищаем доступ к списку клиентов
    std::stringstream ss;
    ss << "Активные пользователи: ";
    for (size_t i = 0; i < clients.size(); ++i) {
        if (i > 0) ss << ", ";  // Добавляем запятую между именами (кроме первого)
        ss << clients[i].username;
    }
    return ss.str();
}

//Функция обработки клиента (работает в отдельном потоке для КАЖДОГО клиента)
void handleClient(Client client) {
    char buffer[BUFFER_SIZE];
    std::string username = client.username;

    // Логируем в файл и выводим в консоль информацию о новом подключении
    server_logger.info("Подключен клиент: " + username + " с адреса " + client.address + ":" + std::to_string(client.port));
    std::cout << "Подключен клиент: " << username << " с адреса " << client.address << ":" << client.port << std::endl;

    sendToClient(client.socket, "Добро пожаловать в мессенджер, " + username + "!\n");
    sendToClient(client.socket, "Команды:\n");
    sendToClient(client.socket, "  /users - показать список активных пользователей\n");
    sendToClient(client.socket, "  /msg <имя_пользователя> <сообщение> - отправить личное сообщение\n");
    sendToClient(client.socket, "  /quit - отключиться\n\n");

    broadcastMessage("*** " + username + " присоединился к чату ***\n", client.socket);
    broadcastMessage(getUserList() + "\n");

    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client.socket, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received <= 0) {
            server_logger.info("Клиент отключился: " + username);
            break;
        }

        std::string message(buffer);
        message.erase(message.find_last_not_of("\n\r") + 1);

        server_logger.info("Сообщение от " + username + ": " + message);

        if (message == "/quit") {
            sendToClient(client.socket, "До свидания!\n");
            break;
        }
        else if (message == "/users") {
            sendToClient(client.socket, getUserList() + "\n");
        }
        else if (message.substr(0, 4) == "/msg") {
            size_t first_space = message.find(' ', 5);
            if (first_space != std::string::npos) {
                std::string to_user = message.substr(5, first_space - 5);
                std::string private_msg = message.substr(first_space + 1);

                if (sendPrivateMessage(username, to_user, private_msg)) {
                    sendToClient(client.socket, "[Лично " + to_user + "]: " + private_msg + "\n");
                    server_logger.info("Личное сообщение от " + username + " к " + to_user + ": " + private_msg);
                }
                else {
                    sendToClient(client.socket, "Пользователь '" + to_user + "' не найден!\n");
                    server_logger.warning("Ошибка личного сообщения: пользователь '" + to_user + "' не найден");
                }
            }
            else {
                sendToClient(client.socket, "Использование: /msg <имя_пользователя> <сообщение>\n");
            }
        }
        else {
            std::string broadcast = "[" + username + "]: " + message + "\n";
            broadcastMessage(broadcast, client.socket);
        }
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        auto it = std::remove_if(clients.begin(), clients.end(),
            [&client](const Client& c) { return c.socket == client.socket; });
        clients.erase(it, clients.end());
        user_sockets.erase(username);
    }

    closesocket(client.socket);

    broadcastMessage("*** " + username + " покинул чат ***\n");
    broadcastMessage(getUserList() + "\n");

    server_logger.info("Клиент удален: " + username);
    std::cout << "Клиент удален: " << username << std::endl;  // Эту строчку можно оставить (показывает отключение)
}

//Инициализация Winsock (обязательно для Windows)
bool initWinsock() {
    WSADATA wsaData;
    // WSAStartup - инициализация библиотеки сокетов
    // MAKEWORD(2, 2) - запрашиваем версию 2.2
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        server_logger.error("Ошибка инициализации Winsock");
        return false;
    }
    return true;
}

//Главная функция - точка входа в программу
int main() {
    // Устанавливаем кодировку UTF-8 для корректного отображения русского языка
    SET_CONSOLE_UTF8();

    // Выводим информацию о запуске сервера
    std::cout << "=== Сервер мессенджера ===" << std::endl;
    std::cout << "Запуск сервера на порту " << PORT << "..." << std::endl;

    server_logger.info("=== СЕРВЕР ЗАПУЩЕН ===");

    // Инициализируем Winsock
    if (!initWinsock()) {
        std::cerr << "Не удалось инициализировать Winsock" << std::endl;
        return 1;
    }

    // --- СОЗДАНИЕ СОКЕТА ---
    // AF_INET = IPv4, SOCK_STREAM = TCP (надёжная передача)
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        server_logger.error("Не удалось создать сокет");
        std::cerr << "Не удалось создать сокет" << std::endl;
        return 1;
    }

    // --- НАСТРОЙКА АДРЕСА СЕРВЕРА ---
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;              // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;      // Слушаем все сетевые интерфейсы (и localhost, и реальный IP)
    server_addr.sin_port = htons(PORT);            // Порт (htons - перевод в сетевой порядок байт)

    // --- ПРИВЯЗКА СОКЕТА К АДРЕСУ ---
    // bind() связывает сокет с адресом и портом
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        server_logger.error("Не удалось привязать сокет");
        std::cerr << "Не удалось привязать сокет" << std::endl;
        closesocket(server_socket);
        return 1;
    }

    // --- НАЧАЛО ПРОСЛУШИВАНИЯ ---
    // listen() переводит сокет в режим ожидания подключений
    // Второй параметр (10) - максимальный размер очереди ожидания
    if (listen(server_socket, 10) == SOCKET_ERROR) {
        server_logger.error("Не удалось начать прослушивание сокета");
        std::cerr << "Не удалось начать прослушивание" << std::endl;
        closesocket(server_socket);
        return 1;
    }

    server_logger.info("Сервер слушает порт " + std::to_string(PORT));
    std::cout << "Сервер запущен. Нажмите Ctrl+C для остановки." << std::endl;
    std::cout << "Ожидание подключений..." << std::endl;

    // --- ОСНОВНОЙ ЦИКЛ СЕРВЕРА (ПРИЁМ ПОДКЛЮЧЕНИЙ) ---
    while (true) {
        sockaddr_in client_addr;           // Структура для хранения адреса клиента
        int addr_len = sizeof(client_addr); // Размер структуры

        // accept() - блокирующий вызов, ждёт новое подключение
        // Возвращает НОВЫЙ сокет для общения с подключившимся клиентом
        SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);

        if (client_socket == INVALID_SOCKET) {
            server_logger.error("Не удалось принять подключение");
            continue;  // Продолжаем ждать другие подключения
        }

        // --- ПОЛУЧАЕМ ИМЯ ПОЛЬЗОВАТЕЛЯ ---
        char username_buffer[256];
        memset(username_buffer, 0, 256);
        int recv_len = recv(client_socket, username_buffer, 255, 0);

        if (recv_len <= 0) {
            closesocket(client_socket);  // Ошибка - закрываем сокет
            continue;
        }

        std::string username(username_buffer);
        username.erase(username.find_last_not_of("\n\r") + 1);  // Удаляем символы перевода строки

        // --- ПРОВЕРКА: НЕ ЗАНЯТО ЛИ ИМЯ? ---
        bool name_taken = false;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (user_sockets.find(username) != user_sockets.end()) {
                name_taken = true;  // Имя уже используется другим клиентом
            }
        }

        if (name_taken) {
            // Отправляем сообщение об ошибке и закрываем соединение
            sendToClient(client_socket, "ОШИБКА: Имя пользователя уже занято!\n");
            closesocket(client_socket);
            continue;
        }

        // --- СОЗДАЁМ СТРУКТУРУ КЛИЕНТА ---
        Client new_client;
        new_client.socket = client_socket;
        new_client.username = username;
        new_client.address = inet_ntoa(client_addr.sin_addr);  // Преобразуем IP из двоичного в строку
        new_client.port = ntohs(client_addr.sin_port);         // Преобразуем порт в нормальный порядок

        // --- ДОБАВЛЯЕМ КЛИЕНТА В СПИСКИ ---
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(new_client);           // Добавляем в вектор клиентов
            user_sockets[username] = client_socket;  // Добавляем в карту (имя -> сокет)
        }

        // Подтверждаем клиенту успешное подключение
        sendToClient(client_socket, "ПОДКЛЮЧЕНО\n");

        // --- ЗАПУСКАЕМ ПОТОК ДЛЯ ОБРАБОТКИ КЛИЕНТА ---
        // Каждый клиент получает свой собственный поток, чтобы не блокировать других
        std::thread client_thread(handleClient, new_client);
        client_thread.detach();  // detach() - поток будет работать автономно, не ждём его завершения
    }

    // --- ЗАВЕРШЕНИЕ РАБОТЫ (СЮДА НИКОГДА НЕ ДОХОДИТ ПРИ НОРМАЛЬНОЙ РАБОТЕ) ---
    closesocket(server_socket);
    WSACleanup();  // Освобождаем ресурсы Winsock

    server_logger.info("=== СЕРВЕР ОСТАНОВЛЕН ===");

    return 0;
}