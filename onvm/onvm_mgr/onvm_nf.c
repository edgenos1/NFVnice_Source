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

                              onvm_nf.c

       This file contains all functions related to NF management.

******************************************************************************/


#include "onvm_mgr.h"
#include "onvm_nf.h"
#include "onvm_stats.h"

uint16_t next_instance_id = 0;

#ifdef ENABLE_NF_BACKPRESSURE
// Global mode variables (default service chain without flow_Table entry: can support only 1 flow (i.e all flows have same NFs)
uint8_t  global_bkpr_mode=0;
uint16_t downstream_nf_overflow = 0;
uint16_t highest_downstream_nf_service_id=0;
uint16_t lowest_upstream_to_throttle = 0;
uint64_t throttle_count = 0;
#endif // ENABLE_NF_BACKPRESSURE
//sorted list of NFs based on load requirement on the core
//nfs_per_core_t nf_list_per_core[MAX_CORES_ON_NODE];
nf_schedule_info_t nf_sched_param;

void compute_and_assign_nf_cgroup_weight(void);
void monitor_nf_node_liveliness_via_pid_monitoring(void);
int nf_sort_func(const void * a, const void *b);


#define DEFAULT_NF_CPU_SHARE    (1024)

//Local Data structure to compute nf_load and comp_cost contention on each core
typedef struct nf_core_and_cc_info {
        uint32_t total_comp_cost;       //total computation cost on the core (sum of all NFs computation cost)
        uint32_t total_nf_count;        //total count of the NFs on the core (sum of all NFs)
        uint32_t total_pkts_served;     //total pkts processed on the core (sum of all NFs packet processed).
        uint32_t total_load;            //total pkts (avergae) queued up on the core for processing.
        uint64_t total_load_cost_fct;   //total product of current load and computation cost on core (aggregate demand in total cycles)
}nf_core_and_cc_info_t;
nf_core_and_cc_info_t nf_pool_per_core[MAX_CORES_ON_NODE]; // = {{0,0},}; ////nf_core_and_cc_info_t nf_pool_per_core[rte_lcore_count()+1]; // = {{0,0},};

/********************************Interfaces***********************************/
/*
 * This function computes and assigns weights to each nfs cgroup based on its contention and requirements
 * PRerequisite: clients[]->info->comp_cost and  clients[]->info->load should be already updated.  -- updated by extract_nf_load_and_svc_rate_info()
 */
