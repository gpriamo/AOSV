#include <linux/cpumask.h>
#include <linux/dcache.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

MODULE_DESCRIPTION("UMS Scheduler module");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Giacomo Priamo <priamo.1701568@studenti.uniroma1.it>");
MODULE_VERSION("1.6.0");

#define MODULE_NAME_LOG "UMS: "   ///< The module's name (used for printk logging)
#define SUCCESS 0                 ///< Success value
#define FAILURE 1                 ///< Failure value
#define DEVICE_NAME "umschardev"  ///< Name of the device used to communicate w/ user-space
#define UNBOUND 99                ///< Value used to indicate a worker is not bound to any scheduler
#define BUFSIZE 100               ///< Length of the buffers
#define TEST 0                    ///< Switch to turn on/off the testing mode
#define TEST2 0                   ///< Switch to turn on/off the testing2 mode

/* Identifiers for the routines, used by the device_ioctl function */
#define INITIAL_SETUP 102  ///< Identifier for @initial_setup()
#define CREATE_SCHED 103   ///< Identifier for @create_ums_scheduler_thread()
#define CREATE_WORKER 104  ///< Identifier for @create_worker_thread()
#define COMPUTE_READY 105  ///< Identifier for @compute_ready_workers()
#define GET_READY 106      ///< Identifier for @get_ready_worker()
#define EXE_WORKER 107     ///< Identifier for @execute_worker()
#define WORK_END 108       ///< Identifier for @handle_ending_worker()
#define WORK_YIELD 109     ///< Identifier for @yield_worker()

static int ums_init_module(void);
static void ums_cleanup_module(void);
static inline int update_ready_workers(unsigned int cpu);
static int initial_setup(unsigned long data);
static int create_ums_scheduler_thread(unsigned long data);
static int create_worker_thread(unsigned long data);
static int compute_ready_workers(unsigned long data);
static int get_ready_worker(unsigned long data);
static int execute_worker(unsigned long data);
static int handle_ending_worker(unsigned long data);
static int yield_worker(unsigned long data);
static long device_ioctl(struct file *file, unsigned int request, unsigned long data);

/*
 * Global variables are declared as static, so they are global within the file.
 */
static struct file_operations fops = {
    .unlocked_ioctl = device_ioctl};

static struct miscdevice mdev = {
    .minor = 0,
    .name = DEVICE_NAME,
    .mode = S_IALLUGO,
    .fops = &fops};

static struct proc_dir_entry *ent, *ppid_ent, *pid_ent;

/**
 * @struct UMSScheduler
 * @brief this structure represents 
 * a UMS scheduler thread
 */
typedef struct UMSScheduler {
    pid_t tid;                        ///< The scheduler's tid
    unsigned int cpu;                 ///< The CPU (core) it is assigned to
    int switches_cnt;                 ///< The number of context switches
    int status;                       ///< RUNNING/SLEEPING
    unsigned long long switch_start;  ///< Switch start time in jiffies
    unsigned long long switch_time;   ///< Last switch time
    int running_worker;               ///< The ID of the worker it is running
    struct proc_dir_entry *dir;       ///< The main directory under /proc/ums/<pid>/
    struct proc_dir_entry *info;      ///< The info file under /proc/ums/<pid>/<sched_id>
    struct proc_dir_entry *workers;   ///< The directory of workers under /proc/ums/<pid>/<sched_id>/
    struct task_struct *ts;           ///< The scheduler's pthread task_struct
} UMSScheduler;

/**
 * @struct UMSScheduler_Info
 * @brief used to receive info
 * about a scheduler thread from
 * user-space
 */
typedef struct UMSScheduler_Info {
    long i;     ///< The scheduler's ID
    pid_t tid;  ///< The scheduler's pthread tid
} UMSScheduler_Info;

/**
 * @struct Worker_Info
 * @brief used to receive info
 * about a worker thread from
 * user-space
 */
typedef struct Worker_Info {
    long i;     ///< The worker's ID
    pid_t tid;  ///< The worker's pthread tid
} Worker_Info;

/**
 * @struct UMSWorker_Thread
 * @brief this structure represents 
 * a UMS worker thread
 */
typedef struct UMSWorker_Thread {
    int id;                               ///< The worker's id
    pid_t tid;                            ///< The worker's tid
    int status;                           ///< RUNNING/SLEEPING/ENDED
    unsigned long long tot_running_time;  ///< Total running time (ns)
    int switches_cnt;                     ///< Number of switches
    unsigned int bound;                   ///< CPU/Scheduler the worker is bound to while running
    unsigned int tbound;                  ///< Needed by update_ready_workers()
    struct task_struct *ts;               ///< The worker's pthread task_struct
    struct proc_dir_entry *info;          ///< The info file under /proc/ums/<pid>/<sched_id>/workers

    struct list_head list;  ///< List_head to keep it in @completion_list
    struct list_head endl;  ///< List_head to keep it in @completion_list_backup
} UMSWorker_Thread;

/**
 * @struct UMS_initial_info
 * @brief used to receive info
 * from the main process in
 * user-space
 */
typedef struct UMS_initial_info {
    pid_t ppid;       ///< The main process' pid
    int workers_cnt;  ///< The number of worker threads
} UMS_initial_info;

