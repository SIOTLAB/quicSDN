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
#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/mman.h>

#include <openssl/bio.h>
#include <openssl/err.h>

#include "app_client.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "crypto.h"
#include "shared.h"
#include "keylog.h"

#include <ngtcp2/ngtcp2.h>
// #include "openflow.h"

using namespace ngtcp2;

struct sockaddr_in openflow_cliaddr;
struct sockaddr_in ovsdb_cliaddr;

int mode = -1;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
bool cond_flag = false;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER; 

int ofl_sock = -1;
int ovsdb_sock = -1;

namespace {
auto randgen = util::make_mt19937();
} // namespace

namespace {
Config config{};
} // namespace

Buffer::Buffer(const uint8_t *data, size_t datalen)
    : buf{data, data + datalen},
      begin(buf.data()),
      head(begin),
      tail(begin + datalen) {}
Buffer::Buffer(uint8_t *begin, uint8_t *end)
    : begin(begin), head(begin), tail(end) {}
Buffer::Buffer(size_t datalen)
    : buf(datalen), begin(buf.data()), head(begin), tail(begin) {}
Buffer::Buffer() : begin(buf.data()), head(begin), tail(begin) {}

Stream::Stream(uint64_t stream_id)
    : stream_id(stream_id),
      streambuf_idx(0),
      tx_stream_offset(0),
      should_send_fin(false) {}

Stream::~Stream() {}

void Stream::buffer_file() {
  streambuf.emplace_back(config.data, config.data + config.datalen);
  should_send_fin = true;
}

namespace {
int key_cb(SSL *ssl, int name, const unsigned char *secret, size_t secretlen,
           const unsigned char *key, size_t keylen, const unsigned char *iv,
           size_t ivlen, void *arg) {
  auto c = static_cast<Client *>(arg);

  if (c->on_key(name, secret, secretlen, key, keylen, iv, ivlen) != 0) {
    return 0;
  }

  keylog::log_secret(ssl, name, secret, secretlen);

  return 1;
}
} // namespace

int Client::on_key(int name, const uint8_t *secret, size_t secretlen,
                   const uint8_t *key, size_t keylen, const uint8_t *iv,
                   size_t ivlen) {
  int rv;

  switch (name) {
  case SSL_KEY_CLIENT_EARLY_TRAFFIC:
  case SSL_KEY_CLIENT_HANDSHAKE_TRAFFIC:
  case SSL_KEY_CLIENT_APPLICATION_TRAFFIC:
  case SSL_KEY_SERVER_HANDSHAKE_TRAFFIC:
  case SSL_KEY_SERVER_APPLICATION_TRAFFIC:
    break;
  default:
    return 0;
  }

  // TODO We don't have to call this everytime we get key generated.
  rv = crypto::negotiated_prf(crypto_ctx_, ssl_);
  if (rv != 0) {
    return -1;
  }
  rv = crypto::negotiated_aead(crypto_ctx_, ssl_);
  if (rv != 0) {
    return -1;
  }

  std::array<uint8_t, 64> pn;
  auto pnlen = crypto::derive_pkt_num_protection_key(
      pn.data(), pn.size(), secret, secretlen, crypto_ctx_);
  if (pnlen < 0) {
    return -1;
  }

  // TODO Just call this once.
  ngtcp2_conn_set_aead_overhead(conn_, crypto::aead_max_overhead(crypto_ctx_));

  switch (name) {
  case SSL_KEY_CLIENT_EARLY_TRAFFIC:
    std::cerr << "client_early_traffic" << std::endl;
    ngtcp2_conn_set_early_keys(conn_, key, keylen, iv, ivlen, pn.data(), pnlen);
    break;
  case SSL_KEY_CLIENT_HANDSHAKE_TRAFFIC:
    std::cerr << "client_handshake_traffic" << std::endl;
    ngtcp2_conn_set_handshake_tx_keys(conn_, key, keylen, iv, ivlen, pn.data(),
                                      pnlen);
    break;
  case SSL_KEY_CLIENT_APPLICATION_TRAFFIC:
    std::cerr << "client_application_traffic" << std::endl;
    ngtcp2_conn_update_tx_keys(conn_, key, keylen, iv, ivlen, pn.data(), pnlen);
    break;
  case SSL_KEY_SERVER_HANDSHAKE_TRAFFIC:
#if TLS_DEBUG
    std::cerr << "server_handshake_traffic" << std::endl;
#endif
	ngtcp2_conn_set_handshake_rx_keys(conn_, key, keylen, iv, ivlen, pn.data(),
                                      pnlen);
    break;
  case SSL_KEY_SERVER_APPLICATION_TRAFFIC:
    std::cerr << "server_application_traffic" << std::endl;
    ngtcp2_conn_update_rx_keys(conn_, key, keylen, iv, ivlen, pn.data(), pnlen);
    break;
  }

#if TLS_DEBUG
  std::cerr << "+ secret=" << util::format_hex(secret, secretlen) << "\n"
            << "+ key=" << util::format_hex(key, keylen) << "\n"
            << "+ iv=" << util::format_hex(iv, ivlen) << "\n"
            << "+ pn=" << util::format_hex(pn.data(), pnlen) << std::endl;
#endif

  return 0;
}

namespace {
void msg_cb(int write_p, int version, int content_type, const void *buf,
            size_t len, SSL *ssl, void *arg) {
  int rv;

#if TLS_DEBUG
   std::cerr << "msg_cb: write_p=" << write_p << " version=" << version
            << " content_type=" << content_type << " len=" << len << std::endl;
#endif
  if (!write_p || content_type != SSL3_RT_HANDSHAKE) {
    return;
  }

  auto c = static_cast<Client *>(arg);

  rv = c->write_client_handshake(reinterpret_cast<const uint8_t *>(buf), len);

  assert(0 == rv);
}
} // namespace

namespace {
int bio_write(BIO *b, const char *buf, int len) {
  assert(0);
  return -1;
}
} // namespace

namespace {
int bio_read(BIO *b, char *buf, int len) {
  BIO_clear_retry_flags(b);

  auto c = static_cast<Client *>(BIO_get_data(b));

  len = c->read_server_handshake(reinterpret_cast<uint8_t *>(buf), len);
  if (len == 0) {
    BIO_set_retry_read(b);
    return -1;
  }

  return len;
}
} // namespace

namespace {
int bio_puts(BIO *b, const char *str) { return bio_write(b, str, strlen(str)); }
} // namespace

namespace {
int bio_gets(BIO *b, char *buf, int len) { return -1; }
} // namespace

namespace {
long bio_ctrl(BIO *b, int cmd, long num, void *ptr) {
  switch (cmd) {
  case BIO_CTRL_FLUSH:
    return 1;
  }

  return 0;
}
} // namespace

namespace {
int bio_create(BIO *b) {
  BIO_set_init(b, 1);
  return 1;
}
} // namespace

namespace {
int bio_destroy(BIO *b) {
  if (b == nullptr) {
    return 0;
  }

  return 1;
}
} // namespace

namespace {
BIO_METHOD *create_bio_method() {
  static auto meth = BIO_meth_new(BIO_TYPE_FD, "bio");
  BIO_meth_set_write(meth, bio_write);
  BIO_meth_set_read(meth, bio_read);
  BIO_meth_set_puts(meth, bio_puts);
  BIO_meth_set_gets(meth, bio_gets);
  BIO_meth_set_ctrl(meth, bio_ctrl);
  BIO_meth_set_create(meth, bio_create);
  BIO_meth_set_destroy(meth, bio_destroy);
  return meth;
}
} // namespace

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  ev_io_stop(loop, w);

  auto c = static_cast<Client *>(w->data);

  auto rv = c->on_write();
  switch (rv) {
  case 0:
    return;
  case NETWORK_ERR_SEND_NON_FATAL:
    c->start_wev();
    return;
  }
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
 
 	auto c = static_cast<Client *>(w->data);

  	if (c->on_read() != 0) {
    	return;
  	}

  	auto rv = c->on_write();
  	sleep(1);
	switch (rv) {
			
  		case 0:
    		return;
  		case NETWORK_ERR_SEND_NON_FATAL:
			c->start_wev();
    		return;
  }
}
} // namespace

namespace {
void stdin_readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (c->send_interactive_input()) {
		// printf("Disconnecting\n");
        // c->disconnect();
  }
}

