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
#define smallstring(name, contents) static const char name[sizeof(contents) / (sizeof(*contents)) - 1] = contents
#define write_smallstring(socket, string_contents) { smallstring(data, string_contents); ignore_failure(write(socket, data, sizeof(data))); }
#define die(text) { perror(text); return 1; }
#define ignore_failure(call) if (call == -1) { /* does not matter */ }
#define debug_print(...) { printf(__VA_ARGS__); fflush(stdout); }

struct WorkerInput {
    int client_socket;
};

void send_overloaded(int client_socket) {
    write_smallstring(client_socket,
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "\r\n"
        "The service is overloaded. Please, try requesting again later"
    );
}

void send_not_found(int client_socket) {
    write_smallstring(client_socket,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "\r\n"
        "404 Not Found"
    );
}

struct MimeType {
    const char* name;
    size_t length;
};

void send_file(int file_descriptor, int client_socket, size_t file_size, struct MimeType mime) {
    write_smallstring(client_socket,
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: max-age=31536000, public\r\n"
        "Content-Type: "
    );
    ignore_failure(write(client_socket, mime.name, mime.length));
    write_smallstring(client_socket, "\r\n\r\n");
    ignore_failure(sendfile(client_socket, file_descriptor, /*offset=*/NULL, file_size));
}

void write_full_directory(char* path, char* after_path, char* url_end, int client_socket) {
    ignore_failure(write(client_socket, path, after_path - path));
    write_smallstring(client_socket, "/");
    ignore_failure(write(client_socket, after_path, url_end - after_path));
}

