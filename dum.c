#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>

#define debug_print(format, ...) { printf(format "\n", __VA_ARGS__); fflush(stdout); }
#define debug_print1(string) debug_print("%s", string)

ssize_t recv_all(int socket, void* buf, size_t bufsize) {
    ssize_t bytes_received_total = 0;
    for (;;) {
        size_t nbytes = bufsize - bytes_received_total;
        if (nbytes == 0) break;
        ssize_t bytes_received_current = recv(socket, buf + bytes_received_total, nbytes, MSG_DONTWAIT);
        debug_print("recv_all %zi %d %d", bytes_received_current, socket, errno);
        perror("recv_all_2");
        debug_print("bytes_received_current = %zi", bytes_received_current);
        if (bytes_received_current == 0) break;
        if (bytes_received_current < 0) {
            debug_print("... %d %d", errno, EINTR);
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        bytes_received_total += bytes_received_current;
    }
    debug_print("end of recv_all %zi", bytes_received_total);
    return bytes_received_total;
}

ssize_t send_all(int socket, const void* buf, size_t bufsize) {
    ssize_t bytes_sent_total = 0;
    for (;;) {
        size_t nbytes = bufsize - bytes_sent_total;
        if (nbytes == 0) break;
        ssize_t bytes_sent_current = send(socket, buf + bytes_sent_total, nbytes, MSG_DONTWAIT);
        if (bytes_sent_current == 0) return -1;
        if (bytes_sent_current < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        bytes_sent_total += bytes_sent_current;
    }
    return bytes_sent_total;
}

ssize_t send_full_file(int socket, int filefd, size_t file_size) {
    off_t offset = 0;
    for (;;) {
        if (file_size - offset == 0) break;
        ssize_t bytes_sent_current = sendfile(socket, filefd, &offset, file_size);
        if (bytes_sent_current == 0) return -1;
        if (bytes_sent_current < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
    }
    return 0;
}

#define try_errno(text, expr) if(expr != 0) die_errno(text);
#define smallstring(name, contents) static const char name[sizeof(contents) / (sizeof(*contents)) - 1] = contents
#define send_smallstring(socket, string_contents) { smallstring(data, string_contents); ignore_failure(send_all(socket, data, sizeof(data))); }
#define die_errno(text) { perror(text); return 1; }
#define die(text) { fprintf(stderr, "%s\n", text); return 1; }
#define ignore_failure(call) if (call < 0) { /* does not matter */ }

struct WorkerInput {
    int client_socket;
};

#define send_generic_headers(client_socket, status_code, content_length) { \
    char content_length_buffer[256]; \
    int content_length_buffer_size = snprintf(content_length_buffer, sizeof(content_length_buffer) - 2, "%zu", content_length); \
    send_smallstring(client_socket, \
        "HTTP/1.1 " status_code "\r\n" \
        "Connection: close\r\n" \
        "Content-Length: "); \
    content_length_buffer[content_length_buffer_size++] = '\r'; \
    content_length_buffer[content_length_buffer_size++] = '\n'; \
    ignore_failure(send_all(client_socket, content_length_buffer, content_length_buffer_size)); \
}

#define send_content_type_text_plain(client_socket) { \
    send_smallstring(client_socket, "Content-Type: text/plain; charset=UTF-8\r\n"); \
}

#define send_text(client_socket, status_code, message) { \
    send_generic_headers(client_socket, status_code, sizeof(message) - 1); \
    send_content_type_text_plain(client_socket); \
    send_smallstring(client_socket, \
        "\r\n" \
        "\r\n" \
        message \
    ); \
}

void send_overloaded(int client_socket) {
    send_text(client_socket, "503 Service Unavailable", "The service is overloaded. Please, try requesting again later");
}

void send_not_found(int client_socket) {
    send_text(client_socket, "404 Not Found", "File not found");
}

struct MimeType {
    const char* name;
    size_t length;
};

void send_file(int file_descriptor, int client_socket, size_t file_size, struct MimeType mime) {
    send_generic_headers(client_socket, "200 OK", file_size);
    send_smallstring(client_socket, "Content-Type: ");
    ignore_failure(send_all(client_socket, mime.name, mime.length));
    send_smallstring(client_socket, "\r\n\r\n");
    ignore_failure(send_full_file(client_socket, file_descriptor, file_size));
}