void stdin_ovsdb_readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (c->send_ovsdb_interactive_input()) {
        // printf("Disconnecting\n");
        // c->disconnect();
  }
}

} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (!config.quiet) {
    std::cerr << "Timeout" << std::endl;
  }


  printf("Disconnection point 1\n");
  c->disconnect();
}
} // namespace

namespace {
void retransmitcb(struct ev_loop *loop, ev_timer *w, int revents) {
  int rv;
  auto c = static_cast<Client *>(w->data);
  auto conn = c->conn();
  auto now = util::timestamp(loop);

  if (ngtcp2_conn_loss_detection_expiry(conn) <= now) {
    rv = c->on_write(true);
    if (rv != 0) {
      goto fail;
    }
  }

  if (ngtcp2_conn_ack_delay_expiry(conn) <= now) {
    rv = c->on_write();
    if (rv != 0) {
      goto fail;
    }
  }

  return;

fail:
  switch (rv) {
  case NETWORK_ERR_SEND_NON_FATAL:
    c->start_wev();
    return;
  default:
	printf("Disconnect point 2\n");
    c->disconnect();
    return;
  }
}
} // namespace

namespace {
void siginthandler(struct ev_loop *loop, ev_signal *w, int revents) {
  ev_break(loop, EVBREAK_ALL);
}
} // namespace

Client::Client(struct ev_loop *loop, SSL_CTX *ssl_ctx, FILE *sockfd)
    : remote_addr_{},
      max_pktlen_(0),
      loop_(loop),
      ssl_ctx_(ssl_ctx),
      ssl_(nullptr),
      fd_(-1),
      datafd_(-1),
	  datafd_ovsdb(-1),
      chandshake_idx_(0),
      tx_crypto_offset_(0),
      nsread_(0),
      conn_(nullptr),
      crypto_ctx_{},
      sendbuf_{NGTCP2_MAX_PKTLEN_IPV4},
      last_stream_id_(0),
      nstreams_done_(0),
      resumption_(false) {
  ev_io_init(&wev_, writecb, 0, EV_WRITE);
  ev_io_init(&rev_, readcb, 0, EV_READ);
  ev_io_init(&stdinrev_, stdin_readcb, 0, EV_READ);
  ev_io_init(&ovsdb_, stdin_ovsdb_readcb, 0, EV_READ);
  wev_.data = this;
  rev_.data = this;
  stdinrev_.data = this;
  ovsdb_.data = this;
  ev_timer_init(&timer_, timeoutcb, 0., config.timeout);
  timer_.data = this;
  ev_timer_init(&rttimer_, retransmitcb, 0., 0.);
  rttimer_.data = this;
  ev_signal_init(&sigintev_, siginthandler, SIGINT);
}

Client::~Client() {
  printf("Disconnection point 4\n");
  disconnect();
  close();
}

void Client::disconnect() { disconnect(0); }

void Client::disconnect(int liberr) {
  
	config.tx_loss_prob = 0;

	ev_timer_stop(loop_, &rttimer_);
	ev_timer_stop(loop_, &timer_);

	if(mode == 1){
		ev_io_stop(loop_, &stdinrev_);
	}
	else if(mode == 2){
		ev_io_stop(loop_, &ovsdb_);
	}
	else if(mode == 3){
		ev_io_stop(loop_, &stdinrev_);
		ev_io_stop(loop_, &ovsdb_);
	}

	ev_io_stop(loop_, &rev_);
	ev_signal_stop(loop_, &sigintev_);
	handle_error(liberr);
}

void Client::close() {
  ev_io_stop(loop_, &wev_);

  if (conn_) {
    ngtcp2_conn_del(conn_);
    conn_ = nullptr;
  }

  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }

  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
}

namespace {
int client_initial(ngtcp2_conn *conn, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->tls_handshake(true) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int recv_crypto_data(ngtcp2_conn *conn, uint64_t offset, const uint8_t *data,
                     size_t datalen, void *user_data) {
  if (!config.quiet) {
    debug::print_crypto_data(data, datalen);
  }

  auto c = static_cast<Client *>(user_data);

  c->write_server_handshake(data, datalen);

  if (!ngtcp2_conn_get_handshake_completed(c->conn()) &&
      c->tls_handshake() != 0) {
    return NGTCP2_ERR_CRYPTO;
  }

  // SSL_do_handshake() might not consume all data (e.g.,
  // NewSessionTicket).
  return c->read_tls();
}
} // namespace

namespace {

int recv_stream_data(ngtcp2_conn *conn, uint64_t stream_id, uint8_t fin,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data, void *stream_user_data) {

	socklen_t len;
	len = sizeof(openflow_cliaddr);

    printf("Stream**** id is %d\n", stream_id);
  

 
//	if(stream_id % 2 != 0){
		if(stream_id % 3 == 0){
			 sendto(ofl_sock, (const char *)data, datalen,  
            		MSG_CONFIRM, (const struct sockaddr *) &openflow_cliaddr, 
            		len);
		}
		else{
			printf("Coming to sendto ovsdb\n");
			sendto(ovsdb_sock, (const char *)data, datalen,  
					MSG_CONFIRM, (const struct sockaddr *) &ovsdb_cliaddr, 
					len);
		}

//	} 

/*
	sendto(ofl_sock, (const char *)data, datalen,  
                    MSG_CONFIRM, (const struct sockaddr *) &openflow_cliaddr, 
                    len);
*/
	// debug::print_stream_data(stream_id, data, datalen);

  	if (!config.quiet) {
    	debug::print_stream_data(stream_id, data, datalen);
  	}
 
	printf("After sending packet\n");
 
	ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
  	ngtcp2_conn_extend_max_offset(conn, datalen);

	return 0;
}
} // namespace

namespace {
int acked_crypto_offset(ngtcp2_conn *conn, uint64_t offset, size_t datalen,
                        void *user_data) {
  auto c = static_cast<Client *>(user_data);
  c->remove_tx_crypto_data(offset, datalen);

  return 0;
}
} // namespace

namespace {
int acked_stream_data_offset(ngtcp2_conn *conn, uint64_t stream_id,
                             uint64_t offset, size_t datalen, void *user_data,
                             void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->remove_tx_stream_data(stream_id, offset, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
  if (!config.quiet) {
    debug::handshake_completed(conn, user_data);
  }

  return 0;
}
} // namespace

namespace {
int recv_server_stateless_retry(ngtcp2_conn *conn, void *user_data) {
  // Re-generate handshake secrets here because connection ID might
  // change.
  auto c = static_cast<Client *>(user_data);

  c->setup_initial_crypto_context();

  return 0;
}
} // namespace

namespace {
int stream_close(ngtcp2_conn *conn, uint64_t stream_id, uint16_t app_error_code,
                 void *user_data, void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);

  printf("Stream closed!!!!!\n");

  c->on_stream_close(stream_id);

