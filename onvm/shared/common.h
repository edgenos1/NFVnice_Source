/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2016 George Washington University
 *            2015-2016 University of California Riverside
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
 * common.h - shared data between host and NFs
 ********************************************************************/

#ifndef _COMMON_H_
#define _COMMON_H_

#include <rte_mbuf.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#define ONVM_MAX_CHAIN_LENGTH 4   // the maximum chain length
#define MAX_CLIENTS 16            // total number of NFs allowed
#define MAX_SERVICES 16           // total number of unique services allowed
#define MAX_CLIENTS_PER_SERVICE 8 // max number of NFs per service.

#define ONVM_NF_ACTION_DROP 0   // drop packet
#define ONVM_NF_ACTION_NEXT 1   // to whatever the next action is configured by the SDN controller in the flow table
#define ONVM_NF_ACTION_TONF 2   // send to the NF specified in the argument field (assume it is on the same host)
#define ONVM_NF_ACTION_OUT 3    // send the packet out the NIC port set in the argument field

/* Note: Make the PACKET_READ_SIZE defined in onvm_mgr.h same as PKT_READ_SIZE defined in onvm_nflib_internal.h, better get rid of latter */
#define PRE_PROCESS_DROP_ON_RX  // To lookup NF Tx queue occupancy and drop packets pro-actively before pushing to NFs Rx Ring.
//#define DROP_APPROACH_1
//#define DROP_APPROACH_2s
#define DROP_APPROACH_3
//#define DROP_APPROACH_3_WITH_YIELD

#define INTERRUPT_SEM           // To enable NF thread interrupt mode wake.  Better to move it as option in Makefile
#define USE_SEMAPHORE           // Use Semaphore for IPC
//#define USE_MQ                // USe Message Queue for IPC between NFs and NF manager
//#define USE_FIFO              // Use Named Pipe (FIFO) -- cannot work in our model as Writer cannot be opened in nonblock
//#define USE_SIGNAL             // Use Signals (SIGUSR1) for IPC
//#define USE_SCHED_YIELD         // Use Explicit CPU Relinquish CPU, no explicit IPC other than shared mem read/write
//#define USE_NANO_SLEEP         // Use Sleep call to Reqlinqush CPU no explicit IPC other than shared mem read/write
//#define USE_SOCKET              // Use socket for IPC, NFs block on recv and mgr sends to ublock clients
//#define USE_FLOCK               // USE FILE_LOCK PREMITIVE for Blocking the NFs and mgr opens files in locked mode
//#define USE_MQ2                 // USE SYS_V5 Message Queue
//#define USE_ZMQ                 // Use ZeroMQ sockets for communication
#if (defined(INTERRUPT_SEM) && !defined(USE_SEMAPHORE) && !defined(USE_MQ) && !defined(USE_FIFO) && !defined(USE_SIGNAL) \
&& !defined(USE_SCHED_YIELD) && !defined(USE_NANO_SLEEP) && !defined(USE_SOCKET) && !defined(USE_FLOCK) && !defined(USE_MQ2) && !defined(USE_ZMQ))
#define USE_POLL_MODE
#endif

#ifdef USE_ZMQ
#include <zmq.h>
#endif


//#ifdef USE_MQ2
//typedef struct msgbuf { long mtype; char mtext[32];}msgbuf_t;
//#endif

//extern uint8_t rss_symmetric_key[40];

struct onvm_pkt_meta {
        uint8_t action; /* Action to be performed */
        uint16_t destination; /* where to go next */
        uint16_t src; /* who processed the packet last */
	uint8_t chain_index; /*index of the current step in the service chain*/
};
static inline struct onvm_pkt_meta* onvm_get_pkt_meta(struct rte_mbuf* pkt) {
        return (struct onvm_pkt_meta*)&pkt->udata64;
}

static inline uint8_t onvm_get_pkt_chain_index(struct rte_mbuf* pkt) {
        return ((struct onvm_pkt_meta*)&pkt->udata64)->chain_index;
}

/*
 * Define a structure with stats from the clients.
 */
struct client_tx_stats {
        /* these stats hold how many packets the manager will actually receive,
         * and how many packets were dropped because the manager's queue was full.
         */
        volatile uint64_t tx[MAX_CLIENTS];
        volatile uint64_t tx_drop[MAX_CLIENTS];
        volatile uint64_t tx_buffer[MAX_CLIENTS];
        volatile uint64_t tx_returned[MAX_CLIENTS];

        #ifdef INTERRUPT_SEM
        volatile uint64_t wkup_count[MAX_CLIENTS];
        volatile uint64_t prev_tx[MAX_CLIENTS];
        volatile uint64_t prev_tx_drop[MAX_CLIENTS];
        volatile uint64_t comp_cost[MAX_CLIENTS];
        volatile uint64_t prev_wkup_count[MAX_CLIENTS];
        #endif  //INTERRUPT_SEM

        #ifdef PRE_PROCESS_DROP_ON_RX
        #ifdef DROP_APPROACH_1
        volatile uint64_t tx_predrop[MAX_CLIENTS];
        #endif  //DROP_APPROACH_1
        #endif  //PRE_PROCESS_DROP_ON_RX

