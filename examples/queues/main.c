/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
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
 */

#include <unistd.h>
#include <stdint.h>

#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ethdev.h>

#include "main.h"

static struct queues_conf req_conf;
static struct queues_conf pri_conf;
static struct app_conf app_conf;

static int
main_loop(__attribute__((unused)) void *arg)
{
	struct queues_conf *conf;
	uint32_t lcore_id = rte_lcore_id();

	if (lcore_id == req_conf.rx_core ||
	    lcore_id == req_conf.worker_req_core ||
	    lcore_id == req_conf.worker_pri_core ||
	    lcore_id == req_conf.tx_req_core ||
	    lcore_id == req_conf.tx_pri_core)
		conf = &req_conf;
	else
		conf = &pri_conf;

	if (lcore_id == conf->rx_core) {
		RTE_LOG(INFO, APP, "lcoreid %u reading port %"PRIu8"\n",
			lcore_id, conf->rx_port);
		rx_thread(conf);
	}
	if (lcore_id == conf->worker_req_core) {
		RTE_LOG(INFO, APP, "lcoreid %u req scheduling\n", lcore_id);
		req_thread(conf);
	}
	if (lcore_id == conf->worker_pri_core) {
		RTE_LOG(INFO, APP, "lcoreid %u pri scheduling\n", lcore_id);
		pri_thread(conf);
	}
	if (lcore_id == conf->tx_req_core) {
		conf->m_table = rte_malloc("req_table",
			sizeof(struct rte_mbuf *) * conf->tx_burst_size,
			RTE_CACHE_LINE_SIZE);
		if (conf->m_table == NULL)
			rte_panic("unable to allocate req memory buffer\n");
		RTE_LOG(INFO, APP, "lcoreid %u req writing port %"PRIu8"\n",
			lcore_id, conf->tx_port);
		req_tx_thread(conf);
	}
	if (lcore_id == conf->tx_pri_core) {
		conf->m_table = rte_malloc("pri_table",
			sizeof(struct rte_mbuf *) * conf->tx_burst_size,
			RTE_CACHE_LINE_SIZE);
		if (conf->m_table == NULL)
			rte_panic("unable to allocate pri memory buffer\n");
		RTE_LOG(INFO, APP, "lcoreid %u pri writing port %"PRIu8"\n",
			lcore_id, conf->tx_port);
		pri_tx_thread(conf);
	}

	RTE_LOG(INFO, APP, "lcore %u has nothing to do\n", lcore_id);
	return 0;
}

int
main(int argc, char **argv)
{
	int ret;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		return -1;

	argc -= ret;
	argv += ret;

	ret = queues_init(&app_conf, &req_conf, &pri_conf);
	if (ret < 0)
		return -1;

	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
	return 0;
}
