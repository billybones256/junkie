// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
#include <stdlib.h>
#include <stdint.h>
#undef NDEBUG
#include <assert.h>
#include <time.h>
#include "lib_test_junkie.h"

struct dynports *dynports = NULL;

static uint8_t *random_buffer(size_t *size, size_t min_size, size_t max_size)
{
    *size = min_size + (rand() % (max_size - min_size));
    uint8_t *buf = malloc(*size);
    assert(buf);

    unsigned i;
    for (i = 0; i < *size; i++) {
        buf[i] = rand() & 0xff;
    }

    return buf;
}

void stress_check(struct proto *proto)
{
    struct timeval now;
    timeval_set_now(&now);
    log_set_level(LOG_ERR, NULL);
    srand(time(NULL));

    struct parser *parser = proto->ops->parser_new(proto);
    assert(parser);

    for (unsigned nb_tests = 0; nb_tests < 10000 ; nb_tests ++) {
        size_t size;
        uint8_t *buf = random_buffer(&size, 20, 60);
        parser->proto->ops->parse(parser, NULL, rand()%1, buf, size, size, &now, size, buf);
        free(buf);
    }

    parser_unref(&parser);
}

/*
 * Build fake IP traffic
 */

#include "proto/ip_hdr.h"

int iph_ctor(void *ip_, size_t len, uint32_t src, uint32_t dst)
{
    struct ip_hdr *ip = ip_;
    if (len < sizeof(*ip)) return -1;

    ip->version_hdrlen = 0x04 | (sizeof(*ip)/4);
    ip->tos = 0;
    ip->tot_len = htons(len);
    ip->id = 0;
    ip->flags = 0;
    ip->frag_offset_lo = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->checksum = 0x1234;
    ip->src = htonl(src);
    ip->dst = htonl(dst);

    return 0;
}

int udph_ctor(void *udp_, size_t len, uint16_t src, uint16_t dst)
{
    struct udp_hdr *udp = udp_;
    if (len < sizeof(*udp)) return -1;

    udp->src = htons(src);
    udp->dst = htons(dst);
    udp->len = htons(len);
    udp->checksum = 0x1234;

    return 0;
}

int udp_ctor_random(void *packet, size_t len)
{
    if (0 != iph_ctor(packet, len, rand(), rand())) return -1;
    return udph_ctor((void *)((struct ip_hdr *)packet+1), len - sizeof(struct ip_hdr), rand(), rand());
}

int tcph_ctor(void *tcp_, size_t len, uint16_t src, uint16_t dst, uint32_t seqnum, uint32_t acknum, bool syn, bool fin, bool rst, bool ack)
{
    struct tcp_hdr *tcp = tcp_;
    if (len < sizeof(*tcp)) return -1;

    tcp->src = htons(src);
    tcp->dst = htons(dst);
    tcp->seq_num = htonl(seqnum);
    tcp->ack_seq = htonl(acknum);
    tcp->hdr_len = tcp->flags = 0;
    tcp->hdr_len = (sizeof(*tcp)/4) << 4U;  // no options
    tcp->flags = (syn ? TCP_SYN_MASK:0) | (fin ? TCP_FIN_MASK:0) | (ack ? TCP_ACK_MASK:0) | (rst ? TCP_RST_MASK:0);
    tcp->window = 0x8000;
    tcp->checksum = 0x1234;
    tcp->urg_ptr = 0;

    return 0;
}

int tcp_ctor_random(void *packet, size_t len)
{
    if (0 != iph_ctor(packet, len, rand(), rand())) return -1;
    return tcph_ctor((void *)((struct ip_hdr *)packet+1), len - sizeof(struct ip_hdr), rand(), rand(), rand(), rand(), rand()&1, rand()&1, rand()&1, rand()&1);
}

int tcp_stream_ctor(struct tcp_stream *stream, size_t len, size_t mtu, uint16_t service_port)
{
    stream->packet = malloc(mtu);
    if (! stream->packet) return -1;

    stream->mtu = mtu;
    stream->len = len;
    stream->past_len[0] = stream->past_len[1] = 0;
    stream->isn[0] = rand();
    stream->isn[1] = rand();
    stream->ip[0] = rand();
    stream->ip[1] = rand();
    stream->port[0] = rand();
    stream->port[1] = service_port;
    stream->fin_acked[0] = false;
    stream->fin_acked[1] = false;

    return 0;
}

void tcp_stream_dtor(struct tcp_stream *stream)
{
    free(stream->packet);
}