void send_full_directory_redirect(char* path, char* after_path, char* url_end, int client_socket) {
    write_smallstring(client_socket,
        "HTTP/1.1 308 Permanent Redirect\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Location: "
    );
    write_full_directory(path, after_path, url_end, client_socket);
    write_smallstring(client_socket,
        "\r\n\r\n"
        "Redirect to "
    );
    write_full_directory(path, after_path, url_end, client_socket);
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

bool check_for_dotdot(char* path) {
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

#define set_mime_type(mime_str) { \
    smallstring(mime_array, mime_str); \
    mime.name = mime_array; \
    mime.length = sizeof(mime_array); \
}
#define check_ending_macro(ending_str, mime_str) { \
    smallstring(ending_array, ending_str); \
    if (check_ending(path, path_end, ending_array, ending_array + sizeof(ending_array) - 1)) { \
        set_mime_type(mime_str); \
        goto send_file; \
    } \
}
bool check_ending(char* path_beginning, char* path_end, const char* ending_beginning, const char* ending_end) {
    for (;;) {
        if (*path_end != *ending_end) return false;
        if (ending_beginning == ending_end) return true;
        if (path_beginning == path_end) return false;
        --ending_end;
        --path_end;
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

    char* path = &request[3];
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
            if (after_path[-1] == '/') {
                memcpy(after_path, INDEX_POSTFIX, sizeof(INDEX_POSTFIX));
                after_path += sizeof(INDEX_POSTFIX)/sizeof(*INDEX_POSTFIX) - 1;
                result_descriptor = open_and_stat(path, &statbuf);
            } else {
                *after_path = after_path_char;
                send_full_directory_redirect(&path[1], after_path, url_end, input->client_socket);
                goto end;
            }
        }
    }

    if (result_descriptor == -1) send_not_found(input->client_socket);
    else {
        char* path_end = after_path - 1;
        struct MimeType mime;
        check_ending_macro(".aac", "audio/aac");
        check_ending_macro(".abw", "application/x-abiword");
        check_ending_macro(".apng", "image/apng");
        check_ending_macro(".arc", "application/x-freearc");
        check_ending_macro(".avif", "image/avif");
        check_ending_macro(".avi", "video/x-msvideo");
        check_ending_macro(".azw", "application/vnd.amazon.ebook");
        check_ending_macro(".bin", "application/octet-stream");
        check_ending_macro(".bmp", "image/bmp");
        check_ending_macro(".bz", "application/x-bzip");
        check_ending_macro(".bz2", "application/x-bzip2");
        check_ending_macro(".cda", "application/x-cdf");
        check_ending_macro(".csh", "application/x-csh");
        check_ending_macro(".css", "text/css; charset=UTF-8");
        check_ending_macro(".csv", "text/csv; charset=UTF-8");
        check_ending_macro(".doc", "application/msword");
        check_ending_macro(".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
        check_ending_macro(".eot", "application/vnd.ms-fontobject");
        check_ending_macro(".epub", "application/epub+zip");
        check_ending_macro(".gz", "application/gzip");
        check_ending_macro(".gif", "image/gif");
        check_ending_macro(".htm", "text/html; charset=UTF-8");
        check_ending_macro(".html", "text/html; charset=UTF-8");
        check_ending_macro(".ico", "image/vnd.microsoft.icon");
        check_ending_macro(".ics", "text/calendar; charset=UTF-8");
        check_ending_macro(".jar", "application/java-archive");
        check_ending_macro(".jpeg", "image/jpeg");
        check_ending_macro(".jpg", "image/jpeg");
        check_ending_macro(".js", "text/javascript; charset=UTF-8");
        check_ending_macro(".json", "application/json");
        check_ending_macro(".jsonld", "application/ld+json");
        check_ending_macro(".mid", "audio/x-midi");
        check_ending_macro(".midi", "audio/x-midi");
        check_ending_macro(".mjs", "text/javascript; charset=UTF-8");
        check_ending_macro(".mp3", "audio/mpeg");
        check_ending_macro(".mp4", "video/mp4");
        check_ending_macro(".mpeg", "video/mpeg");
        check_ending_macro(".mpkg", "application/vnd.apple.installer+xml");
        check_ending_macro(".odp", "application/vnd.oasis.opendocument.presentation");
        check_ending_macro(".ods", "application/vnd.oasis.opendocument.spreadsheet");
        check_ending_macro(".odt", "application/vnd.oasis.opendocument.text");
        check_ending_macro(".oga", "audio/ogg");
        check_ending_macro(".ogv", "video/ogg");
        check_ending_macro(".ogx", "application/ogg");
        check_ending_macro(".opus", "audio/ogg");
        check_ending_macro(".otf", "font/otf");
        check_ending_macro(".png", "image/png");
        check_ending_macro(".pdf", "application/pdf");
        check_ending_macro(".php", "application/x-httpd-php");
        check_ending_macro(".ppt", "application/vnd.ms-powerpoint");
        check_ending_macro(".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation");
        check_ending_macro(".rar", "application/vnd.rar");
        check_ending_macro(".rtf", "application/rtf");
        check_ending_macro(".sh", "application/x-sh");
        check_ending_macro(".svg", "image/svg+xml");
        check_ending_macro(".tar", "application/x-tar");
        check_ending_macro(".tif", "image/tiff");
        check_ending_macro(".tiff", "image/tiff");
        check_ending_macro(".ts", "video/mp2t");
        check_ending_macro(".ttf", "font/ttf");
        check_ending_macro(".txt", "text/plain; charset=UTF-8");
        check_ending_macro(".vsd", "application/vnd.visio");
        check_ending_macro(".wav", "audio/wav");
        check_ending_macro(".weba", "audio/webm");
        check_ending_macro(".webm", "video/webm");
        check_ending_macro(".webp", "image/webp");
        check_ending_macro(".woff", "font/woff");
        check_ending_macro(".woff2", "font/woff2");
        check_ending_macro(".xhtml", "application/xhtml+xml");
        check_ending_macro(".xls", "application/vnd.ms-excel");
        check_ending_macro(".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
        check_ending_macro(".xml", "application/xml");
        check_ending_macro(".xul", "application/vnd.mozilla.xul+xml");
        check_ending_macro(".zip", "application/x-zip-compressed.");
        check_ending_macro(".3gp", "video/3gpp");
        check_ending_macro(".3g2", "video/3gpp");
        check_ending_macro(".7z", "application/x-7z-compressed");
        set_mime_type("application/octet-stream");
        send_file:
        send_file(result_descriptor, input->client_socket, statbuf.st_size, mime);
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

        pthread_t thread;
        if (input != NULL && pthread_create(&thread, &thread_attributes, worker_thread, input) == 0) {
            input->client_socket = client_socket;
            try("pthread_detach", pthread_detach(thread));
        } else {
            send_overloaded(client_socket);
            close(input->client_socket);
            free(input);
        }
    }

    return 0;
}
