/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2016 George Washington University
 *            2015-2016 University of California Riverside
 *            2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ********************************************************************/


/******************************************************************************
                                 onvm_wakemgr.c

            This file contains all functions related to NF wakeup management.

******************************************************************************/

#include "onvm_mgr.h"
#include "onvm_pkt.h"
#include "onvm_nf.h"
#include "onvm_wakemgr.h"


#ifdef INTERRUPT_SEM
#include <signal.h>

//#define USE_NF_WAKE_THRESHOLD
#ifdef USE_NF_WAKE_THRESHOLD
unsigned nfs_wakethr[MAX_CLIENTS] = {[0 ... MAX_CLIENTS-1] = 1};
#endif

struct wakeup_info *wakeup_infos;

/***********************Internal Functions************************************/
static inline int
whether_wakeup_client(int instance_id);

static inline void handle_wakeup_old(struct wakeup_info *wakeup_info);
static inline void
wakeup_client(int instance_id, struct wakeup_info *wakeup_info);
static inline int
wakeup_client_internal(int instance_id);

#define WAKE_INTERVAL_IN_US     (100)   //100 micro seconds
#define USLEEP_INTERVAL         (50)    //50 micro seconds
//Note: sleep of 50us and wake_interval of 100us reduces CPU utilization from 100 to 0.3
//Ideal: Get rid of wake thread and merge the functionality with the main_thread.

#ifdef ENABLE_USE_RTE_TIMER_MODE_FOR_WAKE_THREAD
struct rte_timer wake_timer[ONVM_NUM_WAKEUP_THREADS];
static void wake_timer_cb(struct rte_timer *ptr_timer, void *ptr_data);
int initialize_wake_timers(void *data);
#endif //USE_RTE_TIMER_MODE_FOR_WAKE_THREAD

/***********************Timer Functions************************************/
#ifdef ENABLE_USE_RTE_TIMER_MODE_FOR_WAKE_THREAD
static void wake_timer_cb(__attribute__((unused)) struct rte_timer *ptr_timer, void *ptr_data) {

        if(ptr_data) {
                //handle_wakeup((struct wakeup_info *)ptr_data);
                handle_wakeup(NULL);
        }
}

int
initialize_wake_timers(void *data) {
        static uint8_t index = 0;

        if(ONVM_NUM_WAKEUP_THREADS && index >= ONVM_NUM_WAKEUP_THREADS) return -1;

        rte_timer_init(&wake_timer[index]);

        uint64_t ticks = 0;
        ticks = ((uint64_t)WAKE_INTERVAL_IN_US *(rte_get_timer_hz()/1000000));

        rte_timer_reset_sync(&wake_timer[index],
                ticks,
                PERIODICAL,
                rte_lcore_id(),
                &wake_timer_cb, data
                );

        index++;
        return 0;
}
#endif //ENABLE_USE_RTE_TIMER_MODE_FOR_WAKE_THREAD
/***********************Timer Functions************************************/

/***********************Internal Functions************************************/
/*
 * Return status:
 *                  0: wakeup signal not required
 *                  1: wakeup signal is required
 *                 -1: issue forced block/goto_sleep signal
 */
static inline int
whether_wakeup_client(int instance_id)
{

        if (clients[instance_id].rx_q == NULL) {
                return 0;
        }

        #ifdef ENABLE_NF_BACKPRESSURE
        #ifdef NF_BACKPRESSURE_APPROACH_2
        /* Block the upstream (earlier) NFs from getting scheduled, if there is NF at downstream that is bottlenecked! */
        if (downstream_nf_overflow) {
                if (clients[instance_id].info != NULL && is_upstream_NF(highest_downstream_nf_service_id,clients[instance_id].info->service_id)) {
                        throttle_count++;
                        return -1;
                }
        }
        //service chain case
        else if (clients[instance_id].throttle_this_upstream_nf) {
                clients[instance_id].throttle_count++;
                return -1;
        }
        #endif //NF_BACKPRESSURE_APPROACH_2
        #endif //ENABLE_NF_BACKPRESSURE

#ifdef USE_NF_WAKE_THRESHOLD
        uint16_t cur_entries;
        cur_entries = rte_ring_count(clients[instance_id].rx_q);
        if (cur_entries >= nfs_wakethr[instance_id]) {
                return 1;
        }
#else
        if(rte_ring_count(clients[instance_id].rx_q)) return 1;
#endif
        return 0;
}