static ssize_t stream_packet(struct tcp_stream *stream, int way)
{
    bool syn = stream->past_len[way] == 0;
    size_t payload = syn ? 0 : stream->mtu - 100 /* enought room for IP + TCP headers */;
    bool fin;

    if (stream->past_len[way] >= stream->len) { // we are already fined, just ack
        syn = false;
        fin = false;
        payload = 0;
    } else {
        fin = stream->past_len[way] + payload >= stream->len;
        if (fin) {
            payload = stream->len - stream->past_len[way];
        }
    }

    size_t const stream_len = syn + fin + payload;
    size_t packet_len = sizeof(struct ip_hdr) + sizeof(struct tcp_hdr) + payload;

    if (0 != iph_ctor(stream->packet, packet_len, stream->ip[way], stream->ip[!way])) return -1;

    if (0 != tcph_ctor((void *)((struct ip_hdr *)stream->packet+1),
        sizeof(struct tcp_hdr) + payload,
        stream->port[way], stream->port[!way],
        stream->isn[way] + stream->past_len[way],
        stream->isn[!way] + stream->past_len[!way],
        syn, fin,
        false,
        stream->past_len[!way] > 0)) return -1;

    stream->past_len[way] += stream_len;
    if (stream->past_len[!way] >= stream->len) stream->fin_acked[!way] = true;

    return packet_len;
}

ssize_t tcp_stream_next(struct tcp_stream *stream, unsigned *way_)
{
    unsigned way;

    if (stream->past_len[0] >= stream->len) {
        if (stream->past_len[1] >= stream->len) {
            if (! stream->fin_acked[0]) {
                way = 1;
            } else if (! stream->fin_acked[1]) {
                way = 0;
            } else {    // nothing left to be done
                return 0;
            }
        } else {
            way = 1;
        }
    } else if (stream->past_len[1] >= stream->len) {
        way = 0;
    } else if (stream->past_len[0] == 0 && stream->past_len[1] == 0) {
        way = 0;    // we want client to be way=0
    } else if (stream->past_len[0] == 1 && stream->past_len[1] == 0) {
        way = 1;    // synack
    } else if (stream->past_len[0] == 1 && stream->past_len[1] == 1) {
        way = 0;    // ack of synack (+ first datas)
    } else {
        way = !!(rand() & 0x100);
    }

    if (way_) *way_ = way;

    return stream_packet(stream, way);
}

int check_set_values(unsigned value, unsigned expected, unsigned mask, char const *mask_name)
{
    unsigned expected_set = expected & mask;
    unsigned value_set = value & mask;
    if (expected_set != value_set) {
        if (0 != expected_set) {
            printf("Expected %s to be set\n", mask_name);
            return -1;
        } else {
            printf("Unexpected %s set\n", mask_name);
            return -1;
        }
    }
    return 0;
}

void compare_proto_info(struct proto_info const *const info, struct proto_info const *const expected)
{
    CHECK_INT(info->head_len, expected->head_len);
    CHECK_INT(info->payload, expected->payload);
}

int check_big_int(uint64_t value, uint64_t expected, char const *field)
{
    if (expected != value) {
        printf("Expected 0x%"PRIx64" got 0x%"PRIx64" from field %s\n", expected, value, field);
        return -1;
    }
    return 0;
}

int check_int(uint32_t value, uint32_t expected, char const *field)
{
    if (expected != value) {
        printf("Expected %"PRIu32" got %"PRIu32" from field %s\n", expected, value, field);
        return -1;
    }
    return 0;
}

int check_str(char const *value, char const *expected, char const *field)
{
    if (0 != strcmp(expected, value)) {
        printf("Expected '%s'\nGot      '%s' from field %s\n", expected, value, field);
        for (unsigned i = 0; i < strlen(value); i++) {
            if (value[i] != expected[i]) {
                printf("Diff at character %d, got 0x%02x, expected 0x%02x\n", i, value[i], expected[i]);
                break;
            }
        }
        return -1;
    }
    return 0;
}

void compare_ip_proto_info(struct ip_proto_info const *const info, struct ip_proto_info const *const expected)
{
    compare_proto_info(&info->info, &expected->info);
    assert(info->version == expected->version);
    assert(0 == ip_addr_cmp(info->key.addr+0, expected->key.addr+0));
    assert(0 == ip_addr_cmp(info->key.addr+1, expected->key.addr+1));
    assert(info->key.protocol == expected->key.protocol);
    assert(info->ttl == expected->ttl);
}

void set_debug_log(void)
{
    log_set_level(LOG_DEBUG, NULL);
    log_set_level(LOG_WARNING, "mutex");
    log_set_level(LOG_WARNING, "redim_array");
}

