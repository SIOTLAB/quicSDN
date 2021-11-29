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
#include "ngtcp2_conn_test.h"

#include <assert.h>

#include <CUnit/CUnit.h>

#include "ngtcp2_conn.h"
#include "ngtcp2_test_helper.h"
#include "ngtcp2_mem.h"
#include "ngtcp2_pkt.h"
#include "ngtcp2_cid.h"
#include "ngtcp2_conv.h"

static ssize_t null_encrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                            const uint8_t *plaintext, size_t plaintextlen,
                            const uint8_t *key, size_t keylen,
                            const uint8_t *nonce, size_t noncelen,
                            const uint8_t *ad, size_t adlen, void *user_data) {
  (void)conn;
  (void)dest;
  (void)destlen;
  (void)plaintext;
  (void)key;
  (void)keylen;
  (void)nonce;
  (void)noncelen;
  (void)ad;
  (void)adlen;
  (void)user_data;
  return (ssize_t)plaintextlen + NGTCP2_FAKE_AEAD_OVERHEAD;
}

static ssize_t null_decrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                            const uint8_t *ciphertext, size_t ciphertextlen,
                            const uint8_t *key, size_t keylen,
                            const uint8_t *nonce, size_t noncelen,
                            const uint8_t *ad, size_t adlen, void *user_data) {
  (void)conn;
  (void)dest;
  (void)destlen;
  (void)ciphertext;
  (void)key;
  (void)keylen;
  (void)nonce;
  (void)noncelen;
  (void)ad;
  (void)adlen;
  (void)user_data;
  assert(destlen >= ciphertextlen);
  assert(ciphertextlen >= NGTCP2_FAKE_AEAD_OVERHEAD);
  memmove(dest, ciphertext, ciphertextlen - NGTCP2_FAKE_AEAD_OVERHEAD);
  return (ssize_t)ciphertextlen - NGTCP2_FAKE_AEAD_OVERHEAD;
}

static ssize_t fail_decrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                            const uint8_t *ciphertext, size_t ciphertextlen,
                            const uint8_t *key, size_t keylen,
                            const uint8_t *nonce, size_t noncelen,
                            const uint8_t *ad, size_t adlen, void *user_data) {
  (void)conn;
  (void)dest;
  (void)destlen;
  (void)ciphertext;
  (void)ciphertextlen;
  (void)key;
  (void)keylen;
  (void)nonce;
  (void)noncelen;
  (void)ad;
  (void)adlen;
  (void)user_data;
  return NGTCP2_ERR_TLS_DECRYPT;
}

static ssize_t null_encrypt_pn(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                               const uint8_t *ciphertext, size_t ciphertextlen,
                               const uint8_t *key, size_t keylen,
                               const uint8_t *nonce, size_t noncelen,
                               void *user_data) {
  (void)conn;
  (void)dest;
  (void)destlen;
  (void)ciphertext;
  (void)key;
  (void)keylen;
  (void)nonce;
  (void)noncelen;
  (void)user_data;
  assert(destlen >= ciphertextlen);
  memmove(dest, ciphertext, ciphertextlen);
  return (ssize_t)ciphertextlen;
}

static uint8_t null_key[16];
static uint8_t null_iv[16];
static uint8_t null_pn[16];
static uint8_t null_data[4096];

typedef struct {
  uint64_t pkt_num;
  /* stream_data is intended to store the arguments passed in
     recv_stream_data callback. */
  struct {
    uint64_t stream_id;
    uint8_t fin;
    size_t datalen;
  } stream_data;
} my_user_data;

static int client_initial(ngtcp2_conn *conn, void *user_data) {
  (void)user_data;

  ngtcp2_conn_submit_crypto_data(conn, null_data, 217);

  return 0;
}

static int client_initial_early_data(ngtcp2_conn *conn, void *user_data) {
  (void)user_data;

  ngtcp2_conn_submit_crypto_data(conn, null_data, 217);

  ngtcp2_conn_set_early_keys(conn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));

  return 0;
}

static int recv_client_initial(ngtcp2_conn *conn, const ngtcp2_cid *dcid,
                               void *user_data) {
  (void)conn;
  (void)dcid;
  (void)user_data;
  return 0;
}

static int recv_crypto_data(ngtcp2_conn *conn, uint64_t offset,
                            const uint8_t *data, size_t datalen,
                            void *user_data) {
  (void)conn;
  (void)offset;
  (void)data;
  (void)datalen;
  (void)user_data;
  return 0;
}

static int recv_crypto_data_server_early_data(ngtcp2_conn *conn,
                                              uint64_t offset,
                                              const uint8_t *data,
                                              size_t datalen, void *user_data) {
  (void)offset;
  (void)data;
  (void)datalen;
  (void)user_data;

  assert(conn->server);

  ngtcp2_conn_submit_crypto_data(conn, null_data, 179);

  ngtcp2_conn_update_tx_keys(conn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_update_rx_keys(conn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));

  conn->callbacks.recv_crypto_data = recv_crypto_data;

  return 0;
}

static int recv_crypto_handshake_error(ngtcp2_conn *conn, uint64_t offset,
                                       const uint8_t *data, size_t datalen,
                                       void *user_data) {
  (void)conn;
  (void)offset;
  (void)data;
  (void)datalen;
  (void)user_data;
  return NGTCP2_ERR_CRYPTO;
}

static int recv_crypto_fatal_alert_generated(ngtcp2_conn *conn, uint64_t offset,
                                             const uint8_t *data,
                                             size_t datalen, void *user_data) {
  (void)conn;
  (void)offset;
  (void)data;
  (void)datalen;
  (void)user_data;
  return NGTCP2_ERR_CRYPTO;
}

static int recv_crypto_data_server(ngtcp2_conn *conn, uint64_t offset,
                                   const uint8_t *data, size_t datalen,
                                   void *user_data) {
  (void)offset;
  (void)data;
  (void)datalen;
  (void)user_data;

  ngtcp2_conn_submit_crypto_data(conn, null_data, 218);

  return 0;
}

static int recv_stream_data(ngtcp2_conn *conn, uint64_t stream_id, uint8_t fin,
                            uint64_t offset, const uint8_t *data,
                            size_t datalen, void *user_data,
                            void *stream_user_data) {
  my_user_data *ud = user_data;
  (void)conn;
  (void)offset;
  (void)data;
  (void)stream_user_data;

  if (ud) {
    ud->stream_data.stream_id = stream_id;
    ud->stream_data.fin = fin;
    ud->stream_data.datalen = datalen;
  }

  return 0;
}

static int genrand(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                   ngtcp2_rand_ctx ctx, void *user_data) {
  (void)conn;
  (void)ctx;
  (void)user_data;

  memset(dest, 0, destlen);

  return 0;
}

static void server_default_settings(ngtcp2_settings *settings) {
  size_t i;

  settings->log_printf = NULL;
  settings->initial_ts = 0;
  settings->max_stream_data = 65535;
  settings->max_data = 128 * 1024;
  settings->max_bidi_streams = 3;
  settings->max_uni_streams = 2;
  settings->idle_timeout = 60;
  settings->max_packet_size = 65535;
  settings->stateless_reset_token_present = 1;
  for (i = 0; i < NGTCP2_STATELESS_RESET_TOKENLEN; ++i) {
    settings->stateless_reset_token[i] = (uint8_t)i;
  }
}

static void client_default_settings(ngtcp2_settings *settings) {
  settings->log_printf = NULL;
  settings->initial_ts = 0;
  settings->max_stream_data = 65535;
  settings->max_data = 128 * 1024;
  settings->max_bidi_streams = 0;
  settings->max_uni_streams = 2;
  settings->idle_timeout = 60;
  settings->max_packet_size = 65535;
  settings->stateless_reset_token_present = 0;
}