UMSScheduler **schedulers_arr_p;  ///< Contains pointers to the UMS schedulers

LIST_HEAD(completion_list);      ///< Cotains the completion list
static int completion_list_cnt;  ///< Keeps track of the completion list's size

static int ready_workers_cnt;  ///< Keeps track of the available workers

LIST_HEAD(completion_list_backup);  ///< A never modified copy of @completion_list
static int completion_backup_cnt;   ///< Keeps track of completion_list_backup's size

static unsigned int cpus;  ///< The number of CPUS/cores in the system

struct semaphore sched_sem;  ///< To enforce concurrency on @schedulers_arr_p
struct semaphore compl_sem;  ///< To enforce concurrency on @completion_list
struct semaphore bkp_sem;    ///< To enforce concurrency on @completion_list_backup
struct semaphore proc_sem;   ///< To enforce concurrency on /proc

/**
 * myread - application-specific implementation for reads on
 * the devices created under /proc/ums
 * @file: file struct representing the device
 * @ubuf: user-space buffer (where the reply is copied to)
 * @count: needed by the function
 * @ppos: needed by the function
 * 
 * Returns the length of the response
 */
static ssize_t myread(struct file *file, char __user *ubuf, size_t count, loff_t *ppos) {
    char buf[BUFSIZE * 5];
    char stat_buf[BUFSIZE];
    char compl_list_buf[BUFSIZE];
    char *pos = compl_list_buf;
    char *worker_status;
    int len = 0;
    int slen = 0;
    //int clen = 0;
    long tst;
    int id, res;
    unsigned long long rt;
    UMSWorker_Thread *current_worker;
    UMSScheduler *sched;

    printk(KERN_DEBUG MODULE_NAME_LOG "read: pid->%d, length=%ld, offset=%llu - Requesting info for %s/%s\n",
           current->pid, count, *ppos, file->f_path.dentry->d_parent->d_name.name, file->f_path.dentry->d_name.name);

    if (*ppos > 0 || count < BUFSIZE)
        return 0;

    if (strcmp(file->f_path.dentry->d_name.name, "info") == 0) {
        //printk(KERN_DEBUG MODULE_NAME_LOG "Want info!\n");
        //printk(KERN_DEBUG MODULE_NAME_LOG "File: %s - Par = %s!\n", file->f_path.dentry->d_name.name,
        //       file->f_path.dentry->d_parent->d_name.name);

        res = kstrtol(file->f_path.dentry->d_parent->d_name.name, 10, &tst);
        if (res != 0)
            pr_err("Conversion failed!\n");

        //printk(KERN_DEBUG MODULE_NAME_LOG "ID is: %ld!\n", tst);

        id = (int)tst;

        printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Attempting to retrieve scheduler with id %d (tot: %u)", id, cpus);

        sched = schedulers_arr_p[id];

        printk(KERN_DEBUG MODULE_NAME_LOG "Scheduler %d (CPU: %u)", sched->tid, sched->cpu);

        if (sched->status == TASK_RUNNING)
            slen += sprintf(stat_buf, "Current State=RUNNING\nRunning Woker(ID)=%d", sched->running_worker);
        else
            slen += sprintf(stat_buf, "Current State=SLEEPING");  //FUTURE: decide whether to  handle ending schedulers

        list_for_each_entry(current_worker, &completion_list_backup, endl) {
            pos += sprintf(pos, "%d ", current_worker->id);
        }

        len += sprintf(buf, "ID(CPU)=%u\nTID=%d\nSwitches=%d\nLast Switch Time=%llums\n%s\nCompletion List=%s\n",
                       sched->cpu, sched->tid, sched->switches_cnt, sched->switch_time, stat_buf, compl_list_buf);

        goto send;

    } else {
        res = kstrtol(file->f_path.dentry->d_name.name, 10, &tst);
        if (res != 0)
            pr_err("Conversion failed!\n");
        id = (int)tst;

        printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Attempting to retrieve worker with id %d", id);

        list_for_each_entry(current_worker, &completion_list_backup, endl) {
            if (current_worker->id == id)
                goto leave_loop;
        }
    leave_loop:
        switch (current_worker->status) {
            case TASK_RUNNING:
                worker_status = "RUNNING";
                rt = current_worker->ts->utime + current_worker->ts->stime;
                break;
            case TASK_INTERRUPTIBLE:
                worker_status = "SLEEPING";
                rt = current_worker->ts->utime + current_worker->ts->stime;
                break;
            case 3:
                worker_status = "ENDED";
                rt = current_worker->tot_running_time;
                break;
        }

        rt /= 1000000;  // As utime and stime are in nanoseconds (see 'struct taskstat' creation), convert it to ms

        len += sprintf(buf, "ID=%d\nTID=%d\nTotal Running Time=%llums\nSwitches=%d\nCurrent State=%s\n",
                       current_worker->id, current_worker->tid, rt, current_worker->switches_cnt, worker_status);

        goto send;
    }

    len += sprintf(buf, "this is a test proc file %s, pid=%d\n", file->f_path.dentry->d_name.name, current->pid);
send:
    if (copy_to_user(ubuf, buf, len))
        return -EFAULT;
    *ppos = len;

    return len;
}

