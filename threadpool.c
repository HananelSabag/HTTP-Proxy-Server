//Hananel Sabag 208755744
#include "threadpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

threadpool* create_threadpool(int num_threads_in_pool) {
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL) {
        fprintf(stderr, "Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        return NULL;
    }

    threadpool* pool = (threadpool*)malloc(sizeof(threadpool));
    if (pool == NULL) {
        perror("Failed to allocate thread pool");
        return NULL;
    }

    // Initialize the pool structure
    pool->num_threads = num_threads_in_pool;
    pool->qsize = 0;
    pool->qhead = NULL;
    pool->qtail = NULL;
    pool->shutdown = 0;
    pool->dont_accept = 0;
    pool->threads = NULL;

    // Initialize synchronization objects
    if (pthread_mutex_init(&pool->qlock, NULL) != 0) {
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->q_not_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->qlock);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->q_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->qlock);
        pthread_cond_destroy(&pool->q_not_empty);
        free(pool);
        return NULL;
    }

    // Allocate threads array
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads_in_pool);
    if (pool->threads == NULL) {
        pthread_mutex_destroy(&pool->qlock);
        pthread_cond_destroy(&pool->q_not_empty);
        pthread_cond_destroy(&pool->q_empty);
        free(pool);
        return NULL;
    }

    // Create the threads
    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&pool->threads[i], NULL, do_work, (void*)pool) != 0) {
            // Failed to create thread, cleanup and exit
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->q_not_empty);
            
            // Wait for created threads to finish
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }

            // Cleanup
            pthread_mutex_destroy(&pool->qlock);
            pthread_cond_destroy(&pool->q_not_empty);
            pthread_cond_destroy(&pool->q_empty);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

void dispatch(threadpool* from_me, dispatch_fn dispatch_to_here, void* arg) {
    if (from_me == NULL || dispatch_to_here == NULL) {
        return;
    }

    pthread_mutex_lock(&from_me->qlock);

    // Check if we're accepting new work
    if (from_me->dont_accept || from_me->shutdown) {
        pthread_mutex_unlock(&from_me->qlock);
        if (arg != NULL) {
            free(arg);
        }
        return;
    }

    // Create work structure
    work_t* work = (work_t*)malloc(sizeof(work_t));
    if (work == NULL) {
        pthread_mutex_unlock(&from_me->qlock);
        if (arg != NULL) {
            free(arg);
        }
        return;
    }

    // Initialize work
    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;

    // Add to queue
    if (from_me->qsize == 0) {
        from_me->qhead = work;
        from_me->qtail = work;
        pthread_cond_signal(&from_me->q_not_empty);
    } else {
        from_me->qtail->next = work;
        from_me->qtail = work;
    }
    from_me->qsize++;

    pthread_mutex_unlock(&from_me->qlock);
}

void* do_work(void* p) {
    if (p == NULL) return NULL;
    
    threadpool* pool = (threadpool*)p;
    work_t* work = NULL;

    while (1) {
        pthread_mutex_lock(&pool->qlock);

        while (pool->qsize == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->q_not_empty, &pool->qlock);
        }

        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->qlock);
            pthread_exit(NULL);
        }

        // Get work if available
        if (pool->qsize > 0) {
            work = pool->qhead;
            pool->qhead = work->next;
            pool->qsize--;

            if (pool->qsize == 0) {
                pool->qtail = NULL;
                pthread_cond_signal(&pool->q_empty);
            }
        }

        pthread_mutex_unlock(&pool->qlock);

        // Execute work if we got any
        if (work != NULL) {
            work->routine(work->arg);
            free(work);
            work = NULL;
        }
    }

    return NULL;
}

void destroy_threadpool(threadpool* destroyme) {
    if (destroyme == NULL) return;

    pthread_mutex_lock(&destroyme->qlock);
    destroyme->dont_accept = 1;

    // Wait for queue to empty
    while (destroyme->qsize > 0) {
        work_t* work = destroyme->qhead;
        destroyme->qhead = work->next;
        destroyme->qsize--;
        free(work);
    }
    destroyme->qtail = NULL;

    // Signal shutdown
    destroyme->shutdown = 1;
    pthread_cond_broadcast(&destroyme->q_not_empty);
    pthread_mutex_unlock(&destroyme->qlock);

    // Wait for threads to finish
    for (int i = 0; i < destroyme->num_threads; i++) {
        pthread_join(destroyme->threads[i], NULL);
    }

    // Cleanup resources
    pthread_mutex_destroy(&destroyme->qlock);
    pthread_cond_destroy(&destroyme->q_not_empty);
    pthread_cond_destroy(&destroyme->q_empty);
    free(destroyme->threads);
    free(destroyme);
}