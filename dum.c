#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>

#define PORT "80"

#define try(exit_code, expr) if(expr != 0) return exit_code;
#define smallstring(name, contents) const char name[sizeof(contents) - 1] = contents
#define IGNORE(call) do { call; } while (0)

struct WorkerInput {
    int client_socket;
};

void send_not_found(int client_socket) {
    smallstring(response, "HTTP/1.1 404 Not Found\r\n\r\n");
    if (write(client_socket, response, sizeof(response)) == -1) { /* does not matter */ }
}

void send_regular_file(int file_descriptor, int client_socket, size_t file_size) {
    smallstring(heading,
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: max-age=31536000, public\r\n"
        "\r\n"
    );
    if (write(client_socket, heading, sizeof(heading)) == -1) { /* does not matter */ }
    if (sendfile(client_socket, file_descriptor, /*offset=*/NULL, file_size) == -1) { /* does not matter */ }
}

void* worker_thread(void* input_void) {
    struct WorkerInput* input = input_void;

    #define REQUEST_LINE_SIZE 512
    #define INDEX_HTML_POSTFIX "/index.html"
    char request[REQUEST_LINE_SIZE + sizeof('\0') + (sizeof(INDEX_HTML_POSTFIX) - 1)];
    int bytes_read = read(input->client_socket, request, REQUEST_LINE_SIZE);
    if (bytes_read <= 0) goto end;
    request[bytes_read] = '\0';
    smallstring(get, "GET ");
    if (strncmp(request, get, sizeof(get)) != 0) goto end;
    char* path_end = strchr(request + sizeof(get), ' ');
    if (path_end == NULL) goto end;
    *path_end = '\0';
    char* path = request + sizeof(get) - 1;
    *path = '.';
    send_file:;
    int file_descriptor = open(path, O_RDONLY);
    if (file_descriptor == -1) send_not_found(input->client_socket);
    else {
        struct stat statbuf;
        if (fstat(file_descriptor, &statbuf) == -1) send_not_found(input->client_socket);
        else if (S_ISDIR(statbuf.st_mode)) {
            memcpy(path_end, INDEX_HTML_POSTFIX, sizeof(INDEX_HTML_POSTFIX));
            close(file_descriptor);
            int file_descriptor = open(path, O_RDONLY);
            if (file_descriptor == -1) send_not_found(input->client_socket);
            else {
                struct stat statbuf;
                if (fstat(file_descriptor, &statbuf) == -1) send_not_found(input->client_socket);
                else if (S_ISREG(statbuf.st_mode)) send_regular_file(file_descriptor, input->client_socket, statbuf.st_size);
                else send_not_found(input->client_socket);
                close(file_descriptor);
            }
        } else if (S_ISREG(statbuf.st_mode)) send_regular_file(file_descriptor, input->client_socket, statbuf.st_size);
        else send_not_found(input->client_socket);
        close(file_descriptor);
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
    try(1, pthread_attr_init(&thread_attributes));
    try(2, pthread_attr_setstacksize(&thread_attributes, PTHREAD_STACK_MIN));

    struct addrinfo* result;
    try(3, getaddrinfo(/*name=*/NULL, PORT, &hints, &result));
    struct addrinfo* current = result;
    int server_socket;
    for (;;) {
        server_socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);

        if (server_socket != -1) {
            if (bind(server_socket, result->ai_addr, result->ai_addrlen) == -1) close(server_socket);
            else break;
        }

        current = current->ai_next;
        if (current == NULL) return 4;
    }
    freeaddrinfo(current);

    try(5, listen(server_socket, SOMAXCONN));

    for (;;) {
        int client_socket = accept(server_socket, /*addr=*/NULL, /*addrlen=*/NULL);
        if (client_socket == -1) continue;
        struct WorkerInput* input = malloc(sizeof(*input));
        if (input == NULL) return 6;
        pthread_t thread;
        try(7, pthread_create(&thread, &thread_attributes, worker_thread, input));
        try(8, pthread_detach(thread));
    }

    return 0;
}