static struct proc_ops myops =
    {
        .proc_read = myread,
};

/**
 * init_module - initialize the module
 *
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int ums_init_module(void) {
    int ret;

    /* Get the number of available CPUS (= number of future UMS Schedulers) */
    cpus = num_online_cpus();
    printk(KERN_DEBUG MODULE_NAME_LOG "init\n");
    printk(KERN_DEBUG MODULE_NAME_LOG "Number of CPUS: %u\n", cpus);

    /* Allocate the array that will contain the future worker threads */
    schedulers_arr_p = kmalloc(sizeof(UMSScheduler) * (int)cpus, GFP_KERNEL);
    if (schedulers_arr_p == NULL) {
        pr_err("An error occurred while running kmalloc (switch: %d)\n", CREATE_SCHED);
        return FAILURE;
    }

    /* Register the devices that will allow the communication user-space<->module */
    ret = misc_register(&mdev);
    if (ret < 0) {
        printk(KERN_ALERT MODULE_NAME_LOG "Registering char device failed\n");
        return FAILURE;
    }
    printk(KERN_DEBUG MODULE_NAME_LOG "Device registered successfully\n");

    /* Initialize counters */
    ready_workers_cnt = 0;
    completion_list_cnt = 0;
    completion_backup_cnt = 0;

    /* Initialize the semaphores for concurrency handling */
    sema_init(&sched_sem, 1);
    sema_init(&compl_sem, 1);
    sema_init(&bkp_sem, 1);
    sema_init(&proc_sem, 1);

    /* Create the main directory under /proc */
    ent = proc_mkdir("ums", NULL);
    if (ent == NULL) {
        pr_err("NULL!\n");
    }

    return SUCCESS;
}

/**
 * cleanup_module - performs cleanup operations
 * before unloading the module
 */
static void ums_cleanup_module(void) {
    UMSScheduler *sched;
    int i;
    UMSWorker_Thread *cursor, *temp;

    /* Unregister the device */
    misc_deregister(&mdev);

    /* Remove the /proc entry */
    proc_remove(ent);

    /* Free kmalloc-allocated memory  */
    list_for_each_entry_safe(cursor, temp, &completion_list_backup, endl) {
        if (down_interruptible(&proc_sem))
            pr_err("\nTask sleeping on proc_sem interrupted by a signal!\n");

        proc_remove(cursor->info);

        up(&proc_sem);

        //list_del(&cursor->list);
        list_del(&cursor->endl);

        kfree(cursor);
    }

    for (i = 0; i < (int)cpus; i++) {
        sched = schedulers_arr_p[i];

        if (down_interruptible(&proc_sem))
            pr_err("\nTask sleeping on proc_sem interrupted by a signal!\n");

        proc_remove(sched->workers);
        proc_remove(sched->info);
        proc_remove(sched->dir);

        kfree(sched);

        up(&proc_sem);
    }
    kfree(schedulers_arr_p);

    printk(KERN_DEBUG MODULE_NAME_LOG "exit\n");
}

/**
 * update_ready_workers - updates the list of threads
 * that are ready to be executed and temporarily binds
 * them to a specific scheduler
 * @cpu: scheduler to bind the workers to
 * 
 * Returns the number of available worker threads
 */
static inline int update_ready_workers(unsigned int cpu) {
    struct list_head *current_item_list;
    UMSWorker_Thread *current_worker;
    int give;

    if (down_interruptible(&compl_sem))
        pr_err("\nTask %d sleeping on compl_sem (upd) interrupted by a signal!\n", current->pid);

    give = 0;
    ready_workers_cnt = 0;

    list_for_each(current_item_list, &completion_list) {
        current_worker = list_entry(current_item_list, UMSWorker_Thread, list);

        if (current_worker->status == TASK_INTERRUPTIBLE && current_worker->tbound == UNBOUND && current_worker->bound == UNBOUND) {
            /* The task is sleeping & unused by any scheduler, thus it can be used */

            printk(KERN_DEBUG MODULE_NAME_LOG "current_item->id=%d (%d) - status: %d\n",
                   current_worker->id, current_worker->tid, current_worker->status);

            /* Temporarily bind the worker to the cpu (= scheduler) */
            current_worker->tbound = cpu;
            give += 1;

            ready_workers_cnt += 1;
        }
    }

    up(&compl_sem);

    return give;
}

/**
 * initial_setup - Receives info from user-space to set up
 * initial datastructures and variables (e.g. /proc)
 *
 * @data: data coming from usermode via @DEVICE - here used
 * to receive the pid of the process and the number of worker
 * threads
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int initial_setup(unsigned long data) {
    UMS_initial_info info;
    char buf[BUFSIZE];
    int len;

    if (copy_from_user(&info, (UMS_initial_info *)data, sizeof(UMS_initial_info))) {
        pr_err("\nData Write : Err!\n");
        return FAILURE;
    }

    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Running - ppid: %d, schedulers: %u, workers: %d\n", info.ppid, cpus, info.workers_cnt);

    /* Start creating the /proc entries */
    len = 0;
    len += sprintf(buf, "%d", info.ppid);
    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] trying to create dir: %s\n", buf);

    ppid_ent = proc_mkdir(buf, ent);
    if (ppid_ent == NULL) {
        pr_err("NULL!\n");
    }

    pid_ent = proc_mkdir("schedulers", ppid_ent);
    if (pid_ent == NULL) {
        pr_err("NULL!\n");
    }

    return SUCCESS;
}