static void setup_default_server(ngtcp2_conn **pconn) {
  ngtcp2_conn_callbacks cb;
  ngtcp2_settings settings;
  ngtcp2_cid dcid, scid;

  dcid_init(&dcid);
  scid_init(&scid);

  memset(&cb, 0, sizeof(cb));
  cb.in_decrypt = null_decrypt;
  cb.in_encrypt = null_encrypt;
  cb.in_encrypt_pn = null_encrypt_pn;
  cb.decrypt = null_decrypt;
  cb.encrypt = null_encrypt;
  cb.encrypt_pn = null_encrypt_pn;
  cb.recv_crypto_data = recv_crypto_data;
  server_default_settings(&settings);

  ngtcp2_conn_server_new(pconn, &dcid, &scid, NGTCP2_PROTO_VER_MAX, &cb,
                         &settings, NULL);
  ngtcp2_conn_set_handshake_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                    sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_handshake_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                    sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_update_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_update_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_aead_overhead(*pconn, NGTCP2_FAKE_AEAD_OVERHEAD);
  (*pconn)->state = NGTCP2_CS_POST_HANDSHAKE;
  (*pconn)->remote_settings.max_stream_data = 64 * 1024;
  (*pconn)->remote_settings.max_bidi_streams = 0;
  (*pconn)->remote_settings.max_uni_streams = 1;
  (*pconn)->remote_settings.max_data = 64 * 1024;
  (*pconn)->max_local_stream_id_bidi =
      ngtcp2_nth_server_bidi_id((*pconn)->remote_settings.max_bidi_streams);
  (*pconn)->max_local_stream_id_uni =
      ngtcp2_nth_server_uni_id((*pconn)->remote_settings.max_uni_streams);
  (*pconn)->max_tx_offset = (*pconn)->remote_settings.max_data;
}

static void setup_default_client(ngtcp2_conn **pconn) {
  ngtcp2_conn_callbacks cb;
  ngtcp2_settings settings;
  ngtcp2_cid dcid, scid;

  dcid_init(&dcid);
  scid_init(&scid);

  memset(&cb, 0, sizeof(cb));
  cb.in_decrypt = null_decrypt;
  cb.in_encrypt = null_encrypt;
  cb.in_encrypt_pn = null_encrypt_pn;
  cb.decrypt = null_decrypt;
  cb.encrypt = null_encrypt;
  cb.encrypt_pn = null_encrypt_pn;
  cb.recv_crypto_data = recv_crypto_data;
  client_default_settings(&settings);

  ngtcp2_conn_client_new(pconn, &dcid, &scid, NGTCP2_PROTO_VER_MAX, &cb,
                         &settings, NULL);
  ngtcp2_conn_set_handshake_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                    sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_handshake_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                    sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_update_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_update_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_aead_overhead(*pconn, NGTCP2_FAKE_AEAD_OVERHEAD);
  (*pconn)->state = NGTCP2_CS_POST_HANDSHAKE;
  (*pconn)->remote_settings.max_stream_data = 64 * 1024;
  (*pconn)->remote_settings.max_bidi_streams = 1;
  (*pconn)->remote_settings.max_uni_streams = 1;
  (*pconn)->remote_settings.max_data = 64 * 1024;
  (*pconn)->max_local_stream_id_bidi =
      ngtcp2_nth_client_bidi_id((*pconn)->remote_settings.max_bidi_streams);
  (*pconn)->max_local_stream_id_uni =
      ngtcp2_nth_client_uni_id((*pconn)->remote_settings.max_uni_streams);
  (*pconn)->max_tx_offset = (*pconn)->remote_settings.max_data;
}

static void setup_handshake_server(ngtcp2_conn **pconn) {
  ngtcp2_conn_callbacks cb;
  ngtcp2_settings settings;
  ngtcp2_cid dcid, scid;

  dcid_init(&dcid);
  scid_init(&scid);

  memset(&cb, 0, sizeof(cb));
  cb.recv_client_initial = recv_client_initial;
  cb.recv_crypto_data = recv_crypto_data_server;
  cb.in_decrypt = null_decrypt;
  cb.in_encrypt = null_encrypt;
  cb.in_encrypt_pn = null_encrypt_pn;
  cb.decrypt = null_decrypt;
  cb.encrypt = null_encrypt;
  cb.encrypt_pn = null_encrypt_pn;
  cb.rand = genrand;
  server_default_settings(&settings);

  ngtcp2_conn_server_new(pconn, &dcid, &scid, NGTCP2_PROTO_VER_MAX, &cb,
                         &settings, NULL);
  ngtcp2_conn_set_initial_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_initial_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_handshake_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                    sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_handshake_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                    sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_aead_overhead(*pconn, NGTCP2_FAKE_AEAD_OVERHEAD);
}

static void setup_handshake_client(ngtcp2_conn **pconn) {
  ngtcp2_conn_callbacks cb;
  ngtcp2_settings settings;
  ngtcp2_cid rcid, scid;

  rcid_init(&rcid);
  scid_init(&scid);

  memset(&cb, 0, sizeof(cb));
  cb.client_initial = client_initial;
  cb.recv_crypto_data = recv_crypto_data;
  cb.in_decrypt = null_decrypt;
  cb.in_encrypt = null_encrypt;
  cb.in_encrypt_pn = null_encrypt_pn;
  client_default_settings(&settings);

  ngtcp2_conn_client_new(pconn, &rcid, &scid, NGTCP2_PROTO_VER_MAX, &cb,
                         &settings, NULL);
  ngtcp2_conn_set_initial_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_initial_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
}

