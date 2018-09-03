/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015, Magnus Edenhill
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

/**
 * @name Test rd_kafka_purge()
 *
 * Local test:
 *  - produce 20 messages (that will be held up in queues),
 *    for specific partitions and UA.
 *  - purge(INFLIGHT) => no change in len()
 *  - purge(QUEUE) => len() should drop to 0, dr errs should be ERR__PURGE_QUEUE
 *
 * Remote test (WITH_SOCKEM):
 *  - Limit in-flight messages to 10
 *  - Produce 20 messages to the same partition, in batches of 10.
 *  - Make sure only first batch is sent.
 *  - purge(QUEUE) => len should drop to 10, dr err ERR__PURGE_QUEUE
 *  - purge(INFLIGHT|QUEUE) => len should drop to 0, ERR__PURGE_INFLIGHT
 */



/**
 * @brief Sockem connect, called from **internal librdkafka thread** through
 *        librdkafka's connect_cb
 */
static int connect_cb (struct test *test, sockem_t *skm, const char *id) {
        sockem_set(skm, "delay", 500, NULL);
        return 0;
}

static mtx_t produce_req_lock;
static cnd_t produce_req_cnd;
static int produce_req_cnt = 0;

static const int msgcnt = 20;
struct waitmsgs {
        rd_kafka_resp_err_t exp_err[20];
        int cnt;
};


