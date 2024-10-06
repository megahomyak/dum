#include <pthread.h>
#include <signal.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>

/* configuration constants begin */
#define HANDLER_THREAD_COUNT 40
#define PORT "80"
/* configuration constants end */

#define NO_JOB -1
#define SHOULD_STOP -2

pthread_mutex_t current_job_guard;
pthread_cond_t current_job_placed;
pthread_cond_t current_job_taken;
int current_job = NO_JOB;

void close_for_sure(int file_descriptor) {
    restart:
    if (close(file_descriptor) == -1 && errno == EINTR) goto restart;
}

int poll_for_sure(int socket, int timeout_ms) {
    struct pollfd pollfd = {
        .fd = socket,
        .events = POLLIN,
    };
    nfds_t pollfd_count = 1;
    int result;
    restart:
    result = poll(&pollfd, pollfd_count, timeout_ms);
    if (result == -1 && errno == EINTR) goto restart;
    return result;
}

int recv_for_sure(int file_descriptor, void* buf, int buf_size) {
    int flags = MSG_DONTWAIT;
    int result;
    restart:
    result = recv(file_descriptor, buf, buf_size, flags);
    if (result == -1 && errno == EINTR) goto restart;
    return result;
}

typedef struct {
    int file_descriptor;
    char buffer[512];
    int seeking_index;
    int _fill_index;
    int _is_ended;
} RecvMore;

void init_recv_more(RecvMore* recv_more, int file_descriptor) {
    recv_more->file_descriptor = file_descriptor;
    recv_more->seeking_index = 0;
    recv_more->_fill_index = 0;
}

int recv_more(RecvMore* context) {
    if (context->_fill_index == context->seeking_index) return -1;
    int flags = 0;
    int remaining_space = sizeof(context->buffer) - context->_fill_index;
    if (remaining_space != 0) {
        int result = recv(context->file_descriptor, context->buffer + context->_fill_index, remaining_space, flags);
        if (result > 0) context->_fill_index += result;
    }
    return context->buffer[context->seeking_index++];
}

void* worker_thread(void* input) {
    pthread_mutex_lock(&current_job_guard);
    for (;;) {
        while (current_job == NO_JOB) pthread_cond_wait(&current_job_placed, &current_job_guard);
        if (current_job == SHOULD_STOP) break;
        int client_socket = current_job;
        current_job = NO_JOB;
        pthread_cond_signal(&current_job_taken);
        pthread_mutex_unlock(&current_job_guard);

        /* serve the client begin */

        RecvMore output;
        init_recv_more(&output, client_socket);
        recv_more(&output);

        for (;;) {
            char get[] = "GET ";
            for (int i = 0; i < sizeof(get); ++i) {
                char c;
                size_t char_amount = 1;
                int flags = 0;
                if (recv(client_socket, &c, char_amount, flags) <= 0) goto stop_serving;
                if (c != get[i]) goto stop_serving;
            }

            char path[512];
            path[0] = '.';
            int i = 1;
            for (;;) {
                char c;
                size_t char_amount = 1;
                int flags = 0;
                if (recv(client_socket, &c, char_amount, flags) <= 0) goto stop_serving;
                if (c == ' ') break;
                ++i;
                if (i == sizeof(path)) goto stop_serving;
            }
        }

        stop_serving:
        close_for_sure(client_socket);

        /* serve the client end */

        pthread_mutex_lock(&current_job_guard);
    }
    pthread_mutex_unlock(&current_job_guard);
    return NULL;
}

void set_signal_handler(void (*handler) (int)) {
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
}

void stop(void) {
    pthread_mutex_lock(&current_job_guard);
    current_job = SHOULD_STOP;
    pthread_cond_broadcast(&current_job_placed);
    pthread_cond_signal(&current_job_taken);
    pthread_mutex_unlock(&current_job_guard);
}

void stop_signal_handler(int signal_identifier) {
    (void) signal_identifier;
    set_signal_handler(SIG_IGN);
    stop();
}

int main(void) {
    #define try(expr) if(expr != 0) return 1;

    pthread_condattr_t* condition_attributes = NULL;
    pthread_mutexattr_t* mutex_attributes = NULL;
    try(pthread_mutex_init(&current_job_guard, mutex_attributes));
    try(pthread_cond_init(&current_job_placed, condition_attributes));
    try(pthread_cond_init(&current_job_taken, condition_attributes));

    set_signal_handler(stop_signal_handler);

    pthread_t threads[HANDLER_THREAD_COUNT];

    pthread_attr_t thread_attributes;
    try(pthread_attr_init(&thread_attributes));
    try(pthread_attr_setstacksize(&thread_attributes, 1024));

    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        void* input = NULL;
        try(pthread_create(&threads[i], &thread_attributes, worker_thread, input));
    }

    /* listen for new connections begin */

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result;
    char* name = NULL;
    try(getaddrinfo(name, PORT, &hints, &result));

    int server_socket = -1;
    struct addrinfo* current = result;
    for (;;) {
        if ((server_socket = socket(current->ai_family, current->ai_socktype | SOCK_NONBLOCK, current->ai_protocol)) == -1) goto next_address;
        if (bind(server_socket, result->ai_addr, result->ai_addrlen) == -1) {
            close_for_sure(server_socket);
            goto next_address;
        };
        break;

        next_address:
        current = current->ai_next;
        if (current == NULL) return 1;
    }

    freeaddrinfo(result);

    try(listen(server_socket, SOMAXCONN));

    for (;;) {
        int timeout_ms = 250;
        int can_listen = poll_in(server_socket, timeout_ms) > 0;

        pthread_mutex_lock(&current_job_guard);
        while (current_job >= 0) pthread_cond_wait(&current_job_taken, &current_job_guard);
        if (current_job == NO_JOB) {
            if (can_listen) {
                struct sockaddr* addr = NULL;
                socklen_t* addr_len = NULL;
                current_job = accept(server_socket, addr, addr_len);
                pthread_cond_signal(&current_job_placed);
            }
        } else if (current_job == SHOULD_STOP) {
            break;
        }
        pthread_mutex_unlock(&current_job_guard);
    }

    /* listen for new connections end */

    close_for_sure(server_socket);

    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        void* output;
        pthread_join(threads[i], &output);
    }

    return 0;
}