static inline void
notify_client(int instance_id)
{
        #ifdef USE_MQ
        static int msg = '\0';
        //struct timespec timeout = {.tv_sec=0, .tv_nsec=1000};
        //clock_gettime(CLOCK_REALTIME, &timeout);timeout..tv_nsec+=1000;
        //msg = (unsigned int)mq_timedsend(clients[instance_id].mutex, (const char*) &msg, sizeof(msg),(unsigned int)prio, &timeout);
        //msg = (unsigned int)mq_send(clients[instance_id].mutex, (const char*) &msg, sizeof(msg),(unsigned int)prio);
        msg = mq_send(clients[instance_id].mutex, (const char*) &msg,0,0);
        if (0 > msg) { perror ("mq_send failed!");}
        #endif

        #ifdef USE_FIFO
        unsigned msg = 1;
        msg = write(clients[instance_id].mutex, (void*) &msg, sizeof(msg));
        #endif


        #ifdef USE_SIGNAL
        //static int count = 0;
        //if (count < 100) { count++;
        int sts = sigqueue(clients[instance_id].info->pid, SIGUSR1, (const union sigval)0);
        if (sts) perror ("sigqueue failed!!");
        //}
        #endif

        #ifdef USE_SEMAPHORE
        sem_post(clients[instance_id].mutex);
        #endif

        #ifdef USE_SCHED_YIELD
        rte_atomic16_read(clients[instance_id].shm_server);
        #endif

        #ifdef USE_NANO_SLEEP
        rte_atomic16_read(clients[instance_id].shm_server);
        #endif

        #ifdef USE_SOCKET
        static char msg[2] = "\0";
        sendto(onvm_socket_id, msg, sizeof(msg), 0, (struct sockaddr *) &clients[instance_id].mutex, (socklen_t) sizeof(struct sockaddr_un));
        #endif

        #ifdef USE_FLOCK
        if (0 > (flock(clients[instance_id].mutex, LOCK_UN|LOCK_NB))) { perror ("FILE UnLock Failed!!");}
        #endif

        #ifdef USE_MQ2
        static unsigned long msg = 1;
        //static msgbuf_t msg = {.mtype = 1, .mtext[0]='\0'};
        //if (0 > msgsnd(clients[instance_id].mutex, (const void*) &msg, sizeof(msg.mtext), IPC_NOWAIT)) {
        if (0 > msgsnd(clients[instance_id].mutex, (const void*) &msg, 0, IPC_NOWAIT)) {
                perror ("Msgsnd Failed!!");
        }
        #endif

        #ifdef USE_ZMQ
        static char msg[2] = "\0";
        zmq_connect (onvm_socket_id,get_sem_name(instance_id));
        zmq_send (onvm_socket_id, msg, sizeof(msg), 0);
        #endif

        #ifdef USE_POLL_MODE
        rte_atomic16_read(clients[instance_id].shm_server);
        #endif
}


static inline int
wakeup_client_internal(int instance_id) {
        int ret = whether_wakeup_client(instance_id);
        if ( 1 == ret) {
                if (rte_atomic16_read(clients[instance_id].shm_server) ==1) {
                        rte_atomic16_set(clients[instance_id].shm_server, 0);
                        notify_client(instance_id);
                }
        }
        #ifdef ENABLE_NF_BACKPRESSURE
        #ifdef NF_BACKPRESSURE_APPROACH_2
        else if (-1 == ret) {
                /* Make sure to set the flag here and check for flag in nf_lib and block */
                rte_atomic16_set(clients[instance_id].shm_server, 1);
        }
        #endif //NF_BACKPRESSURE_APPROACH_2
        #endif //ENABLE_NF_BACKPRESSURE
        return ret;
}

static inline void
wakeup_client(int instance_id, struct wakeup_info *wakeup_info)  {

        int ret = wakeup_client_internal(instance_id);

        if(1 == ret && wakeup_info) {
                wakeup_info->num_wakeups += 1;
                clients[instance_id].stats.wakeup_count+=1;
        }
        return;

#if 0
        int wkup_sts = whether_wakeup_client(instance_id);
        if ( wkup_sts == 1) {
                if (rte_atomic16_read(clients[instance_id].shm_server) ==1) {
                        wakeup_info->num_wakeups += 1;
                        //if(wakeup_info->num_wakeups) {}//populate_and_sort_rdata();}
                        clients[instance_id].stats.wakeup_count+=1;
                        rte_atomic16_set(clients[instance_id].shm_server, 0);
                        notify_client(instance_id);
                }
        }
        #ifdef ENABLE_NF_BACKPRESSURE
        #ifdef NF_BACKPRESSURE_APPROACH_2
        else if (-1 == wkup_sts) {
                /* Make sure to set the flag here and check for flag in nf_lib and block */
                rte_atomic16_set(clients[instance_id].shm_server, 1);
        }
        #endif //NF_BACKPRESSURE_APPROACH_2
        #endif //ENABLE_NF_BACKPRESSURE
#endif
}