        /* FIXME: Why are these stats kept separately from the rest?
         * Would it be better to have an array of struct client_tx_stats instead
         * of putting the array inside the struct? How can we avoid cache
         * invalidations from different NFs updating these stats?
         */
};

extern struct client_tx_stats *clients_stats;

/*
 * Define a structure to describe one NF
 */
struct onvm_nf_info {
        uint16_t instance_id;
        uint16_t service_id;
        uint8_t status;
        const char *tag;
        #if defined (INTERRUPT_SEM) && defined (USE_SIGNAL)
        pid_t pid;
        #endif
};

/*
 * Define a structure to describe a service chain entry
 */
struct onvm_service_chain_entry {
	uint16_t destination;
	uint8_t action;
};

struct onvm_service_chain {
	struct onvm_service_chain_entry sc[ONVM_MAX_CHAIN_LENGTH];
	uint8_t chain_length;
	int ref_cnt;
};

/* define common names for structures shared between server and client */
#define MP_CLIENT_RXQ_NAME "MProc_Client_%u_RX"
#define MP_CLIENT_TXQ_NAME "MProc_Client_%u_TX"
#define PKTMBUF_POOL_NAME "MProc_pktmbuf_pool"
#define MZ_PORT_INFO "MProc_port_info"
#define MZ_CLIENT_INFO "MProc_client_info"
#define MZ_SCP_INFO "MProc_scp_info"
#define MZ_FTP_INFO "MProc_ftp_info"

/* interrupt semaphore specific updates */
#ifdef INTERRUPT_SEM
#define SHMSZ 4                         // size of shared memory segement (page_size)
#define KEY_PREFIX 123                  // prefix len for key
#ifdef USE_MQ
#define MP_CLIENT_SEM_NAME "/MProc_Client_%u_SEM"
#elif defined USE_FIFO
#define MP_CLIENT_SEM_NAME "/MProc_Client_%u_SEM"
#elif defined USE_SOCKET
#define MP_CLIENT_SEM_NAME "/MProc_Client_%u_SEM"
#elif defined USE_ZMQ
#define MP_CLIENT_SEM_NAME "ipc:///MProc_Client_%u_SEM"
#else
#define MP_CLIENT_SEM_NAME "MProc_Client_%u_SEM"
#endif //USE_MQ
#define MONITOR                         // Unused remove it
#define ONVM_NUM_WAKEUP_THREADS 1
#define CHAIN_LEN 4                     // Duplicate, remove and instead use ONVM_MAX_CHAIN_LENGTH
#define SAMPLING_RATE 1000000           // sampling rate to estimate NFs computation cost
#define ONVM_SPECIAL_NF 0               // special NF for flow table entry management
#endif


/* common names for NF states */
#define _NF_QUEUE_NAME "NF_INFO_QUEUE"
#define _NF_MEMPOOL_NAME "NF_INFO_MEMPOOL"

#define NF_WAITING_FOR_ID 0     // First step in startup process, doesn't have ID confirmed by manager yet
#define NF_STARTING 1           // When a NF is in the startup process and already has an id
#define NF_RUNNING 2            // Running normally
#define NF_PAUSED  3            // NF is not receiving packets, but may in the future
#define NF_STOPPED 4            // NF has stopped and in the shutdown process
#define NF_ID_CONFLICT 5        // NF is trying to declare an ID already in use
#define NF_NO_IDS 6             // There are no available IDs for this NF

#define NF_NO_ID -1

/*
 * Given the rx queue name template above, get the queue name
 */
static inline const char *
get_rx_queue_name(unsigned id) {
        /* buffer for return value. Size calculated by %u being replaced
         * by maximum 3 digits (plus an extra byte for safety) */
        static char buffer[sizeof(MP_CLIENT_RXQ_NAME) + 2];

        snprintf(buffer, sizeof(buffer) - 1, MP_CLIENT_RXQ_NAME, id);
        return buffer;
}

/*
 * Given the tx queue name template above, get the queue name
 */
static inline const char *
get_tx_queue_name(unsigned id) {
        /* buffer for return value. Size calculated by %u being replaced
         * by maximum 3 digits (plus an extra byte for safety) */
        static char buffer[sizeof(MP_CLIENT_TXQ_NAME) + 2];

        snprintf(buffer, sizeof(buffer) - 1, MP_CLIENT_TXQ_NAME, id);
        return buffer;
}

#ifdef INTERRUPT_SEM
/*
 * Given the rx queue name template above, get the key of the shared memory
 */
static inline key_t
get_rx_shmkey(unsigned id)
{
        return KEY_PREFIX * 10 + id;
}

/*
 * Given the sem name template above, get the sem name
 */
static inline const char *
get_sem_name(unsigned id)
{
        /* buffer for return value. Size calculated by %u being replaced
         * by maximum 3 digits (plus an extra byte for safety) */
        static char buffer[sizeof(MP_CLIENT_SEM_NAME) + 2];

        snprintf(buffer, sizeof(buffer) - 1, MP_CLIENT_SEM_NAME, id);
        return buffer;
}
#endif

#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

#endif  // _COMMON_H_
