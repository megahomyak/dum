#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#define PORT "80"

#define try(expr) if(expr != 0) return 1;

struct WorkerInput {
    int client_socket;
};

void* worker_thread(void* input_void) {
    struct WorkerInput* input = input_void;

    char request[512];
    int bytes_read = recv(input->client_socket, request, sizeof(request) - 1, 0);
    if (bytes_read <= 0) goto end;
    request[bytes_read] = '\0';
    char get[4] = "GET ";
    if (strncmp(request, get, sizeof(get)) != 0) goto end;
    char* path_end = strchr(request + sizeof(get), ' ');
    if (path_end == NULL) goto end;
    *path_end = '\0';
    char* path = request + sizeof(get) - 1;
    *path = '.';
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        char 
        send()
    } else {

    }

    end:
    close(input->client_socket);
    free(input);
    return NULL;
}

int main(void) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    pthread_attr_t thread_attributes;
    try(pthread_attr_init(&thread_attributes));
    try(pthread_attr_setstacksize(&thread_attributes, 1024));

    struct addrinfo* result;
    try(getaddrinfo(/*name=*/ NULL, PORT, &hints, &result));
    struct addrinfo* current = result;
    int server_socket;
    for (;;) {
        server_socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);

        if (server_socket != -1) {
            if (bind(server_socket, result->ai_addr, result->ai_addrlen) == -1) close(server_socket);
            else break;
        }

        current = current->ai_next;
        if (current == NULL) return 1;
    }
    freeaddrinfo(current);

    try(listen(server_socket, SOMAXCONN));

    for (;;) {
        int client_socket = accept(server_socket, /*addr=*/ NULL, /*addrlen=*/ NULL);
        if (client_socket == -1) continue;
        struct WorkerInput* input = malloc(sizeof(*input));
        if (input == NULL) return 1;
        pthread_t thread;
        try(pthread_create(&thread, &thread_attributes, worker_thread, input));
        try(pthread_detach(thread));
    }

    return 0;
}
