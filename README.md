# AOSV Final Project Report
_A.Y. 2020/2021_

Author(s): Giacomo Priamo (1701568)

# Introduction

This project was developed as a **kernel module** for the linux kernel version `5.12.19`. 

It mainly consists of 4 files:
* `/src/module/ums.c` contains the implementation of the kernel module. 
* `/src/user-space/libUMS.c` is the C library that can be used to interact with the module from user-space.
* `/src/user-space/libUMS.h` is the header file for the aforementioned library.
* `/src/user-space/example.c` contains the example code to test the library.

Besides the 4 strictly required APIs, also the `GetNextUmsListItem` API was implemented.

# Usage
## Initial operations
A user-space program that wants to use this module can call the `UMSScheduler()` function of the library to start setting up the environment in order to use the UMS Scheduling facility, this function takes as input:
1. the number of worker threads to be created;
2. the entrypoint function for the UMS scheduler threads;
3. the function the worker threads should execute;

and is tasked to call the `enter_ums_scheduling_mode()`\* function, which creates as many threads as the number of CPUs/cores in the system and makes them run the entrypoint function.

The scheduler threads' entrypoint function is wrapped by `entrypoint_wrapper()` in the library, which is tasked with binding the pthread to a specific core/CPU and introducing it the to the module, which will transform it into a UMS scheduler thread via the `create_ums_scheduler_thread()` function. Eventually, the wrapper executes the entrypoint function passed by the user.

Also the workers' routine is wrapped (by `worker_wrapper()`) in a similar way as for the scheduler threads. 

Wrapping both the entrypoints gives transparency to the programmer and relieves them from performing those operations by themselves.

\* _Note: the user is not obliged to use `UMSScheduler()`, they can also decide to use `enter_ums_scheduling_mode()` directly, but this requires them to create the completion list by themselves (as a struct of type `UMSinfo`)._

## Scheduling operations
The UMS scheduler's entrypoint function can now call the APIs to interact with the module, in particular it should:
1. call `dequeue_ums_completion_list_items()`, which tells the module to build the list of worker threads ready to be executed and returns its size.
2. call `get_next_ums_list_item()` to ask the module to assign a worker thread to the scheduler (this can be called as many times as the user wants, provided that he builds an array of the appropriate size, like `run_queue` in [`example.c`](../src/user-space/example.c)).
3. call `execute_ums_thread()` to tell the module to execute a worker. 

This cycle can be repeated as long as there are worker threads that need to be executed.

# The module
## Interaction
The library interacts with the module using the _character device_ "umschardev".

The main entrypoint to the module is the `device_ioctl()` function, which receives the requests from user-space (issued via the `ioctl()` syscall) and, depending on the value of the `request` parameter, dispatches the request to the appropriate function (using a `switch-case` construct). 

In order to transfer data from and to user-space, the `data` parameter is used jointly with the `copy_from_user()` and `copy_to_user()` functions.

## Completion list
The worker threads are collected in the _linked list_ called `completion_list` as soon as they introduce themselves to the module, which is used for retrieving the available workers and keep track of those whose execution has ended by removing them.

The workers are also put in the `completion_list_backup` _linked list_ to keep track of all the existing workers. This list is used for reading purposes only and no element is ever removed from it.

## Retrieving the ready workers
When a scheduler thread calls `dequeue_ums_completion_list_items()`, instead of returning a list of available threads to user-space, the ready worker threads are temporarily bound - using the `tbound` field of the `UMSWorker_Thread` struct - to the scheduler issuing the call, so that it can reserve as many workers as it wants (using `get_next_ums_list_item()`) without worrying about concurrent schedulers trying to do the same operation at the same time.

These operations are carried out by the `update_ready_workers()` function, which also returns the number of available workers (i.e. the workers that have successfully been assigned to the scheduler). In case the number of available workers is 0, the back-end of `dequeue_ums_completion_list_items()` blocks the current scheduler thread inside a loop where it sleeps for 1 second and then tries again to reserve some worker threads for itself. 