  return 0;
}
} // namespace

namespace {
int extend_max_stream_id(ngtcp2_conn *conn, uint64_t max_stream_id,
                         void *user_data) {
  
	auto c = static_cast<Client *>(user_data);

	if(mode == 1){
		if (c->on_extend_max_stream_id(max_stream_id, OFL) != 0) {
    		return NGTCP2_ERR_CALLBACK_FAILURE;
  		}
  	}
	else if(mode == 2){
		if (c->on_extend_max_stream_id(max_stream_id, OVSDB) != 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
	}
	else if(mode == 3) {
		if (c->on_extend_max_stream_id(max_stream_id, OFL) != 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
		if (c->on_extend_max_stream_id(max_stream_id, OVSDB) != 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
	}

  return 0;
}

} // namespace

namespace {
ssize_t do_hs_encrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                      const uint8_t *plaintext, size_t plaintextlen,
                      const uint8_t *key, size_t keylen, const uint8_t *nonce,
                      size_t noncelen, const uint8_t *ad, size_t adlen,
                      void *user_data) {
  auto c = static_cast<Client *>(user_data);

  auto nwrite = c->hs_encrypt_data(dest, destlen, plaintext, plaintextlen, key,
                                   keylen, nonce, noncelen, ad, adlen);
  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}
} // namespace

namespace {
ssize_t do_hs_decrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                      const uint8_t *ciphertext, size_t ciphertextlen,
                      const uint8_t *key, size_t keylen, const uint8_t *nonce,
                      size_t noncelen, const uint8_t *ad, size_t adlen,
                      void *user_data) {
  auto c = static_cast<Client *>(user_data);

  auto nwrite = c->hs_decrypt_data(dest, destlen, ciphertext, ciphertextlen,
                                   key, keylen, nonce, noncelen, ad, adlen);
  if (nwrite < 0) {
    return NGTCP2_ERR_TLS_DECRYPT;
  }

  return nwrite;
}
} // namespace

namespace {
ssize_t do_encrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                   const uint8_t *plaintext, size_t plaintextlen,
                   const uint8_t *key, size_t keylen, const uint8_t *nonce,
                   size_t noncelen, const uint8_t *ad, size_t adlen,
                   void *user_data) {
  auto c = static_cast<Client *>(user_data);

  auto nwrite = c->encrypt_data(dest, destlen, plaintext, plaintextlen, key,
                                keylen, nonce, noncelen, ad, adlen);
  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}
} // namespace

namespace {
ssize_t do_decrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                   const uint8_t *ciphertext, size_t ciphertextlen,
                   const uint8_t *key, size_t keylen, const uint8_t *nonce,
                   size_t noncelen, const uint8_t *ad, size_t adlen,
                   void *user_data) {
  auto c = static_cast<Client *>(user_data);

  auto nwrite = c->decrypt_data(dest, destlen, ciphertext, ciphertextlen, key,
                                keylen, nonce, noncelen, ad, adlen);
  if (nwrite < 0) {
    return NGTCP2_ERR_TLS_DECRYPT;
  }

  return nwrite;
}
} // namespace

namespace {
ssize_t do_hs_encrypt_pn(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                         const uint8_t *plaintext, size_t plaintextlen,
                         const uint8_t *key, size_t keylen,
                         const uint8_t *nonce, size_t noncelen,
                         void *user_data) {
  auto c = static_cast<Client *>(user_data);

  auto nwrite = c->hs_encrypt_pn(dest, destlen, plaintext, plaintextlen, key,
                                 keylen, nonce, noncelen);
  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}
} // namespace

namespace {
ssize_t do_encrypt_pn(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                      const uint8_t *plaintext, size_t plaintextlen,
                      const uint8_t *key, size_t keylen, const uint8_t *nonce,
                      size_t noncelen, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  auto nwrite = c->encrypt_pn(dest, destlen, plaintext, plaintextlen, key,
                              keylen, nonce, noncelen);
  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}
} // namespace

int Client::init(int fd, const Address &remote_addr, const char *addr,
                 int datafd, int d_ovsdb, uint32_t version) {
 
	int rv;
	remote_addr_ = remote_addr;

	switch (remote_addr_.su.storage.ss_family) {
		case AF_INET:
			max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV4;
			break;
  
		case AF_INET6:
			max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV6;
			break;
		
		default:
			return -1;
	}

	fd_ = fd;
	datafd_ = datafd;
	datafd_ovsdb = d_ovsdb;

	if (-1 == connect(fd_, &remote_addr_.su.sa, remote_addr_.len)) {
		std::cerr << "connect: " << strerror(errno) << std::endl;
		return -1;
	}

	ssl_ = SSL_new(ssl_ctx_);
	auto bio = BIO_new(create_bio_method());
	BIO_set_data(bio, this);
	SSL_set_bio(ssl_, bio, bio);
	SSL_set_app_data(ssl_, this);
	SSL_set_connect_state(ssl_);
	SSL_set_msg_callback(ssl_, msg_cb);
	SSL_set_msg_callback_arg(ssl_, this);
	SSL_set_key_callback(ssl_, key_cb, this);

	const uint8_t *alpn = nullptr;
	size_t alpnlen;

	switch (version) {
		case NGTCP2_PROTO_VER_D13:
			alpn = reinterpret_cast<const uint8_t *>(NGTCP2_ALPN_D13);
			alpnlen = str_size(NGTCP2_ALPN_D13);
			break;
	}
  
	if (alpn) 
		SSL_set_alpn_protos(ssl_, alpn, alpnlen);

	if (util::numeric_host(addr)) {
		// If remote host is numeric address, just send "localhost" as SNI
		// for now.
		SSL_set_tlsext_host_name(ssl_, "localhost");
	} else {
		SSL_set_tlsext_host_name(ssl_, addr);
	}

	if (config.session_file) {
		auto f = BIO_new_file(config.session_file, "r");
		if (f == nullptr) {
			std::cerr << "Could not read TLS session file " << config.session_file
					<< std::endl;
		} else {
			auto session = PEM_read_bio_SSL_SESSION(f, nullptr, 0, nullptr);
			BIO_free(f);
      		if (session == nullptr) {
        		std::cerr << "Could not read TLS session file " << config.session_file
                  			<< std::endl;
			} else {
				if (!SSL_set_session(ssl_, session)) {
          			std::cerr << "Could not set session" << std::endl;
        		} else {
          			resumption_ = true;
        		}
        		SSL_SESSION_free(session);
      		}
    	}
  	}

  	auto callbacks = ngtcp2_conn_callbacks{
      				client_initial,
      				nullptr, // recv_client_initial
      				recv_crypto_data,
      				handshake_completed,
      				nullptr, // recv_version_negotiation
      				do_hs_encrypt,
      				do_hs_decrypt,
      				do_encrypt,
      				do_decrypt,
      				do_hs_encrypt_pn,
      				do_encrypt_pn,
      				recv_stream_data,
      				acked_crypto_offset,
      				acked_stream_data_offset,
      				stream_close,
      				nullptr, // recv_stateless_reset,
      				recv_server_stateless_retry,
      				extend_max_stream_id,
  	};

  	auto dis = std::uniform_int_distribution<uint8_t>(
      			0, std::numeric_limits<uint8_t>::max());

  	ngtcp2_cid scid, dcid;
  	scid.datalen = 17;
  	std::generate(std::begin(scid.data), std::begin(scid.data) + scid.datalen,
                		[&dis]() { return dis(randgen); });
  	dcid.datalen = 18;
  	std::generate(std::begin(dcid.data), std::begin(dcid.data) + dcid.datalen,
                		[&dis]() { return dis(randgen); });

  	ngtcp2_settings settings{};
  	settings.log_printf = config.quiet ? nullptr : debug::log_printf;
  	settings.initial_ts = util::timestamp(loop_);
  	settings.max_stream_data = 256_k;
  	settings.max_data = 1_m;
  	settings.max_bidi_streams = 10000;
  	settings.max_uni_streams = 10000;
  	settings.idle_timeout = config.timeout;
  	settings.max_packet_size = NGTCP2_MAX_PKT_SIZE;
  	settings.ack_delay_exponent = NGTCP2_DEFAULT_ACK_DELAY_EXPONENT;

  	rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid, version, &callbacks,
                              	&settings, this);
  	if (rv != 0) {
    	std::cerr << "ngtcp2_conn_client_new: " << ngtcp2_strerror(rv) << std::endl;
    	return -1;
  	}

  	rv = setup_initial_crypto_context();
  		if (rv != 0) {
    		return -1;
  		}

  	ev_io_set(&wev_, fd_, EV_WRITE);
  	ev_io_set(&rev_, fd_, EV_READ);

  	ev_io_start(loop_, &rev_);
  	ev_timer_again(loop_, &timer_);

  	ev_signal_start(loop_, &sigintev_);

  	return 0;
}

