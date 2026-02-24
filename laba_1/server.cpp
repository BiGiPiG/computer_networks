#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdexcept>

constexpr int PORT = 8080;
constexpr size_t BUFFER_SIZE = 1024;

int main() {
    int sockfd;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in servaddr, cliaddr;
    socklen_t len;
    ssize_t n;

    try {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            throw std::runtime_error("Ошибка создания сокета");
        }

        std::memset(&servaddr, 0, sizeof(servaddr));
        std::memset(&cliaddr, 0, sizeof(cliaddr));

        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = INADDR_ANY;
        servaddr.sin_port = htons(PORT);

        if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
            throw std::runtime_error("Ошибка bind");
        }

        std::cout << "Сервер запущен на порту " << PORT << "..." << std::endl;

        while (true) {
            len = sizeof(cliaddr);
            
            n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, 
                         (struct sockaddr *)&cliaddr, &len);
            
            if (n < 0) {
                std::cerr << "Ошибка recvfrom" << std::endl;
                continue;
            }

            buffer[n] = '\0';

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(cliaddr.sin_addr), ip_str, INET_ADDRSTRLEN);
            
            std::cout << "Получено от " << ip_str << ":" 
                      << ntohs(cliaddr.sin_port) 
                      << ": " << buffer << std::endl;

            sendto(sockfd, buffer, n, 0, (struct sockaddr *)&cliaddr, len);
        }
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        if (sockfd >= 0) close(sockfd);
        return EXIT_FAILURE;
    }

    close(sockfd);
    return 0;
}