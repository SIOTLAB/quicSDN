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
#include "ngtcp2_pkt_test.h"

#include <assert.h>

#include <CUnit/CUnit.h>

#include "ngtcp2_pkt.h"
#include "ngtcp2_test_helper.h"
#include "ngtcp2_conv.h"
#include "ngtcp2_cid.h"

void test_ngtcp2_pkt_decode_hd_long(void) {
  ngtcp2_pkt_hd hd, nhd;
  uint8_t buf[256];
  ssize_t rv;
  ngtcp2_cid dcid, scid;
  size_t len;

  dcid_init(&dcid);
  scid_init(&scid);

  /* Handshake */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_LONG_FORM, NGTCP2_PKT_HANDSHAKE,
                     &dcid, &scid, 0xe1e2e3e4u, 4, 0x000000ff, 16383);

  rv = ngtcp2_pkt_encode_hd_long(buf, sizeof(buf), &hd);

  len = 1 + 4 + 1 + dcid.datalen + scid.datalen + 2 + 4;

  CU_ASSERT((ssize_t)len == rv);

  rv = pkt_decode_hd_long(&nhd, buf, len);

  CU_ASSERT((ssize_t)len == rv);
  CU_ASSERT(hd.type == nhd.type);
  CU_ASSERT(hd.flags == nhd.flags);
  CU_ASSERT(ngtcp2_cid_eq(&hd.dcid, &nhd.dcid));
  CU_ASSERT(ngtcp2_cid_eq(&hd.scid, &nhd.scid));
  CU_ASSERT((hd.pkt_num & 0x3fffffffu) == nhd.pkt_num);
  CU_ASSERT(hd.version == nhd.version);
  CU_ASSERT(hd.len == nhd.len);

  /* VN */
  /* Set random packet type */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_LONG_FORM, NGTCP2_PKT_HANDSHAKE,
                     &dcid, &scid, 0, 4, 0, 0);

  rv = ngtcp2_pkt_encode_hd_long(buf, sizeof(buf), &hd);

  len = 1 + 4 + 1 + dcid.datalen + scid.datalen;

  CU_ASSERT((ssize_t)len == rv - 2 /* payloadlen */ - 4 /* pkt_num */);

  rv = pkt_decode_hd_long(&nhd, buf, len);

  CU_ASSERT((ssize_t)len == rv);
  CU_ASSERT(NGTCP2_PKT_VERSION_NEGOTIATION == nhd.type);
  CU_ASSERT(hd.flags == nhd.flags);
  CU_ASSERT(ngtcp2_cid_eq(&hd.dcid, &nhd.dcid));
  CU_ASSERT(ngtcp2_cid_eq(&hd.scid, &nhd.scid));
  CU_ASSERT(hd.pkt_num == nhd.pkt_num);
  CU_ASSERT(hd.version == nhd.version);
  CU_ASSERT(hd.len == nhd.len);
}

