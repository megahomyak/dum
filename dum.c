#include <pthread.h>

#define THREAD_COUNT 20

typedef struct {
    pthread_t thread;
    pthread_cond_t run;
    pthread_mutex_t argument;
} ThreadContext;

ThreadContext threads[THREAD_COUNT];

/*
- Receive "run"
- Lock mutex on "argument"
- Copy the descriptor from there
- Put "-1" into the descriptor
- Unlock the mutex
- Do the work
- Loop back around
*/
void* worker_thread(void* context_void) {
    ThreadContext* context = (ThreadContext*) context_void;
    return NULL;
}

/*
- Try to lock the mutex for the argument
- Check if "-1" is in the mutex
- Put the descriptor into the mutex
- Send "run"
- Unlock the mutex
*/
int main(void) {
    pthread_attr_t threadCreationAttributes;
    pthread_attr_init(&threadCreationAttributes);
    pthread_attr_setstacksize(&threadCreationAttributes, 1024);
    for (int i = 0; i < THREAD_COUNT; ++i) {
        pthread_create(&threads[i].thread, &threadCreationAttributes, worker_thread, &threads[i]);
    }
    pthread_attr_destroy(&threadCreationAttributes);
    return 0;
}
