/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* kdc/replay.c - Replay lookaside cache for the KDC, to avoid extra work */
/*
 * Copyright 1991 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 */

#include "k5-int.h"
#include "k5-queue.h"
#include "kdc_util.h"
#include "extern.h"

#ifndef NOCACHE

struct entry {
    LIST_ENTRY(entry) bucket_links;
    TAILQ_ENTRY(entry) expire_links;
    int num_hits;
    krb5_timestamp timein;
    krb5_data req_packet;
    krb5_data reply_packet;
};

#ifndef LOOKASIDE_HASH_SIZE
#define LOOKASIDE_HASH_SIZE 16384
#endif
#ifndef LOOKASIDE_MAX_SIZE
#define LOOKASIDE_MAX_SIZE (10 * 1024 * 1024)
#endif

LIST_HEAD(entry_list, entry);
TAILQ_HEAD(entry_queue, entry);

static struct entry_list hash_table[LOOKASIDE_HASH_SIZE];
static struct entry_queue expiration_queue;

static int hits = 0;
static int calls = 0;
static int max_hits_per_entry = 0;
static int num_entries = 0;
static size_t total_size = 0;
static krb5_ui_4 seed;

#define STALE_TIME      (2*60)            /* two minutes */
#define STALE(ptr, now) (abs((ptr)->timein - (now)) >= STALE_TIME)

/* Return x rotated to the left by r bits. */
static inline krb5_ui_4
rotl32(krb5_ui_4 x, int r)
{
    return (x << r) | (x >> (32 - r));
}

/*
 * Return a non-cryptographic hash of data, seeded by seed (the global
 * variable), using the MurmurHash3 algorithm by Austin Appleby.  Return the
 * result modulo LOOKASIDE_HASH_SIZE.
 */
static int
murmurhash3(const krb5_data *data)
{
    const krb5_ui_4 c1 = 0xcc9e2d51, c2 = 0x1b873593;
    const unsigned char *start = (unsigned char *)data->data, *endblocks, *p;
    int tail_len = (data->length % 4);
    krb5_ui_4 h = seed, final;

    endblocks = start + data->length - tail_len;
    for (p = start; p < endblocks; p += 4) {
        h ^= rotl32(load_32_le(p) * c1, 15) * c2;
        h = rotl32(h, 13) * 5 + 0xe6546b64;
    }

    final = 0;
    final |= (tail_len >= 3) ? p[2] << 16 : 0;
    final |= (tail_len >= 2) ? p[1] << 8 : 0;
    final |= (tail_len >= 1) ? p[0] : 0;
    h ^= rotl32(final * c1, 15) * c2;

    h ^= data->length;
    h = (h ^ (h >> 16)) * 0x85ebca6b;
    h = (h ^ (h >> 13)) * 0xc2b2ae35;
    h ^= h >> 16;
    return h % LOOKASIDE_HASH_SIZE;
}

/* Return the rough memory footprint of an entry containing req and rep. */
static size_t
entry_size(const krb5_data *req, const krb5_data *rep)
{
    return sizeof(struct entry) + req->length +
        ((rep == NULL) ? 0 : rep->length);
}

/* Remove entry from its hash bucket and the expiration queue, and free it. */
static void
discard_entry(krb5_context context, struct entry *entry)
{
    total_size -= entry_size(&entry->req_packet, &entry->reply_packet);
    LIST_REMOVE(entry, bucket_links);
    TAILQ_REMOVE(&expiration_queue, entry, expire_links);
    krb5_free_data_contents(context, &entry->req_packet);
    krb5_free_data_contents(context, &entry->reply_packet);
    free(entry);
}

/* Return the entry for req_packet, or NULL if we don't have one. */
static struct entry *
find_entry(krb5_data *req_packet)
{
    krb5_ui_4 hash = murmurhash3(req_packet);
    struct entry *e;

    LIST_FOREACH(e, &hash_table[hash], bucket_links) {
        if (data_eq(e->req_packet, *req_packet))
            return e;
    }
    return NULL;
}

/* Initialize the lookaside cache structures and randomize the hash seed. */
krb5_error_code
kdc_init_lookaside(krb5_context context)
{
    krb5_data d = make_data(&seed, sizeof(seed));
    int i;

    for (i = 0; i < LOOKASIDE_HASH_SIZE; i++)
        LIST_INIT(&hash_table[i]);
    TAILQ_INIT(&expiration_queue);
    return krb5_c_random_make_octets(context, &d);
}

/* Remove the lookaside cache entry for a packet. */
void
kdc_remove_lookaside(krb5_context kcontext, krb5_data *req_packet)
{
    struct entry *e;

    e = find_entry(req_packet);
    if (e != NULL)
        discard_entry(kdc_context, e);
}

/* Return true and fill in reply_packet_out if req_packet is in the lookaside
 * cache; otherwise return false.  Also discard old entries in the cache. */
krb5_boolean
kdc_check_lookaside(krb5_data *req_packet, krb5_data **reply_packet_out)
{
    struct entry *e;

    *reply_packet_out = NULL;
    calls++;

    e = find_entry(req_packet);
    if (e == NULL)
        return FALSE;

    e->num_hits++;
    hits++;
    return (krb5_copy_data(kdc_context, &e->reply_packet,
                           reply_packet_out) == 0);
}

/* Insert a request and reply into the lookaside cache.  Assumes it's not
 * already there, and can fail silently on memory exhaustion. */
void
kdc_insert_lookaside(krb5_data *req_packet, krb5_data *reply_packet)
{
    struct entry *e, *next;
    krb5_timestamp timenow;
    krb5_ui_4 hash = murmurhash3(req_packet);
    size_t esize = entry_size(req_packet, reply_packet);

    if (krb5_timeofday(kdc_context, &timenow))
        return;

    /* Purge stale entries and limit the total size of the entries. */
    TAILQ_FOREACH_SAFE(e, &expiration_queue, expire_links, next) {
        if (!STALE(e, timenow) && total_size + esize <= LOOKASIDE_MAX_SIZE)
            break;
        max_hits_per_entry = max(max_hits_per_entry, e->num_hits);
        discard_entry(kdc_context, e);
    }

    /* Create a new entry for this request and reply. */
    e = calloc(1, sizeof(*e));
    if (e == NULL)
        return;
    e->timein = timenow;
    if (krb5int_copy_data_contents(kdc_context, req_packet, &e->req_packet)) {
        free(e);
        return;
    }
    if (reply_packet != NULL &&
        krb5int_copy_data_contents(kdc_context, reply_packet,
                                   &e->reply_packet)) {
        krb5_free_data_contents(kdc_context, &e->req_packet);
        free(e);
        return;
    }

    TAILQ_INSERT_TAIL(&expiration_queue, e, expire_links);
    LIST_INSERT_HEAD(&hash_table[hash], e, bucket_links);
    num_entries++;
    total_size += esize;
    return;
}

/* Free all entries in the lookaside cache. */
void
kdc_free_lookaside(krb5_context kcontext)
{
    struct entry *e, *next;

    TAILQ_FOREACH_SAFE(e, &expiration_queue, expire_links, next) {
        discard_entry(kcontext, e);
    }
}

#endif /* NOCACHE */
