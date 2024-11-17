//Hananel Sabag 208755744
#include "threadpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>


threadpool *create_threadpool(int num_threads_in_pool) {
    // Check the number of threads for the pool.
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL) {
        fprintf(stderr, "Invalid pool size\n");
        return NULL;
    }


    threadpool *pool = (threadpool *) malloc(sizeof(threadpool));
    if (pool == NULL) {
        perror("Failed to allocate thread pool");
        return NULL;
    }

    // Init the thread-pool
    pool->num_threads = num_threads_in_pool; // Set the number of threads.
    pool->qsize = 0;
    pool->qhead = pool->qtail = NULL;
    pool->shutdown = pool->dont_accept = 0; // Indicator

    // Init mutex .
    if (pthread_mutex_init(&pool->qlock, NULL) || // Lock
        pthread_cond_init(&pool->q_not_empty, NULL) || // In use - Not empty
        pthread_cond_init(&pool->q_empty, NULL)) { //Queue is empty.
        //fail
        perror("Mutex or condition variable initialization failed");
        free(pool);
        return NULL;
    }


    pool->threads = (pthread_t *) malloc(sizeof(pthread_t) * num_threads_in_pool);
    if (pool->threads == NULL) {
        perror("Failed to allocate thread pointers");
        destroy_threadpool(pool);
        return NULL;
    }

    // Create each thread in the pool.
    for (int i = 0; i < num_threads_in_pool; i++) {
        // Each thread starts executing in the do_work function.
        if (pthread_create(&pool->threads[i], NULL, do_work, (void *) pool)) {
            perror("Thread creation failed");
            destroy_threadpool(pool);
            return NULL;
        }
    }

    return pool;
}


// This function adds a new task
void dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg) {
    // Check
    if (from_me == NULL || dispatch_to_here == NULL) {
        fprintf(stderr, "Dispatch received null arguments\n");
        return;
    }

    // Lock the queue .
    pthread_mutex_lock(&from_me->qlock);

    // thread pool is shutting down.
    if (from_me->dont_accept) {
        fprintf(stderr, "ThreadPool is not accepting new tasks\n");
        pthread_mutex_unlock(&from_me->qlock);
        return;
    }


    work_t *work = (work_t *) malloc(sizeof(work_t));
    if (work == NULL) {
        perror("Failed to allocate work structure");
        pthread_mutex_unlock(&from_me->qlock);
        return;
    }

    // Init new task .
    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;

    // If the queue is empty.
    if (from_me->qhead == NULL) {
        from_me->qhead = from_me->qtail = work;
    } else {
        // add the task to the end of the queue .
        from_me->qtail->next = work;
        from_me->qtail = work;
    }
    from_me->qsize++;

    // Signal a waiting thread that a new task is available.
    pthread_cond_signal(&from_me->q_not_empty);

    // Unlock mutex.
    pthread_mutex_unlock(&from_me->qlock);
}


// Processes tasks from the queue.
void *do_work(void *p) {
    threadpool *pool = (threadpool *) p; // Recast

    while (1) { // Infinite loop
        pthread_mutex_lock(&pool->qlock); // Lock the queue

        // Wait until there are tasks in the queue or the pool is shutting down.
        while (pool->qsize == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->q_not_empty, &pool->qlock);
        }

        // Pool is shutting down.
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->qlock);
            pthread_exit(NULL);
        }


        // Fetch the first task  for processing
        work_t *work = pool->qhead;
        if (work != NULL) {
            pool->qhead = work->next;
            if (pool->qhead == NULL) {
                pool->qtail = NULL;
            }
            pool->qsize--;
        }

        // Unlock mutex
        pthread_mutex_unlock(&pool->qlock);


        if (work != NULL) {
            int result = work->routine(work->arg); // Execute the task and save the return value.
            free(work); //

            //Thread error handle.
            if (result != 0) {
                fprintf(stderr, "Task execution reported error %d, thread exiting\n", result);
                pthread_exit(NULL);
            }
        }
    }
}


//Responsible for cleaning up and destroying the thread-pool.
void destroy_threadpool(threadpool *destroyme) {
    if (destroyme == NULL) {
        return;
    }

    // Lock the mutex
    pthread_mutex_lock(&destroyme->qlock);
    destroyme->dont_accept = 1; // Set to prevent tasks from enter the queue.


    while (destroyme->qsize != 0) {
        pthread_cond_wait(&destroyme->q_empty, &destroyme->qlock);
    }

    // Cause worker threads to exit their loop.
    destroyme->shutdown = 1;
    pthread_cond_broadcast(&destroyme->q_not_empty); // Wake up all waiting threads to check shutdown flag.
    pthread_mutex_unlock(&destroyme->qlock);

    // Join to ensure execute.
    for (int i = 0; i < destroyme->num_threads; i++) {
        pthread_join(destroyme->threads[i], NULL);
    }

    // Clean up
    pthread_mutex_destroy(&destroyme->qlock);
    pthread_cond_destroy(&destroyme->q_not_empty);
    pthread_cond_destroy(&destroyme->q_empty);
    free(destroyme->threads);
    free(destroyme);
}


//threads tasks.
int task_function(void *arg) {
    // Cast thread ID .
    pthread_t tid = pthread_self();
    for (int i = 0; i < 1000; ++i) {
        printf("Thread %lu executing task number: %d, iteration: %d\n", (unsigned long) tid, *((int *) arg), i);
        usleep(100 * 1000); // Sleep for 100 milliseconds
    }
    return 0;
}


//int main(int argc, char *argv[]) {
//    if (argc != 4) {
//        printf("Usage: pool <pool-size> <number-of-tasks> <max-number-of-request>\n");
//        exit(EXIT_FAILURE);
//    }
//
//    int pool_size = atoi(argv[1]);
//    int number_of_tasks = atoi(argv[2]);
//    int max_number_of_request = atoi(argv[3]);
//
//    // Input Validation
//    if (pool_size <= 0 || number_of_tasks <= 0 || max_number_of_request <= 0) {
//        printf("Error: All arguments must be positive integers\n");
//        exit(EXIT_FAILURE);
//    }
//
//    // Create the thread pool
//    threadpool *pool = create_threadpool(pool_size);
//    if (pool == NULL) {
//        perror("Failed to create thread pool");
//        exit(EXIT_FAILURE);
//    }
//
//    // Dispatch tasks
//    int args[number_of_tasks];
//    for (int i = 0; i < number_of_tasks && i < max_number_of_request; i++) {
//        args[i] = i;
//        dispatch(pool, task_function, &args[i]);
//    }
//
//    // Allow some time for tasks to complete
//    sleep(2);
//
//    // Destroy the thread pool
//    destroy_threadpool(pool);
//
//    return 0;
//}