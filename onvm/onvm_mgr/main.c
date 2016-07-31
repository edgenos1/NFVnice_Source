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
 * main.c - simple onvm host code
 ********************************************************************/

#include "onvm_includes.h"
#include "onvm_stats.h"
#include "onvm_rx_tx.h"
#include "onvm_mgr_nf.h"

/*
 * Stats thread periodically prints per-port and per-NF stats.
 */
static void
master_thread_main(void) {
        const unsigned sleeptime = 1;

        RTE_LOG(INFO, APP, "Core %d: Running master thread\n", rte_lcore_id());

        /* Longer initial pause so above printf is seen */
        sleep(sleeptime * 3);

        /* Loop forever: sleep always returns 0 or <= param */
        while (sleep(sleeptime) <= sleeptime) {
                onvm_mgr_nf_do_check_new_nf_status();
                onvm_stats_display_all(sleeptime);
        }
}

/*
 * Function to receive packets from the NIC
 * and distribute them to the default service
 */
static int
rx_thread_main(void *arg) {
        uint16_t i, rx_count;
        struct rte_mbuf *pkts[PACKET_READ_SIZE];
        struct thread_info *rx = (struct thread_info*)arg;

        RTE_LOG(INFO, APP, "Core %d: Running RX thread for RX queue %d\n", rte_lcore_id(), rx->queue_id);

        for (;;) {
                /* Read ports */
                for (i = 0; i < ports->num_ports; i++) {
                        rx_count = rte_eth_rx_burst(ports->id[i], rx->queue_id, \
                                        pkts, PACKET_READ_SIZE);
                        ports->rx_stats.rx[ports->id[i]] += rx_count;

                        /* Now process the NIC packets read */
                        if (likely(rx_count > 0)) {
                                // If there is no running NF, we drop all the packets of the batch.
                                if(!num_clients) {
                                        onvm_rx_tx_drop_batch(pkts, rx_count);
                                } else {
                                        onvm_rx_process_rx_packet_batch(rx, pkts, rx_count);
                                }
                        }
                }
        }

        return 0;
}

static int
tx_thread_main(void *arg) {
        struct client *cl;
        unsigned i, tx_count;
        struct rte_mbuf *pkts[PACKET_READ_SIZE];
        struct thread_info* tx = (struct thread_info*)arg;

        if (tx->first_cl == tx->last_cl - 1) {
                RTE_LOG(INFO, APP, "Core %d: Running TX thread for NF %d\n", rte_lcore_id(), tx->first_cl);
        } else if (tx->first_cl < tx->last_cl) {
                RTE_LOG(INFO, APP, "Core %d: Running TX thread for NFs %d to %d\n", rte_lcore_id(), tx->first_cl, tx->last_cl-1);
        }

        for (;;) {
                /* Read packets from the client's tx queue and process them as needed */
                for (i = tx->first_cl; i < tx->last_cl; i++) {
                        tx_count = PACKET_READ_SIZE;
                        cl = &clients[i];
                        if (!onvm_mgr_nf_is_valid_nf(cl))
                                continue;
                        /* try dequeuing max possible packets first, if that fails, get the
                         * most we can. Loop body should only execute once, maximum */
                        while (tx_count > 0 &&
                                unlikely(rte_ring_dequeue_bulk(cl->tx_q, (void **) pkts, tx_count) != 0)) {
                                tx_count = (uint16_t)RTE_MIN(rte_ring_count(cl->tx_q),
                                        PACKET_READ_SIZE);
                        }

                        /* Now process the Client packets read */
                        if (likely(tx_count > 0)) {
                                onvm_tx_process_tx_packet_batch(tx, pkts, tx_count, cl);
                        }
                }

                /* Send a burst to every port */
                onvm_rx_tx_flush_all_ports(tx);

                /* Send a burst to every client */
                onvm_rx_tx_flush_all_clients(tx);

        }

        return 0;
}

int
main(int argc, char *argv[]) {
        unsigned cur_lcore, rx_lcores, tx_lcores;
        unsigned clients_per_tx, temp_num_clients;
        unsigned i;

        /* initialise the system */

        /* Reserve ID 0 for internal manager things */
        next_instance_id = 1;
        if (init(argc, argv) < 0 )
                return -1;
        RTE_LOG(INFO, APP, "Finished Process Init.\n");

        /* clear statistics */
        onvm_stats_clear_all_clients();

        /* Reserve n cores for: 1 Stats, 1 final Tx out, and ONVM_NUM_RX_THREADS for Rx */
        cur_lcore = rte_lcore_id();
        rx_lcores = ONVM_NUM_RX_THREADS;
        tx_lcores = rte_lcore_count() - rx_lcores - 1;

        /* Offset cur_lcore to start assigning TX cores */
        cur_lcore += (rx_lcores-1);

        RTE_LOG(INFO, APP, "%d cores available in total\n", rte_lcore_count());
        RTE_LOG(INFO, APP, "%d cores available for handling manager RX queues\n", rx_lcores);
        RTE_LOG(INFO, APP, "%d cores available for handling TX queues\n", tx_lcores);
        RTE_LOG(INFO, APP, "%d cores available for handling stats\n", 1);

        /* Evenly assign NFs to TX threads */

        /*
         * If num clients is zero, then we are running in dynamic NF mode.
         * We do not have a way to tell the total number of NFs running so
         * we have to calculate clients_per_tx using MAX_CLIENTS then.
         * We want to distribute the number of running NFs across available
         * TX threads
         */
        if (num_clients == 0) clients_per_tx = ceil((float)MAX_CLIENTS/tx_lcores), temp_num_clients = (unsigned)MAX_CLIENTS;
        else clients_per_tx = ceil((float)num_clients/tx_lcores), temp_num_clients = (unsigned)num_clients;

        for (i = 0; i < tx_lcores; i++) {
                struct thread_info *tx = calloc(1,sizeof(struct thread_info));
                tx->queue_id = i;
                tx->port_tx_buf = calloc(RTE_MAX_ETHPORTS, sizeof(struct packet_buf));
                tx->nf_rx_buf = calloc(MAX_CLIENTS, sizeof(struct packet_buf));
                tx->first_cl = RTE_MIN(i * clients_per_tx + 1, temp_num_clients);
                tx->last_cl = RTE_MIN((i+1) * clients_per_tx + 1, temp_num_clients);
                cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
                if (rte_eal_remote_launch(tx_thread_main, (void*)tx,  cur_lcore) == -EBUSY) {
                        RTE_LOG(ERR, APP, "Core %d is already busy, can't use for client %d TX\n", cur_lcore, tx->first_cl);
                        return -1;
                }
        }

        /* Launch RX thread main function for each RX queue on cores */
        for (i = 0; i < rx_lcores; i++) {
                struct thread_info *rx = calloc(1, sizeof(struct thread_info));
                rx->queue_id = i;
		rx->port_tx_buf = NULL;
		rx->nf_rx_buf = calloc(MAX_CLIENTS, sizeof(struct packet_buf));
                cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
                if (rte_eal_remote_launch(rx_thread_main, (void *)rx, cur_lcore) == -EBUSY) {
                        RTE_LOG(ERR, APP, "Core %d is already busy, can't use for RX queue id %d\n", cur_lcore, rx->queue_id);
                        return -1;
                }
        }

        /* Master thread handles statistics and NF management */
        master_thread_main();
        return 0;
}
