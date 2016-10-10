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

                                 onvm_nf.h

     This file contains the prototypes for all functions related to packet
     processing.

******************************************************************************/


#ifndef _ONVM_NF_H_
#define _ONVM_NF_H_

extern uint16_t next_instance_id;
extern struct wakeup_info *wakeup_infos;

#ifdef ENABLE_NF_BACKPRESSURE
extern uint16_t downstream_nf_overflow;
extern uint16_t highest_downstream_nf_service_id;
extern uint16_t lowest_upstream_to_throttle;
extern uint64_t throttle_count;
#endif

/********************************Interfaces***********************************/


/*
 * Interface checking if a given nf is "valid", meaning if it's running.
 *
 * Input  : a pointer to the nf
 * Output : a boolean answer 
 *
 */
inline int
onvm_nf_is_valid(struct client *cl);


/*
 * Interface giving the smallest unsigned integer unused for a NF instance.
 *
 * Output : the unsigned integer 
 *
 */
uint16_t
onvm_nf_next_instance_id(void);


/*
 * Interface looking through all registered NFs if one needs to start or stop.
 *
 */
void
onvm_nf_check_status(void);


/*
 * Interface giving a NF for a specific server id, depending on the flow.
 *
 * Inputs  : the service id
             a pointer to the packet whose flow help steer it. 
 * Output  : a NF instance id
 *
 */
inline uint16_t
onvm_nf_service_to_nf_map(uint16_t service_id, struct rte_mbuf *pkt);


/****************************Internal functions*******************************/


/*
 * Function starting a NF.
 *
 * Input  : a pointer to the NF's informations
 * Output : an error code
 *
 */
inline int
onvm_nf_start(struct onvm_nf_info *nf_info);


/*
 * Function stopping a NF.
 *
 * Input  : a pointer to the NF's informations
 * Output : an error code
 *
 */
inline int
onvm_nf_stop(struct onvm_nf_info *nf_info);


#endif  // _ONVM_NF_H_