static void setup_early_server(ngtcp2_conn **pconn) {
  ngtcp2_conn_callbacks cb;
  ngtcp2_settings settings;
  ngtcp2_cid dcid, scid;

  dcid_init(&dcid);
  scid_init(&scid);

  memset(&cb, 0, sizeof(cb));
  cb.recv_client_initial = recv_client_initial;
  cb.recv_crypto_data = recv_crypto_data_server_early_data;
  cb.in_decrypt = null_decrypt;
  cb.in_encrypt = null_encrypt;
  cb.in_encrypt_pn = null_encrypt_pn;
  cb.decrypt = null_decrypt;
  cb.encrypt = null_encrypt;
  cb.encrypt_pn = null_encrypt_pn;
  cb.rand = genrand;
  server_default_settings(&settings);

  ngtcp2_conn_server_new(pconn, &dcid, &scid, NGTCP2_PROTO_VER_MAX, &cb,
                         &settings, NULL);
  ngtcp2_conn_set_initial_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_initial_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_early_keys(*pconn, null_key, sizeof(null_key), null_iv,
                             sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_aead_overhead(*pconn, NGTCP2_FAKE_AEAD_OVERHEAD);
  (*pconn)->remote_settings.max_stream_data = 64 * 1024;
  (*pconn)->remote_settings.max_bidi_streams = 0;
  (*pconn)->remote_settings.max_uni_streams = 1;
  (*pconn)->remote_settings.max_data = 64 * 1024;
  (*pconn)->max_local_stream_id_bidi =
      ngtcp2_nth_server_bidi_id((*pconn)->remote_settings.max_bidi_streams);
  (*pconn)->max_local_stream_id_uni =
      ngtcp2_nth_server_uni_id((*pconn)->remote_settings.max_uni_streams);
  (*pconn)->max_tx_offset = (*pconn)->remote_settings.max_data;
}

static void setup_early_client(ngtcp2_conn **pconn) {
  ngtcp2_conn_callbacks cb;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  ngtcp2_cid dcid, scid;

  dcid_init(&dcid);
  scid_init(&scid);

  memset(&cb, 0, sizeof(cb));
  cb.client_initial = client_initial_early_data;
  cb.recv_crypto_data = recv_crypto_data;
  cb.in_decrypt = null_decrypt;
  cb.in_encrypt = null_encrypt;
  cb.in_encrypt_pn = null_encrypt_pn;
  cb.decrypt = null_decrypt;
  cb.encrypt = null_encrypt;
  cb.encrypt_pn = null_encrypt_pn;
  client_default_settings(&settings);

  ngtcp2_conn_client_new(pconn, &dcid, &scid, NGTCP2_PROTO_VER_MAX, &cb,
                         &settings, NULL);
  ngtcp2_conn_set_initial_tx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_initial_rx_keys(*pconn, null_key, sizeof(null_key), null_iv,
                                  sizeof(null_iv), null_pn, sizeof(null_pn));
  ngtcp2_conn_set_aead_overhead(*pconn, NGTCP2_FAKE_AEAD_OVERHEAD);

  params.initial_max_stream_data = 64 * 1024;
  params.initial_max_bidi_streams = 1;
  params.initial_max_uni_streams = 1;
  params.initial_max_data = 64 * 1024;

  ngtcp2_conn_set_early_remote_transport_params(*pconn, &params);
}

void test_ngtcp2_conn_stream_open_close(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  int rv;
  ngtcp2_frame fr;
  ngtcp2_strm *strm;
  uint64_t stream_id;

  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 17;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NGTCP2_STRM_FLAG_NONE == strm->flags);

  fr.stream.fin = 1;
  fr.stream.offset = 17;
  fr.stream.datalen = 0;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NGTCP2_STRM_FLAG_SHUT_RD == strm->flags);
  CU_ASSERT(fr.stream.offset == strm->last_rx_offset);
  CU_ASSERT(fr.stream.offset == ngtcp2_strm_rx_offset(strm));

  spktlen =
      ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, 4, 1, NULL, 0, 3);

  CU_ASSERT(spktlen > 0);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL != strm);

  /* Open a remote unidirectional stream */
  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 2;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 19;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 3, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, 3);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, 2);

  CU_ASSERT(NGTCP2_STRM_FLAG_SHUT_WR == strm->flags);
  CU_ASSERT(fr.stream.datalen == strm->last_rx_offset);
  CU_ASSERT(fr.stream.datalen == ngtcp2_strm_rx_offset(strm));

  /* Open a local unidirectional stream */
  rv = ngtcp2_conn_open_uni_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);
  CU_ASSERT(3 == stream_id);

  rv = ngtcp2_conn_open_uni_stream(conn, &stream_id, NULL);

  CU_ASSERT(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_stream_rx_flow_control(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  int rv;
  ngtcp2_frame fr;
  ngtcp2_strm *strm;
  size_t i;

  setup_default_server(&conn);

  conn->local_settings.max_stream_data = 2047;

  for (i = 0; i < 3; ++i) {
    uint64_t stream_id = i * 4;
    fr.type = NGTCP2_FRAME_STREAM;
    fr.stream.flags = 0;
    fr.stream.stream_id = stream_id;
    fr.stream.fin = 0;
    fr.stream.offset = 0;
    fr.stream.datalen = 1024;
    fr.stream.data = null_data;

    pktlen =
        write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, i, &fr);
    rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

    CU_ASSERT(0 == rv);

    strm = ngtcp2_conn_find_stream(conn, stream_id);

    CU_ASSERT(NULL != strm);

    rv = ngtcp2_conn_extend_max_stream_offset(conn, stream_id,
                                              fr.stream.datalen);

    CU_ASSERT(0 == rv);
  }

  strm = conn->fc_strms;

  CU_ASSERT(8 == strm->stream_id);

  strm = strm->fc_next;

  CU_ASSERT(4 == strm->stream_id);

  strm = strm->fc_next;

  CU_ASSERT(0 == strm->stream_id);

  strm = strm->fc_next;

  CU_ASSERT(NULL == strm);

  spktlen = ngtcp2_conn_write_pkt(conn, buf, sizeof(buf), 2);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(NULL == conn->fc_strms);

  for (i = 0; i < 3; ++i) {
    uint64_t stream_id = i * 4;
    strm = ngtcp2_conn_find_stream(conn, stream_id);

    CU_ASSERT(2047 + 1024 == strm->max_rx_offset);
  }

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_stream_rx_flow_control_error(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  int rv;
  ngtcp2_frame fr;

  setup_default_server(&conn);

  conn->local_settings.max_stream_data = 1023;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 1024;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_FLOW_CONTROL == rv);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_stream_tx_flow_control(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  int rv;
  ngtcp2_frame fr;
  ngtcp2_strm *strm;
  ssize_t nwrite;
  uint64_t stream_id;

  setup_default_client(&conn);

  conn->remote_settings.max_stream_data = 2047;

  rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(1024 == nwrite);
  CU_ASSERT(1024 == strm->tx_offset);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 2);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(1023 == nwrite);
  CU_ASSERT(2047 == strm->tx_offset);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 3);

  CU_ASSERT(NGTCP2_ERR_STREAM_DATA_BLOCKED == spktlen);

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.max_stream_data.stream_id = stream_id;
  fr.max_stream_data.max_stream_data = 2048;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, 4);

  CU_ASSERT(0 == rv);
  CU_ASSERT(2048 == strm->max_tx_offset);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 5);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(1 == nwrite);
  CU_ASSERT(2048 == strm->tx_offset);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_rx_flow_control(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  int rv;
  ngtcp2_frame fr;

  setup_default_server(&conn);

  conn->local_settings.max_data = 1024;
  conn->max_rx_offset = 1024;
  conn->unsent_max_rx_offset = 1024;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 1023;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  ngtcp2_conn_extend_max_offset(conn, 1023);

  CU_ASSERT(1024 + 1023 == conn->unsent_max_rx_offset);
  CU_ASSERT(1024 == conn->max_rx_offset);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 1023;
  fr.stream.datalen = 1;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(0 == rv);

  ngtcp2_conn_extend_max_offset(conn, 1);

  CU_ASSERT(2048 == conn->unsent_max_rx_offset);
  CU_ASSERT(1024 == conn->max_rx_offset);

  spktlen = ngtcp2_conn_write_pkt(conn, buf, sizeof(buf), 3);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(2048 == conn->max_rx_offset);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_rx_flow_control_error(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  int rv;
  ngtcp2_frame fr;

  setup_default_server(&conn);

  conn->local_settings.max_data = 1024;
  conn->max_rx_offset = 1024;
  conn->unsent_max_rx_offset = 1024;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 1025;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_FLOW_CONTROL == rv);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_tx_flow_control(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  int rv;
  ngtcp2_frame fr;
  ssize_t nwrite;
  uint64_t stream_id;

  setup_default_client(&conn);

  conn->remote_settings.max_data = 2048;
  conn->max_tx_offset = 2048;

  rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(1024 == nwrite);
  CU_ASSERT(1024 == conn->tx_offset);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1023, 2);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(1023 == nwrite);
  CU_ASSERT(1024 + 1023 == conn->tx_offset);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 3);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(1 == nwrite);
  CU_ASSERT(2048 == conn->tx_offset);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 4);

  CU_ASSERT(NGTCP2_ERR_STREAM_DATA_BLOCKED == spktlen);
  CU_ASSERT(-1 == nwrite);

  fr.type = NGTCP2_FRAME_MAX_DATA;
  fr.max_data.max_data = 3072;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, 5);

  CU_ASSERT(0 == rv);
  CU_ASSERT(3072 == conn->max_tx_offset);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &nwrite, stream_id,
                                     0, null_data, 1024, 4);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(1024 == nwrite);
  CU_ASSERT(3072 == conn->tx_offset);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_shutdown_stream_write(void) {
  ngtcp2_conn *conn;
  int rv;
  ngtcp2_frame_chain *frc;
  uint8_t buf[2048];
  ngtcp2_frame fr;
  size_t pktlen;
  ngtcp2_strm *strm;
  uint64_t stream_id;

  /* Stream not found */
  setup_default_server(&conn);

  rv = ngtcp2_conn_shutdown_stream_write(conn, 4, NGTCP2_APP_ERR01);

  CU_ASSERT(NGTCP2_ERR_STREAM_NOT_FOUND == rv);

  ngtcp2_conn_del(conn);

  /* Check final_offset */
  setup_default_client(&conn);

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                           null_data, 1239, 1);
  rv = ngtcp2_conn_shutdown_stream_write(conn, stream_id, NGTCP2_APP_ERR01);

  CU_ASSERT(0 == rv);

  for (frc = conn->frq; frc; frc = frc->next) {
    if (frc->fr.type == NGTCP2_FRAME_RST_STREAM) {
      break;
    }
  }

  CU_ASSERT(NULL != frc);
  CU_ASSERT(stream_id == frc->fr.rst_stream.stream_id);
  CU_ASSERT(NGTCP2_APP_ERR01 == frc->fr.rst_stream.app_error_code);
  CU_ASSERT(1239 == frc->fr.rst_stream.final_offset);

  strm = ngtcp2_conn_find_stream(conn, stream_id);

  CU_ASSERT(NULL != strm);
  CU_ASSERT(NGTCP2_APP_ERR01 == strm->app_error_code);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = stream_id;
  fr.rst_stream.app_error_code = NGTCP2_STOPPING;
  fr.rst_stream.final_offset = 100;

  pktlen =
      write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 890, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL == ngtcp2_conn_find_stream(conn, stream_id));

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_rst_stream(void) {
  ngtcp2_conn *conn;
  int rv;
  uint8_t buf[2048];
  ngtcp2_frame fr;
  size_t pktlen;
  ngtcp2_strm *strm;
  uint64_t stream_id;

  /* Receive RST_STREAM */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, 4, 0, null_data, 354,
                           2);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 955;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 3);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(strm->flags & NGTCP2_STRM_FLAG_SHUT_RD);
  CU_ASSERT(strm->flags & NGTCP2_STRM_FLAG_RECV_RST);

  ngtcp2_conn_del(conn);

  /* Receive RST_STREAM after sending STOP_SENDING */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, 4, 0, null_data, 354,
                           2);
  ngtcp2_conn_shutdown_stream_read(conn, 4, NGTCP2_APP_ERR01);
  ngtcp2_conn_write_pkt(conn, buf, sizeof(buf), 3);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 955;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 4);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL != ngtcp2_conn_find_stream(conn, 4));

  ngtcp2_conn_del(conn);

  /* Receive RST_STREAM after sending RST_STREAM */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, 4, 0, null_data, 354,
                           2);
  ngtcp2_conn_shutdown_stream_write(conn, 4, NGTCP2_APP_ERR01);
  ngtcp2_conn_write_pkt(conn, buf, sizeof(buf), 3);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 955;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 4);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL == ngtcp2_conn_find_stream(conn, 4));

  ngtcp2_conn_del(conn);

  /* Receive RST_STREAM after receiving STOP_SENDING */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, 4, 0, null_data, 354,
                           2);

  fr.type = NGTCP2_FRAME_STOP_SENDING;
  fr.stop_sending.stream_id = 4;
  fr.stop_sending.app_error_code = NGTCP2_APP_ERR01;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 3);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL != ngtcp2_conn_find_stream(conn, 4));

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 955;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 3, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 4);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL == ngtcp2_conn_find_stream(conn, 4));

  ngtcp2_conn_del(conn);

  /* final_offset in RST_STREAM exceeds the already received offset */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 954;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(NGTCP2_ERR_FINAL_OFFSET == rv);

  ngtcp2_conn_del(conn);

  /* final_offset in RST_STREAM differs from the final offset which
     STREAM frame with fin indicated. */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 956;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(NGTCP2_ERR_FINAL_OFFSET == rv);

  ngtcp2_conn_del(conn);

  /* RST_STREAM against local stream which has not been initiated. */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 1;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR01;
  fr.rst_stream.final_offset = 0;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_STREAM_STATE == rv);

  ngtcp2_conn_del(conn);

  /* RST_STREAM against remote stream which is larger than allowed
     maximum */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 16;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR01;
  fr.rst_stream.final_offset = 0;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_STREAM_ID == rv);

  ngtcp2_conn_del(conn);

  /* RST_STREAM against remote stream which is allowed, and no
     ngtcp2_strm object has been created */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR01;
  fr.rst_stream.final_offset = 0;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);
  CU_ASSERT(
      NGTCP2_ERR_STREAM_IN_USE ==
      ngtcp2_idtr_is_open(&conn->remote_bidi_idtr, fr.rst_stream.stream_id));

  ngtcp2_conn_del(conn);

  /* RST_STREAM against remote stream which is allowed, and no
     ngtcp2_strm object has been created, and final_offset violates
     connection-level flow control. */
  setup_default_server(&conn);

  conn->local_settings.max_stream_data = 1 << 21;

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR01;
  fr.rst_stream.final_offset = 1 << 20;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_FLOW_CONTROL == rv);

  ngtcp2_conn_del(conn);

  /* RST_STREAM against remote stream which is allowed, and no
      ngtcp2_strm object has been created, and final_offset violates
      stream-level flow control. */
  setup_default_server(&conn);

  conn->max_rx_offset = 1 << 21;

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR01;
  fr.rst_stream.final_offset = 1 << 20;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_FLOW_CONTROL == rv);

  ngtcp2_conn_del(conn);

  /* final_offset in RST_STREAM violates connection-level flow
     control */
  setup_default_server(&conn);

  conn->local_settings.max_stream_data = 1 << 21;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 1024 * 1024;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(NGTCP2_ERR_FLOW_CONTROL == rv);

  ngtcp2_conn_del(conn);

  /* final_offset in RST_STREAM violates stream-level flow
     control */
  setup_default_server(&conn);

  conn->max_rx_offset = 1 << 21;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 955;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 1024 * 1024;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(NGTCP2_ERR_FLOW_CONTROL == rv);

  ngtcp2_conn_del(conn);

  /* Receiving RST_STREAM for a local unidirectional stream is a
     protocol violation. */
  setup_default_server(&conn);

  rv = ngtcp2_conn_open_uni_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = stream_id;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR02;
  fr.rst_stream.final_offset = 0;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_PROTO == rv);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_stop_sending(void) {
  ngtcp2_conn *conn;
  int rv;
  uint8_t buf[2048];
  ngtcp2_frame fr;
  size_t pktlen;
  ngtcp2_strm *strm;
  ngtcp2_tstamp t = 0;
  uint64_t pkt_num = 0;
  ngtcp2_frame_chain *frc;
  uint64_t stream_id;

  /* Receive STOP_SENDING */
  setup_default_client(&conn);

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                           null_data, 333, ++t);

  fr.type = NGTCP2_FRAME_STOP_SENDING;
  fr.stop_sending.stream_id = stream_id;
  fr.stop_sending.app_error_code = NGTCP2_APP_ERR01;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, stream_id);

  CU_ASSERT(strm->flags & NGTCP2_STRM_FLAG_SHUT_WR);
  CU_ASSERT(strm->flags & NGTCP2_STRM_FLAG_SENT_RST);

  for (frc = conn->frq; frc; frc = frc->next) {
    if (frc->fr.type == NGTCP2_FRAME_RST_STREAM) {
      break;
    }
  }

  CU_ASSERT(NULL != frc);
  CU_ASSERT(NGTCP2_STOPPING == frc->fr.rst_stream.app_error_code);
  CU_ASSERT(333 == frc->fr.rst_stream.final_offset);

  ngtcp2_conn_del(conn);

  /* Receive STOP_SENDING after receiving RST_STREAM */
  setup_default_client(&conn);

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                           null_data, 333, ++t);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = stream_id;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR01;
  fr.rst_stream.final_offset = 0;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_STOP_SENDING;
  fr.stop_sending.stream_id = stream_id;
  fr.stop_sending.app_error_code = NGTCP2_APP_ERR01;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL == ngtcp2_conn_find_stream(conn, stream_id));

  for (frc = conn->frq; frc; frc = frc->next) {
    if (frc->fr.type == NGTCP2_FRAME_RST_STREAM) {
      break;
    }
  }

  CU_ASSERT(NULL != frc);
  CU_ASSERT(NGTCP2_STOPPING == frc->fr.rst_stream.app_error_code);
  CU_ASSERT(333 == frc->fr.rst_stream.final_offset);

  ngtcp2_conn_del(conn);

  /* STOP_SENDING against local bidirectional stream which has not
     been initiated. */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STOP_SENDING;
  fr.stop_sending.stream_id = 1;
  fr.stop_sending.app_error_code = NGTCP2_APP_ERR01;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_STREAM_STATE == rv);

  ngtcp2_conn_del(conn);

  /* Receiving STOP_SENDING for a local unidirectional stream */
  setup_default_server(&conn);

  rv = ngtcp2_conn_open_uni_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_STOP_SENDING;
  fr.stop_sending.stream_id = stream_id;
  fr.stop_sending.app_error_code = NGTCP2_APP_ERR01;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NGTCP2_FRAME_RST_STREAM == conn->frq->fr.type);

  ngtcp2_conn_del(conn);

  /* STOP_SENDING against local unidirectional stream which has not
     been initiated. */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STOP_SENDING;
  fr.stop_sending.stream_id = 3;
  fr.stop_sending.app_error_code = NGTCP2_APP_ERR01;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(NGTCP2_ERR_STREAM_STATE == rv);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_conn_id_omitted(void) {
  ngtcp2_conn *conn;
  int rv;
  uint8_t buf[2048];
  ngtcp2_frame fr;
  size_t pktlen;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 100;
  fr.stream.data = null_data;

  /* Receiving packet which has no connection ID while SCID of server
     is not empty. */
  setup_default_server(&conn);

  pktlen =
      write_single_frame_pkt_without_conn_id(conn, buf, sizeof(buf), 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  /* packet is just ignored */
  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL == ngtcp2_conn_find_stream(conn, 4));

  ngtcp2_conn_del(conn);

  /* Allow omission of connection ID */
  setup_default_server(&conn);
  ngtcp2_cid_zero(&conn->scid);

  pktlen =
      write_single_frame_pkt_without_conn_id(conn, buf, sizeof(buf), 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL != ngtcp2_conn_find_stream(conn, 4));

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_short_pkt_type(void) {
  ngtcp2_conn *conn;
  ngtcp2_pkt_hd hd;
  uint8_t buf[2048];
  ssize_t spktlen;
  uint64_t stream_id;

  /* 1 octet pkt num */
  setup_default_client(&conn);

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(1 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);

  /* 2 octets pkt num */
  setup_default_client(&conn);
  conn->pktns.rtb.largest_acked_tx_pkt_num = 0x6afa2f;
  conn->pktns.last_tx_pkt_num = 0x6afd78;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(2 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);

  /* 4 octets pkt num */
  setup_default_client(&conn);
  conn->pktns.rtb.largest_acked_tx_pkt_num = 0x6afa2f;
  conn->pktns.last_tx_pkt_num = 0x6bc106;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(4 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);

  /* 1 octet pkt num (largest)*/
  setup_default_client(&conn);
  conn->pktns.rtb.largest_acked_tx_pkt_num = 1;
  conn->pktns.last_tx_pkt_num = 63;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(1 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);

  /* 2 octet pkt num (shortest)*/
  setup_default_client(&conn);
  conn->pktns.rtb.largest_acked_tx_pkt_num = 1;
  conn->pktns.last_tx_pkt_num = 64;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(2 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);

  /* 2 octet pkt num (largest)*/
  setup_default_client(&conn);
  conn->pktns.rtb.largest_acked_tx_pkt_num = 1;
  conn->pktns.last_tx_pkt_num = 8191;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(2 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);

  /* 4 octet pkt num (shortest)*/
  setup_default_client(&conn);
  conn->pktns.rtb.largest_acked_tx_pkt_num = 1;
  conn->pktns.last_tx_pkt_num = 8192;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(4 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);

  /* Overflow */
  setup_default_client(&conn);
  conn->pktns.rtb.largest_acked_tx_pkt_num = 1;
  conn->pktns.last_tx_pkt_num = NGTCP2_MAX_PKT_NUM - 1;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 19, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(pkt_decode_hd_short(&hd, buf, (size_t)spktlen, conn->scid.datalen) >
            0);
  CU_ASSERT(4 == hd.pkt_numlen);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_stateless_reset(void) {
  ngtcp2_conn *conn;
  uint8_t buf[256];
  ssize_t spktlen;
  int rv;
  size_t i;
  uint8_t token[NGTCP2_STATELESS_RESET_TOKENLEN];
  ngtcp2_pkt_hd hd;
  ngtcp2_cid dcid;

  dcid_init(&dcid);

  for (i = 0; i < NGTCP2_STATELESS_RESET_TOKENLEN; ++i) {
    token[i] = (uint8_t)~i;
  }

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &dcid, NULL,
                     0xe1, 1, 0, 0);

  /* server: Just ignore SR */
  setup_default_server(&conn);
  conn->callbacks.decrypt = fail_decrypt;
  conn->pktns.max_rx_pkt_num = 24324325;

  spktlen = ngtcp2_pkt_write_stateless_reset(
      buf, sizeof(buf), &hd, conn->local_settings.stateless_reset_token,
      null_data, 17);

  CU_ASSERT(spktlen > 0);

  rv = ngtcp2_conn_recv(conn, buf, (size_t)spktlen, 1);

  CU_ASSERT(NGTCP2_ERR_TLS_DECRYPT == rv);

  ngtcp2_conn_del(conn);

  /* client */
  setup_default_client(&conn);
  conn->callbacks.decrypt = fail_decrypt;
  conn->pktns.max_rx_pkt_num = 3255454;
  conn->remote_settings.stateless_reset_token_present = 1;
  memcpy(conn->remote_settings.stateless_reset_token, token,
         NGTCP2_STATELESS_RESET_TOKENLEN);

  spktlen = ngtcp2_pkt_write_stateless_reset(buf, sizeof(buf), &hd, token,
                                             null_data, 19);

  CU_ASSERT(spktlen > 0);

  rv = ngtcp2_conn_recv(conn, buf, (size_t)spktlen, 1);

  CU_ASSERT(NGTCP2_ERR_DRAINING == rv);
  CU_ASSERT(NGTCP2_CS_DRAINING == conn->state);

  ngtcp2_conn_del(conn);

  /* token does not match */
  setup_default_client(&conn);
  conn->callbacks.decrypt = fail_decrypt;
  conn->pktns.max_rx_pkt_num = 24324325;

  spktlen = ngtcp2_pkt_write_stateless_reset(buf, sizeof(buf), &hd, token,
                                             null_data, 17);

  CU_ASSERT(spktlen > 0);

  rv = ngtcp2_conn_recv(conn, buf, (size_t)spktlen, 1);

  CU_ASSERT(NGTCP2_ERR_TLS_DECRYPT == rv);
  CU_ASSERT(NGTCP2_CS_DRAINING != conn->state);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_server_stateless_retry(void) {
  /* TODO Retry packet handling is TBD */
  /* ngtcp2_conn *conn; */
  /* my_user_data ud; */
  /* uint8_t buf[2048]; */
  /* ssize_t spktlen; */
  /* size_t pktlen; */
  /* ngtcp2_frame fra[2]; */
  /* ngtcp2_frame *fr; */

  /* memset(&ud, 0, sizeof(ud)); */
  /* ud.pkt_num = 0; */
  /* setup_handshake_client(&conn); */
  /* conn->user_data = &ud; */

  /* spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), NULL, 0, 1); */

  /* CU_ASSERT(spktlen > 0); */

  /* fr = &fra[0]; */
  /* fr->type = NGTCP2_FRAME_STREAM; */
  /* fr->stream.flags = 0; */
  /* fr->stream.stream_id = 0; */
  /* fr->stream.fin = 0; */
  /* fr->stream.offset = 0; */
  /* fr->stream.datalen = 333; */
  /* fr->stream.data = null_data; */

  /* fr = &fra[1]; */
  /* fr->type = NGTCP2_FRAME_ACK; */
  /* fr->ack.largest_ack = conn->in_pktns.last_tx_pkt_num; */
  /* fr->ack.ack_delay = 0; */
  /* fr->ack.first_ack_blklen = 0; */
  /* fr->ack.num_blks = 0; */

  /* pktlen = write_handshake_pkt( */
  /*     conn, buf, sizeof(buf), NGTCP2_PKT_RETRY, &conn->scid, &conn->dcid, */
  /*     conn->in_pktns.last_tx_pkt_num, conn->version, fra, arraylen(fra)); */

  /* spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, 2); */

  /* CU_ASSERT(spktlen > 0); */
  /* CU_ASSERT(1 == conn->in_pktns.last_tx_pkt_num); */

  /* ngtcp2_conn_del(conn); */
}

void test_ngtcp2_conn_recv_delayed_handshake_pkt(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ngtcp2_frame fr;
  int rv;

  /* STREAM frame within final_hs_rx_offset */
  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.flags = 0;
  fr.stream.stream_id = 0;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 567;
  fr.stream.data = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_HANDSHAKE, &conn->scid, &conn->dcid, 1,
      NGTCP2_PROTO_VER_MAX, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);
  CU_ASSERT(1 == ngtcp2_ksl_len(&conn->hs_pktns.acktr.ents));
  CU_ASSERT(conn->hs_pktns.acktr.flags & NGTCP2_ACKTR_FLAG_ACTIVE_ACK);

  ngtcp2_conn_del(conn);

  /* STREAM frame beyond final_hs_rx_offset */
  /* TODO This is not implemented yet */
  /* setup_default_client(&conn); */

  /* conn->final_hs_tx_offset = 999; */
  /* conn->final_hs_rx_offset = 100; */

  /* fr.type = NGTCP2_FRAME_STREAM; */
  /* fr.stream.flags = 0; */
  /* fr.stream.stream_id = 0; */
  /* fr.stream.fin = 0; */
  /* fr.stream.offset = 0; */
  /* fr.stream.datalen = 567; */
  /* fr.stream.data = null_data; */

  /* pktlen = write_single_frame_handshake_pkt( */
  /*     conn, buf, sizeof(buf), NGTCP2_PKT_HANDSHAKE, &conn->scid, &conn->dcid, 1, */
  /*     NGTCP2_PROTO_VER_MAX, &fr); */
  /* rv = ngtcp2_conn_recv(conn, buf, pktlen, 1); */

  /* CU_ASSERT(NGTCP2_ERR_PROTO == rv); */

  /* ngtcp2_conn_del(conn); */

  /* ACK frame only */
  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_ACK;
  fr.ack.largest_ack = 1000000007;
  fr.ack.ack_delay = 122;
  fr.ack.first_ack_blklen = 0;
  fr.ack.num_blks = 0;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_HANDSHAKE, &conn->scid, &conn->dcid, 1,
      NGTCP2_PROTO_VER_MAX, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);
  CU_ASSERT(1 == ngtcp2_ksl_len(&conn->hs_pktns.acktr.ents));
  CU_ASSERT(!conn->hs_pktns.acktr.flags);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_max_stream_id(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  int rv;
  ngtcp2_frame fr;

  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_MAX_STREAM_ID;
  fr.max_stream_id.max_stream_id = 999;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 1, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 1);

  CU_ASSERT(0 == rv);
  CU_ASSERT(999 == conn->max_local_stream_id_uni);

  fr.type = NGTCP2_FRAME_MAX_STREAM_ID;
  fr.max_stream_id.max_stream_id = 997;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid, 2, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, 2);

  CU_ASSERT(0 == rv);
  CU_ASSERT(997 == conn->max_local_stream_id_bidi);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_handshake(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  ngtcp2_frame fr;
  uint64_t pkt_num = 12345689, t = 0;
  ngtcp2_cid rcid;

  rcid_init(&rcid);

  setup_handshake_server(&conn);

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 45;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &rcid, &conn->dcid, ++pkt_num,
      conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(spktlen > 0);
  /* No path challenge at the moment. */
  CU_ASSERT(0 == ngtcp2_ringbuf_len(&conn->tx_path_challenge));

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_handshake_error(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  ngtcp2_frame fr;
  uint64_t pkt_num = 107, t = 0;
  ngtcp2_cid rcid;

  rcid_init(&rcid);

  /* client side */
  setup_handshake_client(&conn);
  conn->callbacks.recv_crypto_data = recv_crypto_handshake_error;
  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), NULL, 0, ++t);

  CU_ASSERT(spktlen > 0);

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 333;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &conn->scid, &conn->dcid,
      ++pkt_num, conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_CRYPTO == spktlen);

  ngtcp2_conn_del(conn);

  /* server side */
  setup_handshake_server(&conn);
  conn->callbacks.recv_crypto_data = recv_crypto_handshake_error;

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 551;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &rcid, &conn->dcid, ++pkt_num,
      conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_CRYPTO == spktlen);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_client_handshake(void) {
  ngtcp2_conn *conn;
  uint8_t buf[1240];
  ssize_t spktlen;
  ngtcp2_tstamp t = 0;
  uint64_t stream_id;
  int rv;
  ssize_t datalen;

  /* Verify that Handshake packet and 0-RTT Protected packet are
     coalesced into one UDP packet. */
  setup_early_client(&conn);

  rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  spktlen = ngtcp2_conn_client_handshake(conn, buf, sizeof(buf), &datalen, NULL,
                                         0, stream_id, 0, null_data, 199, ++t);

  CU_ASSERT(sizeof(buf) == spktlen);
  CU_ASSERT(199 == datalen);

  ngtcp2_conn_del(conn);

  /* 0 length 0-RTT packet with FIN bit set */
  setup_early_client(&conn);

  rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  spktlen = ngtcp2_conn_client_handshake(conn, buf, sizeof(buf), &datalen, NULL,
                                         0, stream_id, 1, null_data, 0, +t);

  CU_ASSERT(sizeof(buf) == spktlen);
  CU_ASSERT(0 == datalen);

  ngtcp2_conn_del(conn);

  /* Could not send 0-RTT data because buffer is too small. */
  setup_early_client(&conn);

  rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  spktlen = ngtcp2_conn_client_handshake(
      conn, buf,
      NGTCP2_MIN_LONG_HEADERLEN + 1 + conn->dcid.datalen + conn->scid.datalen +
          300,
      &datalen, NULL, 0, stream_id, 1, null_data, 0, +t);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(-1 == datalen);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_retransmit_protected(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  int rv;
  uint64_t pkt_num = 890;
  ngtcp2_tstamp t = 0;
  ngtcp2_frame fr;
  ngtcp2_rtb_entry *ent;
  uint64_t stream_id, stream_id_a, stream_id_b;
  ngtcp2_ksl_it it;

  /* Retransmit a packet completely */
  setup_default_client(&conn);

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 126, ++t);

  CU_ASSERT(spktlen > 0);

  /* Kick delayed ACK timer */
  t += 1000000000;

  it = ngtcp2_rtb_head(&conn->pktns.rtb);
  ent = ngtcp2_ksl_it_get(&it);
  ngtcp2_rtb_detect_lost_pkt(&conn->pktns.rtb, &conn->rcs, 1000000007,
                             1000000007, ++t);
  spktlen = ngtcp2_conn_write_pkt(conn, buf, sizeof(buf), ++t);

  CU_ASSERT(spktlen > 0);

  it = ngtcp2_rtb_head(&conn->pktns.rtb);

  CU_ASSERT(ent == ngtcp2_ksl_it_get(&it));

  ngtcp2_conn_del(conn);

  /* Partially retransmiting a packet is not possible at the
     moment. */
  setup_default_client(&conn);
  conn->max_local_stream_id_bidi = 8;

  ngtcp2_conn_open_bidi_stream(conn, &stream_id_a, NULL);
  ngtcp2_conn_open_bidi_stream(conn, &stream_id_b, NULL);

  ngtcp2_conn_shutdown_stream_write(conn, stream_id_a, NGTCP2_APP_ERR01);
  ngtcp2_conn_shutdown_stream_write(conn, stream_id_b, NGTCP2_APP_ERR01);

  spktlen = ngtcp2_conn_write_pkt(conn, buf, sizeof(buf), ++t);

  CU_ASSERT(spktlen > 0);

  /* Kick delayed ACK timer */
  t += 1000000000;

  it = ngtcp2_rtb_head(&conn->pktns.rtb);
  ent = ngtcp2_ksl_it_get(&it);
  ngtcp2_rtb_detect_lost_pkt(&conn->pktns.rtb, &conn->rcs, 1000000007,
                             1000000007, ++t);
  spktlen = ngtcp2_conn_write_pkt(conn, buf, (size_t)(spktlen - 1), ++t);

  CU_ASSERT(NGTCP2_ERR_NOBUF == spktlen);

  it = ngtcp2_rtb_head(&conn->pktns.rtb);

  CU_ASSERT(ngtcp2_ksl_it_end(&it));
  CU_ASSERT(ent == conn->pktns.rtb.lost);
  CU_ASSERT(1 == rtb_entry_length(ngtcp2_rtb_lost_head(&conn->pktns.rtb)));

  ngtcp2_conn_del(conn);

  /* ngtcp2_rtb_entry is reused because buffer was too small */
  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_PING;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), NULL, stream_id, 0,
                                     null_data, 1000, ++t);

  CU_ASSERT(spktlen > 0);

  /* Kick delayed ACK timer */
  t += 1000000000;

  it = ngtcp2_rtb_head(&conn->pktns.rtb);
  ent = ngtcp2_ksl_it_get(&it);
  ngtcp2_rtb_detect_lost_pkt(&conn->pktns.rtb, &conn->rcs, 1000000007,
                             1000000007, ++t);

  /* This should not send ACK only packet */
  spktlen = ngtcp2_conn_write_pkt(conn, buf, 999, ++t);

  CU_ASSERT(NGTCP2_ERR_NOBUF == spktlen);
  CU_ASSERT(ent == ngtcp2_rtb_lost_head(&conn->pktns.rtb));

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_send_max_stream_data(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ngtcp2_strm *strm;
  uint64_t pkt_num = 890;
  ngtcp2_tstamp t = 0;
  ngtcp2_frame fr;
  int rv;
  const uint32_t datalen = 1024;

  /* MAX_STREAM_DATA should be sent */
  setup_default_server(&conn);
  conn->local_settings.max_stream_data = datalen;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = datalen;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  rv = ngtcp2_conn_extend_max_stream_offset(conn, 4, datalen);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL != strm->fc_pprev);

  ngtcp2_conn_del(conn);

  /* MAX_STREAM_DATA should not be sent on incoming fin */
  setup_default_server(&conn);
  conn->local_settings.max_stream_data = datalen;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 0;
  fr.stream.datalen = datalen;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  rv = ngtcp2_conn_extend_max_stream_offset(conn, 4, datalen);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL == strm->fc_pprev);

  ngtcp2_conn_del(conn);

  /* MAX_STREAM_DATA should not be sent if STOP_SENDING frame is being
     sent by local endpoint */
  setup_default_server(&conn);
  conn->local_settings.max_stream_data = datalen;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = datalen;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  rv = ngtcp2_conn_shutdown_stream_read(conn, 4, NGTCP2_APP_ERR01);

  CU_ASSERT(0 == rv);

  rv = ngtcp2_conn_extend_max_stream_offset(conn, 4, datalen);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL == strm->fc_pprev);

  ngtcp2_conn_del(conn);

  /* MAX_STREAM_DATA should not be sent if stream is being reset by
     remote endpoint */
  setup_default_server(&conn);
  conn->local_settings.max_stream_data = datalen;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = datalen;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_RST_STREAM;
  fr.rst_stream.stream_id = 4;
  fr.rst_stream.app_error_code = NGTCP2_APP_ERR01;
  fr.rst_stream.final_offset = datalen;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  rv = ngtcp2_conn_extend_max_stream_offset(conn, 4, datalen);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL == conn->fc_strms);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_stream_data(void) {
  uint8_t buf[1024];
  ngtcp2_conn *conn;
  my_user_data ud;
  uint64_t pkt_num = 612;
  ngtcp2_tstamp t = 0;
  ngtcp2_frame fr;
  size_t pktlen;
  int rv;
  uint64_t stream_id;

  /* 2 STREAM frames are received in the correct order. */
  setup_default_server(&conn);
  conn->callbacks.recv_stream_data = recv_stream_data;
  conn->user_data = &ud;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 111;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(4 == ud.stream_data.stream_id);
  CU_ASSERT(0 == ud.stream_data.fin);
  CU_ASSERT(111 == ud.stream_data.datalen);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 111;
  fr.stream.datalen = 99;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(4 == ud.stream_data.stream_id);
  CU_ASSERT(1 == ud.stream_data.fin);
  CU_ASSERT(99 == ud.stream_data.datalen);

  ngtcp2_conn_del(conn);

  /* 2 STREAM frames are received in the correct order, and 2nd STREAM
     frame has 0 length, and FIN bit set. */
  setup_default_server(&conn);
  conn->callbacks.recv_stream_data = recv_stream_data;
  conn->user_data = &ud;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 111;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(4 == ud.stream_data.stream_id);
  CU_ASSERT(0 == ud.stream_data.fin);
  CU_ASSERT(111 == ud.stream_data.datalen);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 111;
  fr.stream.datalen = 0;
  fr.stream.data = NULL;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(4 == ud.stream_data.stream_id);
  CU_ASSERT(1 == ud.stream_data.fin);
  CU_ASSERT(0 == ud.stream_data.datalen);

  ngtcp2_conn_del(conn);

  /* Re-ordered STREAM frame; we first gets 0 length STREAM frame with
     FIN bit set. Then the remaining STREAM frame is received. */
  setup_default_server(&conn);
  conn->callbacks.recv_stream_data = recv_stream_data;
  conn->user_data = &ud;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 599;
  fr.stream.datalen = 0;
  fr.stream.data = NULL;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(0 == ud.stream_data.stream_id);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 599;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(4 == ud.stream_data.stream_id);
  CU_ASSERT(1 == ud.stream_data.fin);
  CU_ASSERT(599 == ud.stream_data.datalen);

  ngtcp2_conn_del(conn);

  /* Simulate the case where packet is lost.  We first gets 0 length
     STREAM frame with FIN bit set.  Then the lost STREAM frame is
     retransmitted with FIN bit set is received. */
  setup_default_server(&conn);
  conn->callbacks.recv_stream_data = recv_stream_data;
  conn->user_data = &ud;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 599;
  fr.stream.datalen = 0;
  fr.stream.data = NULL;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(0 == ud.stream_data.stream_id);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 0;
  fr.stream.datalen = 599;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(4 == ud.stream_data.stream_id);
  CU_ASSERT(1 == ud.stream_data.fin);
  CU_ASSERT(599 == ud.stream_data.datalen);

  ngtcp2_conn_del(conn);

  /* Receive an unidirectional stream data */
  setup_default_client(&conn);
  conn->callbacks.recv_stream_data = recv_stream_data;
  conn->user_data = &ud;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 3;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 911;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(3 == ud.stream_data.stream_id);
  CU_ASSERT(0 == ud.stream_data.fin);
  CU_ASSERT(911 == ud.stream_data.datalen);

  ngtcp2_conn_del(conn);

  /* Receive an unidirectional stream which is beyond the limit. */
  setup_default_server(&conn);
  conn->callbacks.recv_stream_data = recv_stream_data;
  conn->max_remote_stream_id_uni = 0;
  conn->user_data = &ud;

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 2;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 911;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  memset(&ud, 0, sizeof(ud));
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_STREAM_ID == rv);

  ngtcp2_conn_del(conn);

  /* Receiving nonzero payload to an local unidirectional stream is a
     protocol violation. */
  setup_default_client(&conn);

  rv = ngtcp2_conn_open_uni_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = stream_id;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 9;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_PROTO == rv);

  ngtcp2_conn_del(conn);

  /* DATA on crypto stream, and TLS alert is generated. */
  setup_default_server(&conn);
  conn->callbacks.recv_crypto_data = recv_crypto_fatal_alert_generated;

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 139;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_CRYPTO == rv);

  ngtcp2_conn_del(conn);

  /* 0 length STREAM frame is allowed */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 0;
  fr.stream.data = null_data;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL != ngtcp2_conn_find_stream(conn, 4));

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_ping(void) {
  uint8_t buf[1024];
  ngtcp2_conn *conn;
  uint64_t pkt_num = 133;
  ngtcp2_tstamp t = 0;
  ngtcp2_frame fr;
  size_t pktlen;
  int rv;

  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_PING;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);
  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(NULL == conn->frq);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_max_stream_data(void) {
  uint8_t buf[1024];
  ngtcp2_conn *conn;
  uint64_t pkt_num = 1000000007;
  ngtcp2_tstamp t = 0;
  ngtcp2_frame fr;
  size_t pktlen;
  int rv;
  ngtcp2_strm *strm;

  /* Receiving MAX_STREAM_DATA to an uninitiated local bidirectional
     stream ID is an error */
  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.max_stream_data.stream_id = 4;
  fr.max_stream_data.max_stream_data = 8092;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_STREAM_STATE == rv);

  ngtcp2_conn_del(conn);

  /* Receiving MAX_STREAM_DATA to an uninitiated local unidirectional
     stream ID is an error */
  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.max_stream_data.stream_id = 2;
  fr.max_stream_data.max_stream_data = 8092;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_PROTO == rv);

  ngtcp2_conn_del(conn);

  /* Receiving MAX_STREAM_DATA to a remote bidirectional stream which
     exceeds limit */
  setup_default_client(&conn);

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.max_stream_data.stream_id = 1;
  fr.max_stream_data.max_stream_data = 1000000009;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_STREAM_ID == rv);

  ngtcp2_conn_del(conn);

  /* Receiving MAX_STREAM_DATA to a remote bidirectional stream which
     the local endpoint has not received yet. */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.max_stream_data.stream_id = 4;
  fr.max_stream_data.max_stream_data = 1000000009;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL != strm);
  CU_ASSERT(1000000009 == strm->max_tx_offset);

  ngtcp2_conn_del(conn);

  /* Receiving MAX_STREAM_DATA to a idle remote unidirectional stream
     is a protocol violation. */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.max_stream_data.stream_id = 2;
  fr.max_stream_data.max_stream_data = 1000000009;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(NGTCP2_ERR_PROTO == rv);

  ngtcp2_conn_del(conn);

  /* Receiving MAX_STREAM_DATA to an existing bidirectional stream */
  setup_default_server(&conn);

  strm = open_stream(conn, 4);

  fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
  fr.max_stream_data.stream_id = 4;
  fr.max_stream_data.max_stream_data = 1000000009;

  pktlen = write_single_frame_pkt(conn, buf, sizeof(buf), &conn->scid,
                                  ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);
  CU_ASSERT(1000000009 == strm->max_tx_offset);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_send_early_data(void) {
  ngtcp2_conn *conn;
  ssize_t spktlen;
  ssize_t datalen;
  uint8_t buf[1024];
  uint64_t stream_id;
  int rv;

  setup_early_client(&conn);

  rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);

  CU_ASSERT(0 == rv);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), NULL, 0, 1);

  CU_ASSERT(spktlen > 0);

  spktlen = ngtcp2_conn_write_stream(conn, buf, sizeof(buf), &datalen,
                                     stream_id, 1, null_data, 911, 1);

  CU_ASSERT(spktlen > 0);
  CU_ASSERT(911 == datalen);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_early_data(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  ngtcp2_frame fr;
  uint64_t pkt_num = 1;
  ngtcp2_tstamp t = 0;
  ngtcp2_strm *strm;
  ngtcp2_cid rcid;

  rcid_init(&rcid);

  setup_early_server(&conn);

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 121;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &rcid, &conn->dcid, ++pkt_num,
      conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(spktlen > 0);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 0;
  fr.stream.datalen = 911;
  fr.stream.data = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_0RTT_PROTECTED, &rcid, &conn->dcid,
      ++pkt_num, conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(spktlen > 0);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL != strm);
  CU_ASSERT(911 == strm->last_rx_offset);

  ngtcp2_conn_del(conn);

  /* Re-ordered 0-RTT Protected packet */
  setup_early_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 0;
  fr.stream.datalen = 119;
  fr.stream.data = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_0RTT_PROTECTED, &rcid, &conn->dcid,
      ++pkt_num, conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(0 == spktlen);

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 319;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &rcid, &conn->dcid, ++pkt_num,
      conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(spktlen > 0);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL != strm);
  CU_ASSERT(119 == strm->last_rx_offset);

  ngtcp2_conn_del(conn);

  /* Compound packet */
  setup_early_server(&conn);

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 111;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &rcid, &conn->dcid, ++pkt_num,
      conn->version, &fr);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 1;
  fr.stream.offset = 0;
  fr.stream.datalen = 999;
  fr.stream.data = null_data;

  pktlen += write_single_frame_handshake_pkt(
      conn, buf + pktlen, sizeof(buf) - pktlen, NGTCP2_PKT_0RTT_PROTECTED,
      &rcid, &conn->dcid, ++pkt_num, conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(spktlen > 0);

  strm = ngtcp2_conn_find_stream(conn, 4);

  CU_ASSERT(NULL != strm);
  CU_ASSERT(999 == strm->last_rx_offset);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_recv_compound_pkt(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  ngtcp2_frame fr;
  uint64_t pkt_num = 1;
  ngtcp2_tstamp t = 0;
  ngtcp2_acktr_entry *ackent;
  int rv;
  ngtcp2_ksl_it it;

  /* 2 QUIC long packets in one UDP packet */
  setup_handshake_server(&conn);

  fr.type = NGTCP2_FRAME_CRYPTO;
  fr.crypto.offset = 0;
  fr.crypto.datacnt = 1;
  fr.crypto.data[0].len = 131;
  fr.crypto.data[0].base = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &conn->scid, &conn->dcid,
      ++pkt_num, conn->version, &fr);

  pktlen += write_single_frame_handshake_pkt(
      conn, buf + pktlen, sizeof(buf) - pktlen, NGTCP2_PKT_INITIAL, &conn->scid,
      &conn->scid, ++pkt_num, conn->version, &fr);

  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(spktlen > 0);

  it = ngtcp2_acktr_get(&conn->in_pktns.acktr);
  ackent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(ackent->pkt_num == pkt_num);

  ngtcp2_ksl_it_next(&it);
  ackent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(ackent->pkt_num == pkt_num - 1);

  ngtcp2_conn_del(conn);

  /* 1 long packet and 1 short packet in one UDP packet */
  setup_default_server(&conn);

  fr.type = NGTCP2_FRAME_PADDING;
  fr.padding.len = 1;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_HANDSHAKE, &conn->scid, &conn->dcid,
      ++pkt_num, conn->version, &fr);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 4;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 426;
  fr.stream.data = null_data;

  pktlen += write_single_frame_pkt(conn, buf + pktlen, sizeof(buf) - pktlen,
                                   &conn->scid, ++pkt_num, &fr);

  rv = ngtcp2_conn_recv(conn, buf, pktlen, ++t);

  CU_ASSERT(0 == rv);

  it = ngtcp2_acktr_get(&conn->pktns.acktr);
  ackent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(ackent->pkt_num == pkt_num);

  it = ngtcp2_acktr_get(&conn->hs_pktns.acktr);
  ackent = ngtcp2_ksl_it_get(&it);

  CU_ASSERT(ackent->pkt_num == pkt_num - 1);

  ngtcp2_conn_del(conn);
}

