#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE 1024

static void pexit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static ssize_t write_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }

    return (ssize_t)sent;
}

int main(int argc, char *argv[]) {
    int sockfd;
    struct sockaddr_in serv_addr;
    char buf[BUF_SIZE];

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server-ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        pexit("socket");
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP: %s\n", argv[1]);
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        pexit("connect");
    }

    printf("Connected. Send your username as the first line, then IM commands.\n");
    fflush(stdout);

    fd_set readfds;
    int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sockfd, &readfds);

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            pexit("select");
        }

        if (FD_ISSET(sockfd, &readfds)) {
            ssize_t n = read(sockfd, buf, sizeof(buf) - 1);
            if (n == 0) {
                printf("\nServer closed connection.\n");
                break;
            }
            if (n < 0) {
                pexit("read");
            }
            buf[n] = '\0';
            fputs(buf, stdout);
            fflush(stdout);
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(buf, sizeof(buf), stdin)) {
                break;
            }

            if (write_all(sockfd, buf, strlen(buf)) < 0) {
                pexit("write");
            }
        }
    }

    close(sockfd);
    return 0;
}