int Client::setup_initial_crypto_context() {
  int rv;

  std::array<uint8_t, 32> initial_secret, secret;
  auto dcid = ngtcp2_conn_get_dcid(conn_);
  rv = crypto::derive_initial_secret(
      initial_secret.data(), initial_secret.size(), dcid,
      reinterpret_cast<const uint8_t *>(NGTCP2_INITIAL_SALT),
      str_size(NGTCP2_INITIAL_SALT));
  if (rv != 0) {
    std::cerr << "crypto::derive_initial_secret() failed" << std::endl;
    return -1;
  }

  if (!config.quiet && config.show_secret) {
    debug::print_initial_secret(initial_secret.data(), initial_secret.size());
  }

  crypto::prf_sha256(hs_crypto_ctx_);
  crypto::aead_aes_128_gcm(hs_crypto_ctx_);

  rv = crypto::derive_client_initial_secret(secret.data(), secret.size(),
                                            initial_secret.data(),
                                            initial_secret.size());
  if (rv != 0) {
    std::cerr << "crypto::derive_client_initial_secret() failed" << std::endl;
    return -1;
  }

  std::array<uint8_t, 16> key, iv, pn;

  auto keylen = crypto::derive_packet_protection_key(
      key.data(), key.size(), secret.data(), secret.size(), hs_crypto_ctx_);
  if (keylen < 0) {
    return -1;
  }

  auto ivlen = crypto::derive_packet_protection_iv(
      iv.data(), iv.size(), secret.data(), secret.size(), hs_crypto_ctx_);
  if (ivlen < 0) {
    return -1;
  }

  auto pnlen = crypto::derive_pkt_num_protection_key(
      pn.data(), pn.size(), secret.data(), secret.size(), hs_crypto_ctx_);
  if (pnlen < 0) {
    return -1;
  }

  if (!config.quiet && config.show_secret) {
    debug::print_client_in_secret(secret.data(), secret.size());
    debug::print_client_pp_key(key.data(), keylen);
    debug::print_client_pp_iv(iv.data(), ivlen);
    debug::print_client_pp_pn(pn.data(), pnlen);
  }

  ngtcp2_conn_set_initial_tx_keys(conn_, key.data(), keylen, iv.data(), ivlen,
                                  pn.data(), pnlen);

  rv = crypto::derive_server_initial_secret(secret.data(), secret.size(),
                                            initial_secret.data(),
                                            initial_secret.size());
  if (rv != 0) {
    std::cerr << "crypto::derive_server_initial_secret() failed" << std::endl;
    return -1;
  }

  keylen = crypto::derive_packet_protection_key(
      key.data(), key.size(), secret.data(), secret.size(), hs_crypto_ctx_);
  if (keylen < 0) {
    return -1;
  }

  ivlen = crypto::derive_packet_protection_iv(
      iv.data(), iv.size(), secret.data(), secret.size(), hs_crypto_ctx_);
  if (ivlen < 0) {
    return -1;
  }

  pnlen = crypto::derive_pkt_num_protection_key(
      pn.data(), pn.size(), secret.data(), secret.size(), hs_crypto_ctx_);
  if (pnlen < 0) {
    return -1;
  }

  if (!config.quiet && config.show_secret) {
    debug::print_server_in_secret(secret.data(), secret.size());
    debug::print_server_pp_key(key.data(), keylen);
    debug::print_server_pp_iv(iv.data(), ivlen);
    debug::print_server_pp_pn(pn.data(), pnlen);
  }

  ngtcp2_conn_set_initial_rx_keys(conn_, key.data(), keylen, iv.data(), ivlen,
                                  pn.data(), pnlen);

  return 0;
}

int Client::tls_handshake(bool initial) {
  ERR_clear_error();

  int rv;
  /* Note that SSL_SESSION_get_max_early_data() and
     SSL_get_max_early_data() return completely different value. */
  if (initial && resumption_ &&
      SSL_SESSION_get_max_early_data(SSL_get_session(ssl_))) {
    size_t nwrite;
    // OpenSSL returns error if SSL_write_early_data is called when
    // resumption is not attempted.  Sending empty string is a trick
    // to just early_data extension.
    rv = SSL_write_early_data(ssl_, "", 0, &nwrite);
    if (rv == 0) {
      auto err = SSL_get_error(ssl_, rv);
      switch (err) {
      case SSL_ERROR_SSL:
        std::cerr << "TLS handshake error: "
                  << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        return -1;
      default:
        std::cerr << "TLS handshake error: " << err << std::endl;
        return -1;
      }
    }
  }

  rv = SSL_do_handshake(ssl_);
  if (rv <= 0) {
    auto err = SSL_get_error(ssl_, rv);
    switch (err) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return 0;
    case SSL_ERROR_SSL:
      std::cerr << "TLS handshake error: "
                << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
      return -1;
    default:
      std::cerr << "TLS handshake error: " << err << std::endl;
      return -1;
    }
  }

  // SSL_get_early_data_status works after handshake completes.
  if (resumption_ &&
      SSL_get_early_data_status(ssl_) != SSL_EARLY_DATA_ACCEPTED) {
    std::cerr << "Early data was rejected by server" << std::endl;
    ngtcp2_conn_early_data_rejected(conn_);
  }

  ngtcp2_conn_handshake_completed(conn_);

  if (!config.quiet) {
    std::cerr << "Negotiated cipher suite is " << SSL_get_cipher_name(ssl_)
              << std::endl;

    const unsigned char *alpn = nullptr;
    unsigned int alpnlen;

    SSL_get0_alpn_selected(ssl_, &alpn, &alpnlen);
    if (alpn) {
      std::cerr << "Negotiated ALPN is ";
      std::cerr.write(reinterpret_cast<const char *>(alpn), alpnlen);
      std::cerr << std::endl;
    }
  }

  return 0;
}

int Client::read_tls() {
  ERR_clear_error();

  std::array<uint8_t, 4096> buf;
  size_t nread;

  for (;;) {
    auto rv = SSL_read_ex(ssl_, buf.data(), buf.size(), &nread);
    if (rv == 1) {
      std::cerr << "Read " << nread << " bytes from TLS crypto stream"
                << std::endl;
      continue;
    }
    auto err = SSL_get_error(ssl_, 0);
    switch (err) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return 0;
    case SSL_ERROR_SSL:
    case SSL_ERROR_ZERO_RETURN:
      std::cerr << "TLS read error: "
                << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
      return NGTCP2_ERR_CRYPTO;
    default:
      std::cerr << "TLS read error: " << err << std::endl;
      return NGTCP2_ERR_CRYPTO;
    }
  }
}

int Client::feed_data(uint8_t *data, size_t datalen) {
  int rv;

  if (ngtcp2_conn_get_handshake_completed(conn_)) {
    rv = ngtcp2_conn_recv(conn_, data, datalen, util::timestamp(loop_));
    if (rv != 0) {
      std::cerr << "ngtcp2_conn_recv: " << ngtcp2_strerror(rv) << std::endl;
      if (rv != NGTCP2_ERR_TLS_DECRYPT) {
		printf("Disconnect point 5\n");
        disconnect(rv);
        return -1;
      }
    }
  } else {
    return do_handshake(data, datalen);
  }

  return 0;
}

ssize_t Client::do_handshake_once(const uint8_t *data, size_t datalen) {
  auto nwrite = ngtcp2_conn_handshake(conn_, sendbuf_.wpos(), max_pktlen_, data,
                                      datalen, util::timestamp(loop_));
  if (nwrite < 0) {
    switch (nwrite) {
    case NGTCP2_ERR_TLS_DECRYPT:
      std::cerr << "ngtcp2_conn_handshake: " << ngtcp2_strerror(nwrite)
                << std::endl;
    case NGTCP2_ERR_NOBUF:
    case NGTCP2_ERR_CONGESTION:
      return 0;
    }

    std::cerr << "ngtcp2_conn_handshake: " << ngtcp2_strerror(nwrite)
              << std::endl;
	printf("Disconnect point 6\n");
    disconnect(nwrite);
    return -1;
  }

  if (nwrite == 0) {
    return 0;
  }

  sendbuf_.push(nwrite);

  auto rv = send_packet();
  if (rv == NETWORK_ERR_SEND_NON_FATAL) {
    schedule_retransmit();
    return rv;
  }
  if (rv != NETWORK_ERR_OK) {
    return rv;
  }

  return nwrite;
}

int Client::do_handshake(const uint8_t *data, size_t datalen) {
  ssize_t nwrite;

  if (sendbuf_.size() > 0) {
    auto rv = send_packet();
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }
  }

  nwrite = do_handshake_once(data, datalen);
  if (nwrite < 0) {
    return nwrite;
  }
  if (nwrite == 0) {
    return 0;
  }

  // For 0-RTT
  auto rv = write_0rtt_streams();
  if (rv != 0) {
    return rv;
  }

  for (;;) {
    nwrite = do_handshake_once(nullptr, 0);
    if (nwrite < 0) {
      return nwrite;
    }
    if (nwrite == 0) {
      return 0;
    }
  }
}