void test_ngtcp2_conn_pkt_payloadlen(void) {
  ngtcp2_conn *conn;
  uint8_t buf[2048];
  size_t pktlen;
  ssize_t spktlen;
  ngtcp2_frame fr;
  uint64_t pkt_num = 1;
  ngtcp2_tstamp t = 0;
  uint64_t payloadlen;

  /* Payload length is invalid */
  setup_handshake_server(&conn);

  fr.type = NGTCP2_FRAME_STREAM;
  fr.stream.stream_id = 0;
  fr.stream.fin = 0;
  fr.stream.offset = 0;
  fr.stream.datalen = 131;
  fr.stream.data = null_data;

  pktlen = write_single_frame_handshake_pkt(
      conn, buf, sizeof(buf), NGTCP2_PKT_INITIAL, &conn->scid, &conn->dcid,
      ++pkt_num, conn->version, &fr);

  payloadlen = read_pkt_payloadlen(buf, &conn->dcid, &conn->scid);
  write_pkt_payloadlen(buf, &conn->dcid, &conn->scid, payloadlen + 1);

  /* The incoming packet should be ignored */
  spktlen = ngtcp2_conn_handshake(conn, buf, sizeof(buf), buf, pktlen, ++t);

  CU_ASSERT(spktlen == 0);
  CU_ASSERT(0 == ngtcp2_ksl_len(&conn->in_pktns.acktr.ents));

  ngtcp2_conn_del(conn);
}