/**
 * create_ums_scheduler_thread - coverts a standard pthread
 * into a UMS scheduler thread (struct UMSScheduler) and
 * adds it to the array of schedulers
 * @data: data coming from usermode via @DEVICE - here used
 * to receive info about the pthread from user-space
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int create_ums_scheduler_thread(unsigned long data) {
    UMSScheduler *sched;
    UMSScheduler_Info sched_info;
    struct task_struct *p;
    struct proc_dir_entry *folder, *info, *workers;
    char buf[BUFSIZE];
    int i, len;
    unsigned int c;

    if (copy_from_user(&sched_info, (UMSScheduler_Info *)data, sizeof(UMSScheduler_Info))) {
        pr_err("\nData Write : Err!\n");
        return FAILURE;
    }

    //printk(KERN_DEBUG MODULE_NAME_LOG "i: %ld tid: %d\n", sched_info.i, sched_info.tid);

    p = current;

    /* Assign one Scheduler thread to each cpu */
    printk(KERN_DEBUG MODULE_NAME_LOG "[Scheduler-%ld] Moving task %d to cpu %ld (%u)...\n", sched_info.i, sched_info.tid, sched_info.i, (unsigned int)sched_info.i);

    c = p->cpu;
    printk(KERN_DEBUG MODULE_NAME_LOG "Running on cpu: %u\n", c);

    i = (int)sched_info.i;

    /* Covert into scheduler thread & put it inside the array of schedulers */
    sched = kmalloc(sizeof(UMSScheduler), GFP_KERNEL);
    if (sched == NULL) {
        pr_err("An error occurred while running kmalloc (switch: %d)\n", CREATE_SCHED);
        return FAILURE;
    }

    sched->tid = sched_info.tid;
    sched->cpu = c;
    sched->running_worker = 0;
    sched->switch_time = 0;
    sched->switches_cnt = 0;
    sched->switch_start = 0;
    sched->ts = kmalloc(sizeof(p), GFP_KERNEL);
    if (sched->ts == NULL) {
        pr_err("An error occurred while running kmalloc (switch: %d)\n", CREATE_SCHED);
        return FAILURE;
    }
    sched->ts = p;

    /* Create /proc tree */
    // [CONC] concurrency handling
    if (down_interruptible(&proc_sem))
        pr_err("\nTask %d sleeping on proc_sem interrupted by a signal!\n", current->pid);

    sched->dir = kmalloc(sizeof(struct proc_dir_entry *), GFP_KERNEL);
    if (sched->dir == NULL) {
        pr_err("An error occurred while running kmalloc (switch: %d)\n", CREATE_SCHED);
        return FAILURE;
    }
    len += sprintf(buf, "%d", i);
    folder = proc_mkdir(buf, pid_ent);
    if (folder == NULL) {
        pr_err("NULL!\n");
    }
    sched->dir = folder;

    sched->workers = kmalloc(sizeof(struct proc_dir_entry *), GFP_KERNEL);
    if (sched->workers == NULL) {
        pr_err("An error occurred while running kmalloc (switch: %d)\n", CREATE_SCHED);
        return FAILURE;
    }
    workers = proc_mkdir("workers", sched->dir);
    if (workers == NULL) {
        pr_err("NULL!\n");
    }
    sched->workers = workers;

    if (sched->workers == NULL)
        pr_err("NULLLLL\n");

    sched->info = kmalloc(sizeof(struct proc_dir_entry *), GFP_KERNEL);
    if (sched->info == NULL) {
        pr_err("An error occurred while running kmalloc (switch: %d)\n", CREATE_SCHED);
        return FAILURE;
    }
    info = proc_create("info", S_IALLUGO, sched->dir, &myops);
    if (info == NULL) {
        pr_err("NULL!\n");
    }
    sched->info = info;

    up(&proc_sem);

    /* Collect all the schedulers inside the array of schedulers */
    if (down_interruptible(&sched_sem))
        pr_err("\nTask %d sleeping on sched_sem interrupted by a signal!\n", current->pid);

    schedulers_arr_p[i] = sched;

    up(&sched_sem);

    return SUCCESS;
}