int Client::on_read() {
  std::array<uint8_t, 65536> buf;

  for (;;) {
    auto nread =
        recvfrom(fd_, buf.data(), buf.size(), MSG_DONTWAIT, nullptr, nullptr);

    if (nread == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "recvfrom: " << strerror(errno) << std::endl;
      }
      break;
    }

    if (debug::packet_lost(config.rx_loss_prob)) {
      if (!config.quiet) {
        std::cerr << "** Simulated incoming packet loss **" << std::endl;
      }
      break;
    }

    if (feed_data(buf.data(), nread) != 0) {
      return -1;
    }
  }

  ev_timer_again(loop_, &timer_);

  return 0;
}

int Client::on_write(bool retransmit) {
  if (sendbuf_.size() > 0) {
    auto rv = send_packet();
    if (rv != NETWORK_ERR_OK) {
      if (rv != NETWORK_ERR_SEND_NON_FATAL) {
        disconnect(NGTCP2_ERR_INTERNAL);
		printf("Disconnect point 7\n");
      }
      return rv;
    }
  }

  assert(sendbuf_.left() >= max_pktlen_);

  if (retransmit) {
    auto rv =
        ngtcp2_conn_on_loss_detection_alarm(conn_, util::timestamp(loop_));
    if (rv != 0) {
      std::cerr << "ngtcp2_conn_on_loss_detection_alarm: "
                << ngtcp2_strerror(rv) << std::endl;
		printf("Disconnect point 8\n");
      disconnect(NGTCP2_ERR_INTERNAL);
      return -1;
    }
  }

  if (!ngtcp2_conn_get_handshake_completed(conn_)) {
    auto rv = do_handshake(nullptr, 0);
    schedule_retransmit();
    return rv;
  }

  for (;;) {
    auto n = ngtcp2_conn_write_pkt(conn_, sendbuf_.wpos(), max_pktlen_,
                                   util::timestamp(loop_));
    if (n < 0) {
      if (n == NGTCP2_ERR_NOBUF || n == NGTCP2_ERR_CONGESTION) {
        break;
      }
      std::cerr << "ngtcp2_conn_write_pkt: " << ngtcp2_strerror(n) << std::endl;
	  printf("Disconnect point 9\n");
      disconnect(n);
      return -1;
    }
    if (n == 0) {
      break;
    }

    sendbuf_.push(n);

    auto rv = send_packet();
    if (rv == NETWORK_ERR_SEND_NON_FATAL) {
      schedule_retransmit();
      return rv;
    }
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }
  }

  if (!retransmit) {
    auto rv = write_streams();
    if (rv != 0) {
      return rv;
    }
  }

  schedule_retransmit();
  return 0;
}

int Client::write_streams() {
  for (auto &p : streams_) {
    auto &stream = p.second;
    auto &streambuf = stream->streambuf;
    auto &streambuf_idx = stream->streambuf_idx;

    for (auto it = std::begin(streambuf) + streambuf_idx;
         it != std::end(streambuf); ++it) {
      auto &v = *it;
      auto fin = stream->should_send_fin && it + 1 == std::end(streambuf);
      auto rv = on_write_stream(stream->stream_id, fin, v);
      if (rv != 0) {
        if (rv == NETWORK_ERR_SEND_NON_FATAL) {
          schedule_retransmit();
          return 0;
        }
        return rv;
      }
      if (v.size() > 0) {
        break;
      }
      ++streambuf_idx;
    }
  }

  return 0;
}

int Client::on_write_stream(uint64_t stream_id, uint8_t fin, Buffer &data) {
  ssize_t ndatalen;

  for (;;) {
    auto n = ngtcp2_conn_write_stream(conn_, sendbuf_.wpos(), max_pktlen_,
                                      &ndatalen, stream_id, fin, data.rpos(),
                                      data.size(), util::timestamp(loop_));
    if (n < 0) {
      switch (n) {
      case NGTCP2_ERR_EARLY_DATA_REJECTED:
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
      case NGTCP2_ERR_STREAM_SHUT_WR:
      case NGTCP2_ERR_STREAM_NOT_FOUND: // This means that stream is
                                        // closed.
      case NGTCP2_ERR_NOBUF:
      case NGTCP2_ERR_CONGESTION:
        return 0;
      }
      std::cerr << "ngtcp2_conn_write_stream: " << ngtcp2_strerror(n)
                << std::endl;
		printf("Disconnect point 11\n");
      disconnect(n);
      return -1;
    }

    if (n == 0) {
      return 0;
    }

    if (ndatalen > 0) {
      data.seek(ndatalen);
    }

    sendbuf_.push(n);

    auto rv = send_packet();
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }

    if (data.size() == 0) {
      break;
    }
  }

  return 0;
}

int Client::write_0rtt_streams() {
  for (auto &p : streams_) {
    auto &stream = p.second;
    auto &streambuf = stream->streambuf;
    auto &streambuf_idx = stream->streambuf_idx;

    for (auto it = std::begin(streambuf) + streambuf_idx;
         it != std::end(streambuf); ++it) {
      auto &v = *it;
      auto fin = stream->should_send_fin && it + 1 == std::end(streambuf);
      auto rv = on_write_0rtt_stream(stream->stream_id, fin, v);
      if (rv != 0) {
        if (rv == NETWORK_ERR_SEND_NON_FATAL) {
          schedule_retransmit();
          return 0;
        }
        return rv;
      }
      if (v.size() > 0) {
        break;
      }
      ++streambuf_idx;
    }
  }

  return 0;
}

int Client::on_write_0rtt_stream(uint64_t stream_id, uint8_t fin,
                                 Buffer &data) {
  ssize_t ndatalen;

  for (;;) {
    auto n = ngtcp2_conn_client_handshake(
        conn_, sendbuf_.wpos(), max_pktlen_, &ndatalen, nullptr, 0, stream_id,
        fin, data.rpos(), data.size(), util::timestamp(loop_));
    if (n < 0) {
      switch (n) {
      case NGTCP2_ERR_EARLY_DATA_REJECTED:
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
      case NGTCP2_ERR_STREAM_SHUT_WR:
      case NGTCP2_ERR_STREAM_NOT_FOUND: // This means that stream is
                                        // closed.
      case NGTCP2_ERR_NOBUF:
      case NGTCP2_ERR_CONGESTION:
        return 0;
      }
      std::cerr << "ngtcp2_conn_client_handshake: " << ngtcp2_strerror(n)
                << std::endl;
		printf("Disconnect point 13\n");
      disconnect(n);
      return -1;
    }

    if (n == 0) {
      return 0;
    }

    if (ndatalen > 0) {
      data.seek(ndatalen);
    }

    sendbuf_.push(n);

    auto rv = send_packet();
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }

    if (data.size() == 0) {
      break;
    }
  }

  return 0;
}

void Client::schedule_retransmit() {
  auto expiry = std::min(ngtcp2_conn_loss_detection_expiry(conn_),
                         ngtcp2_conn_ack_delay_expiry(conn_));

  auto now = util::timestamp(loop_);
  auto t =
      expiry < now ? 1e-9 : static_cast<ev_tstamp>(expiry - now) / 1000000000;
  rttimer_.repeat = t;
  ev_timer_again(loop_, &rttimer_);
}

int Client::write_client_handshake(const uint8_t *data, size_t datalen) {
  write_client_handshake(chandshake_, chandshake_idx_, data, datalen);

  return 0;
}

void Client::write_client_handshake(std::deque<Buffer> &dest, size_t &idx,
                                    const uint8_t *data, size_t datalen) {
  dest.emplace_back(data, datalen);
  ++idx;

  auto &buf = dest.back();

  ngtcp2_conn_submit_crypto_data(conn_, buf.rpos(), buf.size());
}

size_t Client::read_client_handshake(const uint8_t **pdest) {
  if (chandshake_idx_ == chandshake_.size()) {
    return 0;
  }
  const auto &v = chandshake_[chandshake_idx_++];
  *pdest = v.rpos();
  return v.size();
}

size_t Client::read_server_handshake(uint8_t *buf, size_t buflen) {
  auto n = std::min(buflen, shandshake_.size() - nsread_);
  std::copy_n(std::begin(shandshake_) + nsread_, n, buf);
  nsread_ += n;
  return n;
}

void Client::write_server_handshake(const uint8_t *data, size_t datalen) {
  std::copy_n(data, datalen, std::back_inserter(shandshake_));
}

