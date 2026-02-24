#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdexcept>

constexpr int PORT = 8080;
constexpr const char* SERVER_IP = "127.0.0.1";
constexpr size_t BUFFER_SIZE = 1024;

int main() {
    int sockfd;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in servaddr;
    ssize_t n;
    socklen_t len;

    try {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            throw std::runtime_error("Ошибка создания сокета");
        }

        std::memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(PORT);

        if (inet_pton(AF_INET, SERVER_IP, &servaddr.sin_addr) <= 0) {
            throw std::runtime_error("Неверный адрес сервера");
        }

        std::cout << "Клиент запущен. Введите сообщение (или 'exit' для выхода):" << std::endl;

        std::string input;
        while (true) {
            std::cout << "> ";
            std::getline(std::cin, input);

            if (input == "exit") {
                break;
            }

            std::strncpy(buffer, input.c_str(), BUFFER_SIZE - 1);
            buffer[BUFFER_SIZE - 1] = '\0';

            sendto(sockfd, buffer, input.length(), 0, 
                   (struct sockaddr *)&servaddr, sizeof(servaddr));

            len = sizeof(servaddr);
            n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, 
                         (struct sockaddr *)&servaddr, &len);
            
            if (n < 0) {
                throw std::runtime_error("Ошибка получения ответа");
            }

            buffer[n] = '\0';
            std::cout << "Ответ от сервера: " << buffer << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        if (sockfd >= 0) close(sockfd);
        return EXIT_FAILURE;
    }

    close(sockfd);
    std::cout << "Клиент завершен." << std::endl;
    return 0;
}