void compute_and_assign_nf_cgroup_weight(void) {
#if defined (USE_CGROUPS_PER_NF_INSTANCE)


        static int update_rate = 0;
        if (update_rate != 10) {
                update_rate++;
                return;
        }
        update_rate = 0;
        const uint64_t total_cycles_in_epoch = ARBITER_PERIOD_IN_US *(rte_get_timer_hz()/1000000);
        uint16_t nf_id = 0;
        memset(nf_pool_per_core, 0, sizeof(nf_pool_per_core));

        //First build the total cost and contention info per core
        for (nf_id=0; nf_id < MAX_CLIENTS; nf_id++) {
                if (onvm_nf_is_valid(&clients[nf_id])){
                        nf_pool_per_core[clients[nf_id].info->core_id].total_comp_cost += clients[nf_id].info->comp_cost;
                        nf_pool_per_core[clients[nf_id].info->core_id].total_nf_count++;
                        nf_pool_per_core[clients[nf_id].info->core_id].total_load += clients[nf_id].info->load;            //clients[nf_id].info->avg_load;
                        nf_pool_per_core[clients[nf_id].info->core_id].total_pkts_served += clients[nf_id].info->svc_rate; //clients[nf_id].info->avg_svc;
                        nf_pool_per_core[clients[nf_id].info->core_id].total_load_cost_fct += (clients[nf_id].info->comp_cost*clients[nf_id].info->load);
                }
        }

        //evaluate and assign the cost of each NF
        for (nf_id=0; nf_id < MAX_CLIENTS; nf_id++) {
                if ((onvm_nf_is_valid(&clients[nf_id])) && (clients[nf_id].info->comp_cost)) {

                        // share of NF = 1024* NF_comp_cost/Total_comp_cost
                        //Note: ideal share of NF is 100%(1024) so for N NFs sharing core => N*100 or (N*1024) then divide the cost proportionally
#ifndef USE_DYNAMIC_LOAD_FACTOR_FOR_CPU_SHARE
                        //Static accounting based on computation_cost_only
                        if(nf_pool_per_core[clients[nf_id].info->core_id].total_comp_cost) {
                                clients[nf_id].info->cpu_share = (uint32_t) ((DEFAULT_NF_CPU_SHARE*nf_pool_per_core[clients[nf_id].info->core_id].total_nf_count)*(clients[nf_id].info->comp_cost))
                                                /((nf_pool_per_core[clients[nf_id].info->core_id].total_comp_cost));

                                clients[nf_id].info->exec_period = ((clients[nf_id].info->comp_cost)*total_cycles_in_epoch)/nf_pool_per_core[clients[nf_id].info->core_id].total_comp_cost; //(total_cycles_in_epoch)*(total_load_on_core)/(load_of_nf)
                        }
                        else {
                                clients[nf_id].info->cpu_share = (uint32_t)DEFAULT_NF_CPU_SHARE;
                                clients[nf_id].info->exec_period = 0;

                        }
                        #ifdef __DEBUG_LOGS__
                        printf("\n ***** Client [%d] with cost [%d] on core [%d] with total_demand [%d] shared by [%d] NFs, got cpu share [%d]***** \n ", clients[nf_id].info->instance_id, clients[nf_id].info->comp_cost, clients[nf_id].info->core_id,
                                                                                                                                                   nf_pool_per_core[clients[nf_id].info->core_id].total_comp_cost,
                                                                                                                                                   nf_pool_per_core[clients[nf_id].info->core_id].total_nf_count,
                                                                                                                                                   clients[nf_id].info->cpu_share);
                        #endif //__DEBUG_LOGS__

#else
                        uint64_t num = 0;
                        //Dynamic: Based on accounting the product of Load*comp_cost factors. We can define the weights Alpha(α) and Beta(β) for apportioning Load and Comp_Costs: (α*clients[nf_id].info->load)*(β*clients[nf_id].info->comp_cost) | β*α = 1.
                        if (nf_pool_per_core[clients[nf_id].info->core_id].total_load_cost_fct) {

                                num = (uint64_t)(nf_pool_per_core[clients[nf_id].info->core_id].total_nf_count)*(DEFAULT_NF_CPU_SHARE)*(clients[nf_id].info->comp_cost)*(clients[nf_id].info->load);
                                clients[nf_id].info->cpu_share = (uint32_t) (num/nf_pool_per_core[clients[nf_id].info->core_id].total_load_cost_fct);
                                //clients[nf_id].info->cpu_share = ((uint64_t)(((DEFAULT_NF_CPU_SHARE*nf_pool_per_core[clients[nf_id].info->core_id].total_nf_count)*(clients[nf_id].info->comp_cost*clients[nf_id].info->load)))
                                //                /((nf_pool_per_core[clients[nf_id].info->core_id].total_load_cost_fct)));
                                clients[nf_id].info->exec_period = ((clients[nf_id].info->comp_cost)*(clients[nf_id].info->load)*total_cycles_in_epoch)/nf_pool_per_core[clients[nf_id].info->core_id].total_load_cost_fct; //(total_cycles_in_epoch)*(total_load_on_core)/(load_of_nf)
                        }
                        else {
                                clients[nf_id].info->cpu_share = (uint32_t)DEFAULT_NF_CPU_SHARE;
                                clients[nf_id].info->exec_period = 0;
                        }
                        #ifdef __DEBUG_LOGS__
                        printf("\n ***** Client [%d] with cost [%d] and load [%d] on core [%d] with total_demand_comp_cost=%"PRIu64", shared by [%d] NFs, got num=%"PRIu64", cpu share [%d]***** \n ", clients[nf_id].info->instance_id, clients[nf_id].info->comp_cost, clients[nf_id].info->load, clients[nf_id].info->core_id,
                                                                                                                                                   nf_pool_per_core[clients[nf_id].info->core_id].total_load_cost_fct,
                                                                                                                                                   nf_pool_per_core[clients[nf_id].info->core_id].total_nf_count,
                                                                                                                                                   num, clients[nf_id].info->cpu_share);
                        #endif //__DEBUG_LOGS__
#endif //USE_DYNAMIC_LOAD_FACTOR_FOR_CPU_SHARE

                        //set_cgroup_nf_cpu_share(clients[nf_id].info->instance_id, clients[nf_id].info->cpu_share);
                        set_cgroup_nf_cpu_share_from_onvm_mgr(clients[nf_id].info->instance_id, clients[nf_id].info->cpu_share);
                }
        }
#endif // #if defined (USE_CGROUPS_PER_NF_INSTANCE)
}


