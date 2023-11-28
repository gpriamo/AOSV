#include "libUMS.h"
#define NUM_WORKERS 6
#define RQLen 1

pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/**
 * entry_point - entrypoint function for
 * the UMS scheduler threads
 */
void entry_point() {
    pid_t rq;
    pid_t rtid = gettid();

    pid_t* run_queue = malloc(sizeof(pid_t)*RQLen);
    if (run_queue == NULL) {
        puts("Malloc error!");
        exit(EXIT_FAILURE);
    }

    //start looping for worker threads to schedule
    while(1) { // While there are worker threads that still need to complete their execution
        printf("[Scheduler-%d] Running...\n", rtid);

        int ready = dequeue_ums_completion_list_items();

        // Exit if there are no more available worker  threads
        if (ready <= 0) {
            printf("[Scheduler-%d] There are no more available workers, exiting!\n", rtid);
            break;
        }

        int tot_take = RQLen;
        /* If there are not as many workers available as the scheduler wants (RQLen),
         * take as many as there are available
         */
        if (ready < RQLen)
            tot_take = ready;
        printf("[Scheduler-%d] taking %d threads\n", rtid, tot_take);

        for (int i = 0; i < tot_take; i++) {
            rq = get_next_ums_list_item();

            printf("[Scheduler-%d] #%d %d\n", rtid, i, rq);

            run_queue[i] = rq;
        }

        //printf("[Scheduler-%d] Running rq: %d\n", rtid, rq);

        for (int i = 0; i < tot_take; i++)
            execute_ums_thread(run_queue[i]);

        sleep(1);
    }

    free(run_queue);
}

/**
 * fact - recursively computes
 * the factorial of a number
 * @n: the number to compute the
 * factorial of
 * 
 * Returns: @n!
 */
unsigned long long fact(int n) {
    if (n <= 1)
        return 1;
    return n*fact(n-1);
}

/**
 * worker_routine - entrypoint
 * for the worker threads
 */
void worker_routine() {
    unsigned long long i = 0;
    long j = 1000000000;
    pid_t rtid = gettid();

    if (rtid % 2 == 0) {
        printf("[Worker-%d] Hi hi!\n", rtid);

        while(i < j) {
            i++;
        }

    }
    else {
        printf("[Worker-%d] Hello hello!\n", rtid);

        i = fact(50);
        printf("Fact(50) %llu\n", i);

        ums_thread_yield();

        sleep(20);

        printf("[Worker-%d] I'm back!\n", rtid);
    }

}

/**
 * main - initializes the UMS Scheduler
 * library and starts the whole system
 * 
 * Returns: 0 on success
 */
int main(int argc, char **argv) {
    puts("Example code starting!\n");

    UMSScheduler(NUM_WORKERS, entry_point, worker_routine);

    return 0;
}
