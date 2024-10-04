#include <pthread.h>
#include <signal.h>

#define HANDLER_THREAD_COUNT 20

typedef struct {
    int client_descriptor;
    pthread_mutex_t client_descriptor_mutex;
    pthread_cond_t thread_state_updated;
    pthread_t thread;
} ThreadContext;

ThreadContext threads[HANDLER_THREAD_COUNT];

pthread_mutex_t should_stop_mutex;
int should_stop = 0;

void* worker_thread(void* context_void_pointer) {
    ThreadContext* context_pointer = (ThreadContext*) context_void_pointer;
    pthread_mutex_lock(&context_pointer->client_descriptor_mutex);
    for (;;) {
        int client_descriptor;
        for (;;) {
            pthread_mutex_lock(&should_stop_mutex);
            int should_stop_ = should_stop;
            pthread_mutex_unlock(&should_stop_mutex);
            if (should_stop_) {
                goto stop_worker;
            }
            if ((client_descriptor = context_pointer->client_descriptor) != -1) {
                goto respond;
            }
            pthread_cond_wait(&context_pointer->thread_state_updated, &context_pointer->client_descriptor_mutex);
        }
        respond:
        pthread_mutex_unlock(&context_pointer->client_descriptor_mutex);

        /* Send the file here */

        pthread_mutex_lock(&context_pointer->client_descriptor_mutex);
    }
    stop_worker:
    pthread_mutex_unlock(&context_pointer->client_descriptor_mutex);
    return NULL;
}

void set_signal_handler(void (*handler) (int)) {
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
}

void stop_signal_handler(int signal_identifier) {
    (void)signal_identifier;
    set_signal_handler(SIG_IGN);
    pthread_mutex_lock(&should_stop_mutex);
    should_stop = 1;
    pthread_mutex_unlock(&should_stop_mutex);
    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        pthread_cond_signal(&threads[i].thread_state_updated);
    }
}

int main(void) {
    pthread_condattr_t condition_attributes;
    pthread_condattr_init(&condition_attributes);
    pthread_attr_t thread_attributes;
    pthread_attr_init(&thread_attributes);
    pthread_attr_setstacksize(&thread_attributes, 1024);
    pthread_mutexattr_t mutex_attributes;
    pthread_mutexattr_init(&mutex_attributes);

    pthread_mutex_init(&should_stop_mutex, &mutex_attributes);
    set_signal_handler(stop_signal_handler);

    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        threads[i].client_descriptor = -1;
        pthread_mutex_init(&threads[i].client_descriptor_mutex, &mutex_attributes);
        pthread_cond_init(&threads[i].thread_state_updated, &condition_attributes);
        pthread_create(&threads[i].thread, &thread_attributes, worker_thread, &threads[i]);
    }

    for (int i = 0; i < HANDLER_THREAD_COUNT; ++i) {
        void* thread_return_value = NULL;
        pthread_join(threads[i].thread, &thread_return_value);
        pthread_cond_destroy(&threads[i].thread_state_updated);
        pthread_mutex_destroy(&threads[i].client_descriptor_mutex);
    }

    pthread_mutex_destroy(&should_stop_mutex);

    pthread_mutexattr_destroy(&mutex_attributes);
    pthread_attr_destroy(&thread_attributes);
    pthread_condattr_destroy(&condition_attributes);

    return 0;
}
