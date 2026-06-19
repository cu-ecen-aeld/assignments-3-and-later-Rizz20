#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

int server_fd = -1;
volatile sig_atomic_t caught_sig = 0;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        caught_sig = 1;
    }
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction failed for SIGINT");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction failed for SIGTERM");
        return -1;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        syslog(LOG_ERR, "getaddrinfo failed");
        return -1;
    }

    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd == -1) {
        syslog(LOG_ERR, "socket failed");
        freeaddrinfo(res);
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        syslog(LOG_ERR, "setsockopt failed");
        freeaddrinfo(res);
        return -1;
    }

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) == -1) {
        syslog(LOG_ERR, "bind failed");
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "fork failed");
            return -1;
        }
        if (pid > 0) {
            exit(0);
        }
        if (setsid() < 0) {
            syslog(LOG_ERR, "setsid failed");
            return -1;
        }
        chdir("/");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    }

    if (listen(server_fd, 10) == -1) {
        syslog(LOG_ERR, "listen failed");
        return -1;
    }

    while (!caught_sig) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            break;
        }
        
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", ip_str);
        
        int data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_APPEND, 0644);
        if (data_fd == -1) {
            syslog(LOG_ERR, "open data file failed");
            close(client_fd);
            continue;
        }
        
        size_t buffer_cap = BUFFER_SIZE;
        char *packet = malloc(buffer_cap);
        if (!packet) {
            syslog(LOG_ERR, "malloc failed");
            close(data_fd);
            close(client_fd);
            continue;
        }
        size_t packet_size = 0;
        
        char buffer[BUFFER_SIZE];
        ssize_t bytes_recv;
        
        while (!caught_sig) {
            bytes_recv = recv(client_fd, buffer, BUFFER_SIZE, 0);
            if (bytes_recv == -1) {
                if (errno == EINTR) {
                    continue;
                }
                syslog(LOG_ERR, "recv failed: %s", strerror(errno));
                break; 
            }
            if (bytes_recv == 0) {
                break; 
            }

            int memory_error = 0;
            for (ssize_t i = 0; i < bytes_recv; i++) {
                if (packet_size >= buffer_cap) {
                    buffer_cap *= 2;
                    char *new_packet = realloc(packet, buffer_cap);
                    if (!new_packet) {
                        syslog(LOG_ERR, "realloc failed");
                        free(packet);
                        packet = NULL;
                        memory_error = 1;
                        break;
                    }
                    packet = new_packet;
                }
                packet[packet_size++] = buffer[i];
                
                if (buffer[i] == '\n') {
                    if (write(data_fd, packet, packet_size) == -1) {
                        syslog(LOG_ERR, "write failed: %s", strerror(errno));
                    }
                    
                    lseek(data_fd, 0, SEEK_SET);
                    char read_buf[BUFFER_SIZE];
                    ssize_t bytes_read;
                    while ((bytes_read = read(data_fd, read_buf, BUFFER_SIZE)) > 0) {
                        ssize_t sent = 0;
                        while (sent < bytes_read) {
                            ssize_t n = send(client_fd, read_buf + sent, bytes_read - sent, 0);
                            if (n == -1) {
                                if (errno == EINTR) continue;
                                syslog(LOG_ERR, "send failed: %s", strerror(errno));
                                break;
                            }
                            sent += n;
                        }
                    }
                    
                    packet_size = 0;
                }
            }
            if (memory_error) {
                break;
            }
        }
        
        if (packet) {
            free(packet);
        }
        
        close(data_fd);
        syslog(LOG_INFO, "Closed connection from %s", ip_str);
        close(client_fd);
    }

    remove(DATA_FILE);
    if (server_fd != -1) close(server_fd);
    closelog();
    return 0;
}
