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
    }

    return 0;
}
