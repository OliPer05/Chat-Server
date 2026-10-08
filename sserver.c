#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE 2048
#define MAX_CLIENTS 100
#define MAX_NAME 64

typedef struct {
    int fd;
    char name[MAX_NAME + 1];
} client_slot;

static client_slot clients[MAX_CLIENTS];
static int num_clients;
static pthread_mutex_t clients_mu = PTHREAD_MUTEX_INITIALIZER;

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

static int send_text(int fd, const char *text) {
    return write_all(fd, text, strlen(text)) < 0 ? -1 : 0;
}

static ssize_t read_line(int fd, char *buf, size_t max_len) {
    size_t used = 0;

    if (max_len == 0) {
        return -1;
    }

    while (used + 1 < max_len) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0) {
            if (used == 0) {
                return 0;
            }
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (c == '\r') {
            continue;
        }

        buf[used++] = c;
        if (c == '\n') {
            break;
        }
    }

    buf[used] = '\0';
    return (ssize_t)used;
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

static int find_user_index(const char *name) {
    for (int i = 0; i < num_clients; i++) {
        if (strcmp(clients[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void remove_slot_at(int idx) {
    if (idx < 0 || idx >= num_clients) {
        return;
    }
    for (int j = idx; j < num_clients - 1; j++) {
        clients[j] = clients[j + 1];
    }
    num_clients--;
}

static void send_to_index_locked(int idx, const char *msg) {
    if (idx < 0 || idx >= num_clients) {
        return;
    }
    if (send_text(clients[idx].fd, msg) < 0) {
        /* ignore; peer may have vanished */
    }
}

static const char *payload_after_cmd(const char *line, const char *cmd) {
    const char *p = line;

    p += strspn(p, " \t");
    size_t n = strlen(cmd);
    if (strncmp(p, cmd, n) != 0) {
        return NULL;
    }
    p += n;
    if (*p != '\0' && *p != ' ' && *p != '\t') {
        return NULL;
    }
    p += strspn(p, " \t");
    return p;
}

static int parse_send(const char *line, char *target, size_t tlen, char *msg,
                      size_t mlen) {
    const char *p = line;

    p += strspn(p, " \t");
    if (strncmp(p, "send", 4) != 0) {
        return -1;
    }
    p += 4;
    if (*p != '\0' && *p != ' ' && *p != '\t') {
        return -1;
    }
    p += strspn(p, " \t");
    size_t tl = strcspn(p, " \t\n");
    if (tl == 0) {
        return -1;
    }
    if (tl >= tlen) {
        tl = tlen - 1;
    }
    memcpy(target, p, tl);
    target[tl] = '\0';
    p += tl;
    p += strspn(p, " \t");
    if (*p == '\0') {
        return -1;
    }
    strncpy(msg, p, mlen - 1);
    msg[mlen - 1] = '\0';
    trim_newline(msg);
    return 0;
}

static void leave_session(const char *myname, int connfd) {
    char notify[BUF_SIZE];

    pthread_mutex_lock(&clients_mu);
    int idx = find_user_index(myname);
    if (idx < 0) {
        pthread_mutex_unlock(&clients_mu);
        close(connfd);
        return;
    }

    remove_slot_at(idx);

    int n = num_clients;
    pthread_mutex_unlock(&clients_mu);

    snprintf(notify, sizeof(notify), "*** %s left (%d user%s online).\n", myname,
             n, n == 1 ? "" : "s");

    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < num_clients; i++) {
        send_to_index_locked(i, notify);
    }
    pthread_mutex_unlock(&clients_mu);

    close(connfd);
}

static void *client_thread(void *arg) {
    int connfd = *(int *)arg;
    free(arg);

    char line[BUF_SIZE];
    char myname[MAX_NAME + 1];
    char target[MAX_NAME + 1];
    char msg[BUF_SIZE];
    char out[BUF_SIZE * 2];

    ssize_t n = read_line(connfd, line, sizeof(line));
    if (n <= 0) {
        close(connfd);
        return NULL;
    }
    trim_newline(line);
    if (line[0] == '\0' || strlen(line) >= sizeof(myname)) {
        send_text(connfd, "Invalid username.\n");
        close(connfd);
        return NULL;
    }
    strcpy(myname, line);

    pthread_mutex_lock(&clients_mu);
    if (num_clients >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mu);
        send_text(connfd, "Server full.\n");
        close(connfd);
        return NULL;
    }
    if (find_user_index(myname) >= 0) {
        pthread_mutex_unlock(&clients_mu);
        send_text(connfd, "Username already taken.\n");
        close(connfd);
        return NULL;
    }

    int my_index = num_clients;
    clients[my_index].fd = connfd;
    strcpy(clients[my_index].name, myname);
    num_clients++;
    int total = num_clients;
    pthread_mutex_unlock(&clients_mu);

    snprintf(out, sizeof(out), "*** %s joined (%d user%s online).\n", myname,
             total, total == 1 ? "" : "s");

    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < num_clients; i++) {
        if (strcmp(clients[i].name, myname) != 0) {
            send_to_index_locked(i, out);
        }
    }
    pthread_mutex_unlock(&clients_mu);

    snprintf(out, sizeof(out),
             "Welcome %s!\n"
             "Commands:\n"
             "  list                 - who is online\n"
             "  send <user> <msg>    - private message\n"
             "  broadcast <msg>      - everyone (including you)\n"
             "  random <msg>         - one random online user\n"
             "  close                - disconnect\n",
             myname);
    send_text(connfd, out);

    while (read_line(connfd, line, sizeof(line)) > 0) {
        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }

        char linecopy[BUF_SIZE];
        strncpy(linecopy, line, sizeof(linecopy));
        linecopy[sizeof(linecopy) - 1] = '\0';
        char *cmd = strtok(linecopy, " \t\n");
        if (!cmd) {
            continue;
        }

        if (strcmp(cmd, "list") == 0) {
            pthread_mutex_lock(&clients_mu);
            out[0] = '\0';
            for (int i = 0; i < num_clients; i++) {
                strncat(out, clients[i].name, sizeof(out) - strlen(out) - 1);
                if (i + 1 < num_clients) {
                    strncat(out, " ", sizeof(out) - strlen(out) - 1);
                }
            }
            strncat(out, "\n", sizeof(out) - strlen(out) - 1);
            send_to_index_locked(find_user_index(myname), out);
            pthread_mutex_unlock(&clients_mu);
        } else if (strcmp(cmd, "send") == 0) {
            if (parse_send(line, target, sizeof(target), msg, sizeof(msg)) < 0) {
                pthread_mutex_lock(&clients_mu);
                send_to_index_locked(find_user_index(myname),
                                     "Usage: send <user> <message>\n");
                pthread_mutex_unlock(&clients_mu);
                continue;
            }
            snprintf(out, sizeof(out), "%s says: %s\n", myname, msg);
            char ack[BUF_SIZE];

            pthread_mutex_lock(&clients_mu);
            int sender_idx = find_user_index(myname);
            int tgt = find_user_index(target);
            if (tgt >= 0) {
                send_to_index_locked(tgt, out);
                snprintf(ack, sizeof(ack), "Message sent to %s.\n", target);
                send_to_index_locked(sender_idx, ack);
            } else {
                snprintf(ack, sizeof(ack), "User '%s' is not online.\n", target);
                send_to_index_locked(sender_idx, ack);
            }
            pthread_mutex_unlock(&clients_mu);
        } else if (strcmp(cmd, "broadcast") == 0) {
            const char *payload = payload_after_cmd(line, "broadcast");
            if (!payload) {
                pthread_mutex_lock(&clients_mu);
                send_to_index_locked(find_user_index(myname),
                                     "Usage: broadcast <message>\n");
                pthread_mutex_unlock(&clients_mu);
                continue;
            }
            snprintf(out, sizeof(out), "[broadcast] %s: %s\n", myname, payload);

            pthread_mutex_lock(&clients_mu);
            for (int i = 0; i < num_clients; i++) {
                send_to_index_locked(i, out);
            }
            pthread_mutex_unlock(&clients_mu);
        } else if (strcmp(cmd, "random") == 0) {
            const char *payload = payload_after_cmd(line, "random");
            if (!payload) {
                pthread_mutex_lock(&clients_mu);
                send_to_index_locked(find_user_index(myname),
                                     "Usage: random <message>\n");
                pthread_mutex_unlock(&clients_mu);
                continue;
            }

            pthread_mutex_lock(&clients_mu);
            if (num_clients == 0) {
                pthread_mutex_unlock(&clients_mu);
                continue;
            }
            int r = rand() % num_clients;
            snprintf(out, sizeof(out), "[random to you] %s: %s\n", myname,
                     payload);
            send_to_index_locked(r, out);
            snprintf(out, sizeof(out), "Random delivery to %s.\n",
                     clients[r].name);
            send_to_index_locked(find_user_index(myname), out);
            pthread_mutex_unlock(&clients_mu);
        } else if (strcmp(cmd, "close") == 0) {
            send_text(connfd, "Goodbye.\n");
            leave_session(myname, connfd);
            return NULL;
        } else {
            pthread_mutex_lock(&clients_mu);
            send_to_index_locked(
                find_user_index(myname),
                "Unknown command. Try: list, send, broadcast, random, close\n");
            pthread_mutex_unlock(&clients_mu);
        }
    }

    leave_session(myname, connfd);
    return NULL;
}

int main(int argc, char *argv[]) {
    int listenfd;
    int connfd;
    struct sockaddr_in serv_addr;

    signal(SIGPIPE, SIG_IGN);

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int start_port = 5000;
    if (argc == 2) {
        start_port = atoi(argv[1]);
        if (start_port <= 0 || start_port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        pexit("socket");
    }

    int opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        pexit("setsockopt");
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int port = start_port - 1;
    do {
        port++;
        serv_addr.sin_port = htons(port);
    } while (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0 &&
             port < 65535);

    if (port >= 65535) {
        pexit("bind");
    }

    printf("IM server listening on port %d\n", port);
    fflush(stdout);

    if (listen(listenfd, 16) < 0) {
        pexit("listen");
    }

    srand((unsigned int)(time(NULL) ^ (unsigned)getpid()));

    while (1) {
        connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        int *fdptr = malloc(sizeof(int));
        if (!fdptr) {
            close(connfd);
            continue;
        }
        *fdptr = connfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, fdptr) != 0) {
            free(fdptr);
            close(connfd);
            perror("pthread_create");
            continue;
        }
        pthread_detach(tid);
    }
}