void extract_nf_load_and_svc_rate_info(__attribute__((unused)) unsigned long interval) {
#if defined (USE_CGROUPS_PER_NF_INSTANCE) && defined(INTERRUPT_SEM)
        uint16_t nf_id = 0;
        for (; nf_id < MAX_CLIENTS; nf_id++) {
                struct client *cl = &clients[nf_id];
                if (onvm_nf_is_valid(cl)){
                        static onvm_stats_snapshot_t st;
                        get_onvm_nf_stats_snapshot_v2(nf_id,&st,0);
                        cl->info->load      =  (st.rx_delta + st.rx_drop_delta);//(cl->stats.rx - cl->stats.prev_rx + cl->stats.rx_drop - cl->stats.prev_rx_drop); //rte_ring_count(cl->rx_q);
                        cl->info->avg_load  =  ((cl->info->avg_load == 0) ? (cl->info->load):((cl->info->avg_load + cl->info->load) /2));
                        cl->info->svc_rate  =  (st.tx_delta); //(clients_stats->tx[nf_id] -  clients_stats->prev_tx[nf_id]);
                        cl->info->avg_svc   =  ((cl->info->avg_svc == 0) ? (cl->info->svc_rate):((cl->info->avg_svc + cl->info->svc_rate) /2));
                        cl->info->drop_rate =  (st.rx_drop_rate);

                        #ifdef STORE_HISTOGRAM_OF_NF_COMPUTATION_COST
                        //Get the Median Computation cost, instead of running average; else running average is expected to be set already.
                        cl->info->comp_cost = hist_extract_v2(&cl->info->ht2, VAL_TYPE_MEDIAN);
                        #endif //STORE_HISTOGRAM_OF_NF_COMPUTATION_COST
                }
                else if (cl && cl->info) {
                        cl->info->load      = 0;
                        cl->info->avg_load  = 0;
                        cl->info->svc_rate  = 0;
                        cl->info->avg_svc   = 0;
                }
        }
        //sort and prepare the list of nfs_per_core_per_pool in the decreasing order of priority; use this list to wake up the NFs
        setup_nfs_priority_per_core_list(interval);
#endif
}

int nf_sort_func(const void * a, const void *b) {
        uint32_t nfid1 = *(const uint32_t*)a;
        uint32_t nfid2 = *(const uint32_t*)b;
        struct client *cl1 = &clients[nfid1];
        struct client *cl2 = &clients[nfid2];

        if(!cl1 || !cl2) return 0;
        if(cl1->info->load < cl2->info->load) return 1;
        else if (cl1->info->load > cl2->info->load) return (-1);
        return 0;

}


void setup_nfs_priority_per_core_list(__attribute__((unused)) unsigned long interval) {

        memset(&nf_sched_param, 0, sizeof(nf_sched_param));
        uint16_t nf_id = 0;
        for (nf_id=0; nf_id < MAX_CLIENTS; nf_id++) {
                if ((onvm_nf_is_valid(&clients[nf_id])) /* && (clients[nf_id].info->comp_cost)*/) {
                        nf_sched_param.nf_list_per_core[clients[nf_id].info->core_id].nf_ids[nf_sched_param.nf_list_per_core[clients[nf_id].info->core_id].count++] = nf_id;
                        nf_sched_param.nf_list_per_core[clients[nf_id].info->core_id].run_time[nf_id] = clients[nf_id].info->exec_period;
                }
        }
        uint16_t core_id = 0;
        for(core_id=0; core_id < MAX_CORES_ON_NODE; core_id++) {
                if(!nf_sched_param.nf_list_per_core[core_id].count) continue;
                onvm_sort_generic(nf_sched_param.nf_list_per_core[core_id].nf_ids, ONVM_SORT_TYPE_CUSTOM, SORT_DESCENDING, nf_sched_param.nf_list_per_core[core_id].count, sizeof(nf_sched_param.nf_list_per_core[core_id].nf_ids[0]), nf_sort_func);
                nf_sched_param.nf_list_per_core[core_id].sorted=1;
#if 0
                {
                        unsigned x = 0;
                        printf("\n********** Sorted NFs on Core [%d]: ", core_id);
                        for (x=0; x< nf_sched_param.nf_list_per_core[core_id].count; x++) {
                                printf("[%d],", nf_sched_param.nf_list_per_core[core_id].nf_ids[x]);
                        }
                }
#endif
        }
        nf_sched_param.sorted=1;
}


