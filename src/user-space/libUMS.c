#include "libUMS.h"

/**
 * entrypoint_wrapper - wrapper routine for
 * the UMS scheduler threads. Used to send info
 * to the module without obliging the
 * programmer to do so
 * @args: arguments (here a struct of type ums_args)
 */ 
void *entrypoint_wrapper(void *args) {
    long tid;
    UMSScheduler_Info sched_info;

    ums_args* arguments = (ums_args*) args;

    //tid = (long)threadid;
    tid = arguments->t;
    pid_t rtid = gettid();
    printf("Scheduler thread %ld, kthread ID - %d\n", tid, rtid);
    fflush(stdout);

    sched_info.i = tid;
    sched_info.tid = rtid;

    // Bind the scheduler thread to the specific CPU
    cpu_set_t  mask;
    CPU_ZERO(&mask);
    CPU_SET((unsigned int)tid, &mask);
    int result = sched_setaffinity(0, sizeof(mask), &mask);
    //printf("[Scheduler-%d] Binding to cpu %u - res: %d\n", rtid, (unsigned int)tid, result);

    // Thread introduces itself to the module
    pthread_mutex_lock(&smtx);
    sched_pids[tid] = rtid;
    pthread_mutex_unlock(&smtx);
    
    int fd = opendev();

    int ret = ioctl(fd, 103, (UMSScheduler_Info *)&sched_info);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }
    //printf("[Scheduler-%d] Conversion to scheduler thread: ioctl returned %d\n", rtid, ret);

    close(fd);    

    arguments->ptr();

    free(args);

    //pthread_exit(NULL);
}

/**
 * worker_wrapper - wrapper routine for
 * the worker threads. Used to send info
 * to the module without obliging the
 * programmer to do so
 * @args: arguments (here a struct of type ums_args)
 */ 
void *worker_wrapper(void *args) {
    Worker_Info winfo;
    long tid;
    int status = -1;

    ums_args* arguments = (ums_args*) args;

    //tid = (long)threadid;
    tid = arguments->t;
    pid_t rtid = gettid();
    printf("Worker here! Thread %ld, kthread ID - %d\n", tid, rtid);
    fflush(stdout);

    winfo.i = tid;
    winfo.tid = rtid;
    
    // Thread introduces itself to the module -- Needs to be converted into worker & put to sleep
    pthread_mutex_lock(&smtx);
    pids[tid] = rtid;
    pthread_mutex_unlock(&smtx);

    int fd = opendev();

    int ret = ioctl(fd, 104, (Worker_Info *)&winfo);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }
    //printf("[Worker-%d] Conversion to worker: ioctl returned %d\n", rtid, ret);


    printf("[Worker-%d] Resumed execution!\n", rtid);

    arguments->ptr();
    
    printf("[Worker-%d] Bye! thread %ld, kthread ID - %d\n", rtid, tid, rtid);
    
    free(args);

    status = 9;
    ret = ioctl(fd, 108, (int *)&status);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }
    //printf("[Worker-%d] ioctl returned %d\n", rtid, ret);

    close(fd);    

    pthread_exit(NULL);
}

int get_CPUs_count() {
    printf("This system has %d processors available.\n", get_nprocs());
    return get_nprocs();
}

int opendev() {
    int fd = open(DEVICE, 0);
    if (fd < 0) {
        printf("An error occurred while trying to open the character device");
        exit(EXIT_FAILURE);
    }

    return fd;    
}

void ums_thread_yield() {
    printf("[Worker-%d] Yielding...\n", gettid());

    int msg = 0;

    int fd = opendev();

    int ret = ioctl(fd, 109, (int *)&msg);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }
    //printf("[Worker-%d] ioctl returned %d\n", gettid(), ret);

    close(fd);
}

void execute_ums_thread(pid_t worker) {
    printf("[Scheduler-%d] executing %d\n", gettid(), worker);

    int fd = opendev();

    int ret = ioctl(fd, 107, (int *)&worker);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }
    //printf("[Scheduler-%d] ioctl returned %d\n", gettid(), ret);

    close(fd);      

    // Maybe return smt   
}

pid_t get_next_ums_list_item() {
    pid_t ready_worker;

    int fd = opendev();

    int ret = ioctl(fd, 106, (pid_t *)&ready_worker);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }

    printf("[Scheduler-%d] Got ready worker %d\n", gettid(), ready_worker);

    close(fd);

    return ready_worker;
}