In case the completion list is empty, `-1` is returned to user-space, in order to communicate that there are no more workers to be executed. 

After that, the scheduler can be assigned some worker threads using `get_next_ums_list_item()`, whose back-end implementation permanently binds a worker to a scheduler - using the `bound` field of the `UMSWorker_Thread` struct - until said worker finishes executing or yields.

## Executing a worker: Switching between processes
When calling `execute_ums_thread()`, the first operation the module performs is releasing all the temporarily-bound workers, so that other schedulers can exit the loop from `dequeue_ums_completion_list_items()` and request some worker threads for themselves.

After releasing the unreserved workers, the module resumes the execution of the worker thread specified by the scheduler. In order to do this, **process states** are used:
* As soon as a worker pthread is introduced to the module (`create_worker_thread()`), it changes its state to _TASK_INTERRUPTIBLE_ and calls `schedule()` to sleep.
* Upon request of executing a worker, the module wakes up the worker using the `wake_up_process()` function, puts the current scheduler to sleep using _TASK_INTERRUPTIBLE_ and calling `schedule()`.

## Worker yielding

When a worker calls the `ums_thread_yield()` API, the module puts it back to sleep with _TASK_INTERRUPTIBLE_, retrieves its scheduler thread and wakes it up, releases the worker from the scheduler (`UMSWorker_Thread->bound`) and eventually calls `schedule()`. The worker is moved to the end of the completion list in order to ensure some fairness in the selection of the workers.

## Worker finishes executing

When a worker ends executing, `worker_wrapper()` communicates it to the module, so that it is removed from the completion list and its scheduler is woken up again.

## Concurrency handling

Concurrency is enforced via _semaphores_ used as _mutexes_. Every access to the _completion list_ is locked by the same semaphore (`compl_sem`) to prevent concurrent accesses and modifications. In this way, scheduler threads can be put to sleep while waiting for their turn to access the list. 

This is a safe and valid solution, as the schedulers run in _process context_, so they are allowed to sleep even in kernel mode.

Semaphores are also used to regulate the access to: `/proc`; the array holding the scheduler threads (`schedulers_arr_p`); and `completion_list_backup`.

## Retrieving info via /proc
A directory tree in `/proc` is created in order to allow the user to retrieve info about the schedulers and workers from user-space.  

The `/proc/ums/<pid>/schedulers` path is created as soon as the module is initialized, while a new directory is added for each scheduler thread as they introduce themselves to the module. 

The `info` entry for each scheduler thread contains the following data:
* ID: the thread's ID, corresponding to the CPU the thread is running on;
* TID: the pthread's tid;
* Switches: the number of switches to a worker thread;
* Last switch time: the time needed for the last worker thread switch. This is measured using the `get_jiffies_64()` function;
* State: SLEEPING or RUNNING. In the first case, also the currently running worker's ID is printed;
* Completion List: the ID of all the worker threads;

As for the workers, an entry is created in the `/proc/ums/<pid>/schedulers/<id>/workers/` path as soon as a worker starts executing on the scheduler with ID <id\>.
The entry contains the following data:
* ID: the ID of the worker thread;
* TID: the pthread's tid;
* Total running time: measured using the `stime` and `utime` fields of the worker thread's `task_struct`;
* Switches: the number of switches;
* Status: RUNNING, SLEEPING or ENDED;

# Versions
## Version 1.x.x
This is the first version of the project that was developed. This version uses _linked lists_ to contain the _completion list_.

