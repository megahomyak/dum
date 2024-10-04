#include <pthread.h>
#include <signal.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>

#define HANDLER_THREAD_COUNT 20

#define NO_JOB -1
#define SHOULD_STOP -2

#define PORT "80"

#define DIE_AND(new_exit_code, label) { exit_code = new_exit_code; goto label; }

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
    if (pthread_mutex_init(&current_job_guard, mutex_attributes) != 0) DIE_AND(1, exit);
    if (pthread_cond_init(&current_job_changed, condition_attributes) != 0) DIE_AND(2, destroy_current_job_guard);

    set_signal_handler(stop_signal_handler);

    pthread_t threads[HANDLER_THREAD_COUNT];

    pthread_attr_t thread_attributes;
    if (pthread_attr_init(&thread_attributes) != 0) DIE_AND(3, destroy_current_job_changed);
    if (pthread_attr_setstacksize(&thread_attributes, 1024) != 0) DIE_AND(4, destroy_current_job_changed);

    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        void* input = NULL;
        if (pthread_create(&threads[i], &thread_attributes, worker_thread, input) != 0) DIE_AND(4, );
    }

    {
        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        struct addrinfo* result;
        char* name = NULL;
        if (getaddrinfo(name, PORT, &hints, &result) != 0) {
            exit_code = 1;
            stop();
            goto destroy_threads;
        }

        int server_descriptor = -1;
        for (struct addrinfo* current = result; current != NULL; current = current->ai_next) {

        }

        if (server_descriptor == -1) {
            exit_code = 2;
            stop();
            goto destroy_threads;
        }

        freeaddrinfo(result);
    }

    destroy_threads:
    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        void* output;
        pthread_join(threads[i], &output);
    }

    destroy_thread_attributes:
    pthread_attr_destroy(&thread_attributes);

    destroy_current_job_changed:
    pthread_cond_destroy(&current_job_changed);
    destroy_current_job_guard:
    pthread_mutex_destroy(&current_job_guard);

    exit:
    return exit_code;
}