void test_ngtcp2_pkt_decode_hd_short(void) {
  ngtcp2_pkt_hd hd, nhd;
  uint8_t buf[256];
  ssize_t rv;
  size_t expectedlen;
  ngtcp2_cid dcid, zcid;

  dcid_init(&dcid);
  ngtcp2_cid_zero(&zcid);

  /* 4 bytes packet number */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     0xe1e2e3e4u, 4, 0xd1d2d3d4u, 0);

  expectedlen = 1 + dcid.datalen + 4;

  rv = ngtcp2_pkt_encode_hd_short(buf, sizeof(buf), &hd);

  CU_ASSERT((ssize_t)expectedlen == rv);

  rv = pkt_decode_hd_short(&nhd, buf, expectedlen, dcid.datalen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(hd.flags == nhd.flags);
  CU_ASSERT(NGTCP2_PKT_SHORT == nhd.type);
  CU_ASSERT(ngtcp2_cid_eq(&dcid, &nhd.dcid));
  CU_ASSERT(ngtcp2_cid_empty(&nhd.scid));
  CU_ASSERT((hd.pkt_num & 0x3fffffffu) == nhd.pkt_num);
  CU_ASSERT(hd.pkt_numlen == nhd.pkt_numlen);
  CU_ASSERT(0 == nhd.version);
  CU_ASSERT(0 == nhd.len);

  /* 2 bytes packet number */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     0xe1e2e3e4u, 2, 0xd1d2d3d4u, 0);

  expectedlen = 1 + dcid.datalen + 2;

  rv = ngtcp2_pkt_encode_hd_short(buf, sizeof(buf), &hd);

  CU_ASSERT((ssize_t)expectedlen == rv);

  rv = pkt_decode_hd_short(&nhd, buf, expectedlen, dcid.datalen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(hd.flags == nhd.flags);
  CU_ASSERT(NGTCP2_PKT_SHORT == nhd.type);
  CU_ASSERT(ngtcp2_cid_eq(&dcid, &nhd.dcid));
  CU_ASSERT(ngtcp2_cid_empty(&nhd.scid));
  CU_ASSERT((hd.pkt_num & 0x3fff) == nhd.pkt_num);
  CU_ASSERT(hd.pkt_numlen == nhd.pkt_numlen);
  CU_ASSERT(0 == nhd.version);
  CU_ASSERT(0 == nhd.len);

  /* 1 byte packet number */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     0xe1e2e3e4u, 1, 0xd1d2d3d4u, 0);

  expectedlen = 1 + dcid.datalen + 1;

  rv = ngtcp2_pkt_encode_hd_short(buf, sizeof(buf), &hd);

  CU_ASSERT((ssize_t)expectedlen == rv);

  rv = pkt_decode_hd_short(&nhd, buf, expectedlen, dcid.datalen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(hd.flags == nhd.flags);
  CU_ASSERT(NGTCP2_PKT_SHORT == nhd.type);
  CU_ASSERT(ngtcp2_cid_eq(&dcid, &nhd.dcid));
  CU_ASSERT(ngtcp2_cid_empty(&nhd.scid));
  CU_ASSERT((hd.pkt_num & 0x7f) == nhd.pkt_num);
  CU_ASSERT(hd.pkt_numlen == nhd.pkt_numlen);
  CU_ASSERT(0 == nhd.version);
  CU_ASSERT(0 == nhd.len);

  /* With Key Phase */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_KEY_PHASE, NGTCP2_PKT_SHORT, &dcid,
                     NULL, 0xe1e2e3e4u, 4, 0xd1d2d3d4u, 0);

  expectedlen = 1 + dcid.datalen + 4;

  rv = ngtcp2_pkt_encode_hd_short(buf, sizeof(buf), &hd);

  CU_ASSERT((ssize_t)expectedlen == rv);

  rv = pkt_decode_hd_short(&nhd, buf, expectedlen, dcid.datalen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(hd.flags == nhd.flags);
  CU_ASSERT(NGTCP2_PKT_SHORT == nhd.type);
  CU_ASSERT(ngtcp2_cid_eq(&dcid, &nhd.dcid));
  CU_ASSERT(ngtcp2_cid_empty(&nhd.scid));
  CU_ASSERT((hd.pkt_num & 0x3fffffff) == nhd.pkt_num);
  CU_ASSERT(hd.pkt_numlen == nhd.pkt_numlen);
  CU_ASSERT(0 == nhd.version);
  CU_ASSERT(0 == nhd.len);

  /* With empty DCID */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, NULL, NULL,
                     0xe1e2e3e4u, 4, 0xd1d2d3d4u, 0);

  expectedlen = 1 + 4;

  rv = ngtcp2_pkt_encode_hd_short(buf, sizeof(buf), &hd);

  CU_ASSERT((ssize_t)expectedlen == rv);

  rv = pkt_decode_hd_short(&nhd, buf, expectedlen, 0);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(hd.flags == nhd.flags);
  CU_ASSERT(NGTCP2_PKT_SHORT == nhd.type);
  CU_ASSERT(ngtcp2_cid_empty(&nhd.dcid));
  CU_ASSERT(ngtcp2_cid_empty(&nhd.scid));
  CU_ASSERT((hd.pkt_num & 0x3fffffff) == nhd.pkt_num);
  CU_ASSERT(hd.pkt_numlen == nhd.pkt_numlen);
  CU_ASSERT(0 == nhd.version);
  CU_ASSERT(0 == nhd.len);
}

