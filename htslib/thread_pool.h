/*
Copyright (c) 2013-2016 Genome Research Ltd.
Author: James Bonfield <jkb@sanger.ac.uk>

Redistribution and use in source and binary forms, with or without 
modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, 
this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice, 
this list of conditions and the following disclaimer in the documentation 
and/or other materials provided with the distribution.

   3. Neither the names Genome Research Ltd and Wellcome Trust Sanger
Institute nor the names of its contributors may be used to endorse or promote
products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY GENOME RESEARCH LTD AND CONTRIBUTORS "AS IS" AND 
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
DISCLAIMED. IN NO EVENT SHALL GENOME RESEARCH LTD OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * This file implements a thread pool for multi-threading applications.
 * It consists of two distinct interfaces: thread pools an thread job queues.
 *
 * The pool of threads is given a function pointer and void* data to pass in.
 * This means the pool can run jobs of multiple types, albeit first come
 * first served with no job scheduling except to pick tasks from
 * queues that have room to store the result.
 *
 * Upon completion, the return value from the function pointer is
 * added to back to the queue if the result is required.  We may have
 * multiple queues in use for the one pool.
 *
 * To see example usage, please look at the #ifdef TEST_MAIN code in
 * thread_pool.c.
 */

#ifndef _THREAD_POOL_H_
#define _THREAD_POOL_H_

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

struct t_pool;
struct t_pool_queue;

/*
 * An input job, before execution.
 */
typedef struct t_pool_job {
    void *(*func)(void *arg);
    void *arg;
    struct t_pool_job *next;

    struct t_pool *p;
    struct t_pool_queue *q;
    int serial;
} t_pool_job;

/*
 * An output, after job has executed.
 */
typedef struct t_res {
    struct t_res *next;
    int serial; // sequential number for ordering
    void *data; // result itself
} t_pool_result;

/*
 * A per-thread worker struct.
 */
typedef struct {
    struct t_pool *p;
    int idx;
    pthread_t tid;
    pthread_cond_t  pending_c; // when waiting for a job
    long long wait_time;
} t_pool_worker_t;

/*
 * An IO queue consists of a queue of jobs to execute
 * (the "input" side) and a queue of job results post-
 * execution (the "output" side).
 *
 * We have size limits to prevent either queue from
 * growing too large and serial numbers to ensure
 * sequential consumption of the output.
 *
 * The thread pool may have many hetergeneous tasks, each
 * using its own io_queue mixed into the same thread pool.
 */
typedef struct t_pool_queue {
    struct t_pool *p;                // thread pool
    t_pool_job    *input_head;       // input list
    t_pool_job    *input_tail;
    t_pool_result *output_head;      // output list
    t_pool_result *output_tail;
    int qsize;                       // max size of i/o queues
    int next_serial;                 // next serial for input
    int curr_serial;                 // current serial (next output)

    int n_input;                     // no. items in input queue; was njobs
    int n_output;                    // no. items in output queue
    int n_processing;                // no. items being processed (executing)

    int shutdown;                    // true if pool is being destroyed
    int in_only;                     // if true, don't queue result up.
    pthread_cond_t output_avail_c;   // Signalled on each new output
    pthread_cond_t input_not_full_c; // Input queue is no longer full
    pthread_cond_t input_empty_c;    // Input queue has become empty
    pthread_cond_t none_processing_c;// n_processing has hit zero

    struct t_pool_queue *next, *prev;// to form circular linked list.
} t_pool_queue;

/*
 * The single pool structure itself.
 *
 * This knows nothing about the nature of the jobs or where their
 * output is going, but it maintains a list of queues associated with
 * this pool from which the jobs are taken.
 */
typedef struct t_pool {
    int nwaiting; // how many workers waiting for new jobs
    int njobs;    // how many total jobs are waiting in all queues
    int shutdown; // true if pool is being destroyed

    // I/O queues to check for jobs in and to put results.
    // Forms a circular linked list.  (q_head may be amended
    // to point to the most recently updated.)
    t_pool_queue *q_head;

    // threads
    int tsize;    // maximum number of jobs
    t_pool_worker_t *t;
    // array of worker IDs free
    int *t_stack, t_stack_top;

    // A single mutex used when updating this and any associated structure.
    pthread_mutex_t pool_m;

    // Tracking of average number of running jobs.
    // This can be used to dampen any hysteresis caused by bursty
    // input availability.
    int n_count, n_running;

    // Debugging to check wait time.
    // FIXME: should we just delete these and cull the associated code?
    long long total_time, wait_time;
} t_pool;