/**
 * create_worker_thread - convert a standard pthread into a 
 * worker thread and adds it to @completion_list 
 * @data: data coming from usermode via @DEVICE - here used
 * to receive the pthread's info from user-space
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int create_worker_thread(unsigned long data) {
    Worker_Info winfo;
    UMSWorker_Thread *worker;
    struct task_struct *p;

    if (copy_from_user(&winfo, (Worker_Info *)data, sizeof(Worker_Info))) {
        pr_err("\nData Write : Err!\n");
        return FAILURE;
    }
    p = current;

    worker = kmalloc(sizeof(UMSWorker_Thread), GFP_KERNEL);
    if (worker == NULL) {
        pr_err("\nAn error occurred while running kmalloc (switch: %d)\n", CREATE_WORKER);
        return FAILURE;
    }
    worker->id = (int)winfo.i;
    worker->tid = winfo.tid;
    worker->status = 0;
    worker->bound = UNBOUND;
    worker->tbound = UNBOUND;
    worker->switches_cnt = 0;
    worker->tot_running_time = 0;
    worker->ts = kmalloc(sizeof(p), GFP_KERNEL);
    if (worker->ts == NULL) {
        pr_err("\nAn error occurred while running kmalloc (switch: %d)\n", CREATE_WORKER);
        return FAILURE;
    }
    worker->ts = p;

    //printk(KERN_DEBUG MODULE_NAME_LOG "i: %ld tid: %d\n", winfo.i, winfo.tid);

    /* Add the worker thread to the completion list  */
    if (down_interruptible(&compl_sem))
        pr_err("\nTask %d sleeping on compl_sem (cr_worker) interrupted by a signal!\n", current->pid);

    list_add(&worker->list, &completion_list);
    completion_list_cnt += 1;

    up(&compl_sem);

    if (down_interruptible(&bkp_sem))
        pr_err("\nTask %d sleeping on bkp_sem (cr_worker) interrupted by a signal!\n", current->pid);

    list_add(&worker->endl, &completion_list_backup);
    completion_backup_cnt += 1;

    up(&bkp_sem);

    /* Put the task to sleep */
    printk(KERN_DEBUG MODULE_NAME_LOG "[Worker-%ld] Setting task %d to stopped (%d)...\n", winfo.i, winfo.tid, TASK_INTERRUPTIBLE);
    p->state = TASK_INTERRUPTIBLE;
    worker->status = TASK_INTERRUPTIBLE;

    printk(KERN_DEBUG MODULE_NAME_LOG "Task %d state is %ld - Calling schedule()\n", p->pid, p->state);

    schedule();

    return 0;
}

/**
 * compute_ready_workers - updates the ready workers in @completion_list,
 * if there are no workers available, the function blocks the running 
 * scheduler until at least 1 worker becomes available
 * @data: data coming from usermode via @DEVICE - here used to return
 * the number of ready workers to user-space
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int compute_ready_workers(unsigned long data) {
    int ret, avail;
    UMSScheduler *sched;

    sched = schedulers_arr_p[(int)current->cpu];
    sched->switch_start = get_jiffies_64();

    if (list_empty(&completion_list)) {
        printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] There are no worker threads to be executed\n");
        ready_workers_cnt = -1;

#if TEST == 1
        printk(KERN_DEBUG MODULE_NAME_LOG "[Sched-%d] JIFFIES from dequeue to EXIT = Start: %llu End: %llu Diff: %llu Msecs: %llu \n", sched->tid,
               sched->switch_start, get_jiffies_64(), get_jiffies_64() - sched->switch_start, jiffies64_to_msecs(get_jiffies_64() - sched->switch_start));
#endif

        ret = copy_to_user((int *)data, &ready_workers_cnt, sizeof ready_workers_cnt);

        return 1;
    }

    avail = update_ready_workers(current->cpu);

    while (avail == 0) {
        if (list_empty(&completion_list)) {
            printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] There are no worker threads to be executed\n");
            ready_workers_cnt = -1;

#if TEST == 1
            printk(KERN_DEBUG MODULE_NAME_LOG "[Sched-%d] JIFFIES from dequeue to EXIT = Start: %llu End: %llu Diff: %llu Msecs: %llu \n", sched->tid,
                   sched->switch_start, get_jiffies_64(), get_jiffies_64() - sched->switch_start, jiffies64_to_msecs(get_jiffies_64() - sched->switch_start));
#endif

            ret = copy_to_user((int *)data, &ready_workers_cnt, sizeof ready_workers_cnt);

            return 1;
        }

        printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Waiting for worker threads to become available\n");

        msleep(1000);

        avail = update_ready_workers(current->cpu);
    }
    printk(KERN_DEBUG MODULE_NAME_LOG ">>[Sched%d] There are %d available threads!\n", current->pid, ready_workers_cnt);

    ret = copy_to_user((int *)data, &avail, sizeof avail);

    return 0;
}

/**
 * get_ready_worker - assigns a worker thread to the running scheduler
 * @data: data coming from usermode via @DEVICE - here used to send
 * the assigned worker's pid to user-space
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int get_ready_worker(unsigned long data) {
    UMSScheduler *sched;
    struct proc_dir_entry *info;
    char buf[BUFSIZE];
    int ret, i, len;
    unsigned int c;

    UMSWorker_Thread *current_worker;
    struct list_head *current_item_list;

    pid_t ready_worker;

    c = current->cpu;
    printk(KERN_DEBUG MODULE_NAME_LOG "[Sched] Current pid %d - CPU: %u\n:", current->pid, c);

    if (down_interruptible(&compl_sem))
        pr_err("\nTask %d sleeping on compl_sem (get_worker) interrupted by a signal!\n", current->pid);

    list_for_each(current_item_list, &completion_list) {
        current_worker = list_entry(current_item_list, UMSWorker_Thread, list);

        if (current_worker->bound == UNBOUND) {
            ready_worker = current_worker->tid;
            current_worker->bound = c;

            printk(KERN_DEBUG MODULE_NAME_LOG "Selecting worker: current_item->id=%d (%d) - status: %d - bound to: %u\n",
                   current_worker->id, current_worker->tid, current_worker->status, current_worker->bound);

            goto send_worker;
        }
    }

send_worker:
    up(&compl_sem);

    /* Retrieve the worker's scheduler */
    i = (int)c;
    printk(KERN_DEBUG MODULE_NAME_LOG "Attempting to retrieve scheduler with id %d (tot: %u)", i, cpus);

    sched = schedulers_arr_p[i];

    printk(KERN_DEBUG MODULE_NAME_LOG "Scheduler %d (CPU: %u)", sched->tid, sched->cpu);

    /* Add entry to /proc/ums */
    if (down_interruptible(&proc_sem))
        pr_err("\nTask %d sleeping on proc_sem interrupted by a signal!\n", current->pid);

    if (current_worker->info != NULL)
        proc_remove(current_worker->info);
    else {
        current_worker->info = kmalloc(sizeof(struct proc_dir_entry *), GFP_KERNEL);
        if (current_worker->info == NULL) {
            pr_err("An error occurred while running kmalloc (switch: %d)\n", CREATE_WORKER);
            return FAILURE;
        }
    }

    len = 0;
    len += sprintf(buf, "%d", current_worker->id);

    info = proc_create(buf, S_IALLUGO, sched->workers, &myops);

    current_worker->info = info;
    up(&proc_sem);

    ret = copy_to_user((pid_t *)data, &ready_worker, sizeof ready_worker);
    printk(KERN_DEBUG MODULE_NAME_LOG "Worker sent: %d\n", ready_worker);

    return 0;
}

