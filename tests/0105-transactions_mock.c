/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2019, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "test.h"

#include "rdkafka.h"

#include "../src/rdkafka_proto.h"
#include "../src/rdunittest.h"

/**
 * @name Producer transaction tests using the mock cluster
 *
 */



/**
 * @brief Create a transactional producer and a mock cluster.
 */
static rd_kafka_t *create_txn_producer (rd_kafka_mock_cluster_t **mclusterp) {
        rd_kafka_conf_t *conf;
        rd_kafka_t *rk;

        test_conf_init(&conf, NULL, 0);

        rd_kafka_conf_set(conf, "transactional.id", "txnid", NULL, 0);
        rd_kafka_conf_set(conf, "test.mock.num.brokers", "3", NULL, 0);

        rk = test_create_handle(RD_KAFKA_PRODUCER, conf);

        if (mclusterp) {
                *mclusterp = rd_kafka_handle_mock_cluster(rk);
                TEST_ASSERT(*mclusterp,  "failed to create mock cluster");
        }

        return rk;
}


/**
 * @brief Test recoverable errors using mock broker error injections
 *        and code coverage checks.
 */
static void do_test_txn_recoverable_errors (void) {
        rd_kafka_t *rk;
        rd_kafka_mock_cluster_t *mcluster;
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_topic_partition_list_t *offsets;

        rk = create_txn_producer(&mcluster);

        /*
         * Inject som InitProducerId errors that causes retries
         */
        rd_kafka_mock_push_request_errors(
                mcluster,
                RD_KAFKAP_InitProducerId,
                3,
                RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE,
                RD_KAFKA_RESP_ERR_NOT_COORDINATOR,
                RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS);

        TEST_CALL__(rd_kafka_init_transactions(rk, 5000,
                                               errstr, sizeof(errstr)));

        RD_UT_COVERAGE_CHECK(0); /* idemp_request_pid_failed(retry) */
        RD_UT_COVERAGE_CHECK(1); /* txn_idemp_state_change(READY) */

        /*
         * Start a transaction
         */
        TEST_CALL__(rd_kafka_begin_transaction(rk, errstr, sizeof(errstr)));

        /*
         * Produce a message, let it fail on the first attempt, then succeed.
         */
        rd_kafka_mock_push_request_errors(
                mcluster,
                RD_KAFKAP_Produce,
                1,
                RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS);

        err = rd_kafka_producev(rk,
                                RD_KAFKA_V_TOPIC("mytopic"),
                                RD_KAFKA_V_VALUE("hi", 2),
                                RD_KAFKA_V_END);
        TEST_ASSERT(!err, "produce failed: %s", rd_kafka_err2str(err));

        /*
         * Send some arbitrary offsets, first with some failures, then
         * succeed.
         */
        offsets = rd_kafka_topic_partition_list_new(4);
        rd_kafka_topic_partition_list_add(offsets, "srctopic", 3)->offset = 12;
        rd_kafka_topic_partition_list_add(offsets, "srctop2", 99)->offset =
                999999111;
        rd_kafka_topic_partition_list_add(offsets, "srctopic", 0)->offset = 999;
        rd_kafka_topic_partition_list_add(offsets, "srctop2", 3499)->offset =
                123456789;

        rd_kafka_mock_push_request_errors(
                mcluster,
                RD_KAFKAP_AddPartitionsToTxn,
                1,
                RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS);

        TEST_CALL__(rd_kafka_send_offsets_to_transaction(
                            rk, offsets,
                            "myGroupId",
                            errstr, sizeof(errstr)));
        rd_kafka_topic_partition_list_destroy(offsets);

        /*
         * Commit transaction, first with som failures, then succeed.
         */
        rd_kafka_mock_push_request_errors(
                mcluster,
                RD_KAFKAP_EndTxn,
                3,
                RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE,
                RD_KAFKA_RESP_ERR_NOT_COORDINATOR,
                RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS);

        TEST_CALL__(rd_kafka_commit_transaction(rk, 5000,
                                                errstr, sizeof(errstr)));

        /* All done */

        rd_kafka_destroy(rk);
}


int main_0105_transactions_mock (int argc, char **argv) {

        do_test_txn_recoverable_errors();

        return 0;
}