void test_ngtcp2_pkt_decode_stream_frame(void) {
  uint8_t buf[256];
  size_t buflen;
  ngtcp2_frame fr;
  ssize_t rv;
  size_t expectedlen;

  /* 32 bits Stream ID + 62 bits Offset + Data Length */
  buflen = ngtcp2_t_encode_stream_frame(buf, NGTCP2_STREAM_LEN_BIT, 0xf1f2f3f4u,
                                        0x31f2f3f4f5f6f7f8llu, 0x14);

  expectedlen = 1 + 8 + 8 + 1 + 20;

  CU_ASSERT(expectedlen == buflen);

  rv = ngtcp2_pkt_decode_stream_frame(&fr.stream, buf, buflen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(0 == fr.stream.fin);
  CU_ASSERT(0xf1f2f3f4u == fr.stream.stream_id);
  CU_ASSERT(0x31f2f3f4f5f6f7f8llu == fr.stream.offset);
  CU_ASSERT(0x14 == fr.stream.datalen);

  /* Cutting 1 bytes from the tail must cause invalid argument
     error */
  rv = ngtcp2_pkt_decode_stream_frame(&fr.stream, buf, buflen - 1);

  CU_ASSERT(NGTCP2_ERR_FRAME_ENCODING == rv);

  memset(&fr, 0, sizeof(fr));

  /* 6 bits Stream ID + no Offset + Data Length */
  buflen = ngtcp2_t_encode_stream_frame(buf, NGTCP2_STREAM_LEN_BIT, 0x31, 0x00,
                                        0x14);

  expectedlen = 1 + 1 + 0 + 1 + 20;

  CU_ASSERT(expectedlen == buflen);

  rv = ngtcp2_pkt_decode_stream_frame(&fr.stream, buf, buflen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(0 == fr.stream.fin);
  CU_ASSERT(0x31 == fr.stream.stream_id);
  CU_ASSERT(0x00 == fr.stream.offset);
  CU_ASSERT(0x14 == fr.stream.datalen);

  /* Cutting 1 bytes from the tail must cause invalid argument
     error */
  rv = ngtcp2_pkt_decode_stream_frame(&fr.stream, buf, buflen - 1);

  CU_ASSERT(NGTCP2_ERR_FRAME_ENCODING == rv);

  memset(&fr, 0, sizeof(fr));

  /* Fin bit set + no Data Length */
  buflen = ngtcp2_t_encode_stream_frame(buf, NGTCP2_STREAM_FIN_BIT, 0x31f2f3f4u,
                                        0x00, 0x14);

  expectedlen = 1 + 4 + 20;

  CU_ASSERT(expectedlen == buflen);

  rv = ngtcp2_pkt_decode_stream_frame(&fr.stream, buf, buflen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(1 == fr.stream.fin);
  CU_ASSERT(0x31f2f3f4u == fr.stream.stream_id);
  CU_ASSERT(0x00 == fr.stream.offset);
  CU_ASSERT(0x14 == fr.stream.datalen);

  memset(&fr, 0, sizeof(fr));
}

void test_ngtcp2_pkt_decode_ack_frame(void) {
  uint8_t buf[256];
  size_t buflen;
  ngtcp2_frame fr;
  ssize_t rv;
  size_t expectedlen;

  /* 62 bits Largest Acknowledged */
  buflen = ngtcp2_t_encode_ack_frame(buf, 0x31f2f3f4f5f6f7f8llu,
                                     0x31e2e3e4e5e6e7e8llu, 99,
                                     0x31d2d3d4d5d6d7d8llu);

  expectedlen = 1 + 8 + 1 + 1 + 8 + 2 + 8;

  CU_ASSERT(expectedlen == buflen);

  rv = ngtcp2_pkt_decode_ack_frame(&fr.ack, buf, buflen);

  CU_ASSERT((ssize_t)expectedlen == rv);
  CU_ASSERT(0x31f2f3f4f5f6f7f8llu == fr.ack.largest_ack);
  CU_ASSERT(1 == fr.ack.num_blks);
  CU_ASSERT(0x31e2e3e4e5e6e7e8llu == fr.ack.first_ack_blklen);
  CU_ASSERT(99 == fr.ack.blks[0].gap);
  CU_ASSERT(0x31d2d3d4d5d6d7d8llu == fr.ack.blks[0].blklen);
}

void test_ngtcp2_pkt_decode_padding_frame(void) {
  uint8_t buf[256];
  ngtcp2_frame fr;
  size_t rv;
  size_t paddinglen = 31;

  memset(buf, 0, paddinglen);
  buf[paddinglen] = NGTCP2_FRAME_STREAM;

  rv = ngtcp2_pkt_decode_padding_frame(&fr.padding, buf, paddinglen + 1);

  CU_ASSERT(paddinglen == rv);
  CU_ASSERT((size_t)31 == fr.padding.len);
}

void test_ngtcp2_pkt_encode_stream_frame(void) {
  const uint8_t data[] = "0123456789abcdef0";
  uint8_t buf[256];
  ngtcp2_frame fr, nfr;
  ssize_t rv;
  size_t framelen;
  size_t i;

  /* 32 bits Stream ID + 62 bits Offset + Data Length */
  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.fin = 0;
  fr.stream.stream_id = 0xf1f2f3f4u;
  fr.stream.offset = 0x31f2f3f4f5f6f7f8llu;
  fr.stream.datalen = strsize(data);
  fr.stream.data = data;

  framelen = 1 + 8 + 8 + 1 + 17;

  rv = ngtcp2_pkt_encode_stream_frame(buf, sizeof(buf), &fr.stream);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_stream_frame(&nfr.stream, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT((NGTCP2_STREAM_OFF_BIT | NGTCP2_STREAM_LEN_BIT) ==
            nfr.stream.flags);
  CU_ASSERT(fr.stream.fin == nfr.stream.fin);
  CU_ASSERT(fr.stream.stream_id == nfr.stream.stream_id);
  CU_ASSERT(fr.stream.offset == nfr.stream.offset);
  CU_ASSERT(fr.stream.datalen == nfr.stream.datalen);
  CU_ASSERT(0 == memcmp(fr.stream.data, nfr.stream.data, fr.stream.datalen));

  memset(&nfr, 0, sizeof(nfr));

  /* 6 bits Stream ID + No Offset + Data Length */
  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.fin = 0;
  fr.stream.stream_id = 0x31;
  fr.stream.offset = 0;
  fr.stream.datalen = strsize(data);
  fr.stream.data = data;

  framelen = 1 + 1 + 1 + 17;

  rv = ngtcp2_pkt_encode_stream_frame(buf, sizeof(buf), &fr.stream);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_stream_frame(&nfr.stream, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(NGTCP2_STREAM_LEN_BIT == nfr.stream.flags);
  CU_ASSERT(fr.stream.fin == nfr.stream.fin);
  CU_ASSERT(fr.stream.stream_id == nfr.stream.stream_id);
  CU_ASSERT(fr.stream.offset == nfr.stream.offset);
  CU_ASSERT(fr.stream.datalen == nfr.stream.datalen);
  CU_ASSERT(0 == memcmp(fr.stream.data, nfr.stream.data, fr.stream.datalen));

  memset(&nfr, 0, sizeof(nfr));

  /* Fin + 32 bits Stream ID + 62 bits Offset + Data Length */
  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.fin = 1;
  fr.stream.stream_id = 0xf1f2f3f4u;
  fr.stream.offset = 0x31f2f3f4f5f6f7f8llu;
  fr.stream.datalen = strsize(data);
  fr.stream.data = data;

  framelen = 1 + 8 + 8 + 1 + 17;

  rv = ngtcp2_pkt_encode_stream_frame(buf, sizeof(buf), &fr.stream);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_stream_frame(&nfr.stream, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT((NGTCP2_STREAM_FIN_BIT | NGTCP2_STREAM_OFF_BIT |
             NGTCP2_STREAM_LEN_BIT) == nfr.stream.flags);
  CU_ASSERT(fr.stream.fin == nfr.stream.fin);
  CU_ASSERT(fr.stream.stream_id == nfr.stream.stream_id);
  CU_ASSERT(fr.stream.offset == nfr.stream.offset);
  CU_ASSERT(fr.stream.datalen == nfr.stream.datalen);
  CU_ASSERT(0 == memcmp(fr.stream.data, nfr.stream.data, fr.stream.datalen));

  /* Make sure that we check the length properly. */
  for (i = 1; i < framelen; ++i) {
    rv = ngtcp2_pkt_decode_stream_frame(&nfr.stream, buf, i);

    CU_ASSERT(NGTCP2_ERR_FRAME_ENCODING == rv);
  }

  memset(&nfr, 0, sizeof(nfr));

  /* NOBUF: Fin + 32 bits Stream ID + 62 bits Offset + Data Length */
  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.fin = 1;
  fr.stream.stream_id = 0xf1f2f3f4u;
  fr.stream.offset = 0x31f2f3f4f5f6f7f8llu;
  fr.stream.datalen = strsize(data);
  fr.stream.data = data;

  framelen = 1 + 8 + 8 + 1 + 17;

  rv = ngtcp2_pkt_encode_stream_frame(buf, framelen - 1, &fr.stream);

  CU_ASSERT(NGTCP2_ERR_NOBUF == rv);
}

void test_ngtcp2_pkt_encode_ack_frame(void) {
  uint8_t buf[256];
  ngtcp2_max_frame mfr, nmfr;
  ngtcp2_frame *fr = &mfr.fr, *nfr = &nmfr.fr;
  ssize_t rv;
  size_t framelen;
  size_t i;
  ngtcp2_ack_blk *blks;

  /* 0 Num Blocks */
  fr->type = NGTCP2_FRAME_ACK;
  fr->ack.largest_ack = 0xf1f2f3f4llu;
  fr->ack.first_ack_blklen = 0;
  fr->ack.ack_delay = 0;
  fr->ack.num_blks = 0;

  framelen = 1 + 8 + 1 + 1 + 1;

  rv = ngtcp2_pkt_encode_ack_frame(buf, sizeof(buf), &fr->ack);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_ack_frame(&nfr->ack, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr->type == nfr->type);
  CU_ASSERT(fr->ack.largest_ack == nfr->ack.largest_ack);
  CU_ASSERT(fr->ack.ack_delay == nfr->ack.ack_delay);
  CU_ASSERT(fr->ack.num_blks == nfr->ack.num_blks);

  memset(&nmfr, 0, sizeof(nmfr));

  /* 2 Num Blocks */
  fr->type = NGTCP2_FRAME_ACK;
  fr->ack.largest_ack = 0xf1f2f3f4llu;
  fr->ack.first_ack_blklen = 0xe1e2e3e4llu;
  fr->ack.ack_delay = 0xf1f2;
  fr->ack.num_blks = 2;
  blks = fr->ack.blks;
  blks[0].gap = 255;
  blks[0].blklen = 0xd1d2d3d4llu;
  blks[1].gap = 1;
  blks[1].blklen = 0xd1d2d3d4llu;

  framelen = 1 + 8 + 4 + 1 + 8 + (2 + 8) + (1 + 8);

  rv = ngtcp2_pkt_encode_ack_frame(buf, sizeof(buf), &fr->ack);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_ack_frame(&nfr->ack, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr->type == nfr->type);
  CU_ASSERT(fr->ack.largest_ack == nfr->ack.largest_ack);
  CU_ASSERT(fr->ack.ack_delay == nfr->ack.ack_delay);
  CU_ASSERT(fr->ack.num_blks == nfr->ack.num_blks);

  for (i = 0; i < fr->ack.num_blks; ++i) {
    CU_ASSERT(fr->ack.blks[i].gap == nfr->ack.blks[i].gap);
    CU_ASSERT(fr->ack.blks[i].blklen == nfr->ack.blks[i].blklen);
  }

  memset(&nmfr, 0, sizeof(nmfr));
}

void test_ngtcp2_pkt_encode_rst_stream_frame(void) {
  uint8_t buf[32];
  ngtcp2_rst_stream fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 4 + 2 + 8;

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.stream_id = 1000000007;
  fr.app_error_code = 0xe1e2;
  fr.final_offset = 0x31f2f3f4f5f6f7f8llu;

  rv = ngtcp2_pkt_encode_rst_stream_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_rst_stream_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.stream_id == nfr.stream_id);
  CU_ASSERT(fr.app_error_code == nfr.app_error_code);
  CU_ASSERT(fr.final_offset == nfr.final_offset);
}

void test_ngtcp2_pkt_encode_connection_close_frame(void) {
  uint8_t buf[2048];
  ngtcp2_frame fr, nfr;
  ssize_t rv;
  size_t framelen;
  uint8_t reason[1024];

  memset(reason, 0xfa, sizeof(reason));

  /* no Reason Phrase */
  fr.type = NGTCP2_FRAME_CONNECTION_CLOSE;
  fr.connection_close.error_code = 0xf1f2u;
  fr.connection_close.frame_type = 255;
  fr.connection_close.reasonlen = 0;
  fr.connection_close.reason = NULL;

  framelen = 1 + 2 + 2 + 1;

  rv = ngtcp2_pkt_encode_connection_close_frame(buf, sizeof(buf),
                                                &fr.connection_close);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_connection_close_frame(&nfr.connection_close, buf,
                                                framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.connection_close.error_code == nfr.connection_close.error_code);
  CU_ASSERT(fr.connection_close.reasonlen == nfr.connection_close.reasonlen);
  CU_ASSERT(fr.connection_close.reason == nfr.connection_close.reason);

  memset(&nfr, 0, sizeof(nfr));

  /* 1024 bytes Reason Phrase */
  fr.type = NGTCP2_FRAME_CONNECTION_CLOSE;
  fr.connection_close.error_code = 0xf3f4u;
  fr.connection_close.frame_type = 0;
  fr.connection_close.reasonlen = sizeof(reason);
  fr.connection_close.reason = reason;

  framelen = 1 + 2 + 1 + 2 + sizeof(reason);

  rv = ngtcp2_pkt_encode_connection_close_frame(buf, sizeof(buf),
                                                &fr.connection_close);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_connection_close_frame(&nfr.connection_close, buf,
                                                framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.connection_close.error_code == nfr.connection_close.error_code);
  CU_ASSERT(fr.connection_close.reasonlen == nfr.connection_close.reasonlen);
  CU_ASSERT(0 == memcmp(reason, nfr.connection_close.reason, sizeof(reason)));

  memset(&nfr, 0, sizeof(nfr));
}

void test_ngtcp2_pkt_encode_application_close_frame(void) {
  uint8_t buf[2048];
  ngtcp2_frame fr, nfr;
  ssize_t rv;
  size_t framelen;
  uint8_t reason[1024];

  memset(reason, 0xfa, sizeof(reason));

  /* no Reason Phrase */
  fr.type = NGTCP2_FRAME_APPLICATION_CLOSE;
  fr.application_close.app_error_code = 0xf1f2u;
  fr.application_close.reasonlen = 0;
  fr.application_close.reason = NULL;

  framelen = 1 + 2 + 1;

  rv = ngtcp2_pkt_encode_application_close_frame(buf, sizeof(buf),
                                                 &fr.application_close);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_application_close_frame(&nfr.application_close, buf,
                                                 framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.application_close.app_error_code ==
            nfr.application_close.app_error_code);
  CU_ASSERT(fr.application_close.reasonlen == nfr.application_close.reasonlen);
  CU_ASSERT(fr.application_close.reason == nfr.application_close.reason);

  memset(&nfr, 0, sizeof(nfr));

  /* 1024 bytes Reason Phrase */
  fr.type = NGTCP2_FRAME_APPLICATION_CLOSE;
  fr.application_close.app_error_code = 0xf3f4u;
  fr.application_close.reasonlen = sizeof(reason);
  fr.application_close.reason = reason;

  framelen = 1 + 2 + 2 + sizeof(reason);

  rv = ngtcp2_pkt_encode_application_close_frame(buf, sizeof(buf),
                                                 &fr.application_close);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_application_close_frame(&nfr.application_close, buf,
                                                 framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.application_close.app_error_code ==
            nfr.application_close.app_error_code);
  CU_ASSERT(fr.application_close.reasonlen == nfr.application_close.reasonlen);
  CU_ASSERT(0 == memcmp(reason, nfr.application_close.reason, sizeof(reason)));

  memset(&nfr, 0, sizeof(nfr));
}

void test_ngtcp2_pkt_encode_max_data_frame(void) {
  uint8_t buf[16];
  ngtcp2_max_data fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8;

  fr.type = NGTCP2_FRAME_MAX_DATA;
  fr.max_data = 0x31f2f3f4f5f6f7f8llu;

  rv = ngtcp2_pkt_encode_max_data_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_max_data_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.max_data == nfr.max_data);
}

void test_ngtcp2_pkt_encode_max_stream_data_frame(void) {
  uint8_t buf[17];
  ngtcp2_max_stream_data fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8 + 8;

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.stream_id = 0xf1f2f3f4u;
  fr.max_stream_data = 0x35f6f7f8f9fafbfcllu;

  rv = ngtcp2_pkt_encode_max_stream_data_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_max_stream_data_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.stream_id == nfr.stream_id);
  CU_ASSERT(fr.max_stream_data == nfr.max_stream_data);
}

void test_ngtcp2_pkt_encode_max_stream_id_frame(void) {
  uint8_t buf[16];
  ngtcp2_max_stream_id fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8;

  fr.type = NGTCP2_FRAME_MAX_STREAM_ID;
  fr.max_stream_id = 0xf1f2f3f4u;

  rv = ngtcp2_pkt_encode_max_stream_id_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_max_stream_id_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.max_stream_id == nfr.max_stream_id);
}

void test_ngtcp2_pkt_encode_ping_frame(void) {
  uint8_t buf[3];
  ngtcp2_ping fr, nfr;
  ssize_t rv;
  size_t framelen;

  fr.type = NGTCP2_FRAME_PING;

  framelen = 1;

  rv = ngtcp2_pkt_encode_ping_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_ping_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
}

void test_ngtcp2_pkt_encode_blocked_frame(void) {
  uint8_t buf[9];
  ngtcp2_blocked fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8;

  fr.type = NGTCP2_FRAME_BLOCKED;
  fr.offset = 0x31f2f3f4f5f6f7f8llu;

  rv = ngtcp2_pkt_encode_blocked_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_blocked_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.offset == nfr.offset);
}

void test_ngtcp2_pkt_encode_stream_blocked_frame(void) {
  uint8_t buf[17];
  ngtcp2_stream_blocked fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8 + 8;

  fr.type = NGTCP2_FRAME_STREAM_BLOCKED;
  fr.stream_id = 0xf1f2f3f4u;
  fr.offset = 0x35f6f7f8f9fafbfcllu;

  rv = ngtcp2_pkt_encode_stream_blocked_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_stream_blocked_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.stream_id == nfr.stream_id);
  CU_ASSERT(fr.offset == nfr.offset);
}

