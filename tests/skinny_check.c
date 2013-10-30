// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>
#include <time.h>
#include <junkie/cpp.h>
#include <junkie/tools/ext.h>
#include <junkie/tools/mutex.h>
#include <junkie/proto/pkt_wait_list.h>
#include <junkie/proto/cap.h>
#include <junkie/proto/eth.h>
#include <junkie/proto/ip.h>
#include <junkie/proto/udp.h>
#include <junkie/proto/tcp.h>
#include <junkie/proto/cnxtrack.h>
#include "junkie/tools/hash.h"
#include "lib.h"
#include "proto/skinny.c"

/*
 * Parse check
 */

static struct parse_test {
    uint8_t const *packet;
    int size;
    int ret;    // expected return code
    struct skinny_proto_info expected;
} parse_tests[] = {
    {
        .packet = (uint8_t const []) {
            0x0c,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,
            0x06,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,
        },
        .size = 4 * 5,
        .expected = {
            .info = { .head_len = 8, .payload = 12 },
            .msgid = SKINNY_STATION_OFF_HOOK,
        },
        .ret = PROTO_OK,
    },
    {
        // Basic header
        .packet = (uint8_t const []) {
            0x78, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x8a, 0x00, 0x00, 0x00,
            0xea, 0xed, 0x48, 0x02,
            0xf8, 0x35, 0x05, 0x02,

            0x0a, 0xa1, 0xca, 0x40,
            0xaa, 0x74, 0x00, 0x00,
            0x14, 0x00, 0x00, 0x00,

            0x04, 0x00, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x29, 0xf0, 0x48, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        },
        .size = 128,
        .expected = {
            .info = { .head_len = 8, .payload = 120 },
            .msgid = SKINNY_MGR_START_MEDIA_TRANSMIT,
            .conf_id = 38333930,
            .media_ip = IP4(10, 161, 202, 64),
            .media_port = 29866,
        },
        .ret = PROTO_OK,
    },
    {
        // CM7 type a header
        .packet = (uint8_t const []) {
            0x88, 0x00, 0x00, 0x00,
            0x12, 0x00, 0x00, 0x00,
            0x8a, 0x00, 0x00, 0x00,
            0x09, 0x67, 0xa4, 0x01,
            0x5f, 0x01, 0x00, 0x01,

            0x00, 0x00, 0x00, 0x00,
            0xc0, 0xa8, 0x0b, 0x03,  // Ip
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x02, 0x43, 0x00, 0x00,  // Remote port

            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x43, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
            0x04, 0x00, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x09, 0x67, 0xa4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        },
        .size = 144,
        .expected = {
            .info = { .head_len = 8, .payload = 136 },
            .msgid = SKINNY_MGR_START_MEDIA_TRANSMIT,
            .conf_id = 27551497,
            .media_ip = IP4(192, 168, 11, 3),
            .media_port = 17154,
        },
        .ret = PROTO_OK,
    },

    {
        // CM7 header call info message
        .packet = (uint8_t const []) {
            0x68, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x4a, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x09, 0x67, 0xa4, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x34, 0x32, 0x35,
            0x39, 0x39, 0x32, 0x34, 0x36, 0x00, 0x00, 0x30, 0x36, 0x35, 0x37, 0x36, 0x38, 0x31, 0x31, 0x31,
            0x00, 0x30, 0x36, 0x35, 0x37, 0x36, 0x38, 0x31, 0x31, 0x31, 0x00, 0x30, 0x36, 0x35, 0x37, 0x36,
            0x38, 0x31, 0x31, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x65, 0x6e, 0x6f, 0x69, 0x74, 0x20,
            0x41, 0x7a, 0x61, 0x72, 0x64, 0x28, 0x32, 0x34, 0x36, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        },
        .size = 112,
        .expected = {
            .info = { .head_len = 8, .payload = 104 },
            .msgid = SKINNY_MGR_CALL_INFO,
            .call_id = 27551497,
            .called_party = "065768111",
            .calling_party = "042599246",
        },
        .ret = PROTO_OK,
    },

    {
        // Basic header call info message
        .packet = (uint8_t const []) {
            0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x54, 0xee, 0x48, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0x30, 0x34, 0x30,
            0x31, 0x00, 0x30, 0x30, 0x31, 0x35, 0x33, 0x34, 0x36, 0x31, 0x35, 0x33, 0x33, 0x00, 0x30, 0x30,
            0x31, 0x35, 0x33, 0x34, 0x36, 0x31, 0x35, 0x33, 0x33, 0x00, 0x30, 0x30, 0x31, 0x35, 0x33, 0x34,
            0x36, 0x31, 0x35, 0x33, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0x30, 0x34, 0x30, 0x31, 0x2d,
            0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x61, 0x6e, 0x74, 0x65, 0x20, 0x73, 0x6f, 0x63, 0x69, 0x61,
            0x6c, 0x65, 0x20, 0x32, 0x00, 0x00, 0x00, 0x00
        },
        .size = 120,
        .expected = {
            .info = { .head_len = 8, .payload = 112 },
            .msgid = SKINNY_MGR_CALL_INFO,
            .call_id = 38334036,
            .called_party = "00153461533",
            .calling_party = "10401",
        },
        .ret = PROTO_OK,
    },

};