/**
 * execute_worker - resumes the execution of the specified worker
 * @data: data coming from usermode via @DEVICE - here used to receive
 * the pid of the worker to run from user-space
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int execute_worker(unsigned long data) {
    pid_t run_worker;
    unsigned int c;
    int i;
    struct task_struct *p;
    UMSScheduler *sched;

    unsigned long long tst2;

    UMSWorker_Thread *current_worker;
    struct list_head *current_item_list;

    // 1) Free all tmpbound - [FUTURE] decide whether to make this execute only once
    if (down_interruptible(&compl_sem))
        pr_err("\nTask %d sleeping on compl_sem (exe) interrupted by a signal!\n", current->pid);
    i = 0;

    list_for_each(current_item_list, &completion_list) {
        current_worker = list_entry(current_item_list, UMSWorker_Thread, list);

        //printk(KERN_DEBUG MODULE_NAME_LOG "[%i]\n",i);
        //printk(KERN_DEBUG MODULE_NAME_LOG "worker: current_item->id=%d (%d) - status: %d\n",
        //           current_worker->id, current_worker->tid, current_worker->status);

        if (current_worker->bound == UNBOUND) {
            //printk(KERN_DEBUG MODULE_NAME_LOG "RELEASING!\n");
            current_worker->tbound = UNBOUND;
        }

        i += 1;
    }

    up(&compl_sem);

    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] All tmpbound workers released\n");

    // 2) Retrieve the worker to be executed
    if (copy_from_user(&run_worker, (pid_t *)data, sizeof(pid_t))) {
        pr_err("Data Write : Err!\n");
        return FAILURE;
    }

    printk(KERN_DEBUG MODULE_NAME_LOG "[Sched] Executing worker: %d\n", run_worker);

    if (down_interruptible(&compl_sem))
        pr_err("\nTask %d sleeping on compl_sem (exe) interrupted by a signal!\n", current->pid);

    tst2 = get_jiffies_64();

    list_for_each(current_item_list, &completion_list) {
        current_worker = list_entry(current_item_list, UMSWorker_Thread, list);

        if (current_worker->tid == run_worker) {
            printk(KERN_DEBUG MODULE_NAME_LOG "Worker found!");
            printk(KERN_DEBUG MODULE_NAME_LOG "current_item->id=%d (%d) - status: %d\n",
                   current_worker->id, current_worker->tid, current_worker->status);

            goto found_worker;
        }
    }

found_worker:
    //p = pid_task(find_vpid(current_worker->tid), PIDTYPE_PID);

#if TEST2 == 1
    printk(KERN_DEBUG MODULE_NAME_LOG "[%d] JIFFIES for retrieve (exe) = Start: %llu End: %llu Diff: %llu Msecs: %llu \n", current->pid,
           tst2, get_jiffies_64(), get_jiffies_64() - tst2, jiffies64_to_msecs(get_jiffies_64() - tst2));
#endif

    p = current_worker->ts;
    printk(KERN_DEBUG MODULE_NAME_LOG "Task struct for %d retrieved\n", p->pid);

    c = current_worker->bound;
    i = (int)c;
    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Attempting to retrieve scheduler with id %d (tot: %u)", i, cpus);

    sched = schedulers_arr_p[i];
    sched->switches_cnt += 1;

    printk(KERN_DEBUG MODULE_NAME_LOG "Scheduler %d (CPU: %u)", sched->tid, sched->cpu);

    //p->state = TASK_RUNNING;
    current_worker->status = TASK_RUNNING;
    //p->cpu = current_worker->bound;

    printk(KERN_DEBUG MODULE_NAME_LOG "&current_item=%p, current_item->id=%d (%d) - status: %d\n",
           current_worker, current_worker->id, current_worker->tid, current_worker->status);

#if TEST == 1
    printk(KERN_DEBUG MODULE_NAME_LOG "[Sched-%d] JIFFIES from dequeue to switch = Start: %llu End: %llu Diff: %llu Msecs: %llu \n", sched->tid,
           sched->switch_start, get_jiffies_64(), get_jiffies_64() - sched->switch_start, jiffies64_to_msecs(get_jiffies_64() - sched->switch_start));
#endif

    sched->switch_start = get_jiffies_64();
    current->state = TASK_INTERRUPTIBLE;  // Put the scheduler to sleep

    // Wake up the worker
    wake_up_process(p);

    // Release the semaphore, so that other schedulers can select workers & execute them
    up(&compl_sem);

    schedule();

    return 0;
}

/**
 * handle_ending_worker - handles an worker whose execution has just ended
 * @data: data coming from usermode via @DEVICE - here used to receive
 * the pid of the worker that has ended its user-space execution
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int handle_ending_worker(unsigned long data) {
    int status, i;
    struct task_struct *p;
    unsigned int c;

    unsigned long long tst2;

    UMSScheduler *sched;
    UMSWorker_Thread *current_worker;
    struct list_head *current_item_list;
    p = current;

    if (copy_from_user(&status, (int *)data, sizeof(int))) {
        pr_err("Data Write : Err!\n");
        return FAILURE;
    }
    printk(KERN_DEBUG MODULE_NAME_LOG "[Worker] Execution ended: %d (%d) (CPU: %u)\n", p->pid, status, p->cpu);

    if (down_interruptible(&compl_sem))
        pr_err("\nTask %d  sleeping on compl_sem (handle_ending) interrupted by a signal!\n", current->pid);

    tst2 = get_jiffies_64();

    list_for_each(current_item_list, &completion_list) {
        current_worker = list_entry(current_item_list, UMSWorker_Thread, list);

        if (current_worker->tid == p->pid) {
            printk(KERN_DEBUG MODULE_NAME_LOG "Worker found!");
            printk(KERN_DEBUG MODULE_NAME_LOG "current_item->id=%d (%d) - status: %d, bound: %u (real %u)\n",
                   current_worker->id, current_worker->tid, current_worker->status, current_worker->bound, p->cpu);

            c = current_worker->bound;
            goto found_ended_worker;
        }
    }

found_ended_worker:
#if TEST2 == 1
    printk(KERN_DEBUG MODULE_NAME_LOG "[%d] JIFFIES for retrieve (exe) = Start: %llu End: %llu Diff: %llu Msecs: %llu \n", current->pid,
           tst2, get_jiffies_64(), get_jiffies_64() - tst2, jiffies64_to_msecs(get_jiffies_64() - tst2));
#endif

    printk(KERN_DEBUG MODULE_NAME_LOG "Removing worker from completion list!");

    list_del(&current_worker->list);

    current_worker->status = 3;
    current_worker->switches_cnt += 1;
    current_worker->tot_running_time = current_worker->ts->utime + current_worker->ts->stime;
    //printk(KERN_DEBUG MODULE_NAME_LOG "CWRT: %lld - Conv: %lld(ms)\n",current_worker->ts->utime, jiffies64_to_msecs(current_worker->ts->utime));
    up(&compl_sem);

    /* Retrieve worker's scheduler */
    i = (int)c;
    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Attempting to retrieve scheduler with id %d (tot: %u)", i, cpus);

    sched = schedulers_arr_p[i];

    printk(KERN_DEBUG MODULE_NAME_LOG "Scheduler %d (CPU: %u)", sched->tid, sched->cpu);

    sched->status = TASK_RUNNING;
    sched->running_worker = current_worker->id;
    //printk(KERN_DEBUG MODULE_NAME_LOG "[JIFF] Start: %llu End: %llu Diff: %llu Msecs: %llu ",
    //       sched->switch_start, get_jiffies_64(), get_jiffies_64() - sched->switch_start, jiffies64_to_msecs(get_jiffies_64() - sched->switch_start));

    sched->switch_time = jiffies64_to_msecs(get_jiffies_64() - sched->switch_start);

    // Wake up the scheduler
    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Waking up scheduler %d (CPU: %u - real: %u)", sched->tid, sched->cpu, sched->ts->cpu);

    wake_up_process(sched->ts);

    return 0;
}

