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
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>
#include <sys/time.h>

#define PORT "9000"
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"
#else
#define DATA_FILE "/var/tmp/aesdsocketdata"
#endif
#define BUFFER_SIZE 1024

int server_fd = -1;
volatile sig_atomic_t caught_sig = 0;
pthread_mutex_t aesddata_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct thread_data_s thread_data_t;
struct thread_data_s {
    pthread_t thread;
    int client_fd;
    char ip_str[INET_ADDRSTRLEN];
    bool thread_complete_success;
    SLIST_ENTRY(thread_data_s) entries;
};

SLIST_HEAD(slisthead, thread_data_s) head;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        caught_sig = 1;
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

void* timer_thread_func(void* arg) {
    while (!caught_sig) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10000000; // 10ms
        int ticks = 1000;
        while (ticks > 0 && !caught_sig) {
            nanosleep(&ts, NULL);
            ticks--;
        }

        if (caught_sig) break;

        time_t t = time(NULL);
        struct tm *tmp = localtime(&t);
        if (tmp == NULL) {
            continue;
        }

        char outstr[200];
        size_t s = strftime(outstr, sizeof(outstr), "%a, %d %b %Y %T %z", tmp);
        if (s == 0) continue;

        char timestamp_str[256];
        snprintf(timestamp_str, sizeof(timestamp_str), "timestamp:%s\n", outstr);

        pthread_mutex_lock(&aesddata_mutex);
        int data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_APPEND, 0644);
        if (data_fd != -1) {
            if (write(data_fd, timestamp_str, strlen(timestamp_str)) == -1) {
                syslog(LOG_ERR, "write failed: %s", strerror(errno));
            }
            close(data_fd);
        } else {
            syslog(LOG_ERR, "open failed: %s", strerror(errno));
        }
        pthread_mutex_unlock(&aesddata_mutex);
    }
    return NULL;
}

void* client_thread_func(void* thread_param) {
    thread_data_t *thread_data = (thread_data_t*)thread_param;

    size_t buffer_cap = BUFFER_SIZE;
    char *packet = malloc(buffer_cap);
    if (!packet) {
        syslog(LOG_ERR, "malloc failed");
        close(thread_data->client_fd);
        thread_data->thread_complete_success = true;
        return thread_param;
    }
    size_t packet_size = 0;
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_recv;
    
    while (!caught_sig) {
        bytes_recv = recv(thread_data->client_fd, buffer, BUFFER_SIZE, 0);
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
                pthread_mutex_lock(&aesddata_mutex);
                int data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_APPEND, 0644);
                if (data_fd == -1) {
                    syslog(LOG_ERR, "open data file failed");
                    pthread_mutex_unlock(&aesddata_mutex);
                    continue;
                }

                if (write(data_fd, packet, packet_size) == -1) {
                    syslog(LOG_ERR, "write failed: %s", strerror(errno));
                }
                
                lseek(data_fd, 0, SEEK_SET);
                char read_buf[BUFFER_SIZE];
                ssize_t bytes_read;
                int send_error = 0;
                while ((bytes_read = read(data_fd, read_buf, BUFFER_SIZE)) > 0) {
                    ssize_t sent = 0;
                    while (sent < bytes_read) {
                        ssize_t n = send(thread_data->client_fd, read_buf + sent, bytes_read - sent, 0);
                        if (n == -1) {
                            if (errno == EINTR) continue;
                            syslog(LOG_ERR, "send failed: %s", strerror(errno));
                            send_error = 1;
                            break;
                        }
                        sent += n;
                    }
                    if (send_error) break;
                }
                if (bytes_read == -1) {
                    syslog(LOG_ERR, "read failed: %s", strerror(errno));
                }
                close(data_fd);
                pthread_mutex_unlock(&aesddata_mutex);
                
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
    
    syslog(LOG_INFO, "Closed connection from %s", thread_data->ip_str);
    close(thread_data->client_fd);
    thread_data->thread_complete_success = true;

    return thread_param;
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

#if !USE_AESD_CHAR_DEVICE
    remove(DATA_FILE);
#endif

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

    SLIST_INIT(&head);

#if !USE_AESD_CHAR_DEVICE
    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, timer_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "pthread_create failed for timer_thread");
    }
#endif

    while (!caught_sig) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (caught_sig) break; // In case shutdown caused it to return with error
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            break;
        }
        
        thread_data_t *new_thread = malloc(sizeof(thread_data_t));
        if (!new_thread) {
            close(client_fd);
            continue;
        }
        new_thread->client_fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, new_thread->ip_str, INET_ADDRSTRLEN);
        new_thread->thread_complete_success = false;

        syslog(LOG_INFO, "Accepted connection from %s", new_thread->ip_str);

        if (pthread_create(&new_thread->thread, NULL, client_thread_func, new_thread) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            free(new_thread);
            close(client_fd);
            continue;
        }

        SLIST_INSERT_HEAD(&head, new_thread, entries);

        thread_data_t *current = SLIST_FIRST(&head);
        while (current != NULL) {
            thread_data_t *next = SLIST_NEXT(current, entries);
            if (current->thread_complete_success) {
                pthread_join(current->thread, NULL);
                SLIST_REMOVE(&head, current, thread_data_s, entries);
                free(current);
            }
            current = next;
        }
    }

    if (server_fd != -1) close(server_fd);

    thread_data_t *current = SLIST_FIRST(&head);
    while (current != NULL) {
        thread_data_t *next = SLIST_NEXT(current, entries);
        pthread_join(current->thread, NULL);
        SLIST_REMOVE(&head, current, thread_data_s, entries);
        free(current);
        current = next;
    }
    
#if !USE_AESD_CHAR_DEVICE
    pthread_join(timer_thread, NULL);
    remove(DATA_FILE);
#endif
    closelog();
    pthread_mutex_destroy(&aesddata_mutex);
    return 0;
}