//#include <sys/types.h>
//#include <signal.h>
void monitor_nf_node_liveliness_via_pid_monitoring(void) {
        uint16_t nf_id = 0;

        for (; nf_id < MAX_CLIENTS; nf_id++) {
                if (onvm_nf_is_valid(&clients[nf_id])){
                        if (kill(clients[nf_id].info->pid, 0)) {
                                //clients[nf_id].info->status = NF_STOPPED;
                                printf("\n\n******* Moving NF with InstanceID:%d state %d to STOPPED\n\n",clients[nf_id].info->instance_id, clients[nf_id].info->status);
                                clients[nf_id].info->status = NF_STOPPED;
                                //**** TO DO: Take necessary actions here: It still doesn't clean-up until the new_nf_pool is populated by adding/killing another NF instance.
                                rte_ring_enqueue(nf_info_queue, clients[nf_id].info);
                                rte_mempool_put(nf_info_pool, clients[nf_id].info);
                                // Still the IDs are not recycled.. missing some additional changes:: found bug in the way the IDs are recycled-- fixed change in onvm_nf_next_instance_id()
                        }
                }
        }
}

inline int
onvm_nf_is_valid(struct client *cl) {
        return cl && cl->info && cl->info->status == NF_RUNNING;
}


uint16_t
onvm_nf_next_instance_id(void) {
        struct client *cl;
        /*
        while (next_instance_id < MAX_CLIENTS) {
                cl = &clients[next_instance_id];
                if (!onvm_nf_is_valid(cl))
                        break;
                next_instance_id++;
        }
        return next_instance_id;
        */

        if(next_instance_id >= MAX_CLIENTS) {
                next_instance_id = 1;   // don't know why the id=0 is reserved?
        }
        while (next_instance_id < MAX_CLIENTS) {
                cl = &clients[next_instance_id];
                if (!onvm_nf_is_valid(cl))
                        break;
                next_instance_id++;
        }
        return next_instance_id++;

}


void
onvm_nf_check_status(void) {
        int i;
        void *new_nfs[MAX_CLIENTS];
        struct onvm_nf_info *nf;
        int num_new_nfs = rte_ring_count(nf_info_queue);

        if (rte_ring_dequeue_bulk(nf_info_queue, new_nfs, num_new_nfs) != 0)
        #if !defined(USE_CGROUPS_PER_NF_INSTANCE) || !defined (ENABLE_DYNAMIC_CGROUP_WEIGHT_ADJUSTMENT)
                return;
        #else
        {}   // do nothing
        #endif //USE_CGROUPS_PER_NF_INSTANCE

        for (i = 0; i < num_new_nfs; i++) {
                nf = (struct onvm_nf_info *) new_nfs[i];

                if (nf->status == NF_WAITING_FOR_ID) {
                        if (!onvm_nf_start(nf))
                                num_clients++;
                } else if (nf->status == NF_STOPPED) {
                        if (!onvm_nf_stop(nf))
                                num_clients--;
                }
        }

        /* Add PID monitoring to assert active NFs (non crashed) */
        monitor_nf_node_liveliness_via_pid_monitoring();

        /* Ideal location to re-compute the NF weight
        #if defined (USE_CGROUPS_PER_NF_INSTANCE) && defined(ENABLE_DYNAMIC_CGROUP_WEIGHT_ADJUSTMENT)
        compute_and_assign_nf_cgroup_weight();
        #endif //USE_CGROUPS_PER_NF_INSTANCE
        */
}

// This function must
void
onvm_nf_stats_update(__attribute__((unused)) unsigned long interval) {


        //move this functionality to arbiter instead;
        #if defined (USE_CGROUPS_PER_NF_INSTANCE)
        //extract_nf_load_and_svc_rate_info(interval);
        #endif

        /* Ideal location to re-compute the NF weight */
        #if defined (USE_CGROUPS_PER_NF_INSTANCE) && defined(ENABLE_DYNAMIC_CGROUP_WEIGHT_ADJUSTMENT)
        compute_and_assign_nf_cgroup_weight();
        #endif //USE_CGROUPS_PER_NF_INSTANCE
}