/**
 * yield_worker - relinquishes the CPU from the worker asking so,
 * and puts it to sleep
 * @data: data coming from usermode via @DEVICE - here used to receive
 * the pid of the worker that is yielding the CPU
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static int yield_worker(unsigned long data) {
    int status, i;
    struct task_struct *p;
    unsigned int c;

    unsigned long long tst2;

    UMSScheduler *sched;

    UMSWorker_Thread *current_worker;
    struct list_head *current_item_list;

    p = current;

    if (copy_from_user(&status, (int *)data, sizeof(int))) {
        pr_err("Data Write : Err!\n");
        return FAILURE;
    }

    printk(KERN_DEBUG MODULE_NAME_LOG "[Worker] Yielding: %d (%d) (CPU: %u)\n", p->pid, status, p->cpu);

    if (down_interruptible(&compl_sem))
        pr_err("\nTask %d sleeping on compl_sem (handle_yielding) interrupted by a signal!\n", current->pid);

    tst2 = get_jiffies_64();

    list_for_each(current_item_list, &completion_list) {
        current_worker = list_entry(current_item_list, UMSWorker_Thread, list);

        if (current_worker->tid == p->pid) {
            printk(KERN_DEBUG MODULE_NAME_LOG "Worker found!");
            printk(KERN_DEBUG MODULE_NAME_LOG "current_item->id=%d (%d) - status: %d, bound: %u (real %u)\n",
                   current_worker->id, current_worker->tid, current_worker->status, current_worker->bound, p->cpu);

            c = current_worker->bound;
            goto found_yielding_worker;
        }
    }

found_yielding_worker:

#if TEST2 == 1
    printk(KERN_DEBUG MODULE_NAME_LOG "[%d] JIFFIES for retrieve (exe) = Start: %llu End: %llu Diff: %llu Msecs: %llu \n", current->pid,
           tst2, get_jiffies_64(), get_jiffies_64() - tst2, jiffies64_to_msecs(get_jiffies_64() - tst2));
#endif

    /* Retrieve worker's scheduler */
    i = (int)c;
    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Attempting to retrieve scheduler with id %d (tot: %u)", i, cpus);

    sched = schedulers_arr_p[i];

    printk(KERN_DEBUG MODULE_NAME_LOG "Scheduler %d (CPU: %u)", sched->tid, sched->cpu);

    p->state = TASK_INTERRUPTIBLE;
    current_worker->status = TASK_INTERRUPTIBLE;
    current_worker->bound = UNBOUND;
    current_worker->tbound = UNBOUND;
    current_worker->switches_cnt += 1;

    /* Move the worker at the end of the completion list to ensure scheduling fairness */

    list_del(&current_worker->list);
    list_add(&current_worker->list, &completion_list);
    completion_list_cnt -= 1;

    up(&compl_sem);

    printk(KERN_DEBUG MODULE_NAME_LOG "[UMS] Waking up scheduler %d (CPU: %u - real: %u)", sched->tid, sched->cpu, sched->ts->cpu);

    //printk(KERN_DEBUG MODULE_NAME_LOG "[Sched-%d] JIFFIES = Start: %llu End: %llu Diff: %llu Msecs: %llu ", sched->tid,
    //       sched->switch_start, get_jiffies_64(), get_jiffies_64() - sched->switch_start, jiffies64_to_msecs(get_jiffies_64() - sched->switch_start));

    /* Wake up the scheduler */
    sched->switch_time = jiffies64_to_msecs(get_jiffies_64() - sched->switch_start);
    wake_up_process(sched->ts);

    schedule();

    return 0;
}

