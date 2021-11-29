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
#include "ngtcp2_rtb_test.h"

#include <CUnit/CUnit.h>

#include "ngtcp2_rtb.h"
#include "ngtcp2_test_helper.h"
#include "ngtcp2_mem.h"
#include "ngtcp2_pkt.h"

static void cc_stat_init(ngtcp2_cc_stat *ccs) {
  memset(ccs, 0, sizeof(ngtcp2_cc_stat));
}

void test_ngtcp2_rtb_add(void) {
  ngtcp2_rtb rtb;
  ngtcp2_rtb_entry *ent;
  int rv;
  ngtcp2_mem *mem = ngtcp2_mem_default();
  ngtcp2_pkt_hd hd;
  ngtcp2_log log;
  ngtcp2_cid dcid;
  ngtcp2_ksl_it it;
  ngtcp2_cc_stat ccs;

  dcid_init(&dcid);
  cc_stat_init(&ccs);
  ngtcp2_log_init(&log, NULL, NULL, 0, NULL);
  ngtcp2_rtb_init(&rtb, &ccs, &log, mem);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     1000000007, 1, NGTCP2_PROTO_VER_MAX, 0);

  rv = ngtcp2_rtb_entry_new(&ent, &hd, NULL, 10, 0, NGTCP2_RTB_FLAG_NONE, mem);

  CU_ASSERT(0 == rv);

  ngtcp2_rtb_add(&rtb, ent);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     1000000008, 2, NGTCP2_PROTO_VER_MAX, 0);

  rv = ngtcp2_rtb_entry_new(&ent, &hd, NULL, 9, 0, NGTCP2_RTB_FLAG_NONE, mem);

  CU_ASSERT(0 == rv);

  ngtcp2_rtb_add(&rtb, ent);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     1000000009, 4, NGTCP2_PROTO_VER_MAX, 0);

  rv = ngtcp2_rtb_entry_new(&ent, &hd, NULL, 11, 0, NGTCP2_RTB_FLAG_NONE, mem);

  CU_ASSERT(0 == rv);

  ngtcp2_rtb_add(&rtb, ent);

  it = ngtcp2_rtb_head(&rtb);
  ent = ngtcp2_ksl_it_get(&it);

  /* Check the top of the queue */
  CU_ASSERT(1000000009 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(1000000008 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(1000000007 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);

  CU_ASSERT(ngtcp2_ksl_it_end(&it));

  ngtcp2_rtb_free(&rtb);
}

static void add_rtb_entry_range(ngtcp2_rtb *rtb, uint64_t base_pkt_num,
                                size_t len, ngtcp2_mem *mem) {
  ngtcp2_pkt_hd hd;
  ngtcp2_rtb_entry *ent;
  uint64_t i;
  ngtcp2_cid dcid;

  dcid_init(&dcid);

  for (i = base_pkt_num; i < base_pkt_num + len; ++i) {
    ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                       i, 1, NGTCP2_PROTO_VER_MAX, 0);
    ngtcp2_rtb_entry_new(&ent, &hd, NULL, 0, 0, NGTCP2_RTB_FLAG_NONE, mem);
    ngtcp2_rtb_add(rtb, ent);
  }
}

static void setup_rtb_fixture(ngtcp2_rtb *rtb, ngtcp2_mem *mem) {
  /* 100, ..., 154 */
  add_rtb_entry_range(rtb, 100, 55, mem);
  /* 180, ..., 184 */
  add_rtb_entry_range(rtb, 180, 5, mem);
  /* 440, ..., 446 */
  add_rtb_entry_range(rtb, 440, 7, mem);
}

static void assert_rtb_entry_not_found(ngtcp2_rtb *rtb, uint64_t pkt_num) {
  ngtcp2_ksl_it it = ngtcp2_rtb_head(rtb);
  ngtcp2_rtb_entry *ent;

  for (; !ngtcp2_ksl_it_end(&it); ngtcp2_ksl_it_next(&it)) {
    ent = ngtcp2_ksl_it_get(&it);
    CU_ASSERT(ent->hd.pkt_num != pkt_num);
  }
}

