#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "threadpool.h"
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <errno.h> 

#define BUFFER_SIZE 4096
#define MAX_FILTERS 1024
#define ERROR_DIR "./proxy-files/"

char blocked_hosts[MAX_FILTERS][BUFFER_SIZE];
int blocked_count = 0;

void load_filter(const char* filterFilePath) {
    FILE* file = fopen(filterFilePath, "r");
    if (!file) {
        perror("Failed to open filter file");
        exit(EXIT_FAILURE);
    }

    while (fgets(blocked_hosts[blocked_count], BUFFER_SIZE, file) != NULL && blocked_count < MAX_FILTERS) {
        // Remove trailing newline and carriage return
        char* pos;
        if ((pos = strchr(blocked_hosts[blocked_count], '\n')) != NULL) *pos = '\0';
        if ((pos = strchr(blocked_hosts[blocked_count], '\r')) != NULL) *pos = '\0';
        
        // Skip empty lines
        if (strlen(blocked_hosts[blocked_count]) > 0) {
            blocked_count++;
        }
    }
    fclose(file);
}

int is_blocked(const char* host) {
    if (!host) return 0;
    
    // Clean the input host
    char clean_host[BUFFER_SIZE];
    strncpy(clean_host, host, BUFFER_SIZE-1);
    clean_host[BUFFER_SIZE-1] = '\0';
    
    // Convert to lowercase for case-insensitive comparison
    for(int i = 0; clean_host[i]; i++) {
        clean_host[i] = tolower(clean_host[i]);
    }
    
    for (int i = 0; i < blocked_count; i++) {
        char clean_blocked[BUFFER_SIZE];
        strncpy(clean_blocked, blocked_hosts[i], BUFFER_SIZE-1);
        clean_blocked[BUFFER_SIZE-1] = '\0';
        
        // Convert blocked host to lowercase
        for(int j = 0; clean_blocked[j]; j++) {
            clean_blocked[j] = tolower(clean_blocked[j]);
        }
        
        if (strstr(clean_host, clean_blocked) != NULL) {
            return 1;
        }
    }
    return 0;
}

void send_error_response(int client_fd, const char* error_code) {
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s%s.txt", ERROR_DIR, error_code);
    printf("DEBUG: Sending error %s, opening file: %s\n", error_code, filepath);

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        printf("DEBUG: Error file not found\n");
        const char* default_error = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(client_fd, default_error, strlen(default_error), 0);
        return;
    }

    // Read the entire file into memory
    char response[BUFFER_SIZE * 2] = {0};
    size_t total_read = 0;
    size_t bytes_read;
    
    while ((bytes_read = fread(response + total_read, 1, BUFFER_SIZE, fp)) > 0) {
        total_read += bytes_read;
    }
    
    printf("DEBUG: Sending error response:\n%s\n", response);
    send(client_fd, response, total_read, 0);
    fclose(fp);
}

int parse_http_request(const char* request, char* host, char* method) {
    printf("DEBUG: Parsing request:\n%s\n", request);

    // Parse method
    char* space = strchr(request, ' ');
    if (!space) {
        printf("DEBUG: No space found after method\n");
        return 0;
    }
    size_t method_len = space - request;
    if (method_len >= 32) {
        printf("DEBUG: Method too long\n");
        return 0;
    }
    strncpy(method, request, method_len);
    method[method_len] = '\0';
    printf("DEBUG: Method parsed: %s\n", method);

    // Find Host header
    const char* host_start = strstr(request, "\r\nHost: ");
    if (!host_start) {
        host_start = strstr(request, "\nHost: ");
        if (!host_start) {
            printf("DEBUG: No Host header found\n");
            return 0;
        }
        host_start += 7;
    } else {
        host_start += 8;
    }

    const char* host_end = strstr(host_start, "\r\n");
    if (!host_end) {
        printf("DEBUG: No end of Host header found\n");
        return 0;
    }

    size_t host_len = host_end - host_start;
    strncpy(host, host_start, host_len);
    host[host_len] = '\0';
    printf("DEBUG: Host parsed: %s\n", host);

    return 1;
}

int handle_client(void* arg) {
    printf("DEBUG: New client connection handling started\n");
    
    int client_fd = *(int*)arg;
    free(arg);

    char buffer[BUFFER_SIZE] = {0};
    ssize_t bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read <= 0) {
        printf("DEBUG: Failed to read request (bytes_read: %zd)\n", bytes_read);
        close(client_fd);
        return -1;
    }
    buffer[bytes_read] = '\0';
    printf("DEBUG: Received request (%zd bytes):\n%s\n", bytes_read, buffer);

    char method[32] = {0};
    char host[BUFFER_SIZE] = {0};

    if (!parse_http_request(buffer, host, method)) {
        printf("DEBUG: Failed to parse HTTP request\n");
        send_error_response(client_fd, "400");
        close(client_fd);
        return -1;
    }

    if (strcmp(method, "GET") != 0) {
        printf("DEBUG: Method not supported: %s\n", method);
        send_error_response(client_fd, "501");
        close(client_fd);
        return -1;
    }

    if (is_blocked(host)) {
        printf("DEBUG: Host is blocked: %s\n", host);
        send_error_response(client_fd, "403");
        close(client_fd);
        return 0;
    }
    struct addrinfo hints, *server_info;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, "80", &hints, &server_info) != 0) {
        send_error_response(client_fd, "404");
        close(client_fd);
        return -1;
    }

    int server_fd = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);
    if (server_fd < 0) {
        freeaddrinfo(server_info);
        send_error_response(client_fd, "500");
        close(client_fd);
        return -1;
    }

    if (connect(server_fd, server_info->ai_addr, server_info->ai_addrlen) < 0) {
        freeaddrinfo(server_info);
        close(server_fd);
        send_error_response(client_fd, "502");
        close(client_fd);
        return -1;
    }

    freeaddrinfo(server_info);

    // Forward request to server
    if (send(server_fd, buffer, bytes_read, 0) != bytes_read) {
        close(server_fd);
        send_error_response(client_fd, "500");
        close(client_fd);
        return -1;
    }

    // Forward response to client
    while ((bytes_read = recv(server_fd, buffer, BUFFER_SIZE, 0)) > 0) {
        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read) {
            ssize_t ret = send(client_fd, buffer + bytes_sent, bytes_read - bytes_sent, 0);
            if (ret <= 0) break;
            bytes_sent += ret;
        }
        if (bytes_sent != bytes_read) break;
    }

    close(server_fd);
    close(client_fd);
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 5) {
        fprintf(stderr, "Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    int poolSize = atoi(argv[2]);
    int maxNumberOfRequests = atoi(argv[3]);

    if (port <= 0 || poolSize <= 0 || maxNumberOfRequests <= 0) {
        fprintf(stderr, "Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        exit(EXIT_FAILURE);
    }

    load_filter(argv[4]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    threadpool* pool = create_threadpool(poolSize);
    if (!pool) {
        perror("Failed to create thread pool");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    int requests_handled = 0;
    while (requests_handled < maxNumberOfRequests) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        // Set socket timeouts
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        int* client_fd_ptr = malloc(sizeof(int));
        if (!client_fd_ptr) {
            perror("Failed to allocate memory");
            close(client_fd);
            continue;
        }

        *client_fd_ptr = client_fd;
        dispatch(pool, (dispatch_fn)handle_client, client_fd_ptr);
        requests_handled++;
    }

    destroy_threadpool(pool);
    close(server_fd);
    return 0;
}