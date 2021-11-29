/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef NGTCP2_ACKTR_H
#define NGTCP2_ACKTR_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <ngtcp2/ngtcp2.h>

#include "ngtcp2_mem.h"
#include "ngtcp2_ringbuf.h"
#include "ngtcp2_ksl.h"

/* NGTCP2_ACKTR_MAX_ENT is the maximum number of ngtcp2_acktr_entry
   which ngtcp2_acktr stores. */
#define NGTCP2_ACKTR_MAX_ENT 1024

/* ns */
#define NGTCP2_DEFAULT_ACK_DELAY 25000000

struct ngtcp2_conn;
typedef struct ngtcp2_conn ngtcp2_conn;

struct ngtcp2_acktr_entry;
typedef struct ngtcp2_acktr_entry ngtcp2_acktr_entry;

struct ngtcp2_log;
typedef struct ngtcp2_log ngtcp2_log;

/*
 * ngtcp2_acktr_entry is a single packet which needs to be acked.
 */
struct ngtcp2_acktr_entry {
  uint64_t pkt_num;
  ngtcp2_tstamp tstamp;
};

/*
 * ngtcp2_acktr_entry_new allocates memory for ent, and initializes it
 * with the given parameters.
 */
int ngtcp2_acktr_entry_new(ngtcp2_acktr_entry **ent, uint64_t pkt_num,
                           ngtcp2_tstamp tstamp, ngtcp2_mem *mem);

/*
 * ngtcp2_acktr_entry_del deallocates memory allocated for |ent|.  It
 * deallocates memory pointed by |ent|.
 */
void ngtcp2_acktr_entry_del(ngtcp2_acktr_entry *ent, ngtcp2_mem *mem);

typedef struct {
  ngtcp2_ack *ack;
  uint64_t pkt_num;
  ngtcp2_tstamp ts;
  uint8_t ack_only;
} ngtcp2_acktr_ack_entry;

typedef enum {
  NGTCP2_ACKTR_FLAG_NONE = 0x00,
  /* NGTCP2_ACKTR_FLAG_DELAYED_ACK indicates that delayed ACK is
     enabled. */
  NGTCP2_ACKTR_FLAG_DELAYED_ACK = 0x01,
  /* NGTCP2_ACKTR_FLAG_ACTIVE_ACK indicates that there are
     pending protected packet to be acknowledged. */
  NGTCP2_ACKTR_FLAG_ACTIVE_ACK = 0x02,
  /* NGTCP2_ACKTR_FLAG_PENDING_ACK_FINISHED is set when server
     received TLSv1.3 Finished message, and its acknowledgement is
     pending. */
  NGTCP2_ACKTR_FLAG_PENDING_FINISHED_ACK = 0x40,
  /* NGTCP2_ACKTR_FLAG_ACK_FINISHED_ACK is set when server received
     acknowledgement for ACK which acknowledges the last handshake
     packet from client (which contains TLSv1.3 Finished message). */
  NGTCP2_ACKTR_FLAG_ACK_FINISHED_ACK = 0x80,
  /* NGTCP2_ACKTR_FLAG_DELAYED_ACK_EXPIRED is set when delayed ACK
     timer is expired. */
  NGTCP2_ACKTR_FLAG_DELAYED_ACK_EXPIRED = 0x0100,
} ngtcp2_acktr_flag;

/*
 * ngtcp2_acktr tracks received packets which we have to send ack.
 */
typedef struct {
  ngtcp2_ringbuf acks;
  /* ents includes ngtcp2_acktr_entry sorted by decreasing order of
     packet number. */
  ngtcp2_ksl ents;
  ngtcp2_log *log;
  ngtcp2_mem *mem;
  /* flags is bitwise OR of zero, or more of ngtcp2_ack_flag. */
  uint16_t flags;
  /* first_unacked_ts is timestamp when ngtcp2_acktr_entry is added
     first time after the last outgoing protected ACK frame. */
  ngtcp2_tstamp first_unacked_ts;
} ngtcp2_acktr;

/*
 * ngtcp2_acktr_init initializes |acktr|.  If |delayed_ack| is
 * nonzero, delayed ack is enabled.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
int ngtcp2_acktr_init(ngtcp2_acktr *acktr, int delayed_ack, ngtcp2_log *log,
                      ngtcp2_mem *mem);

/*
 * ngtcp2_acktr_free frees resources allocated for |acktr|.  It frees
 * any ngtcp2_acktr_entry added to |acktr|.
 */
void ngtcp2_acktr_free(ngtcp2_acktr *acktr);

/*
 * ngtcp2_acktr_add adds |ent|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Same packet number has already been included in |acktr|.
 */
int ngtcp2_acktr_add(ngtcp2_acktr *acktr, ngtcp2_acktr_entry *ent,
                     int active_ack, ngtcp2_tstamp ts);

/*
 * ngtcp2_acktr_forget removes all entries which have the packet
 * number that is equal to or less than ent->pkt_num.  This function
 * assumes that |acktr| includes |ent|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
int ngtcp2_acktr_forget(ngtcp2_acktr *acktr, ngtcp2_acktr_entry *ent);

/*
 * ngtcp2_acktr_get returns the pointer to pointer to the entry which
 * has the largest packet number to be acked.  If there is no entry,
 * returned value satisfies ngtcp2_ksl_it_end(&it) != 0.
 */
ngtcp2_ksl_it ngtcp2_acktr_get(ngtcp2_acktr *acktr);

/*
 * ngtcp2_acktr_add_ack adds the outgoing ACK frame |fr| to |acktr|.
 * |pkt_num| is the packet number which |fr| belongs.  This function
 * transfers the ownership of |fr| to |acktr|.  |ack_only| is nonzero
 * if the packet contains an ACK frame only.  This function returns a
 * pointer to the object it adds.
 */
ngtcp2_acktr_ack_entry *ngtcp2_acktr_add_ack(ngtcp2_acktr *acktr,
                                             uint64_t pkt_num, ngtcp2_ack *fr,
                                             ngtcp2_tstamp ts, int ack_only);

/*
 * ngtcp2_acktr_recv_ack processes the incoming ACK frame |fr|.
 * |pkt_num| is a packet number which includes |fr|.  If we receive
 * ACK which acknowledges the ACKs added by ngtcp2_acktr_add_ack,
 * ngtcp2_acktr_entry which the outgoing ACK acknowledges is removed.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
int ngtcp2_acktr_recv_ack(ngtcp2_acktr *acktr, const ngtcp2_ack *fr,
                          ngtcp2_conn *conn, ngtcp2_tstamp ts);

/*
 * ngtcp2_acktr_commit_ack tells |acktr| that ACK frame is generated.
 */
void ngtcp2_acktr_commit_ack(ngtcp2_acktr *acktr);

/*
 * ngtcp2_acktr_require_active_ack returns nonzero if ACK frame should
 * be generated actively.
 */
int ngtcp2_acktr_require_active_ack(ngtcp2_acktr *acktr, uint64_t max_ack_delay,
                                    ngtcp2_tstamp ts);

/*
 * ngtcp2_acktr_expire_delayed_ack expires delayed ACK timer.  This
 * function sets NGTCP2_ACKTR_FLAG_DELAYED_ACK_EXPIRED so that we know
 * that the timer has expired.
 */
void ngtcp2_acktr_expire_delayed_ack(ngtcp2_acktr *acktr);

/*
 * ngtcp2_acktr_delayed_ack returns nonzero if |acktr| enables delayed
 * ACK.
 */
int ngtcp2_acktr_delayed_ack(ngtcp2_acktr *acktr);

#endif /* NGTCP2_ACKTR_H */