static rd_kafka_resp_err_t on_request_sent (rd_kafka_t *rk,
                                            int sockfd,
                                            const char *brokername,
                                            int32_t brokerid,
                                            int16_t ApiKey,
                                            int16_t ApiVersion,
                                            int32_t CorrId,
                                            size_t  size,
                                            void *ic_opaque) {

        /* Ignore if not a ProduceRequest */
        if (ApiKey != 0)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        TEST_SAY("ProduceRequest sent to %s (%"PRId32")\n",
                 brokername, brokerid);

        mtx_lock(&produce_req_lock);
        produce_req_cnt++;
        cnd_broadcast(&produce_req_cnd);
        mtx_unlock(&produce_req_lock);

        /* Stall the connection */
        test_socket_sockem_set(sockfd, "delay", 5000);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

static rd_kafka_resp_err_t on_new_producer (rd_kafka_t *rk,
                                            const rd_kafka_conf_t *conf,
                                            void *ic_opaque,
                                            char *errstr, size_t errstr_size) {
        return rd_kafka_interceptor_add_on_request_sent(
                rk, "catch_producer_req",
                on_request_sent, NULL);
}



static void dr_msg_cb (rd_kafka_t *rk, const rd_kafka_message_t *rkmessage,
                       void *opaque) {
        int msgid;
        struct waitmsgs *waitmsgs = rkmessage->_private;

        TEST_ASSERT(waitmsgs->cnt > 0, "wait_msg_cnt is zero on DR");

        waitmsgs->cnt--;

        TEST_ASSERT(rkmessage->len == sizeof(msgid),
                    "invalid message size %"PRIusz", expected sizeof(int)",
                    rkmessage->len);

        memcpy(&msgid, rkmessage->payload, rkmessage->len);

        TEST_ASSERT(msgid >= 0 && msgid < msgcnt,
                    "msgid %d out of range 0..%d", msgid, msgcnt - 1);

        TEST_ASSERT(waitmsgs->exp_err[msgid] != 12345,
                    "msgid %d delivered twice", msgid);

        TEST_SAY("DeliveryReport for msg #%d: %s\n",
                 msgid, rd_kafka_err2name(rkmessage->err));

        if (rkmessage->err != waitmsgs->exp_err[msgid]) {
                TEST_FAIL_LATER("Expected message #%d to fail with %s, not %s",
                                msgid,
                                rd_kafka_err2str(waitmsgs->exp_err[msgid]),
                                rd_kafka_err2str(rkmessage->err));
        }
}







static void purge_and_expect (const char *what, int line,
                              rd_kafka_t *rk, int purge_flags,
                              struct waitmsgs *waitmsgs,
                              int exp_remain, const char *reason) {
        test_timing_t t_purge;
        rd_kafka_resp_err_t err;

        TIMING_START(&t_purge, "%s:%d: purge(0x%x)", what, line, purge_flags);
        err = rd_kafka_purge(rk, purge_flags);
        TIMING_STOP(&t_purge);

        TEST_ASSERT(!err, "purge(0x%x) at %d failed: %s",
                    purge_flags, line, rd_kafka_err2str(err));

        rd_kafka_poll(rk, 0);
        TEST_ASSERT(waitmsgs->cnt == exp_remain,
                    "%s:%d: expected %d messages remaining, not %d",
                    what, line, exp_remain, waitmsgs->cnt);
}


static void do_test_purge (const char *what, int remote) {
        const char *topic = test_mk_topic_name("0086_purge", 0);
        rd_kafka_conf_t *conf;
        rd_kafka_t *rk;
        int i;
        rd_kafka_resp_err_t err;
        struct waitmsgs waitmsgs = RD_ZERO_INIT;

        TEST_SAY(_C_MAG "Test rd_kafka_purge(): %s\n" _C_CLR, what);

        test_conf_init(&conf, NULL, 20);

        test_conf_set(conf, "batch.num.messages", "10");
        test_conf_set(conf, "max.in.flight", "1");
        test_conf_set(conf, "linger.ms", "500");
        rd_kafka_conf_set_dr_msg_cb(conf, dr_msg_cb);

        if (remote) {
#if !WITH_SOCKEM
                TEST_SKIP("%s: no sockem support\n", what);
                return;
#else
                test_socket_enable(conf);
                test_curr->connect_cb = connect_cb;
#endif
                rd_kafka_conf_interceptor_add_on_new(conf, "on_new_producer",
                                                     on_new_producer, NULL);

                mtx_init(&produce_req_lock, mtx_plain);
                cnd_init(&produce_req_cnd);
        } else {
                test_conf_set(conf, "bootstrap.servers", NULL);
        }

        rk = test_create_handle(RD_KAFKA_PRODUCER, conf);

        TEST_SAY("Producing %d messages to topic %s\n", msgcnt, topic);

        for (i = 0 ; i < msgcnt ; i++) {
                int32_t partition;

                if (remote) {
                        /* We need all messages in the same partition
                         * so that remaining messages are queued
                         * up behind the first messageset */
                        partition = 0;
                } else {
                        partition = (i < 10 ? i % 3 : RD_KAFKA_PARTITION_UA);
                }

                err = rd_kafka_producev(rk,
                                        RD_KAFKA_V_TOPIC(topic),
                                        RD_KAFKA_V_PARTITION(partition),
                                        RD_KAFKA_V_VALUE((void *)&i, sizeof(i)),
                                        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                                        RD_KAFKA_V_OPAQUE(&waitmsgs),
                                        RD_KAFKA_V_END);
                TEST_ASSERT(!err, "producev(#%d) failed: %s",
                            i, rd_kafka_err2str(err));

                waitmsgs.exp_err[i] = (remote && i < 10 ?
                                       RD_KAFKA_RESP_ERR__PURGE_INFLIGHT :
                                       RD_KAFKA_RESP_ERR__PURGE_QUEUE);

                waitmsgs.cnt++;
        }


        if (remote) {
                /* Wait for ProduceRequest to be sent */
                mtx_lock(&produce_req_lock);
                cnd_timedwait_ms(&produce_req_cnd, &produce_req_lock, 15*1000);
                TEST_ASSERT(produce_req_cnt > 0,
                            "First Produce request should've been sent by now");
                mtx_unlock(&produce_req_lock);

                purge_and_expect(what, __LINE__, rk, RD_KAFKA_PURGE_F_QUEUE,
                                 &waitmsgs, 10,
                                 "in-flight messages should not be purged");

                purge_and_expect(what, __LINE__, rk,
                                 RD_KAFKA_PURGE_F_INFLIGHT|
                                 RD_KAFKA_PURGE_F_QUEUE,
                                 &waitmsgs, 0,
                                 "all messages should have been purged");
        } else {
                purge_and_expect(what, __LINE__, rk, RD_KAFKA_PURGE_F_INFLIGHT,
                                 &waitmsgs, msgcnt,
                                 "no messagess should have been purged");

                purge_and_expect(what, __LINE__, rk, RD_KAFKA_PURGE_F_QUEUE,
                                 &waitmsgs, 0,
                                 "no messagess should have been purged");
        }


        rd_kafka_destroy(rk);

        TEST_LATER_CHECK();
}


int main_0086_purge_remote (int argc, char **argv) {
        do_test_purge("remote", 1/*remote*/);
        return 0;
}


int main_0086_purge_local (int argc, char **argv) {
        do_test_purge("local", 0/*local*/);
        return 0;
}