void test_ngtcp2_pkt_encode_stream_id_blocked_frame(void) {
  uint8_t buf[9];
  ngtcp2_stream_id_blocked fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8;

  fr.type = NGTCP2_FRAME_STREAM_ID_BLOCKED;
  fr.stream_id = 0xf1f2f3f4u;

  rv = ngtcp2_pkt_encode_stream_id_blocked_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_stream_id_blocked_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.stream_id == nfr.stream_id);
}

void test_ngtcp2_pkt_encode_new_connection_id_frame(void) {
  uint8_t buf[256];
  ngtcp2_new_connection_id fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 4 + 1 + 18 + NGTCP2_STATELESS_RESET_TOKENLEN;

  fr.type = NGTCP2_FRAME_NEW_CONNECTION_ID;
  fr.seq = 0xf1f2;
  scid_init(&fr.cid);
  memset(fr.stateless_reset_token, 0xe1, sizeof(fr.stateless_reset_token));

  rv = ngtcp2_pkt_encode_new_connection_id_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_new_connection_id_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.seq = nfr.seq);
  CU_ASSERT(ngtcp2_cid_eq(&fr.cid, &nfr.cid));
  CU_ASSERT(0 == memcmp(fr.stateless_reset_token, nfr.stateless_reset_token,
                        sizeof(fr.stateless_reset_token)));
}

