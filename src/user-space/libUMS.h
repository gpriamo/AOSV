#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/sysinfo.h>

#include <sched.h>

#define DEVICE "/dev/umschardev"
#define THREADS_MAX 50

static int tot_pids;
//static pid_t *pids = NULL;
static pid_t pids[THREADS_MAX];

static int tot_sched_pids;
//static pid_t *sched_pids = NULL;
static pid_t sched_pids[THREADS_MAX];

static pthread_mutex_t smtx = PTHREAD_MUTEX_INITIALIZER;

/**
 * @struct UMSinfo
 * @brief this structure represents 
 * a UMS completion list in user-space
 */
typedef struct UMSinfo {
    int n;                          ///< The number of worker threads
    pid_t worker_tids[THREADS_MAX]; ///< The tids of the worker threads
} UMSinfo;

/**
 * @struct UMSSchedulers
 * @brief this structure holds
 * the UMS Schedulers in user-space
 */
typedef struct UMSSchedulers {
    int n;                          ///< The number of scheduler threads
    pid_t sched_tids[THREADS_MAX];  ///< The tids of the scheduler threads
} UMSSchedulers;

/**
 * @struct UMSScheduler_Info
 * @brief used to send info
 * about a scheduler thread
 * to the kernel module
 */
typedef struct UMSScheduler_Info {
    long i;                         ///< The scheduler's ID
    pid_t tid;                      ///< The scheduler's pthread tid
} UMSScheduler_Info;

typedef int (*fptr)();              ///< A typedef for a function pointer

/**
 * @struct ums_args
 * @brief used to hold the
 * arguments for the worker
 * threads' routines
 */
typedef struct ums_args {
    long t;                         ///< The worker's ID
    fptr ptr;                       ///< The function pointer to the routine
} ums_args;

/**
 * @struct Worker_Info
 * @brief used to send info
 * about a worker thread 
 * to the kernel module 
 */
typedef struct Worker_Info {
    long i;                         ///< The worker's ID
    pid_t tid;                      ///< The worker's pthread tid
} Worker_Info;

/**
 * @struct UMS_initial_info
 * @brief used to send info
 * from the main process in
 * user-space to the kernel
 * module
 */
typedef struct UMS_initial_info {
    pid_t ppid;                     ///< The main process' pid
    int workers_cnt;                ///< The number of worker threads
} UMS_initial_info;

/**
 * do_cleanup - performs cleanup operations
 * upon exit from the main loop of the
 * user-space application
 * 
 * Returns: 0 on success
 */
int do_cleanup();

/**
 * get_CPUs_count - Retrieves the number 
 * of online CPUS in the current system
 * 
 * Returns: the number specified above
 */
int get_CPUs_count();

/**
 * opendev - opens the device named @DEVICE,
 * used for allowing the communication
 * between user-space and the kernel mode
 * 
 * Returns: the file descriptor representing
 * the device, if successful
 */
int opendev();

/**
 * ums_thread_yield - UMS API #4
 * used to communicate the module that
 * the currently executing worker thread
 * wants to relinquish the CPU and go to
 * sleep
 */
void ums_thread_yield();

/**
 * execute_ums_thread - UMS API #3
 * tells the module to start executing
 * a worker thread
 * @worker: pid of the worker to
 * wake up and execute
 */
void execute_ums_thread(pid_t worker);

/**
 * get_next_ums_list_item - UMS API #5
 * tells the module to return a worker thread from the list
 * created by dequeue_ums_completion_list_items(), to be added
 * to the runqueue of the calling scheduler thread
 * 
 * Returns: the tid of the received
 * worker thread
 */
pid_t get_next_ums_list_item();

/**
 * dequeue_ums_completion_list_items - UMS API #2
 * tells the module to compute the list of currently
 * available worker threads
 * 
 * Returns: the number of available worker threads
 */ 
int dequeue_ums_completion_list_items();

/**
 * enter_ums_scheduling_mode - UMS API #1
 * Creates those pthreads that will be converted
 * into UMS scheduler threads by the module
 * @completion_list: list of the worker threads (here,
 * a struct of type UMSinfo)
 * @entry_point: the function that will be run by the
 * UMS scheduler threads
 * 
 * Returns: 0 on success
 */
int enter_ums_scheduling_mode(UMSinfo* completion_list, void* entry_point);

/**
 * init - function needed to initialize the whole
 * UMS Scheduling system, creates the pthreads that
 * will be converted to worker threads
 * @num_threads: the number of pthreads that will 
 * become worker threads
 * @worker_entrypoint: the function that the worker
 * threads will execute
 * 
 * Returns: a struct of type UMSinfo containing
 * the info about the created pthreads
 */ 
UMSinfo init(int num_threads, void* worker_entrypoint);

/**
 * UMSScheduler - entrypoint to simplify & automate the use of
 * this library. It needs to be called explicitly from the user 
 * to initialize the UMS Scheduling system and eventually 
 * call enter_ums_scheduling_mode()
 * @worker_threads_cnt: number of worker threads that will
 * be created
 * @entry_pt: entry point function for the UMS scheduler
 * threads
 * @worker_ept: entry point function for the worker
 * threads
 * 
 * Returns: 0 on success
 */  
int UMSScheduler(int worker_threads_cnt, void* entry_pt, void* worker_ept);