/**
 * device_ioctl - Receives the requests to the module issued from user-space 
 * and dispatches them to the appropriate handler function
 * @file: file struct representing the device
 * @request: request number, needed to select the right handler function
 * @data: data received/to be sent from/to user-space
 * 
 * Returns @SUCCESS if successful, @FAILURE otherwise
 */
static long device_ioctl(struct file *file, unsigned int request, unsigned long data) {
    int req;

    printk(KERN_DEBUG MODULE_NAME_LOG "device_ioctl: pid->%d, path=%s, request=%u\n", current->pid, file->f_path.dentry->d_iname, request);

    req = request;

    printk(KERN_DEBUG MODULE_NAME_LOG "\n\nSwitching on %d\n", req);

    switch (req) {
        case INITIAL_SETUP:
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Received initial info from the module\n", INITIAL_SETUP);

            initial_setup(data);

            break;
        case CREATE_SCHED: /* Receive a pthread to be converted into scheduler thread */
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Received scheduler thread\n", CREATE_SCHED);

            create_ums_scheduler_thread(data);

            break;

        case CREATE_WORKER: /* Receive a pthread to be converted into worker thread, add it to the list & put it to sleep */
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Received worker thread\n", CREATE_WORKER);

            create_worker_thread(data);

            break;

        case COMPUTE_READY: /* Update the list of available workers, if none is available block */
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Compute ready workers\n", COMPUTE_READY);

            compute_ready_workers(data);

            break;

        case GET_READY: /* Take one element from the ready list & return it to user space */
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Return one element from the ready list\n", GET_READY);

            get_ready_worker(data);

            break;
        case EXE_WORKER: /* Execute thread */
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Execute worker thread\n", EXE_WORKER);

            execute_worker(data);

            break;

        case WORK_END: /* Worker finished running */
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Worker thread ended executing\n", WORK_END);

            handle_ending_worker(data);

            break;

        case WORK_YIELD: /* Worker called yield */
            printk(KERN_DEBUG MODULE_NAME_LOG "%d - Worker thread yielding\n", WORK_YIELD);

            yield_worker(data);

            break;

        default:
            printk(KERN_DEBUG MODULE_NAME_LOG "Default\n");
    }

    return SUCCESS;
}

module_init(ums_init_module);
module_exit(ums_cleanup_module);