void test_ngtcp2_pkt_encode_stop_sending_frame(void) {
  uint8_t buf[16];
  ngtcp2_stop_sending fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8 + 2;

  fr.type = NGTCP2_FRAME_STOP_SENDING;
  fr.stream_id = 0xf1f2f3f4u;
  fr.app_error_code = 0xe1e2u;

  rv = ngtcp2_pkt_encode_stop_sending_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_stop_sending_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.stream_id == nfr.stream_id);
  CU_ASSERT(fr.app_error_code == nfr.app_error_code);
}

void test_ngtcp2_pkt_encode_path_challenge_frame(void) {
  uint8_t buf[9];
  ngtcp2_path_challenge fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8;
  size_t i;

  fr.type = NGTCP2_FRAME_PATH_CHALLENGE;
  for (i = 0; i < sizeof(fr.data); ++i) {
    fr.data[i] = (uint8_t)(i + 1);
  }

  rv = ngtcp2_pkt_encode_path_challenge_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_path_challenge_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(0 == memcmp(fr.data, nfr.data, sizeof(fr.data)));
}

void test_ngtcp2_pkt_encode_path_response_frame(void) {
  uint8_t buf[9];
  ngtcp2_path_response fr, nfr;
  ssize_t rv;
  size_t framelen = 1 + 8;
  size_t i;

  fr.type = NGTCP2_FRAME_PATH_RESPONSE;
  for (i = 0; i < sizeof(fr.data); ++i) {
    fr.data[i] = (uint8_t)(i + 1);
  }

  rv = ngtcp2_pkt_encode_path_response_frame(buf, sizeof(buf), &fr);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_path_response_frame(&nfr, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(0 == memcmp(fr.data, nfr.data, sizeof(fr.data)));
}

void test_ngtcp2_pkt_encode_crypto_frame(void) {
  const uint8_t data[] = "0123456789abcdef1";
  uint8_t buf[256];
  ngtcp2_frame fr, nfr;
  ssize_t rv;
  size_t framelen;

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0x31f2f3f4f5f6f7f8llu;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = strsize(data);
  fr.crypto.data[0].base = (uint8_t *)data;

  framelen = 1 + 8 + 1 + 17;

  rv = ngtcp2_pkt_encode_crypto_frame(buf, sizeof(buf), &fr.crypto);

  CU_ASSERT((ssize_t)framelen == rv);

  rv = ngtcp2_pkt_decode_crypto_frame(&nfr.crypto, buf, framelen);

  CU_ASSERT((ssize_t)framelen == rv);
  CU_ASSERT(fr.type == nfr.type);
  CU_ASSERT(fr.crypto.offset == nfr.crypto.offset);
  CU_ASSERT(fr.crypto.datacnt == nfr.crypto.datacnt);
  CU_ASSERT(fr.crypto.data[0].len == nfr.crypto.data[0].len);
  CU_ASSERT(0 == memcmp(fr.crypto.data[0].base, nfr.crypto.data[0].base,
                        fr.crypto.data[0].len));
}

void test_ngtcp2_pkt_adjust_pkt_num(void) {
  CU_ASSERT(0xaa831f94llu ==
            ngtcp2_pkt_adjust_pkt_num(0xaa82f30ellu, 0x1f94, 16));

  CU_ASSERT(0x01ff == ngtcp2_pkt_adjust_pkt_num(0x0100, 0xff, 8));
  CU_ASSERT(0x02ff == ngtcp2_pkt_adjust_pkt_num(0x01ff, 0xff, 8));

  CU_ASSERT(0x3fffffffffffffabllu ==
            ngtcp2_pkt_adjust_pkt_num(NGTCP2_MAX_PKT_NUM, 0xab, 8));
}

void test_ngtcp2_pkt_validate_ack(void) {
  int rv;
  ngtcp2_ack fr;

  /* too long first_ack_blklen */
  fr.largest_ack = 1;
  fr.first_ack_blklen = 2;
  fr.num_blks = 0;

  rv = ngtcp2_pkt_validate_ack(&fr);

  CU_ASSERT(NGTCP2_ERR_ACK_FRAME == rv);

  /* gap is too large */
  fr.largest_ack = 250;
  fr.first_ack_blklen = 1;
  fr.num_blks = 1;
  fr.blks[0].gap = 248;
  fr.blks[0].blklen = 0;

  rv = ngtcp2_pkt_validate_ack(&fr);

  CU_ASSERT(NGTCP2_ERR_ACK_FRAME == rv);

  /* too large blklen */
  fr.largest_ack = 250;
  fr.first_ack_blklen = 0;
  fr.num_blks = 1;
  fr.blks[0].gap = 248;
  fr.blks[0].blklen = 1;

  rv = ngtcp2_pkt_validate_ack(&fr);

  CU_ASSERT(NGTCP2_ERR_ACK_FRAME == rv);
}

void test_ngtcp2_pkt_write_stateless_reset(void) {
  uint8_t buf[256];
  ssize_t spktlen;
  ngtcp2_cid dcid;
  uint8_t token[NGTCP2_STATELESS_RESET_TOKENLEN];
  uint8_t rand[256];
  size_t i;
  uint8_t *p;
  ngtcp2_pkt_hd hd;
  size_t randlen;

  dcid.datalen = NGTCP2_MAX_CIDLEN;
  for (i = 0; i < NGTCP2_MAX_CIDLEN; ++i) {
    dcid.data[i] = 0xff;
  }

  memset(rand, 0, sizeof(rand));
  for (i = 0; i < NGTCP2_STATELESS_RESET_TOKENLEN; ++i) {
    token[i] = (uint8_t)(i + 1);
  }

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_KEY_PHASE, NGTCP2_PKT_SHORT, &dcid,
                     NULL, 0xf1, 1, 0, 0);
  spktlen = ngtcp2_pkt_write_stateless_reset(buf, sizeof(buf), &hd, token, rand,
                                             sizeof(rand));

  p = buf;

  CU_ASSERT(256 == spktlen);
  CU_ASSERT((NGTCP2_KEY_PHASE_BIT | NGTCP2_THIRD_BIT | NGTCP2_FOURTH_BIT) ==
            *p);

  ++p;

  CU_ASSERT(0 == memcmp(dcid.data, p, dcid.datalen));

  p += dcid.datalen;

  CU_ASSERT((0xf1 & 0x7f) == *p);

  ++p;

  randlen = (size_t)(spktlen - (p - buf) - NGTCP2_STATELESS_RESET_TOKENLEN);

  CU_ASSERT(0 == memcmp(rand, p, randlen));

  p += randlen;

  CU_ASSERT(0 == memcmp(token, p, NGTCP2_STATELESS_RESET_TOKENLEN));

  p += NGTCP2_STATELESS_RESET_TOKENLEN;

  CU_ASSERT(spktlen == p - buf);

  /* Not enough buffer */
  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     0xf1, 2, 0, 0);
  spktlen = ngtcp2_pkt_write_stateless_reset(
      buf, 1 + dcid.datalen + 2 + NGTCP2_STATELESS_RESET_TOKENLEN - 1, &hd,
      token, rand, sizeof(rand));

  CU_ASSERT(NGTCP2_ERR_NOBUF == spktlen);
}