ssize_t Client::hs_encrypt_data(uint8_t *dest, size_t destlen,
                                const uint8_t *plaintext, size_t plaintextlen,
                                const uint8_t *key, size_t keylen,
                                const uint8_t *nonce, size_t noncelen,
                                const uint8_t *ad, size_t adlen) {
  return crypto::encrypt(dest, destlen, plaintext, plaintextlen, hs_crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

ssize_t Client::hs_decrypt_data(uint8_t *dest, size_t destlen,
                                const uint8_t *ciphertext, size_t ciphertextlen,
                                const uint8_t *key, size_t keylen,
                                const uint8_t *nonce, size_t noncelen,
                                const uint8_t *ad, size_t adlen) {
  return crypto::decrypt(dest, destlen, ciphertext, ciphertextlen,
                         hs_crypto_ctx_, key, keylen, nonce, noncelen, ad,
                         adlen);
}

ssize_t Client::encrypt_data(uint8_t *dest, size_t destlen,
                             const uint8_t *plaintext, size_t plaintextlen,
                             const uint8_t *key, size_t keylen,
                             const uint8_t *nonce, size_t noncelen,
                             const uint8_t *ad, size_t adlen) {
  return crypto::encrypt(dest, destlen, plaintext, plaintextlen, crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

ssize_t Client::decrypt_data(uint8_t *dest, size_t destlen,
                             const uint8_t *ciphertext, size_t ciphertextlen,
                             const uint8_t *key, size_t keylen,
                             const uint8_t *nonce, size_t noncelen,
                             const uint8_t *ad, size_t adlen) {
  return crypto::decrypt(dest, destlen, ciphertext, ciphertextlen, crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

ssize_t Client::hs_encrypt_pn(uint8_t *dest, size_t destlen,
                              const uint8_t *ciphertext, size_t ciphertextlen,
                              const uint8_t *key, size_t keylen,
                              const uint8_t *nonce, size_t noncelen) {
  return crypto::encrypt_pn(dest, destlen, ciphertext, ciphertextlen,
                            hs_crypto_ctx_, key, keylen, nonce, noncelen);
}

ssize_t Client::encrypt_pn(uint8_t *dest, size_t destlen,
                           const uint8_t *ciphertext, size_t ciphertextlen,
                           const uint8_t *key, size_t keylen,
                           const uint8_t *nonce, size_t noncelen) {
  return crypto::encrypt_pn(dest, destlen, ciphertext, ciphertextlen,
                            crypto_ctx_, key, keylen, nonce, noncelen);
}

ngtcp2_conn *Client::conn() const { return conn_; }

int Client::send_packet() {
  if (debug::packet_lost(config.tx_loss_prob)) {
    if (!config.quiet) {
      std::cerr << "** Simulated outgoing packet loss **" << std::endl;
    }
    sendbuf_.reset();
    return NETWORK_ERR_OK;
  }

  int eintr_retries = 5;
  ssize_t nwrite = 0;

  do {
    nwrite = send(fd_, sendbuf_.rpos(), sendbuf_.size(), 0);
  } while ((nwrite == -1) && (errno == EINTR) && (eintr_retries-- > 0));

  if (nwrite == -1) {
    switch (errno) {
    case EAGAIN:
    case EINTR:
    case 0:
      return NETWORK_ERR_SEND_NON_FATAL;
    default:
      std::cerr << "send: " << strerror(errno) << std::endl;
      return NETWORK_ERR_SEND_FATAL;
    }
  }

  assert(static_cast<size_t>(nwrite) == sendbuf_.size());
  sendbuf_.reset();

  return NETWORK_ERR_OK;
}

int Client::start_interactive_input() {
  
	int rv;
	uint64_t stream_id;

  	std::cerr << "OpenFlow Interactive session started.  Hit Ctrl-D to end the session."
			<< std::endl;

	ev_io_set(&stdinrev_, datafd_, EV_READ);
	ev_io_start(loop_, &stdinrev_);

	rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, 1, OFL);
	if (rv != 0) {
    	std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
              	<< std::endl;
		if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
      		return 0;
    	}
    	return -1;
  	}

	std::cerr << "The stream for openflow " << stream_id << " has opened." << std::endl;
	
	last_stream_id_ = stream_id;
	auto stream = std::make_unique<Stream>(stream_id);
	streams_.emplace(stream_id, std::move(stream));
	return 0;
}

int Client::start_ovsdb_interactive_input() {

    int rv;
    uint64_t stream_id;

    std::cerr << "OVSDB Interactive session started.  Hit Ctrl-D to end the session."
            << std::endl;

	printf("DATAFD is %d\n", datafd_ovsdb);
    ev_io_set(&ovsdb_, datafd_ovsdb, EV_READ);
    ev_io_start(loop_, &ovsdb_);

    rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, 1, OVSDB);
    if (rv != 0) {
        std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
                << std::endl;
        if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
            return 0;
        }
        return -1;
    }

    std::cerr << "The stream for ovsdb " << stream_id << " has opened." << std::endl;

    last_stream_id_ = stream_id;
    auto stream = std::make_unique<Stream>(stream_id);
    streams_.emplace(stream_id, std::move(stream));
    return 0;
}


int Client::send_interactive_input() {
  
	ssize_t nread;
  	std::array<uint8_t, 1_k> buf;
	int len; 
  
    len = sizeof(openflow_cliaddr);  //len is value/resuslt 
  
    nread = recvfrom(datafd_, buf.data(), buf.size(),  
                MSG_WAITALL, ( struct sockaddr *) &openflow_cliaddr, 
                (socklen_t *)&len); 

	// TODO fix this
	assert(!streams_.empty());
	auto &stream = streams_[last_stream_id_];
	stream->streambuf.emplace_back(buf.data(), nread);
	ev_feed_event(loop_, &wev_, EV_WRITE);
	return 0;
}

int Client::send_ovsdb_interactive_input() {
 
    ssize_t nread;
    std::array<uint8_t, 1_k> buf;
    int len;

    len = sizeof(openflow_cliaddr);  //len is value/resuslt 

	//TODO: CHange openflow_cliaddr 
    nread = recvfrom(datafd_ovsdb, buf.data(), buf.size(),
                MSG_WAITALL, ( struct sockaddr *) &ovsdb_cliaddr,
                (socklen_t *)&len);

    // TODO fix this
    assert(!streams_.empty());
    auto &stream = streams_[last_stream_id_];
    stream->streambuf.emplace_back(buf.data(), nread);
    ev_feed_event(loop_, &wev_, EV_WRITE);
    return 0;
}

int Client::stop_interactive_input() {
  assert(!streams_.empty());

  auto &stream = (*std::begin(streams_)).second;

  stream->should_send_fin = true;
  if (stream->streambuf.empty()) {
    stream->streambuf.emplace_back();
  }
  ev_io_stop(loop_, &stdinrev_);

  std::cerr << "Interactive session has ended." << std::endl;

  ev_feed_event(loop_, &wev_, EV_WRITE);

  return 0;
}

int Client::stop_ovsdb_interactive_input() {
  assert(!streams_.empty());

  auto &stream = (*std::begin(streams_)).second;

  stream->should_send_fin = true;
  if (stream->streambuf.empty()) {
    stream->streambuf.emplace_back();
  }
  ev_io_stop(loop_, &ovsdb_);

  std::cerr << "Interactive session has ended." << std::endl;

  ev_feed_event(loop_, &wev_, EV_WRITE);

  return 0;
}


int Client::handle_error(int liberr) {
  if (!conn_ || ngtcp2_conn_is_in_closing_period(conn_)) {
    return 0;
  }

  sendbuf_.reset();
  assert(sendbuf_.left() >= max_pktlen_);

  if (liberr == NGTCP2_ERR_RECV_VERSION_NEGOTIATION) {
    return 0;
  }

  auto n = ngtcp2_conn_write_connection_close(
      conn_, sendbuf_.wpos(), max_pktlen_,
      ngtcp2_err_infer_quic_transport_error_code(liberr),
      util::timestamp(loop_));
  if (n < 0) {
    std::cerr << "ngtcp2_conn_write_connection_close: " << ngtcp2_strerror(n)
              << std::endl;
    return -1;
  }

  sendbuf_.push(n);

  return send_packet();
}

namespace {
size_t remove_tx_stream_data(std::deque<Buffer> &d, size_t &idx,
                             uint64_t &tx_offset, uint64_t offset) {
  size_t len = 0;
  for (; !d.empty() && tx_offset + d.front().bufsize() <= offset;) {
    --idx;
    tx_offset += d.front().bufsize();
    len += d.front().bufsize();
    d.pop_front();
  }
  return len;
}
} // namespace

void Client::remove_tx_crypto_data(uint64_t offset, size_t datalen) {

  ::remove_tx_stream_data(chandshake_, chandshake_idx_, tx_crypto_offset_,
                          offset + datalen);
}

int Client::remove_tx_stream_data(uint64_t stream_id, uint64_t offset,
                                  size_t datalen) {
  auto it = streams_.find(stream_id);
  if (it == std::end(streams_)) {
    std::cerr << "Stream " << stream_id << "not found" << std::endl;
    return 0;
  }
  auto &stream = (*it).second;
  ::remove_tx_stream_data(stream->streambuf, stream->streambuf_idx,
                          stream->tx_stream_offset, offset + datalen);

  return 0;
}

void Client::on_stream_close(uint64_t stream_id) {
  auto it = streams_.find(stream_id);

  if (it == std::end(streams_)) {
    return;
  }

  if (config.interactive) {
	if(mode == 1){
		ev_io_stop(loop_, &stdinrev_);
	}
	else if (mode == 2){
		ev_io_stop(loop_, &ovsdb_);
  	}
	else if(mode == 3){
		ev_io_stop(loop_, &stdinrev_);
		ev_io_stop(loop_, &ovsdb_);
	}
  }

  streams_.erase(it);
}

namespace {
int write_transport_params(const char *path,
                           const ngtcp2_transport_params *params) {
  auto f = std::ofstream(path);
  if (!f) {
    return -1;
  }

  f << "initial_max_bidi_streams=" << params->initial_max_bidi_streams << "\n"
    << "initial_max_uni_streams=" << params->initial_max_uni_streams << "\n"
    << "initial_max_stream_data=" << params->initial_max_stream_data << "\n"
    << "initial_max_data=" << params->initial_max_data << "\n";

  f.close();
  if (!f) {
    return -1;
  }

  return 0;
}
} // namespace

namespace {
int read_transport_params(const char *path, ngtcp2_transport_params *params) {
  auto f = std::ifstream(path);
  if (!f) {
    return -1;
  }

  for (std::string line; std::getline(f, line);) {
    if (util::istarts_with_l(line, "initial_max_bidi_streams=")) {
      params->initial_max_bidi_streams = strtoul(
          line.c_str() + str_size("initial_max_bidi_streams="), nullptr, 10);
    } else if (util::istarts_with_l(line, "initial_max_uni_streams=")) {
      params->initial_max_uni_streams = strtoul(
          line.c_str() + str_size("initial_max_uni_streams="), nullptr, 10);
    } else if (util::istarts_with_l(line, "initial_max_stream_data=")) {
      params->initial_max_stream_data = strtoul(
          line.c_str() + str_size("initial_max_stream_data="), nullptr, 10);
    } else if (util::istarts_with_l(line, "initial_max_data=")) {
      params->initial_max_data =
          strtoul(line.c_str() + str_size("initial_max_data="), nullptr, 10);
    }
  }

  return 0;
}
} // namespace

void Client::make_stream_early() {
  
	int rv;
	uint64_t stream_id;

	if(mode == 1){
		if (config.interactive || datafd_ == -1) {
    		return;
  		}
		if (nstreams_done_ >= config.nstreams) {
			return;
		}

		++nstreams_done_;
		rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, 1, OFL);
    	if (rv != 0) {
        	std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
              << std::endl;
        	return;
    	}
		auto stream = std::make_unique<Stream>(stream_id);
    	stream->buffer_file();
    	streams_.emplace(stream_id, std::move(stream));
	} 
	else if (mode == 2){
		if (config.interactive || datafd_ovsdb == -1) {
			return;
		}
		++nstreams_done_;
		rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, 1, OVSDB);
		if (rv != 0) {
        	std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
              		<< std::endl;
        	return;
    	}
		auto stream = std::make_unique<Stream>(stream_id);
    	stream->buffer_file();
    	streams_.emplace(stream_id, std::move(stream));
	}
	else if (mode == 3) {
		if (config.interactive || datafd_ == -1 || datafd_ovsdb == -1) {
            return;
        }
		++nstreams_done_;
		rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, 1, OFL);
    	if (rv != 0) {
        	std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
            	  << std::endl;
        	return;
    	}

		auto stream = std::make_unique<Stream>(stream_id);
    	stream->buffer_file();
    	streams_.emplace(stream_id, std::move(stream));

		++nstreams_done_;
	 	rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, 1, OVSDB);
    	if (rv != 0) {
        	std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
            	  << std::endl;
        	return;
    	}
		stream = std::make_unique<Stream>(stream_id);
    	stream->buffer_file();
    	streams_.emplace(stream_id, std::move(stream));
	}
}