int dequeue_ums_completion_list_items() {
    int ready_cnt = 0;

    int fd = opendev();

    int ret = ioctl(fd, 105, (int *)&ready_cnt);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }

    printf("[Scheduler-%d] Threads available %d\n", gettid(), ready_cnt);

    close(fd);    

    return ready_cnt;
}

int enter_ums_scheduling_mode(UMSinfo* completion_list, void* entry_point) {
    UMSSchedulers schedulers;
    int cpus;
    int pret;
    long t;
    int i;
    struct timeval t1, t2;
    double elapsed_time;    
    ums_args *args;
    UMS_initial_info info;

    printf("[UMS] Entering ums scheduling mode.\n Completion list size: %d\n", completion_list->n);

    /* Send initial info to the module */
    pid_t pid = getpid();
    printf("My pid is: %d\n", pid);

    info.ppid = pid;
    info.workers_cnt = 0;

    int fd = opendev();

    int ret = ioctl(fd, 102, (int *)&info);
    if (ret < 0) {
        printf("An error occurred while trying to issue the ioctl syscall");
        exit(EXIT_FAILURE);
    }

    close(fd); 

    /* Create the scheduler threads */
    cpus = get_CPUs_count();
    schedulers.n = cpus;
    pthread_t schedulers_tids[cpus];

    tot_sched_pids = (int)cpus;

    // start timer
    gettimeofday(&t1, NULL);

    for (t = 0; t < cpus; t++) {       
        args = malloc(sizeof(ums_args));
        if (args == NULL) {
            puts("Malloc error!");
            exit(EXIT_FAILURE);
        }
        args->t = t;
        args->ptr = entry_point;        

        pret = pthread_create(&schedulers_tids[t], NULL, entrypoint_wrapper, (void *)args);
        if (pret) {
            printf("An error occurred while issuing pthread_create(): %d\n", pret);
            exit(EXIT_FAILURE);
        }
    }

    sleep(cpus);

    for (long i = 0; i < cpus; i++) {
        //printf("Scheduler tid is: %d\n", pids[i]);

        schedulers.sched_tids[i] = sched_pids[i];
    }

    for (long j = 0; j < cpus; j++) {
        (void) pthread_join(schedulers_tids[j], NULL);
    }

    // stop timer
    gettimeofday(&t2, NULL);

    // compute and print the elapsed time in milliseconds
    elapsed_time = (t2.tv_sec - t1.tv_sec) * 1000.0;      // sec to ms
    elapsed_time += (t2.tv_usec - t1.tv_usec) / 1000.0;   // us to ms
    printf("Elapsed time: %f ms. (%f s.) \n", elapsed_time, elapsed_time/1000);

    return 0;
}

UMSinfo init(int num_threads, void* worker_entrypoint) {
    UMSinfo ms;
    int threads_no = num_threads;
    pthread_t worker_tids[threads_no];
    int pret;
    long t;

    printf("[UMS] Allocating: %d worker threads\n", threads_no);

    if (threads_no > THREADS_MAX) {
        printf("The number of threads exceeds the imposed limit (%d)\n", THREADS_MAX);
        exit(EXIT_FAILURE);
    }

    tot_pids = threads_no;

    ms.n = threads_no;
    
    // Create those that will become worker threads
    ums_args *args;

    for (t = 0; t < threads_no; t++) {
        args = malloc(sizeof(ums_args));
        if (args == NULL) {
            puts("Malloc error!");
            exit(EXIT_FAILURE);
        }
        args->t = t;
        args->ptr = worker_entrypoint;

        pret = pthread_create(&worker_tids[t], NULL, worker_wrapper, (void *)args);
        if (pret) {
            printf("An error occurred while issuing pthread_create(): %d\n", pret);
            exit(EXIT_FAILURE);
        }
    }

    sleep(threads_no/2);

    for (long i = 0; i < threads_no; i++) {
        //printf("tid is: %d\n", pids[i]);

        ms.worker_tids[i] = pids[i];
    }

    return ms;
}

int UMSScheduler(int worker_threads_cnt, void* entry_pt, void* worker_ept) {
    UMSinfo ms;

    ms = init(worker_threads_cnt, (void *) worker_ept);
    enter_ums_scheduling_mode(&ms, (void *) entry_pt);

    return EXIT_SUCCESS;
}