void test_ngtcp2_pkt_write_version_negotiation(void) {
  uint8_t buf[256];
  ssize_t spktlen;
  const uint32_t sv[] = {0xf1f2f3f4, 0x1f2f3f4f};
  uint8_t *p;
  size_t i;
  ngtcp2_cid dcid, scid;

  dcid_init(&dcid);
  scid_init(&scid);

  spktlen = ngtcp2_pkt_write_version_negotiation(buf, sizeof(buf), 133, &dcid,
                                                 &scid, sv, arraylen(sv));

  CU_ASSERT((ssize_t)(1 + 4 + 1 + dcid.datalen + scid.datalen +
                      arraylen(sv) * 4) == spktlen);

  p = buf;

  CU_ASSERT((0x80 | 133) == buf[0]);

  ++p;

  CU_ASSERT(0 == ngtcp2_get_uint32(p));

  p += sizeof(uint32_t);

  CU_ASSERT(dcid.datalen == (size_t)((*p >> 4) + 3));
  CU_ASSERT(scid.datalen == (size_t)((*p & 0xf) + 3));

  ++p;

  CU_ASSERT(0 == memcmp(dcid.data, p, dcid.datalen));

  p += dcid.datalen;

  CU_ASSERT(0 == memcmp(scid.data, p, scid.datalen));

  p += scid.datalen;

  for (i = 0; i < arraylen(sv); ++i, p += 4) {
    CU_ASSERT(sv[i] == ngtcp2_get_uint32(p));
  }
}
