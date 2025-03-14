#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>

const char* PORT = "80";

#define try(text, expr) if(expr != 0) die(text);
#define smallstring(name, contents) const char name[sizeof(contents) - 1] = contents
#define die(text) { perror(text); return 1; }
#define ignore_failure(call) if (call == -1) { /* does not matter */ }

struct WorkerInput {
    int client_socket;
};

void send_not_found(int client_socket) {
    smallstring(response,
        "HTTP/1.1 404 Not Found\r\n"
        "\r\n"
        "404 Not Found"
    );
    ignore_failure(write(client_socket, response, sizeof(response)));
}

void send_file(int file_descriptor, int client_socket, size_t file_size) {
    smallstring(heading,
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: max-age=31536000, public\r\n"
        "\r\n"
    );
    ignore_failure(write(client_socket, heading, sizeof(heading)));
    ignore_failure(sendfile(client_socket, file_descriptor, /*offset=*/NULL, file_size));
}

int open_and_stat(char* path, struct stat* statbuf) {
    int file_descriptor = open(path, O_RDONLY);
    if (file_descriptor != -1) {
        if (fstat(file_descriptor, statbuf) == -1) {
            close(file_descriptor);
            file_descriptor = -1;
        }
    }
    return file_descriptor;
}

void* worker_thread(void* input_void) {
    struct WorkerInput* input = input_void;

    #define INDEX_POSTFIX "/index.html"
    #define REQUEST_LINE_SIZE 512
    char request[REQUEST_LINE_SIZE + (sizeof(INDEX_POSTFIX) - 1) + sizeof('\0')];
    int bytes_read = read(input->client_socket, request, REQUEST_LINE_SIZE);
    if (bytes_read <= 0) goto end;
    request[bytes_read] = '\0';

    smallstring(get_prefix, "GET /");
    if (strncmp(request, get_prefix, sizeof(get_prefix)) != 0) goto end;
    char* path = request + sizeof(get_prefix);

    char* path_end = strchr(path, ' ');
    if (path_end == NULL) goto end;
    *path_end = '\0';

    struct stat statbuf;
    int result_descriptor = open_and_stat(path, &statbuf);
    if (result_descriptor != -1) {
        if (S_ISDIR(statbuf.st_mode)) {
            close(result_descriptor);
            memcpy(path_end, INDEX_POSTFIX, sizeof(INDEX_POSTFIX));
            result_descriptor = open_and_stat(path, &statbuf);
        }
    }

    if (result_descriptor == -1) send_not_found(input->client_socket);
    else send_file(result_descriptor, input->client_socket, statbuf.st_size);

    end:
    close(input->client_socket);
    free(input);
    return NULL;
}

void handle_signal(int signal) {
    (void)signal;
    _exit(0);
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    pthread_attr_t thread_attributes;
    try("pthread_attr_init", pthread_attr_init(&thread_attributes));
    try("pthread_attr_setstacksize", pthread_attr_setstacksize(&thread_attributes, PTHREAD_STACK_MIN));

    struct addrinfo* result;
    try("getaddrinfo", getaddrinfo(/*name=*/NULL, PORT, &hints, &result));

    struct addrinfo* current = result;
    int server_socket;
    for (;;) {
        server_socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);

        if (server_socket != -1) {
            int yes = 1;
            try("setsockopt reuseaddr", setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
            if (bind(server_socket, current->ai_addr, current->ai_addrlen) == -1) close(server_socket);
            else break;
        }

        current = current->ai_next;
        if (current == NULL) die("no matching addresses");
    }
    freeaddrinfo(result);

    try("listen", listen(server_socket, SOMAXCONN));

    for (;;) {
        int client_socket = accept(server_socket, /*addr=*/NULL, /*addrlen=*/NULL);
        if (client_socket == -1) continue;

        struct WorkerInput* input = malloc(sizeof(*input));
        if (input == NULL) die("couldn't malloc");
        input->client_socket = client_socket;

        pthread_t thread;
        try("pthread_create", pthread_create(&thread, &thread_attributes, worker_thread, input));
        try("pthread_detach", pthread_detach(thread));
    }

    return 0;
}