static unsigned cur_test;

static void skinny_info_check(struct proto_subscriber unused_ *s, struct proto_info const *info_, size_t unused_ cap_len, uint8_t const unused_ *packet, struct timeval const unused_ *now)
{
    // Check info against parse_tests[cur_test].expected
    struct skinny_proto_info const *const info = DOWNCAST(info_, info, skinny_proto_info);
    struct skinny_proto_info const *const expected = &parse_tests[cur_test].expected;

    assert(info->info.head_len == expected->info.head_len);
    assert(info->info.payload == expected->info.payload);
    if (info->set_values & SKINNY_CALL_ID) assert(info->call_id == expected->call_id);
    if (info->set_values & SKINNY_CONFERENCE_ID) assert(info->conf_id == expected->conf_id);
    if (info->set_values & SKINNY_MEDIA_CNX) {
        assert(ip_addr_eq(&info->media_ip, &expected->media_ip));
        assert(info->media_port == expected->media_port);
    }
    if (info->set_values & SKINNY_CALLED_PARTY) assert(0 == strcmp(info->called_party, expected->called_party));
    if (info->set_values & SKINNY_CALLING_PARTY) assert(0 == strcmp(info->calling_party, expected->calling_party));

    assert(info->msgid == expected->msgid);
}

static void parse_check(void)
{
    struct timeval now;
    timeval_set_now(&now);
    struct parser *skinny_parser = proto_skinny->ops->parser_new(proto_skinny);
    assert(skinny_parser);
    struct proto_subscriber sub;
    hook_subscriber_ctor(&pkt_hook, &sub, skinny_info_check);

    for (cur_test = 0; cur_test < NB_ELEMS(parse_tests); cur_test++) {
        struct parse_test const *test = parse_tests + cur_test;
        int ret = skinny_parse(skinny_parser, NULL, 0, test->packet, test->size, test->size, &now, test->size, test->packet);
        assert(ret == test->ret);
    }

    hook_subscriber_dtor(&pkt_hook, &sub);
    parser_unref(&skinny_parser);
}

int main(void)
{
    ext_init();
    log_init();
    mutex_init();
    objalloc_init();
    streambuf_init();
    hash_init();
    cnxtrack_init();
    pkt_wait_list_init();
    ref_init();
    proto_init();
    cap_init();
    eth_init();
    ip_init();
    ip6_init();
    udp_init();
    tcp_init();
    skinny_init();
    log_set_level(LOG_DEBUG, NULL);
    log_set_file("skinny_check.log");

    parse_check();

    doomer_stop();
    skinny_fini();
    tcp_fini();
    udp_fini();
    ip6_fini();
    ip_fini();
    eth_fini();
    cap_fini();
    proto_fini();
    ref_fini();
    pkt_wait_list_fini();
    cnxtrack_fini();
    hash_fini();
    streambuf_fini();
    objalloc_fini();
    mutex_fini();
    log_fini();
    ext_fini();
    return EXIT_SUCCESS;
}