static inline void handle_wakeup_old(struct wakeup_info *wakeup_info) {

        unsigned i=0;
        for (i = wakeup_info->first_client; i < wakeup_info->last_client; i++) {
                wakeup_client(i, wakeup_info);
        }
}
inline void handle_wakeup(__attribute__((unused))struct wakeup_info *wakeup_info) {

        unsigned i=0;

        /* Firs:t extract load charactersitics in this epoch
         * Second: sort and prioritize NFs based on the demand matrix in this epoch
         * Finally: wake up the tasks in the identified priority
         * */
        #if defined (USE_CGROUPS_PER_NF_INSTANCE)
        extract_nf_load_and_svc_rate_info(0);    //setup_nfs_priority_per_core_list(0);

        /* Now wake up the NFs as per sorted priority:
         * Next step Handle slack period before wake-up and schedule NFs for wake up; otherwise
         * we are at the mercy of OS Scheduler to schedule the NFs in each core */
        if(nf_sched_param.sorted) {
                for(i=0; i<MAX_CORES_ON_NODE; i++) {
                        if(nf_sched_param.nf_list_per_core[i].sorted && nf_sched_param.nf_list_per_core[i].count) {
                                unsigned nf_id=0;
                                for(nf_id=0; nf_id < nf_sched_param.nf_list_per_core[i].count; nf_id++) {
                                        wakeup_client(nf_sched_param.nf_list_per_core[i].nf_ids[nf_id], NULL /*wakeup_info*/);
                                }
                        }
                }
        }
        #else
        handle_wakeup_old(wakeup_info);
        #endif  //USE_CGROUPS_PER_NF_INSTANCE
}

int
wakeup_nfs(void *arg) {

#ifdef ENABLE_USE_RTE_TIMER_MODE_FOR_WAKE_THREAD
        initialize_wake_timers(arg);
#endif

        while (true) {
#ifdef ENABLE_USE_RTE_TIMER_MODE_FOR_WAKE_THREAD
                rte_timer_manage();
                usleep(USLEEP_INTERVAL);
#else
                handle_wakeup_old((struct wakeup_info *)arg);
                usleep(WAKE_INTERVAL_IN_US);
#endif //#ifdef ENABLE_USE_RTE_TIMER_MODE_FOR_WAKE_THREAD

        }

        return 0;
}

static void signal_handler(int sig, siginfo_t *info, void *secret) {
        int i;
        (void)info;
        (void)secret;

        //2 means terminal interrupt, 3 means terminal quit, 9 means kill and 15 means termination
        printf("Got Signal [%d]\n", sig);
        if(info) {
                printf("[signo: %d,errno: %d,code: %d]\n", info->si_signo, info->si_errno, info->si_code);
        }
        if(sig == SIGWINCH) return;

        if (sig <= 15) {
                for (i = 1; i < MAX_CLIENTS; i++) {

                        #ifdef USE_MQ
                        mq_close(clients[i].mutex);
                        mq_unlink(clients[i].sem_name);
                        #endif

                        #ifdef USE_FIFO
                        close(clients[i].mutex);
                        unlink(clients[i].sem_name);
                        #endif

                        #ifdef USE_SIGNAL
                        #endif

                        #ifdef USE_SOCKET
                        #endif

                        #ifdef USE_SEMAPHORE
                        sem_close(clients[i].mutex);
                        sem_unlink(clients[i].sem_name);
                        #endif

                        #ifdef USE_FLOCK
                        flock(clients[i].mutex, LOCK_UN|LOCK_NB);
                        close(clients[i].mutex);
                        #endif

                        #ifdef USE_MQ2
                        msgctl(clients[i].mutex, IPC_RMID, 0);
                        #endif

                        #ifdef USE_ZMQ
                        zmq_close(onvm_socket_id);
                        zmq_ctx_destroy(onvm_socket_ctx);
                        #endif

                }
                #ifdef MONITOR
//                rte_free(port_stats);
//                rte_free(port_prev_stats);
                #endif
        }

        exit(10);
}

void
register_signal_handler(void) {
        unsigned i;
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_SIGINFO;
        act.sa_handler = (void *)signal_handler;

        for (i = 1; i < 31; i++) {
                if(i == SIGWINCH)continue;
                sigaction(i, &act, 0);
        }
}

#endif //INTERRUPT_SEM


#ifdef SORT_EFFICEINCY_TET
static int rdata[MAX_CLIENTS];
void quickSort( int a[], int l, int r);
int partition( int a[], int l, int r);
void quickSort( int a[], int l, int r)
{
   int j;

   if( l < r )
   {
    // divide and conquer
        j = partition( a, l, r);
       quickSort( a, l, j-1);
       quickSort( a, j+1, r);
   }

}

int partition( int a[], int l, int r) {
   int pivot, i, j, t;
   pivot = a[l];
   i = l; j = r+1;

   while( 1)
   {
    do ++i; while( a[i] <= pivot && i <= r );
    do --j; while( a[j] > pivot );
    if( i >= j ) break;
    t = a[i]; a[i] = a[j]; a[j] = t;
   }
   t = a[l]; a[l] = a[j]; a[j] = t;
   return j;
}
void populate_and_sort_rdata(void);
void populate_and_sort_rdata(void) {
        unsigned i = 0;
        for (i=0; i< MAX_CLIENTS; i++) {
                uint16_t demand = rte_ring_count(clients[i].rx_q);
                uint16_t offload = rte_ring_count(clients[i].tx_q);
                uint16_t ccost   = clients[i].info->comp_cost;
                uint32_t prio = demand*ccost - offload;
                rdata[i] = prio;
        }
        quickSort(rdata, 0, MAX_CLIENTS-1);
}
#endif