int Client::on_extend_max_stream_id(uint64_t max_stream_id, int protocol_type) {

    int rv;
    if (config.interactive) {
        if (last_stream_id_ != 0) {
            return 0;
        }

		if(protocol_type == OFL){
        	if (start_interactive_input() != 0) {
            	return -1;
        	}
		}
		else if(protocol_type == OVSDB){
			if (start_ovsdb_interactive_input() != 0) {
                return -1;
            }
		}
		else if(protocol_type == MIX){
			if (start_interactive_input() != 0) {
                return -1;
            }
			if (start_ovsdb_interactive_input() != 0) {
                return -1;
            }
		}
        return 0;
    }
   
    if (datafd_ != -1) {
        for (; nstreams_done_ < config.nstreams; ++nstreams_done_) {
            uint64_t stream_id;
            rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, 1, protocol_type);
            if (rv != 0) {
                assert(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);
                break;
            }
            last_stream_id_ = stream_id;
            auto stream = std::make_unique<Stream>(stream_id);
            stream->buffer_file();
            streams_.emplace(stream_id, std::move(stream));
        }
        return 0;
    }
    return 0;
}


void Client::start_wev() { ev_io_start(loop_, &wev_); }

namespace {
int transport_params_add_cb(SSL *ssl, unsigned int ext_type,
                            unsigned int content, const unsigned char **out,
                            size_t *outlen, X509 *x, size_t chainidx, int *al,
                            void *add_arg) {
  int rv;
  auto c = static_cast<Client *>(SSL_get_app_data(ssl));
  auto conn = c->conn();

  ngtcp2_transport_params params;

  rv = ngtcp2_conn_get_local_transport_params(
      conn, &params, NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO);
  if (rv != 0) {
    *al = SSL_AD_INTERNAL_ERROR;
    return -1;
  }

  constexpr size_t bufsize = 64;
  auto buf = std::make_unique<uint8_t[]>(bufsize);

  auto nwrite = ngtcp2_encode_transport_params(
      buf.get(), bufsize, NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO, &params);
  if (nwrite < 0) {
    std::cerr << "ngtcp2_encode_transport_params: " << ngtcp2_strerror(nwrite)
              << std::endl;
    *al = SSL_AD_INTERNAL_ERROR;
    return -1;
  }

  *out = buf.release();
  *outlen = static_cast<size_t>(nwrite);

  return 1;
}
} // namespace

namespace {
void transport_params_free_cb(SSL *ssl, unsigned int ext_type,
                              unsigned int context, const unsigned char *out,
                              void *add_arg) {
  delete[] const_cast<unsigned char *>(out);
}
} // namespace

namespace {
int transport_params_parse_cb(SSL *ssl, unsigned int ext_type,
                              unsigned int context, const unsigned char *in,
                              size_t inlen, X509 *x, size_t chainidx, int *al,
                              void *parse_arg) {
  auto c = static_cast<Client *>(SSL_get_app_data(ssl));
  auto conn = c->conn();

  int rv;

  ngtcp2_transport_params params;

  rv = ngtcp2_decode_transport_params(
      &params, NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS, in, inlen);
  if (rv != 0) {
    std::cerr << "ngtcp2_decode_transport_params: " << ngtcp2_strerror(rv)
              << std::endl;
    *al = SSL_AD_ILLEGAL_PARAMETER;
    return -1;
  }

  rv = ngtcp2_conn_set_remote_transport_params(
      conn, NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS, &params);
  if (rv != 0) {
    *al = SSL_AD_ILLEGAL_PARAMETER;
    return -1;
  }

  if (config.tp_file && write_transport_params(config.tp_file, &params) != 0) {
    std::cerr << "Could not write transport parameters in " << config.tp_file
              << std::endl;
  }

  return 1;
}
} // namespace