void send_full_directory(char* path, char* after_path, char* url_end, int client_socket) {
    ignore_failure(send_all(client_socket, path, after_path - path));
    send_smallstring(client_socket, "/");
    ignore_failure(send_all(client_socket, after_path, url_end - after_path));
}

void send_full_directory_redirect(char* path, char* after_path, char* url_end, int client_socket) {
    send_generic_headers(client_socket, "308 Permanent Redirect", sizeof("Redirect to ") + url_end - path + 1);
    send_content_type_text_plain(client_socket);
    send_smallstring(client_socket, "Location: ");
    send_full_directory(path, after_path, url_end, client_socket);
    send_smallstring(client_socket,
        "\r\n\r\n"
        "Redirect to "
    );
    send_full_directory(path, after_path, url_end, client_socket);
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
    debug_print1("g");
    struct WorkerInput* input = input_void;

    #define INDEX_POSTFIX "index.html"
    #define REQUEST_LINE_SIZE 512
    char request[REQUEST_LINE_SIZE + (sizeof(INDEX_POSTFIX) - 1) + sizeof('\0')];
    ssize_t bytes_read_total = recv_all(input->client_socket, request, REQUEST_LINE_SIZE);
    if (bytes_read_total < 4) goto end; /* has at least 4 for "GET." */
    debug_print1("l");
    debug_print("%ld", bytes_read_total);
    debug_print1("a");
    request[bytes_read_total] = '\0';

    char* path = &request[3];
    *path = '.';

    char* url_end = strchr(path, ' ');
    if (url_end == NULL) goto end;
    debug_print1("b");
    *url_end = '\0';
    debug_print1(url_end);

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
    debug_print("1 %s", path);
    if (result_descriptor != -1) {
        if (S_ISDIR(statbuf.st_mode)) {
            close(result_descriptor);
            if (after_path[-1] == '/') {
                memcpy(after_path, INDEX_POSTFIX, sizeof(INDEX_POSTFIX));
                after_path += sizeof(INDEX_POSTFIX)/sizeof(*INDEX_POSTFIX) - 1;
                debug_print("2 %s", path);
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
        debug_print1("c");
        send_file(result_descriptor, input->client_socket, statbuf.st_size, mime);
        debug_print1("aftersend");
        close(result_descriptor);
    }

    end:
    close(input->client_socket);
    free(input);
    debug_print1("almostend");
    return NULL;
}

void handle_signal(int signal) {
    (void)signal;
    _exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) die("There should only be one argument, and that should be the port the program will connect to");
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    pthread_attr_t thread_attributes;
    try_errno("pthread_attr_init", pthread_attr_init(&thread_attributes));
    try_errno("pthread_attr_setstacksize", pthread_attr_setstacksize(&thread_attributes, PTHREAD_STACK_MIN));

    struct addrinfo* result;
    try_errno("getaddrinfo", getaddrinfo(/*name=*/NULL, argv[1], &hints, &result));

    struct addrinfo* current = result;
    int server_socket;
    for (;;) {
        server_socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);

        if (server_socket != -1) {
            int yes = 1;
            try_errno("setsockopt reuseaddr", setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
            if (bind(server_socket, current->ai_addr, current->ai_addrlen) == -1) close(server_socket);
            else break;
        }

        current = current->ai_next;
        if (current == NULL) die_errno("no matching addresses");
    }
    freeaddrinfo(result);

    try_errno("listen", listen(server_socket, SOMAXCONN));

    for (;;) {
        int client_socket = accept(server_socket, /*addr=*/NULL, /*addrlen=*/NULL);
        if (client_socket == -1) {
            if (errno == ECONNABORTED || errno == EMFILE || errno == ENFILE || errno == EINTR) continue;
            else die_errno("unexpected error on client_socket");
        }

        struct WorkerInput* input = malloc(sizeof(*input));

        pthread_t thread;
        if (input != NULL) {
            input->client_socket = client_socket;
        }
        if (input != NULL && pthread_create(&thread, &thread_attributes, worker_thread, input) == 0) {
            try_errno("pthread_detach", pthread_detach(thread));
        } else {
            send_overloaded(client_socket);
            close(input->client_socket);
            free(input);
        }
    }

    return 0;
}