inline uint16_t
onvm_nf_service_to_nf_map(uint16_t service_id, struct rte_mbuf *pkt) {
        uint16_t num_nfs_available = nf_per_service_count[service_id];

        if (num_nfs_available == 0)
                return 0;

        if (pkt == NULL)
                return 0;

        uint16_t instance_index = pkt->hash.rss % num_nfs_available;
        uint16_t instance_id = services[service_id][instance_index];
        return instance_id;
}


/******************************Internal functions*****************************/


inline int
onvm_nf_start(struct onvm_nf_info *nf_info) {
        // TODO dynamically allocate memory here - make rx/tx ring
        // take code from init_shm_rings in init.c
        // flush rx/tx queue at the this index to start clean?

        if(nf_info == NULL)
                return 1;

        // if NF passed its own id on the command line, don't assign here
        // assume user is smart enough to avoid duplicates
        uint16_t nf_id = nf_info->instance_id == (uint16_t)NF_NO_ID
                ? onvm_nf_next_instance_id()
                : nf_info->instance_id;

        if (nf_id >= MAX_CLIENTS) {
                // There are no more available IDs for this NF
                printf("\n Invalid NF_ID! Rejecting the NF Start!\n ");
                nf_info->status = NF_NO_IDS;
                return 1;
        }

        if (onvm_nf_is_valid(&clients[nf_id])) {
                // This NF is trying to declare an ID already in use
                printf("\n Invalid NF (conflicting ID!!) Rejecting the NF Start!\n ");
                nf_info->status = NF_ID_CONFLICT;
                return 1;
        }

        // Keep reference to this NF in the manager
        nf_info->instance_id = nf_id;
        clients[nf_id].info = nf_info;
        clients[nf_id].instance_id = nf_id;

        // Register this NF running within its service
        uint16_t service_count = nf_per_service_count[nf_info->service_id]++;
        services[nf_info->service_id][service_count] = nf_id;

        // Let the NF continue its init process
        nf_info->status = NF_STARTING;
        return 0;
}


inline int
onvm_nf_stop(struct onvm_nf_info *nf_info) {
        uint16_t nf_id;
        uint16_t service_id;
        int mapIndex;
        struct rte_mempool *nf_info_mp;

        if(nf_info == NULL){
                printf(" Null Entry for NF! Bad request for Stop!!\n ");
                return 1;
        }


        nf_id = nf_info->instance_id;
        service_id = nf_info->service_id;

        /* Clean up dangling pointers to info struct */
        clients[nf_id].info = NULL;

        /* Reset stats */
        onvm_stats_clear_client(nf_id);

        /* Remove this NF from the service map.
         * Need to shift all elements past it in the array left to avoid gaps */
        nf_per_service_count[service_id]--;
        for (mapIndex = 0; mapIndex < MAX_CLIENTS_PER_SERVICE; mapIndex++) {
                if (services[service_id][mapIndex] == nf_id) {
                        break;
                }
        }

        if (mapIndex < MAX_CLIENTS_PER_SERVICE) {  // sanity error check
                services[service_id][mapIndex] = 0;
                for (; mapIndex < MAX_CLIENTS_PER_SERVICE - 1; mapIndex++) {
                        // Shift the NULL to the end of the array
                        if (services[service_id][mapIndex + 1] == 0) {
                                // Short circuit when we reach the end of this service's list
                                break;
                        }
                        services[service_id][mapIndex] = services[service_id][mapIndex + 1];
                        services[service_id][mapIndex + 1] = 0;
                }
        }

        /* Free info struct */
        /* Lookup mempool for nf_info struct */
        nf_info_mp = rte_mempool_lookup(_NF_MEMPOOL_NAME);
        if (nf_info_mp == NULL) {
                printf(" Null Entry for NF_INFO_MAP in MEMPOOL! No pool available!!\n ");
                return 1;
        }
        rte_mempool_put(nf_info_mp, (void*)nf_info);
        printf(" NF reclaimed to NF pool!!! \n ");
        return 0;
}