namespace {
int new_session_cb(SSL *ssl, SSL_SESSION *session) {
  if (SSL_SESSION_get_max_early_data(session) !=
      std::numeric_limits<uint32_t>::max()) {
    // TODO Find a way to send CONNECTION_CLOSE with
    // PROTOCOL_VIOLATION in this case.
    std::cerr << "max_early_data is not 0xffffffff" << std::endl;
    return 0;
  }
  auto f = BIO_new_file(config.session_file, "w");
  if (f == nullptr) {
    std::cerr << "Could not write TLS session in " << config.session_file
              << std::endl;
    return 0;
  }

  PEM_write_bio_SSL_SESSION(f, session);
  BIO_free(f);

  return 0;
}
} // namespace

namespace {
SSL_CTX *create_ssl_ctx() {
  auto ssl_ctx = SSL_CTX_new(TLS_method());

  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);

  // This makes OpenSSL client not send CCS after an initial
  // ClientHello.
  SSL_CTX_clear_options(ssl_ctx, SSL_OP_ENABLE_MIDDLEBOX_COMPAT);

  SSL_CTX_set_default_verify_paths(ssl_ctx);

  if (SSL_CTX_set_cipher_list(ssl_ctx, config.ciphers) != 1) {
    std::cerr << "SSL_CTX_set_cipher_list: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    exit(EXIT_FAILURE);
  }

  if (SSL_CTX_set1_groups_list(ssl_ctx, config.groups) != 1) {
    std::cerr << "SSL_CTX_set1_groups_list failed" << std::endl;
    exit(EXIT_FAILURE);
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_QUIC_HACK);

  if (SSL_CTX_add_custom_ext(
          ssl_ctx, NGTCP2_TLSEXT_QUIC_TRANSPORT_PARAMETERS,
          SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS,
          transport_params_add_cb, transport_params_free_cb, nullptr,
          transport_params_parse_cb, nullptr) != 1) {
    std::cerr << "SSL_CTX_add_custom_ext(NGTCP2_TLSEXT_QUIC_TRANSPORT_"
                 "PARAMETERS) failed: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    exit(EXIT_FAILURE);
  }

  if (config.session_file) {
    SSL_CTX_set_session_cache_mode(
        ssl_ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(ssl_ctx, new_session_cb);
  }

  return ssl_ctx;
}
} // namespace

namespace {
int create_sock(Address &remote_addr, const char *addr, const char *port) {
  addrinfo hints{};
  addrinfo *res, *rp;
  int rv;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  rv = getaddrinfo(addr, port, &hints, &res);
  if (rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  int fd = -1;

  for (rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == -1) {
      goto next;
    }

    break;

  next:
    close(fd);
  }

  if (!rp) {
    std::cerr << "Could not connect" << std::endl;
    return -1;
  }

  auto val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                 static_cast<socklen_t>(sizeof(val))) == -1) {
    return -1;
  }

  remote_addr.len = rp->ai_addrlen;
  memcpy(&remote_addr.su, rp->ai_addr, rp->ai_addrlen);

  return fd;
}

} // namespace

namespace {
int run(Client &c, const char *addr, const char *port) {
  Address remote_addr;
  ssize_t nwrite;

  auto fd = create_sock(remote_addr, addr, port);
  if (fd == -1) {
    return -1;
  }

  if (c.init(fd, remote_addr, addr, config.fd_ofl, config.fd_ovsdb, config.version) != 0) {
    return -1;
  }

  if (config.tp_file) {
    ngtcp2_transport_params params;
    if (read_transport_params(config.tp_file, &params) != 0) {
      std::cerr << "Could not read transport parameters from " << config.tp_file
                << std::endl;
    } else {
      ngtcp2_conn_set_early_remote_transport_params(c.conn(), &params);
      c.make_stream_early();
    }
  }

  // For 0-RTT
  auto rv = c.write_0rtt_streams();
  if (rv != 0) {
    return rv;
  }

  nwrite = c.do_handshake_once(nullptr, 0);
  if (nwrite < 0) {
    return nwrite;
  }

  c.schedule_retransmit();

  ev_run(EV_DEFAULT, 0);

  return 0;
}
} // namespace

namespace {
std::ofstream keylog_file;
void keylog_callback(const SSL *ssl, const char *line) {
  keylog_file.write(line, strlen(line));
  keylog_file.put('\n');
  keylog_file.flush();
}
} // namespace

namespace {
void print_usage() {
  std::cerr << "Usage: client [OPTIONS] <ADDR> <PORT>" << std::endl;
}
} // namespace

namespace {
void config_set_default(Config &config) {
  config = Config{};
  config.tx_loss_prob = 0.;
  config.rx_loss_prob = 0.;
  config.fd_ofl = -1;
  config.ciphers = "TLS13-AES-128-GCM-SHA256:TLS13-AES-256-GCM-SHA384:TLS13-"
                   "CHACHA20-POLY1305-SHA256";
  config.groups = "P-256:X25519:P-384:P-521";
  config.nstreams = 1;
  config.data = nullptr;
  config.datalen = 0;
  config.version = NGTCP2_PROTO_VER_D13;
  config.timeout = 1000;
  // config.mode= -1;
}
} // namespace

int start_client(int ofl_sockfd, int sockfd_ovsdb, const char *addr, const char *port, int m){

	config_set_default(config);
 	// config.fd = fileno(stdin);
	FILE *fp = NULL;
	FILE *fp_ovsdb = NULL;

	if(m == 1){
		if(ofl_sockfd != -1){
			ofl_sock = ofl_sockfd;	
			fp=fdopen(ofl_sockfd,"r");
			config.fd_ofl = fileno(fp);
		}
	}
	else if(m == 2){
		if(sockfd_ovsdb != -1){
			printf("OVSDB sock is %d\n", sockfd_ovsdb);
			ovsdb_sock = sockfd_ovsdb;
			fp_ovsdb = fdopen(sockfd_ovsdb, "r");
			config.fd_ovsdb = fileno(fp_ovsdb);	
		}
	}
	else if(m == 3){
		if(ofl_sockfd != -1){
            ofl_sock = ofl_sockfd;
            fp=fdopen(ofl_sockfd,"r");
            config.fd_ofl = fileno(fp);
        }
		if(sockfd_ovsdb != -1){
            ovsdb_sock = sockfd_ovsdb;
            fp_ovsdb = fdopen(sockfd_ovsdb, "r");
            config.fd_ovsdb = fileno(fp_ovsdb); 
        }
	}
	else {
        printf("Mode was not recognized\n");
		return EXIT_FAILURE;
    }

	config.interactive = true;
	config.quiet = true;
	mode = m;
 
	auto ssl_ctx = create_ssl_ctx();
	auto ssl_ctx_d = defer(SSL_CTX_free, ssl_ctx);
	auto ev_loop_d = defer(ev_loop_destroy, EV_DEFAULT);

	if (isatty(STDOUT_FILENO)) {
    	debug::set_color_output(true);
  	}

  	auto keylog_filename = getenv("SSLKEYLOGFILE");
  	if (keylog_filename) {
    	keylog_file.open(keylog_filename, std::ios_base::app);
    		if (keylog_file) {
      			SSL_CTX_set_keylog_callback(ssl_ctx, keylog_callback);
    		}
  	}

	if(mode == 1){
		if(fp != NULL){
			Client c(EV_DEFAULT, ssl_ctx, fp);
			if (run(c, addr, port) != 0) {
    			exit(EXIT_FAILURE);
  			}
			return EXIT_SUCCESS;
		}
	}
	else if (mode == 2)
	{
		if(fp_ovsdb != NULL){
            Client c(EV_DEFAULT, ssl_ctx, fp_ovsdb);
            if (run(c, addr, port) != 0) {
                exit(EXIT_FAILURE);
            }
            return EXIT_SUCCESS;
        }
	}
	else if(mode == 3) {
		if(fp != NULL && fp_ovsdb != NULL){
            Client c(EV_DEFAULT, ssl_ctx, fp);
            if (run(c, addr, port) != 0) {
                exit(EXIT_FAILURE);
            }
            return EXIT_SUCCESS;
        }

	}
	return EXIT_FAILURE;
}
