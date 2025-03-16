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
#include <stdbool.h>

const char* PORT = "80";

#define try(text, expr) if(expr != 0) die(text);
#define smallstring(name, contents) const char name[sizeof(contents) / (sizeof(contents[0])) - 1] = contents
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

void write_full_directory_url(char* path, char* after_path, char* url_end, int client_socket) {
    ignore_failure(write(client_socket, path, after_path - path));
    smallstring(slash_after_directory,
        "/"
    );
    ignore_failure(write(client_socket, slash_after_directory, sizeof(slash_after_directory)));
    ignore_failure(write(client_socket, after_path, url_end - after_path));
}

void send_full_directory_redirect(char* path, char* after_path, char* url_end, int client_socket) {
    smallstring(redirection,
        "HTTP/1.1 308 Permanent Redirect\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Location: "
    );
    ignore_failure(write(client_socket, redirection, sizeof(redirection)));
    write_full_directory_url(path, after_path, url_end, client_socket);
    smallstring(after_location,
        "\r\n"
        "Available at "
    );
    ignore_failure(write(client_socket, after_location, sizeof(after_location)));
    write_full_directory_url(path, after_path, url_end, client_socket);
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

int check_for_dotdot(char* path) {
    enum {
        SAFE,
        UNCERTAIN,
        DOT,
        DOTDOT,
    } current_mark = UNCERTAIN;
    for (;;) {
        char current_char = *path;
        if (current_char == '\0' || current_char == '/') {
            if (current_mark == DOTDOT) return true;
            if (current_char == '\0') return false;
            current_mark = UNCERTAIN;
        } else if (current_mark != SAFE) {
            if (current_char == '.' && current_mark != DOTDOT) {
                if (current_mark == UNCERTAIN) {
                    current_mark = DOT;
                } else if (current_mark == DOT) {
                    current_mark = DOTDOT;
                }
            } else {
                current_mark = SAFE;
            }
        }
        ++path;
    }
}

void* worker_thread(void* input_void) {
    struct WorkerInput* input = input_void;

    #define INDEX_POSTFIX "index.html"
    #define REQUEST_LINE_SIZE 512
    char request[REQUEST_LINE_SIZE + (sizeof(INDEX_POSTFIX) - 1) + sizeof('\0')];
    int bytes_read = read(input->client_socket, request, REQUEST_LINE_SIZE);
    if (bytes_read <= 4 /* not empty, not an error and has at least 4 for "GET." */) goto end;
    request[bytes_read] = '\0';

    char* path = request + 3;
    *path = '.';

    char* url_end = strchr(path, ' ');
    if (url_end == NULL) goto end;
    *url_end = '\0';

    char after_path_char = '\0';
    char* after_path = strchr(path, '?');
    if (after_path != NULL) {
        after_path_char = '?';
        *after_path = '\0';
    }
    else if ((after_path = strchr(path, '#')) != NULL) {
        after_path_char = '#';
        *after_path = '\0';
    }
    else after_path = url_end;

    if (check_for_dotdot(path)) goto end;

    struct stat statbuf;
    int result_descriptor = open_and_stat(path, &statbuf);
    if (result_descriptor != -1) {
        if (S_ISDIR(statbuf.st_mode)) {
            close(result_descriptor);
            if (url_end[-1] == '/') {
                memcpy(url_end, INDEX_POSTFIX, sizeof(INDEX_POSTFIX));
                result_descriptor = open_and_stat(path, &statbuf);
            } else {
                *after_path = after_path_char;
                send_full_directory_redirect(path, after_path, url_end, input->client_socket);
                goto end;
            }
        }
    }

    if (result_descriptor == -1) send_not_found(input->client_socket);
    else {
        send_file(result_descriptor, input->client_socket, statbuf.st_size);
        close(result_descriptor);
    }

    end:
    close(input->client_socket);
    free(input);
    return NULL;
}

void handle_signal(int signal) {
    (void)signal;
    _exit(0);
}

#ifndef DEBUG
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
#endif

#ifdef DEBUG
#define test(expected, string) printf("%d %d - %s\n", expected, check_for_dotdot(string), string)
int main(void) {
    test(0, "");
    test(0, ".");
    test(1, "..");
    test(0, "...");
    test(0, ".../");
    test(0, ".../.");
    test(1, ".../..");
    test(0, ".../..blah");
    test(0, ".../blah..");
    test(1, "../blah..");
    test(0, "/blah..//");
    test(1, "/blah..//..");
    return 0;
}
#endif
