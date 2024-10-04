#include <pthread.h>
#include <signal.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>

#define HANDLER_THREAD_COUNT 20

#define NO_JOB -1
#define SHOULD_STOP -2

#define PORT "80"

#define try(expr) if(expr != 0) return 1;

pthread_mutex_t current_job_guard;
pthread_cond_t current_job_changed;
int current_job = NO_JOB;

void* worker_thread(void* input) {
    pthread_mutex_lock(&current_job_guard);
    for (;;) {
        while (current_job == NO_JOB) pthread_cond_wait(&current_job_changed, &current_job_guard);
        if (current_job == SHOULD_STOP) break;
        int client_descriptor = current_job;
        current_job = NO_JOB;
        pthread_mutex_unlock(&current_job_guard);

        /* serve the client */

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
    set_signal_handler(SIG_IGN);
    pthread_mutex_lock(&current_job_guard);
    current_job = SHOULD_STOP;
    pthread_cond_broadcast(&current_job_changed);
    pthread_mutex_unlock(&current_job_guard);
}

void stop_signal_handler(int signal_identifier) {
    (void) signal_identifier;
    stop();
}

int exit_code = 0;

int main(void) {
    pthread_condattr_t* condition_attributes = NULL;
    pthread_mutexattr_t* mutex_attributes = NULL;
    try(pthread_mutex_init(&current_job_guard, mutex_attributes));
    try(pthread_cond_init(&current_job_changed, condition_attributes));

    set_signal_handler(stop_signal_handler);

    pthread_t threads[HANDLER_THREAD_COUNT];

    pthread_attr_t thread_attributes;
    try(pthread_attr_init(&thread_attributes));
    try(pthread_attr_setstacksize(&thread_attributes, 1024));

    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        void* input = NULL;
        try(pthread_create(&threads[i], &thread_attributes, worker_thread, input));
    }

    {
        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        struct addrinfo* result;
        char* name = NULL;
        try(getaddrinfo(name, PORT, &hints, &result));

        int server_descriptor = -1;
        for (struct addrinfo* current = result; current != NULL; current = current->ai_next) {

        }

        if (server_descriptor == -1) return 1;

        freeaddrinfo(result);
    }

    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        void* output;
        pthread_join(threads[i], &output);
    }

    pthread_attr_destroy(&thread_attributes);

    pthread_cond_destroy(&current_job_changed);
    pthread_mutex_destroy(&current_job_guard);

    return exit_code;
}