## Version 3.x.x
Differently from 1.x.x, this version uses _hash tables_ instead of _linked lists_, this allows to use keys (in this case, the workers' pids were used as keys) to hopefully speed up the retrieval of a worker given its pid (in the module functions `execute_worker()`, `handle_ending_worker()` and `yield_worker()`) as well as the time needed for iterating over the list.

# Results
## Experiments set 1
### Experimental setup: 
**4** cores (=**4** scheduler threads), completion list of **6** workers completely shared among the schedulers. The workers execute no instructions (except for `ums_thread_yield()` for half of them).

### What is measured: 
* The time that passes from when a scheduler invokes `compute_ready_workers()` (the back-end counterpart of `dequeue_ums_completion_list_items()`) to be assigned one or more available workers, up to the moment immediately before the scheduler switches to the worker.
* The time that passes from the call to `compute_ready_workers` up to when the scheduler thread exits due to no available worker threads.

### Why: 
* This is a sensible choice, because it analyzes the delays caused by the use of a _linked list_ (or _hash table_ in `v3.x.x`) and especially those caused by the use of _semaphores_ to enforce concurrency for every access to the completion list.

## Version 1.x.x
### Experiment #1 - 6 workers (3 yielding*), 1 worker per scheduler at a time

#### Run #1:
1. Execution
    * Max switch time: 2032 ms
    * Min switch time: 0 ms
    * Avg switch time: 450.6 ms
    * Frequencies: [0 ms: 6; 1008 ms: 1; 1016 ms: 1; 2032 ms: 1]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*: [0 ms: 4] 

#### Run #2:
1. Execution
    * Max switch time: 1024 ms
    * Min switch time: 0 ms
    * Avg switch time: 113.7 ms
    * Frequencies: [0 ms: 8; 1024 ms: 1]

2. Exit
    * Max exit time: 1028 ms
    * Min exit time: 0 ms
    * Avg exit time: 257 ms
    * Frequencies\*\*: [0 ms: 3; 1028 ms: 1]

#### Run #3:
1. Execution
    * Max switch time: 1016 ms
    * Min switch time: 0 ms
    * Avg switch time: 225.3 ms
    * Frequencies: [0 ms: 7; 1012 ms: 1; 1016 ms: 1]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*: [0 ms: 4] 

\* _this means that 3 workers issued the `ums_thread_yield()` function, therefure there is a total of 9 switches from scheduler to worker_.

\*\*_4 schedulers = 4 exits_.


### Experiment #2 - 6 workers (3 yielding*), 2\*\* workers per scheduler at a time


#### Run #1:
1. Execution
    * Max switch time: 0 ms
    * Min switch time: 0 ms
    * Avg switch time: 0 ms
    * Frequencies: [0 ms: 9]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*\*: [0 ms: 4]

#### Run #2:
1. Execution
    * Max switch time: 2056 ms
    * Min switch time: 0 ms
    * Avg switch time: 343.1 ms
    * Frequencies: [0 ms: 7; 1032 ms: 1; 2056 ms: 1]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*\*: [0 ms: 4]


#### Run #3:
1. Execution
    * Max switch time: 1028 ms
    * Min switch time: 0 ms
    * Avg switch time: 114.2 ms
    * Frequencies: [0 ms: 8; 1028 ms: 1]

2. Exit
    * Max exit time: 1016 ms
    * Min exit time: 0 ms
    * Avg exit time: 254 ms
    * Frequencies\*\*\*: [0 ms: 3; 1016 ms: 1]

\* _this means that 3 workers issued the `ums_thread_yield()` function, therefure there is a total of 9 switches from scheduler to worker_.

\*\* _this means each scheduler tried to call `get_next_ums_list_item()` twice before executing the workers_. 

\*\*\*_4 schedulers = 4 exits_.

## Version 3.x.x
### Experiment #1 - 6 workers (3 yielding*), 1 worker per scheduler at a time

#### Run #1:
1. Execution
    * Max switch time: 1024 ms
    * Min switch time: 0 ms
    * Avg switch time: 225.7 ms
    * Frequencies: [0 ms: 7; 1008 ms: 1; 1024 ms: 1]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*: [0 ms: 4] 

#### Run #2:
1. Execution
    * Max switch time: 1024 ms
    * Min switch time: 0 ms
    * Avg switch time: 226.6 ms
    * Frequencies: [0 ms: 8; 1016 ms: 1; 1024 ms: 1]

2. Exit
    * Max exit time: 1036 ms
    * Min exit time: 0 ms
    * Avg exit time: 259 ms
    * Frequencies\*\*: [0 ms: 3; 1036 ms: 1]

#### Run #3:
1. Execution
    * Max switch time: 2064 ms
    * Min switch time: 0 ms
    * Avg switch time: 460 ms
    * Frequencies: [0 ms: 6; 1032 ms: 1; 1044 ms: 1; 2064 ms: 1]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*: [0 ms: 4] 

\* _this means that 3 workers issued the `ums_thread_yield()` function, therefure there is a total of 9 switches from scheduler to worker_.

\*\*_4 schedulers = 4 exits_.


### Experiment #2 - 6 workers (3 yielding*), 2\*\* workers per scheduler at a time


#### Run #1:
1. Execution
    * Max switch time: 1036 ms
    * Min switch time: 0 ms
    * Avg switch time: 229.7 ms
    * Frequencies: [0 ms: 7; 1032 ms: 1; 1036 ms: 1]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*\*: [0 ms: 4]

#### Run #2:
1. Execution
    * Max switch time: 1028 ms
    * Min switch time: 0 ms
    * Avg switch time: 114.2 ms
    * Frequencies: [0 ms: 8; 1028 ms: 1]

2. Exit
    * Max exit time: 0 ms
    * Min exit time: 0 ms
    * Avg exit time: 0 ms
    * Frequencies\*\*\*: [0 ms: 4]


#### Run #3:
1. Execution
    * Max switch time: 1032 ms
    * Min switch time: 0 ms
    * Avg switch time: 229.3 ms
    * Frequencies: [0 ms: 7; 1032 ms: 2]

2. Exit
    * Max exit time: 1036 ms
    * Min exit time: 0 ms
    * Avg exit time: 259 ms
    * Frequencies\*\*\*: [0 ms: 3; 1036 ms: 1]

\* _this means that 3 workers issued the `ums_thread_yield()` function, therefure there is a total of 9 switches from scheduler to worker_.

\*\* _this means each scheduler tried to call `get_next_ums_list_item()` twice before executing the workers_. 

\*\*\*_4 schedulers = 4 exits_.

## Experiments set 1 - Conclusions

The results for both versions denoted that:
* the locking mechanisms may force some schedulers to get blocked sleeping for up to 2 cycles of `compute_ready_workers()` (as for every iteration in the function's loop, the scheduler sleeps for **1 second**).
* when allowing the schedulers to request more than 1 worker at a time, there can rarely be some unfairness in how many workers are selected by the same scheduler (one scheduler may execute half of the workers, while another one may end up executing only 1).

For the 2 points above, there are no significant differences between version `1.x.x` and `3.x.x`.

---

## Experiments set 2
### Experimental setup: 
**4** cores (=**4** scheduler threads), completion list of **32** workers completely shared among the schedulers. **3** runs per version. The workers execute no instructions (except for `ums_thread_yield()` for half of them).

### What is measured: 
1. The time that passes from when the module starts iterating over the completion list up to when it finds the worker thread it was looking for (in `execute_worker()`, `handle_ending_worker()` and `yield_worker()`).
2. The time that passes from when `enter_ums_scheduling_mode()` creates the scheduler threads up to when all the UMS scheduler threads finish executing (i.e. all the worker threads ended their execution).

### Why: 
1. To see if there are any significant differences between the usage of _linked lists_ (version `1.x.x`) and _hash tables_ (version `3.x.x`).
2. To see the overall time taken by the system.

## Experiments set 2 - Conclusions

1. In a total of 3 runs per version, the time taken by the module to retrieve the searched worker was always **0 ms**, concluding that in this application scenario there is no difference between using a _linked list_ or a _hash table_ for the completion list in this implementation of the project.
2. In a total of 3 runs per version, version `1.x.x` always ended executing in **14 seconds**, as for version `3.x.x`, it ended in **14 seconds** twice and once in **15 seconds**.

<!--# References-> 