void test_ngtcp2_rtb_recv_ack(void) {
  ngtcp2_rtb rtb;
  ngtcp2_mem *mem = ngtcp2_mem_default();
  ngtcp2_max_frame mfr;
  ngtcp2_ack *fr = &mfr.ackfr.ack;
  ngtcp2_ack_blk *blks;
  ngtcp2_log log;
  ngtcp2_cc_stat ccs;

  ngtcp2_log_init(&log, NULL, NULL, 0, NULL);

  /* no ack block */
  cc_stat_init(&ccs);
  ngtcp2_rtb_init(&rtb, &ccs, &log, mem);
  setup_rtb_fixture(&rtb, mem);

  CU_ASSERT(67 == ngtcp2_ksl_len(&rtb.ents));

  fr->largest_ack = 446;
  fr->first_ack_blklen = 1;
  fr->num_blks = 0;

  ngtcp2_rtb_recv_ack(&rtb, fr, NULL, 1000000009);

  CU_ASSERT(65 == ngtcp2_ksl_len(&rtb.ents));
  assert_rtb_entry_not_found(&rtb, 446);
  assert_rtb_entry_not_found(&rtb, 445);

  ngtcp2_rtb_free(&rtb);

  /* with ack block */
  cc_stat_init(&ccs);
  ngtcp2_rtb_init(&rtb, &ccs, &log, mem);
  setup_rtb_fixture(&rtb, mem);

  fr->largest_ack = 441;
  fr->first_ack_blklen = 3; /* (441), (440), 439, 438 */
  fr->num_blks = 2;
  blks = fr->blks;
  blks[0].gap = 253;
  blks[0].blklen = 0; /* (183) */
  blks[1].gap = 1;    /* 182, 181 */
  blks[1].blklen = 1; /* (180), 179 */

  ngtcp2_rtb_recv_ack(&rtb, fr, NULL, 1000000009);

  CU_ASSERT(63 == ngtcp2_ksl_len(&rtb.ents));
  CU_ASSERT(441 == rtb.largest_acked_tx_pkt_num);
  assert_rtb_entry_not_found(&rtb, 441);
  assert_rtb_entry_not_found(&rtb, 440);
  assert_rtb_entry_not_found(&rtb, 183);
  assert_rtb_entry_not_found(&rtb, 180);

  ngtcp2_rtb_free(&rtb);

  /* gap+blklen points to pkt_num 0 */
  cc_stat_init(&ccs);
  ngtcp2_rtb_init(&rtb, &ccs, &log, mem);
  add_rtb_entry_range(&rtb, 0, 1, mem);

  fr->largest_ack = 250;
  fr->first_ack_blklen = 0;
  fr->num_blks = 1;
  fr->blks[0].gap = 248;
  fr->blks[0].blklen = 0;

  ngtcp2_rtb_recv_ack(&rtb, fr, NULL, 1000000009);

  assert_rtb_entry_not_found(&rtb, 0);

  ngtcp2_rtb_free(&rtb);

  /* pkt_num = 0 (first ack block) */
  cc_stat_init(&ccs);
  ngtcp2_rtb_init(&rtb, &ccs, &log, mem);
  add_rtb_entry_range(&rtb, 0, 1, mem);

  fr->largest_ack = 0;
  fr->first_ack_blklen = 0;
  fr->num_blks = 0;

  ngtcp2_rtb_recv_ack(&rtb, fr, NULL, 1000000009);

  assert_rtb_entry_not_found(&rtb, 0);

  ngtcp2_rtb_free(&rtb);

  /* pkt_num = 0 */
  cc_stat_init(&ccs);
  ngtcp2_rtb_init(&rtb, &ccs, &log, mem);
  add_rtb_entry_range(&rtb, 0, 1, mem);

  fr->largest_ack = 2;
  fr->first_ack_blklen = 0;
  fr->num_blks = 1;
  fr->blks[0].gap = 0;
  fr->blks[0].blklen = 0;

  ngtcp2_rtb_recv_ack(&rtb, fr, NULL, 1000000009);

  assert_rtb_entry_not_found(&rtb, 0);

  ngtcp2_rtb_free(&rtb);
}

void test_ngtcp2_rtb_insert_range(void) {
  ngtcp2_rtb rtb;
  ngtcp2_mem *mem = ngtcp2_mem_default();
  ngtcp2_log log;
  ngtcp2_rtb_entry *head, *ent1, *ent2, *ent3, *ent4, *ent5, *ent;
  ngtcp2_pkt_hd hd;
  ngtcp2_cid dcid, scid;
  ngtcp2_ksl_it it;
  ngtcp2_cc_stat ccs;

  dcid_init(&dcid);
  scid_init(&scid);
  cc_stat_init(&ccs);
  ngtcp2_log_init(&log, NULL, NULL, 0, NULL);

  ngtcp2_rtb_init(&rtb, &ccs, &log, mem);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 900, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent1, &hd, NULL, 0, 1, NGTCP2_RTB_FLAG_NONE, mem);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 898, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent2, &hd, NULL, 0, 2, NGTCP2_RTB_FLAG_NONE, mem);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 897, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent3, &hd, NULL, 0, 4, NGTCP2_RTB_FLAG_NONE, mem);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 790, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent4, &hd, NULL, 0, 8, NGTCP2_RTB_FLAG_NONE, mem);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 788, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent5, &hd, NULL, 0, 16, NGTCP2_RTB_FLAG_NONE, mem);

  head = ent1;
  ent1->next = ent2;
  ent2->next = ent3;
  ent3->next = ent4;
  ent4->next = ent5;

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 896, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent, &hd, NULL, 0, 0, NGTCP2_RTB_FLAG_NONE, mem);
  ngtcp2_rtb_add(&rtb, ent);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 899, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent, &hd, NULL, 0, 0, NGTCP2_RTB_FLAG_NONE, mem);
  ngtcp2_rtb_add(&rtb, ent);

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_HANDSHAKE, &dcid,
                     &scid, 901, 4, NGTCP2_PROTO_VER_MAX, 0);
  ngtcp2_rtb_entry_new(&ent, &hd, NULL, 0, 0, NGTCP2_RTB_FLAG_NONE, mem);
  ngtcp2_rtb_add(&rtb, ent);

  ngtcp2_rtb_insert_range(&rtb, head);

  CU_ASSERT(31 == rtb.bytes_in_flight);

  it = ngtcp2_rtb_head(&rtb);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(901 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(900 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(899 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(898 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(897 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(896 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(790 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);
  ent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(788 == ent->hd.pkt_num);

  ngtcp2_ksl_it_next(&it);

  CU_ASSERT(ngtcp2_ksl_it_end(&it));

  ngtcp2_rtb_free(&rtb);
}