/*
 * Creates a worker pool with n worker threads.
 *
 * Returns pool pointer on success;
 *         NULL on failure
 */
t_pool *t_pool_init(int n);

/*
 * Adds an item to the work pool.
 *
 * FIXME: permit q to be NULL, indicating a global/default pool held by
 * the thread pool itself?  This pool would be for jobs that have no
 * output, so fire and forget only with..
 *
 * Returns 0 on success
 *        -1 on failure
 */
int t_pool_dispatch(t_pool *p, t_pool_queue *q,
                    void *(*func)(void *arg), void *arg);
int t_pool_dispatch2(t_pool *p, t_pool_queue *q,
                     void *(*func)(void *arg), void *arg, int nonblock);

/*
 * Flushes the queue, but doesn't exit. This simply drains the queue and
 * ensures all worker threads have finished their current tasks associated
 * with this queue.
 *
 * NOT: This does not mean the worker threads are not executing jobs in
 * another queue.
 *
 * Returns 0 on success;
 *        -1 on failure
 */
int t_pool_queue_flush(t_pool_queue *q);

/*
 * Destroys a thread pool. If 'kill' is true the threads are terminated now,
 * otherwise they are joined into the main thread so they will finish their
 * current work load.
 *
 * Use t_pool_destroy(p,0) after a t_pool_flush(p) on a normal shutdown or
 * t_pool_destroy(p,1) to quickly exit after a fatal error.
 */
void t_pool_destroy(t_pool *p, int kill);

/*
 * Pulls a result off the head of the result queue. Caller should
 * free it (and any internals as appropriate) after use. This doesn't
 * wait for a result to be present.
 *
 * Results will be returned in strict order.
 * 
 * Returns t_pool_result pointer if a result is ready.
 *         NULL if not.
 */
t_pool_result *t_pool_next_result(t_pool_queue *q);
t_pool_result *t_pool_next_result_wait(t_pool_queue *q);

/*
 * Frees a result 'r' and if free_data is true also frees
 * the internal r->data result too.
 */
void t_pool_delete_result(t_pool_result *r, int free_data);

/*
 * Initialises a thread job queue.
 *
 * In_only, if true, indicates that the queue does not need to hold
 * any output.  Otherwise an output queue is used to store the results
 * of processing each input job.
 *
 * Results queue pointer on success;
 *         NULL on failure
 */
t_pool_queue *t_pool_queue_init(t_pool *p, int qsize, int in_only);


/* Deallocates memory for a thread queue */
void t_pool_queue_destroy(t_pool_queue *q);

/*
 * Flushes the thread pool, but doesn't exit. This simply drains the
 * queue and ensures all worker threads have finished their current
 * task if associated with this queue.
 *
 * Returns 0 on success;
 *        -1 on failure
 */
int t_pool_queue_flush(t_pool_queue *q);

/*
 * Returns true if there are no items on the finished thread queue and
 * also none still pending.
 */
int t_pool_queue_empty(t_pool_queue *q);

/*
 * Returns the number of completed jobs on the thread queue.
 */
int t_pool_queue_len(t_pool_queue *q);

/*
 * Returns the number of completed jobs plus the number queued up to run.
 */
int t_pool_queue_sz(t_pool_queue *q);

/*
 * Shutdown a queue.
 *
 * This sets the shutdown flag and wakes any threads waiting on queue
 * condition variables.
 */
void t_pool_queue_shutdown(t_pool_queue *q);

/*
 * Attach and detach a thread pool queue with / from the thread pool
 * scheduler.
 *
 * We need to do attach after making a thread queue, but may also wish
 * to temporarily detach if we wish to stop processing jobs on a specific
 * queue while permitting other queues to continue.
 */
void t_pool_queue_attach(t_pool *p, t_pool_queue *q);
void t_pool_queue_detach(t_pool *p, t_pool_queue *q);

#ifdef __cplusplus
}
#endif

#endif /* _THREAD_POOL_H_ */
