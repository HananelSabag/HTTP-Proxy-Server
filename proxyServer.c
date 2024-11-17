//Hananel Sabag 208755744
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "threadpool.h"
#include "signal.h"

#define BUFFER_SIZE 4096
#define MAX_FILTERS 1024

char blocked_hosts[MAX_FILTERS][BUFFER_SIZE];
int blocked_count = 0;
#define ERROR_DIR "./proxy-files/"


// Function prototypes
void load_filter(const char* filterFilePath);
int is_blocked(const char* host);
void send_error_response(int client_fd, const char* error_code);
int parse_http_request(const char* request, char* host);
void handle_request(void* fd_pointer);
int connect_to_server(const char* host, const char* port);

void load_filter(const char* filterFilePath) {
    FILE* file = fopen(filterFilePath, "r");
    if (!file) {
        perror("Failed to open filter file");
        exit(EXIT_FAILURE);
    }

    while (fgets(blocked_hosts[blocked_count], BUFFER_SIZE, file) != NULL && blocked_count < MAX_FILTERS) {
        size_t ln = strlen(blocked_hosts[blocked_count]) - 1;
        if (blocked_hosts[blocked_count][ln] == '\n') {
            blocked_hosts[blocked_count][ln-1] = '\0';
        }
        blocked_count++;
    }
    fclose(file);
}

int is_blocked(const char* host) {
    for (int i = 0; i < blocked_count; ++i) {
        if (strcmp(host, blocked_hosts[i]) == 0) {
            return 1; // Host is blocked
        }
    }
    return 0; // Host is not blocked
}

void send_error_response(int client_fd, const char* error_code) {
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s%s.txt", ERROR_DIR, error_code); // error_code could be "404", "403", etc.

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        perror("Error opening error response file");
        const char* msg = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(client_fd, msg, strlen(msg), 0);
        return;
    }

    char buffer[BUFFER_SIZE];
    while (fgets(buffer, BUFFER_SIZE, fp) != NULL) {
        send(client_fd, buffer, strlen(buffer), 0);
    }
    fclose(fp);
}


int parse_http_request(const char* request, char* host) {
    const char* hostPrefix = "Host: ";
    const char* found = strstr(request, hostPrefix);
    if (found) {
        found += strlen(hostPrefix);
        const char* end = strstr(found, "\r\n");
        if (end) {
            size_t hostLen = end - found;
            strncpy(host, found, hostLen);
            host[hostLen] = '\0'; // Null-terminate the host string
            return 1; // Successfully parsed
        }
    }
    return 0; // Failed to parse
}

void handle_request(void* fd_pointer) {
    int client_fd = *(int*)fd_pointer;
    free(fd_pointer);

    char buffer[BUFFER_SIZE] = {0};
    ssize_t bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read < 0) {
        perror("recv");
        send_error_response(client_fd, "400"); // Bad Request if error in recv
        close(client_fd);
        return;
    }

    char host[BUFFER_SIZE] = {0};
    if (!parse_http_request(buffer, host)) {
        send_error_response(client_fd, "400"); // Bad Request if parsing fails
        close(client_fd);
        return;
    }
    if (is_blocked(host)) {
        printf("Host %s is blocked.\n", host);
        send_error_response(client_fd, "403"); // Forbidden if host is blocked
    } else {
        printf("Host %s is allowed. Forwarding request...\n", host);
        // Attempt to connect to the actual host
        int server_fd = connect_to_server(host, "80"); // Assuming default HTTP port is 80
        if (server_fd < 0) {
            send_error_response(client_fd, "500");
            close(client_fd);
            return;
        }

        // Forward the request to the server
        if (send(server_fd, buffer, bytes_read, 0) < 0) {
            perror("Error forwarding request to server");
            send_error_response(client_fd, "500");
            close(client_fd);
            close(server_fd);
            return;
        }

        // Receive the response from the server and forward it to the client
        char server_buffer[BUFFER_SIZE];
        ssize_t bytes_received;
        while ((bytes_received = recv(server_fd, server_buffer, BUFFER_SIZE, 0)) > 0) {
            send(client_fd, server_buffer, bytes_received, 0); // Relay server's response to client
        }

        close(server_fd); // Close connection to the server
    }

    close(client_fd);
}

int connect_to_server(const char* host, const char* port) {
    struct addrinfo hints, *servinfo, *p;
    int rv;
    int sockfd;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    // loop through all the results and connect to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                             p->ai_protocol)) == -1) {
            perror("client: socket");
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("client: connect");
            continue;
        }

        break; // if we get here, we must have connected successfully
    }

    if (p == NULL) {
        fprintf(stderr, "client: failed to connect\n");
        return -2;
    }

    freeaddrinfo(servinfo); // all done with this structure
    return sockfd;
}



int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    if (argc != 5) {
        printf("Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    int poolSize = atoi(argv[2]);
    int maxNumberOfRequests = atoi(argv[3]);
    char* filterFilePath = argv[4]; // Path to the filter file

    load_filter(filterFilePath); // Load the filter from the specified file path

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    threadpool* pool = create_threadpool(poolSize);
    if (!pool) {
        fprintf(stderr, "Failed to create thread pool\n");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    while (maxNumberOfRequests--) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int* client_fd = malloc(sizeof(int)); // Allocate memory for client socket descriptor
        if (!client_fd) {
            perror("Failed to allocate memory for client socket descriptor");
            continue;
        }

        *client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (*client_fd == -1) {
            perror("accept");
            free(client_fd);
            continue;
        }

        dispatch(pool, (dispatch_fn)handle_request, client_fd);
    }

    // Cleanup
    destroy_threadpool(pool);
    close(listen_fd);

    return 0;
}
