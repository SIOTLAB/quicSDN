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
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <openssl/bio.h>
#include <openssl/err.h>

#include "app_server.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "crypto.h"
#include "shared.h"
#include "http.h"
#include "keylog.h"


using namespace ngtcp2;

int openflow_sock = -1;
int ovsdb_sock = -1;

std::deque<uint8_t *> forward_deque;
FILE *ovsdb_fp = NULL;
FILE *openflow_fp = NULL;
int mode = -1;

int server_nstreams = 1;

// ngtcp2_strm *transport_strm;
// uint64_t transport_stream_id = 0;
// std::unique_ptr<Stream> transport_stream;

namespace {
constexpr size_t NGTCP2_SV_SCIDLEN = 18;
} // namespace

namespace {
auto randgen = util::make_mt19937();
} // namespace

namespace {
Config config{};
} // namespace


struct sockaddr_in ovsdb_servaddr;
struct sockaddr_in ofl_servaddr;

pthread_mutex_t mutex;
pthread_cond_t cond;
bool cond_flag= false;

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

namespace {
int key_cb(SSL *ssl, int name, const unsigned char *secret, size_t secretlen,
           const unsigned char *key, size_t keylen, const unsigned char *iv,
           size_t ivlen, void *arg) {
  auto h = static_cast<Handler *>(arg);

  if (h->on_key(name, secret, secretlen, key, keylen, iv, ivlen) != 0) {
    return 0;
  }

  keylog::log_secret(ssl, name, secret, secretlen);

  return 1;
}
} // namespace

int Handler::on_key(int name, const uint8_t *secret, size_t secretlen,
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
    ngtcp2_conn_set_handshake_rx_keys(conn_, key, keylen, iv, ivlen, pn.data(),
                                      pnlen);
    break;
  case SSL_KEY_CLIENT_APPLICATION_TRAFFIC:
    std::cerr << "client_application_traffic" << std::endl;
    ngtcp2_conn_update_rx_keys(conn_, key, keylen, iv, ivlen, pn.data(), pnlen);
    break;
  case SSL_KEY_SERVER_HANDSHAKE_TRAFFIC:
    #ifdef SSL_DEBUG
		std::cerr << "server_handshake_traffic" << std::endl;
	#endif    

	ngtcp2_conn_set_handshake_tx_keys(conn_, key, keylen, iv, ivlen, pn.data(),
                                      pnlen);
    break;
  case SSL_KEY_SERVER_APPLICATION_TRAFFIC:
    std::cerr << "server_application_traffic" << std::endl;
    ngtcp2_conn_update_tx_keys(conn_, key, keylen, iv, ivlen, pn.data(), pnlen);
    break;
  }

#ifdef SSL_DEBUG
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

#ifdef SSL_DEBUG
  std::cerr << "msg_cb: write_p=" << write_p << " version=" << version
            << " content_type=" << content_type << " len=" << len << std::endl;
 #endif 
	if (!write_p || content_type != SSL3_RT_HANDSHAKE) {
    	return;
  	}

  auto h = static_cast<Handler *>(arg);

  rv = h->write_server_handshake(reinterpret_cast<const uint8_t *>(buf), len);

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

  auto h = static_cast<Handler *>(BIO_get_data(b));

  len = h->read_client_handshake(reinterpret_cast<uint8_t *>(buf), len);
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
int on_msg_begin(http_parser *htp) {
  auto s = static_cast<Stream *>(htp->data);
  if (s->resp_state != RESP_IDLE) {
    return -1;
  }
  return 0;
}
} // namespace

namespace {
int on_url_cb(http_parser *htp, const char *data, size_t datalen) {
  auto s = static_cast<Stream *>(htp->data);
  s->uri.append(data, datalen);
  return 0;
}
} // namespace

namespace {
int on_header_field(http_parser *htp, const char *data, size_t datalen) {
  auto s = static_cast<Stream *>(htp->data);
  if (s->prev_hdr_key) {
    s->hdrs.back().first.append(data, datalen);
  } else {
    s->prev_hdr_key = true;
    s->hdrs.emplace_back(std::string(data, datalen), "");
  }
  return 0;
}
} // namespace

namespace {
int on_header_value(http_parser *htp, const char *data, size_t datalen) {
  auto s = static_cast<Stream *>(htp->data);
  s->prev_hdr_key = false;
  s->hdrs.back().second.append(data, datalen);
  return 0;
}
} // namespace

namespace {
int on_headers_complete(http_parser *htp) {
  auto s = static_cast<Stream *>(htp->data);
  if (s->start_response() != 0) {
    return -1;
  }
  return 0;
}
} // namespace

auto htp_settings = http_parser_settings{
    on_msg_begin,        // on_message_begin
    on_url_cb,           // on_url
    nullptr,             // on_status
    on_header_field,     // on_header_field
    on_header_value,     // on_header_value
    on_headers_complete, // on_headers_complete
    nullptr,             // on_body
    nullptr,             // on_message_complete
    nullptr,             // on_chunk_header,
    nullptr,             // on_chunk_complete
};

Stream::Stream(uint64_t stream_id)
    : stream_id(stream_id),
      streambuf_idx(0),
      tx_stream_offset(0),
      should_send_fin(false),
      resp_state(RESP_IDLE),
      http_major(0),
      http_minor(0),
      prev_hdr_key(false),
      fd(-1),
      data(nullptr),
      datalen(0) {
  http_parser_init(&htp, HTTP_REQUEST);
  htp.data = this;
}

Stream::~Stream() {
  munmap(data, datalen);
  if (fd != -1) {
    close(fd);
  }
}

int Stream::recv_data(uint8_t fin, const uint8_t *data, size_t datalen) {

	printf("**********************Recv data is %s\n", data);
	// write(testing_sock, data, datalen);

  	return 0;
}

namespace {
constexpr char NGTCP2_SERVER[] = "ngtcp2";
} // namespace

namespace {
std::string make_status_body(unsigned int status_code) {
  auto status_string = std::to_string(status_code);
  auto reason_phrase = http::get_reason_phrase(status_code);

  std::string body;
  body = "<html><head><title>";
  body += status_string;
  body += ' ';
  body += reason_phrase;
  body += "</title></head><body><h1>";
  body += status_string;
  body += ' ';
  body += reason_phrase;
  body += "</h1><hr><address>";
  body += NGTCP2_SERVER;
  body += " at port ";
  body += std::to_string(config.port);
  body += "</address>";
  body += "</body></html>";
  return body;
}
} // namespace

namespace {
std::string request_path(const std::string &uri, bool is_connect) {
  http_parser_url u;

  http_parser_url_init(&u);

  auto rv = http_parser_parse_url(uri.c_str(), uri.size(), is_connect, &u);
  if (rv != 0) {
    return "";
  }

  if (u.field_set & (1 << UF_PATH)) {
    // TODO path could be empty?
    auto req_path = std::string(uri.c_str() + u.field_data[UF_PATH].off,
                                u.field_data[UF_PATH].len);
    if (!req_path.empty() && req_path.back() == '/') {
      req_path += "index.html";
    }
    return req_path;
  }

  return "/index.html";
}
} // namespace

namespace {
std::string resolve_path(const std::string &req_path) {
  auto raw_path = config.htdocs + req_path;
  auto malloced_path = realpath(raw_path.c_str(), nullptr);
  if (malloced_path == nullptr) {
    return "";
  }
  auto path = std::string(malloced_path);
  free(malloced_path);

  if (path.size() < config.htdocs.size() ||
      !std::equal(std::begin(config.htdocs), std::end(config.htdocs),
                  std::begin(path))) {
    return "";
  }
  return path;
}
} // namespace

int Stream::open_file(const std::string &path) {
  fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    return -1;
  }

  return 0;
}

int Stream::map_file(size_t len) {
  if (len == 0) {
    return 0;
  }
  data =
      static_cast<uint8_t *>(mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0));
  if (data == MAP_FAILED) {
    std::cerr << "mmap: " << strerror(errno) << std::endl;
    return -1;
  }
  datalen = len;
  return 0;
}

void Stream::buffer_file() {
  streambuf.emplace_back(data, data + datalen);
  should_send_fin = true;
}

//QUICmodi
void Handler::transport_send_response() {

	int rv;
    uint64_t stream_id;

  	rv = ngtcp2_conn_open_uni_stream(conn_, &stream_id, nullptr);
  	if (rv != 0) {
		printf("RETURNING FROM HERE!!!\n");
    	return;
  	}

  	auto stream = std::make_unique<Stream>(stream_id);

	char buf[4096] = {'\0'};
	bzero(buf, sizeof(buf));
    
    int nread = read(ovsdb_sock, buf, sizeof(buf));
	stream->streambuf.emplace_back((const uint8_t*)buf, sizeof(buf));
	stream->should_send_fin = false;
	stream->resp_state = RESP_COMPLETED;
	streams_.emplace(stream_id, std::move(stream));
}


void Stream::send_status_response(unsigned int status_code,
                                  const std::string &extra_headers) {
  auto body = make_status_body(status_code);
  std::string hdr;
  if (http_major >= 1) {
    hdr += "HTTP/";
    hdr += std::to_string(http_major);
    hdr += '.';
    hdr += std::to_string(http_minor);
    hdr += ' ';
    hdr += std::to_string(status_code);
    hdr += " ";
    hdr += http::get_reason_phrase(status_code);
    hdr += "\r\n";
    hdr += "Server: ";
    hdr += NGTCP2_SERVER;
    hdr += "\r\n";
    hdr += "Content-Type: text/html; charset=UTF-8\r\n";
    hdr += "Content-Length: ";
    hdr += std::to_string(body.size());
    hdr += "\r\n";
    hdr += extra_headers;
    hdr += "\r\n";
  }

  auto v = Buffer{hdr.size() + ((htp.method == HTTP_HEAD) ? 0 : body.size())};
  auto p = std::begin(v.buf);
  p = std::copy(std::begin(hdr), std::end(hdr), p);
  if (htp.method != HTTP_HEAD) {
    p = std::copy(std::begin(body), std::end(body), p);
  }
  v.push(std::distance(std::begin(v.buf), p));
  streambuf.emplace_back(std::move(v));
  should_send_fin = true;
  resp_state = RESP_COMPLETED;
}

void Stream::send_redirect_response(unsigned int status_code,
                                    const std::string &path) {
  std::string hdrs = "Location: ";
  hdrs += path;
  hdrs += "\r\n";
  send_status_response(status_code, hdrs);
}

int Stream::start_response() {
  http_major = htp.http_major;
  http_minor = htp.http_minor;

  auto req_path = request_path(uri, htp.method == HTTP_CONNECT);
  auto path = resolve_path(req_path);
  if (path.empty() || open_file(path) != 0) {
    send_status_response(404);
    return 0;
  }

  struct stat st {};

  int64_t content_length = -1;

  if (fstat(fd, &st) == 0) {
    if (st.st_mode & S_IFDIR) {
      send_redirect_response(308, path.substr(config.htdocs.size() - 1) + '/');
      return 0;
    }
    content_length = st.st_size;
  } else {
    send_status_response(404);
    return 0;
  }

  if (map_file(content_length) != 0) {
    send_status_response(500);
    return 0;
  }

  if (http_major >= 1) {
    std::string hdr;
    hdr += "HTTP/";
    hdr += std::to_string(http_major);
    hdr += '.';
    hdr += std::to_string(http_minor);
    hdr += " 200 OK\r\n";
    hdr += "Server: ";
    hdr += NGTCP2_SERVER;
    hdr += "\r\n";
    if (content_length != -1) {
      hdr += "Content-Length: ";
      hdr += std::to_string(content_length);
      hdr += "\r\n";
    }
    hdr += "\r\n";

    auto v = Buffer{hdr.size()};
    auto p = std::begin(v.buf);
    p = std::copy(std::begin(hdr), std::end(hdr), p);
    v.push(std::distance(std::begin(v.buf), p));
    streambuf.emplace_back(std::move(v));
  }

  resp_state = RESP_COMPLETED;

  switch (htp.method) {
  case HTTP_HEAD:
    should_send_fin = true;
    close(fd);
    fd = -1;
    break;
  default:
    buffer_file();
  }

  return 0;
}

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto h = static_cast<Handler *>(w->data);
  auto s = h->server();

  if (ngtcp2_conn_is_in_closing_period(h->conn())) {
    if (!config.quiet) {
      std::cerr << "Closing Period is over" << std::endl;
    }

    s->remove(h);
    return;
  }
  if (h->draining()) {
    if (!config.quiet) {
      std::cerr << "Draining Period is over" << std::endl;
    }

    s->remove(h);
    return;
  }

  if (!config.quiet) {
    std::cerr << "Timeout" << std::endl;
  }

  h->start_draining_period();
}
} // namespace

namespace {
void retransmitcb(struct ev_loop *loop, ev_timer *w, int revents) {
  int rv;

  auto h = static_cast<Handler *>(w->data);
  auto s = h->server();
  auto conn = h->conn();
  auto now = util::timestamp(loop);

  if (ngtcp2_conn_loss_detection_expiry(conn) <= now) {
    rv = h->on_write(true);
    switch (rv) {
    case 0:
    case NETWORK_ERR_CLOSE_WAIT:
      return;
    case NETWORK_ERR_SEND_NON_FATAL:
      s->start_wev();
      return;
    default:
      s->remove(h);
      return;
    }
  }

  if (ngtcp2_conn_ack_delay_expiry(conn) <= now) {
    rv = h->on_write();
    switch (rv) {
    case 0:
    case NETWORK_ERR_CLOSE_WAIT:
      return;
    case NETWORK_ERR_SEND_NON_FATAL:
      s->start_wev();
      return;
    default:
      s->remove(h);
      return;
    }
  }
}
} // namespace

int Handler::send_tx_rx_server() {

	ssize_t nread = 0;
	std::array<uint8_t, 1_k> buf;
  
 
	if(ovsdb_fp){ 
		nread = read(fileno(ovsdb_fp), buf.data(), buf.size());
		if (nread == -1 || nread == 0) {
			// return 1;
        	// return stop_interactive_input();
		}
		else {
			send_greeting(buf.data(), nread, OVSDB);
		}
	}

	nread = 0;

	if(openflow_fp){
		nread = read(fileno(openflow_fp), buf.data(), buf.size());
		if (nread == -1 || nread == 0) {
        	///return 1;
        	// return stop_interactive_input();
    	}
    	else {
        	send_greeting(buf.data(), nread, OFL);
    	}
	}
    return 0;
}

namespace {
void asyncServer_readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto c = static_cast<Handler *>(w->data);

  if (c->send_tx_rx_server()) {
        // printf("Disconnecting\n");
        // c->disconnect();
  }
}
 
}

Handler::Handler(struct ev_loop *loop, SSL_CTX *ssl_ctx, Server *server,
                 const ngtcp2_cid *rcid)
    : remote_addr_{},
      max_pktlen_(0),
      loop_(loop),
      ssl_ctx_(ssl_ctx),
      ssl_(nullptr),
      server_(server),
      fd_(-1),
      ncread_(0),
      shandshake_idx_(0),
      conn_(nullptr),
      rcid_(*rcid),
      crypto_ctx_{},
      sendbuf_{NGTCP2_MAX_PKTLEN_IPV4},
      tx_crypto_offset_(0),
      initial_(true),
      draining_(false),
	  last_stream_id_(0), 
	  nstreams_done_(0) {
  ev_timer_init(&timer_, timeoutcb, 0., config.timeout);
  timer_.data = this;
  ev_timer_init(&rttimer_, retransmitcb, 0., 0.);
  rttimer_.data = this;

  ev_io_init(&asyncServer_, asyncServer_readcb, 0, EV_READ);
  asyncServer_.data = this;
}

Handler::~Handler() {
  if (!config.quiet) {
    std::cerr << "Closing QUIC connection" << std::endl;
  }

  ev_timer_stop(loop_, &rttimer_);
  ev_timer_stop(loop_, &timer_);

  if (conn_) {
    ngtcp2_conn_del(conn_);
  }

  if (ssl_) {
    SSL_free(ssl_);
  }
}

namespace {
int recv_client_initial(ngtcp2_conn *conn, const ngtcp2_cid *dcid,
                        void *user_data) {
  auto h = static_cast<Handler *>(user_data);

  if (h->recv_client_initial(dcid) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
  auto h = static_cast<Handler *>(user_data);

  if (!config.quiet) {
    debug::handshake_completed(conn, user_data);
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
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->hs_encrypt_data(dest, destlen, plaintext, plaintextlen, key,
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
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->hs_decrypt_data(dest, destlen, ciphertext, ciphertextlen,
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
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->encrypt_data(dest, destlen, plaintext, plaintextlen, key,
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
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->decrypt_data(dest, destlen, ciphertext, ciphertextlen, key,
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
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->hs_encrypt_pn(dest, destlen, plaintext, plaintextlen, key,
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
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->encrypt_pn(dest, destlen, plaintext, plaintextlen, key,
                              keylen, nonce, noncelen);
  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}
} // namespace

namespace {
int recv_crypto_data(ngtcp2_conn *conn, uint64_t offset, const uint8_t *data,
                     size_t datalen, void *user_data) {
  int rv;

  if (!config.quiet) {
    debug::print_crypto_data(data, datalen);
  }

  auto h = static_cast<Handler *>(user_data);

  h->write_client_handshake(data, datalen);

  if (!ngtcp2_conn_get_handshake_completed(h->conn())) {
    rv = h->tls_handshake();
    if (rv != 0) {
      return rv;
    }
  }

  // SSL_do_handshake() might not consume all data (e.g.,
  // NewSessionTicket).
  return h->read_tls();
}
} // namespace

namespace {
int recv_stream_data(ngtcp2_conn *conn, uint64_t stream_id, uint8_t fin,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data, void *stream_user_data) {
  
	auto h = static_cast<Handler *>(user_data);

	if (h->recv_stream_data(stream_id, fin, data, datalen) != 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}

  return 0;
}
} // namespace

namespace {
int acked_crypto_offset(ngtcp2_conn *conn, uint64_t offset, size_t datalen,
                        void *user_data) {
  auto h = static_cast<Handler *>(user_data);
  h->remove_tx_crypto_data(offset, datalen);
  return 0;
}
} // namespace

namespace {
int acked_stream_data_offset(ngtcp2_conn *conn, uint64_t stream_id,
                             uint64_t offset, size_t datalen, void *user_data,
                             void *stream_user_data) {
  auto h = static_cast<Handler *>(user_data);
  if (h->remove_tx_stream_data(stream_id, offset, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

namespace {
int stream_close(ngtcp2_conn *conn, uint64_t stream_id, uint16_t app_error_code,
                 void *user_data, void *stream_user_data) {
  auto h = static_cast<Handler *>(user_data);
  h->on_stream_close(stream_id);
  return 0;
}
} // namespace

namespace {
int rand(ngtcp2_conn *conn, uint8_t *dest, size_t destlen, ngtcp2_rand_ctx ctx,
         void *user_data) {
  auto dis = std::uniform_int_distribution<uint8_t>(0, 255);
  std::generate(dest, dest + destlen, [&dis]() { return dis(randgen); });
  return 0;
}
} // namespace

int Handler::start_tx_rx_server() {

	int rv;
	uint64_t stream_id;
	std::cerr << "Interactive session started.  Hit Ctrl-D to end the session."
				<< std::endl;


	if(mode == 1){
		printf("Inside mode 1\n");
		ev_io_set(&asyncServer_, fileno(openflow_fp), EV_READ);
		ev_io_start(loop_, &asyncServer_);
		
		// rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
		rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, SERVER_TYPE, OFL);
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
	}
	else if(mode == 2){
		ev_io_set(&asyncServer_, fileno(ovsdb_fp), EV_READ);
    	ev_io_start(loop_, &asyncServer_);
		rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
		// rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, SERVER_TYPE, OVSDB);
    	if (rv != 0) {
        	std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
                    << std::endl;
        	if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
            	return 0;
       	 	}
        	return -1;
    	}

    	std::cerr << "The stream " << stream_id << " has opened." << std::endl;
    	last_stream_id_ = stream_id;
    	auto stream = std::make_unique<Stream>(stream_id);
    	streams_.emplace(stream_id, std::move(stream));
	}
	else if(mode == 3){
		{
			ev_io_set(&asyncServer_, fileno(openflow_fp), EV_READ);
			ev_io_start(loop_, &asyncServer_);
			// rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
			rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, SERVER_TYPE, OFL);
    		if (rv != 0) {
        		std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
                    << std::endl;
        		if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
            		return 0;
        		}
        		return -1;
    		}

    		std::cerr << "The stream " << stream_id << " has opened." << std::endl;
    		last_stream_id_ = stream_id;
    		auto stream = std::make_unique<Stream>(stream_id);
    		streams_.emplace(stream_id, std::move(stream));
		}
		{
			ev_io_set(&asyncServer_, fileno(ovsdb_fp), EV_READ);
        	ev_io_start(loop_, &asyncServer_);

			rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, SERVER_TYPE, OVSDB);
    		if (rv != 0) {
        		std::cerr << "ngtcp2_conn_open_bidi_stream: " << ngtcp2_strerror(rv)
                    		<< std::endl;
        		if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
            		return 0;
        		}
        		return -1;
    		}

    		std::cerr << "The stream " << stream_id << " has opened." << std::endl;
    		last_stream_id_ = stream_id;
    		auto stream1 = std::make_unique<Stream>(stream_id);
    		streams_.emplace(stream_id, std::move(stream1));
		}
	}

	return 0;
}

int Handler::tx_rx_server(uint64_t max_stream_id) {
  
	int rv;
	if (last_stream_id_ != 0) {
      	return 0;
	}

	if (start_tx_rx_server() != 0) {
      return -1;
    }

  	if(openflow_fp) {
		if (fileno(openflow_fp) != -1) {
    		for (; nstreams_done_ < server_nstreams; ++nstreams_done_) {
      			uint64_t stream_id;

      			rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, SERVER_TYPE, OFL);
      			if (rv != 0) {
        			assert(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);
        			break;
      			}

      			last_stream_id_ = stream_id;

      			auto stream = std::make_unique<Stream>(stream_id);
      			stream->buffer_file();
      			streams_.emplace(stream_id, std::move(stream));
    		}
    	// return 0;
  		}
	}
	
	if(ovsdb_fp) {
		if (fileno(ovsdb_fp) != -1) {
            for (; nstreams_done_ < server_nstreams; ++nstreams_done_) {
                uint64_t stream_id;
                
                rv = ngtcp2_conn_open_bidi_stream_wrapper(conn_, &stream_id, nullptr, SERVER_TYPE, OVSDB);
                if (rv != 0) {
                    assert(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);
                    break;
                }
                
                last_stream_id_ = stream_id;
                
                auto stream = std::make_unique<Stream>(stream_id);
                stream->buffer_file();
                streams_.emplace(stream_id, std::move(stream));
            }
        // return 0;
        }
	}

  return 0;
}

namespace {
int extend_max_stream_id(ngtcp2_conn *conn, uint64_t max_stream_id,
                         void *user_data) {

	auto c = static_cast<Handler *>(user_data);
    if (c->tx_rx_server(max_stream_id) != 0) {
    	return NGTCP2_ERR_CALLBACK_FAILURE;
  	}

  return 0;
}
}

int Handler::init(int fd, const sockaddr *sa, socklen_t salen,
                  const ngtcp2_cid *dcid, uint32_t version) {
  int rv;

  remote_addr_.len = salen;
  memcpy(&remote_addr_.su.sa, sa, salen);

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
  ssl_ = SSL_new(ssl_ctx_);
  auto bio = BIO_new(create_bio_method());
  BIO_set_data(bio, this);
  SSL_set_bio(ssl_, bio, bio);
  SSL_set_app_data(ssl_, this);
  SSL_set_accept_state(ssl_);
  SSL_set_msg_callback(ssl_, msg_cb);
  SSL_set_msg_callback_arg(ssl_, this);
  SSL_set_key_callback(ssl_, key_cb, this);

  auto callbacks = ngtcp2_conn_callbacks{
      nullptr,
      ::recv_client_initial,
      recv_crypto_data,
      handshake_completed,
      nullptr,
      do_hs_encrypt,
      do_hs_decrypt,
      do_encrypt,
      do_decrypt,
      do_hs_encrypt_pn,
      do_encrypt_pn,
      ::recv_stream_data,
      acked_crypto_offset,
      acked_stream_data_offset,
      stream_close,
      nullptr, // recv_stateless_reset
      nullptr, // recv_server_stateless_retry
      extend_max_stream_id, // extend_max_stream_id
      rand,
  };

  ngtcp2_settings settings{};

  settings.log_printf = config.quiet ? nullptr : debug::log_printf;
  settings.initial_ts = util::timestamp(loop_);
  settings.max_stream_data = 256_k;
  settings.max_data = 1_m;
  settings.max_bidi_streams = 100;
  settings.max_uni_streams = 0;
  settings.idle_timeout = config.timeout;
  settings.max_packet_size = NGTCP2_MAX_PKT_SIZE;
  settings.ack_delay_exponent = NGTCP2_DEFAULT_ACK_DELAY_EXPONENT;
  settings.stateless_reset_token_present = 1;

  auto dis = std::uniform_int_distribution<uint8_t>(0, 255);
  std::generate(std::begin(settings.stateless_reset_token),
                std::end(settings.stateless_reset_token),
                [&dis]() { return dis(randgen); });

  ngtcp2_cid scid;
  scid.datalen = NGTCP2_SV_SCIDLEN;
  std::generate(scid.data, scid.data + scid.datalen,
                [&dis]() { return dis(randgen); });

  rv = ngtcp2_conn_server_new(&conn_, dcid, &scid, version, &callbacks,
                              &settings, this);
  if (rv != 0) {
    std::cerr << "ngtcp2_conn_server_new: " << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  ev_timer_again(loop_, &timer_);

  return 0;
}

int Handler::tls_handshake() {
  ERR_clear_error();

  int rv;

  if (initial_) {
    std::array<uint8_t, 8> buf;
    size_t nread;
    rv = SSL_read_early_data(ssl_, buf.data(), buf.size(), &nread);
    initial_ = false;
    switch (rv) {
    case SSL_READ_EARLY_DATA_ERROR: {
      std::cerr << "SSL_READ_EARLY_DATA_ERROR" << std::endl;
      auto err = SSL_get_error(ssl_, rv);
      switch (err) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE: {
        return 0;
      }
      case SSL_ERROR_SSL:
        std::cerr << "TLS handshake error: "
                  << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        return NGTCP2_ERR_CRYPTO;
      default:
        std::cerr << "TLS handshake error: " << err << std::endl;
        return NGTCP2_ERR_CRYPTO;
      }
      break;
    }
    case SSL_READ_EARLY_DATA_SUCCESS:
      std::cerr << "SSL_READ_EARLY_DATA_SUCCESS" << std::endl;
      // Reading 0-RTT data in TLS stream is a protocol violation.
      if (nread > 0) {
        return NGTCP2_ERR_PROTO;
      }
      break;
    case SSL_READ_EARLY_DATA_FINISH:
      std::cerr << "SSL_READ_EARLY_DATA_FINISH" << std::endl;
      break;
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
      return NGTCP2_ERR_CRYPTO;
    default:
      std::cerr << "TLS handshake error: " << err << std::endl;
      return NGTCP2_ERR_CRYPTO;
    }
  }

  // SSL_do_handshake returns 1 if TLS handshake has completed.  With
  // boringSSL, it may return 1 if we have 0-RTT early data.  This is
  // a problem, but for First Implementation draft, 0-RTT early data
  // is out of interest.
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

int Handler::read_tls() {
  ERR_clear_error();

  std::array<uint8_t, 4096> buf;
  size_t nread;

  for (;;) {
    auto rv = SSL_read_ex(ssl_, buf.data(), buf.size(), &nread);
    if (rv == 1) {
      std::cerr << "Read " << nread << " bytes from TLS crypto stream"
                << std::endl;
      return NGTCP2_ERR_PROTO;
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

int Handler::write_server_handshake(const uint8_t *data, size_t datalen) {
  write_server_handshake(shandshake_, shandshake_idx_, data, datalen);

  return 0;
}

void Handler::write_server_handshake(std::deque<Buffer> &dest, size_t &idx,
                                     const uint8_t *data, size_t datalen) {
  dest.emplace_back(data, datalen);
  ++idx;

  auto &buf = dest.back();

  ngtcp2_conn_submit_crypto_data(conn_, buf.rpos(), buf.size());
}

size_t Handler::read_server_handshake(const uint8_t **pdest) {
  if (shandshake_idx_ == shandshake_.size()) {
    return 0;
  }
  auto &v = shandshake_[shandshake_idx_++];
  *pdest = v.rpos();
  return v.size();
}

size_t Handler::read_client_handshake(uint8_t *buf, size_t buflen) {
  auto n = std::min(buflen, chandshake_.size() - ncread_);
  std::copy_n(std::begin(chandshake_) + ncread_, n, buf);
  ncread_ += n;
  return n;
}

void Handler::write_client_handshake(const uint8_t *data, size_t datalen) {
  std::copy_n(data, datalen, std::back_inserter(chandshake_));
}

int Handler::recv_client_initial(const ngtcp2_cid *dcid) {
  int rv;
  std::array<uint8_t, 32> initial_secret, secret;

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

  rv = crypto::derive_server_initial_secret(secret.data(), secret.size(),
                                            initial_secret.data(),
                                            initial_secret.size());
  if (rv != 0) {
    std::cerr << "crypto::derive_server_initial_secret() failed" << std::endl;
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
    debug::print_server_in_secret(secret.data(), secret.size());
    debug::print_server_pp_key(key.data(), keylen);
    debug::print_server_pp_iv(iv.data(), ivlen);
    debug::print_server_pp_pn(pn.data(), pnlen);
  }

  ngtcp2_conn_set_initial_tx_keys(conn_, key.data(), keylen, iv.data(), ivlen,
                                  pn.data(), pnlen);

  rv = crypto::derive_client_initial_secret(secret.data(), secret.size(),
                                            initial_secret.data(),
                                            initial_secret.size());
  if (rv != 0) {
    std::cerr << "crypto::derive_client_initial_secret() failed" << std::endl;
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
    debug::print_client_in_secret(secret.data(), secret.size());
    debug::print_client_pp_key(key.data(), keylen);
    debug::print_client_pp_iv(iv.data(), ivlen);
    debug::print_client_pp_pn(pn.data(), pnlen);
  }

  ngtcp2_conn_set_initial_rx_keys(conn_, key.data(), keylen, iv.data(), ivlen,
                                  pn.data(), pnlen);

  return 0;
}

ssize_t Handler::hs_encrypt_data(uint8_t *dest, size_t destlen,
                                 const uint8_t *plaintext, size_t plaintextlen,
                                 const uint8_t *key, size_t keylen,
                                 const uint8_t *nonce, size_t noncelen,
                                 const uint8_t *ad, size_t adlen) {
  return crypto::encrypt(dest, destlen, plaintext, plaintextlen, hs_crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

ssize_t Handler::hs_decrypt_data(uint8_t *dest, size_t destlen,
                                 const uint8_t *ciphertext,
                                 size_t ciphertextlen, const uint8_t *key,
                                 size_t keylen, const uint8_t *nonce,
                                 size_t noncelen, const uint8_t *ad,
                                 size_t adlen) {
  return crypto::decrypt(dest, destlen, ciphertext, ciphertextlen,
                         hs_crypto_ctx_, key, keylen, nonce, noncelen, ad,
                         adlen);
}

ssize_t Handler::encrypt_data(uint8_t *dest, size_t destlen,
                              const uint8_t *plaintext, size_t plaintextlen,
                              const uint8_t *key, size_t keylen,
                              const uint8_t *nonce, size_t noncelen,
                              const uint8_t *ad, size_t adlen) {
  return crypto::encrypt(dest, destlen, plaintext, plaintextlen, crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

ssize_t Handler::decrypt_data(uint8_t *dest, size_t destlen,
                              const uint8_t *ciphertext, size_t ciphertextlen,
                              const uint8_t *key, size_t keylen,
                              const uint8_t *nonce, size_t noncelen,
                              const uint8_t *ad, size_t adlen) {
  return crypto::decrypt(dest, destlen, ciphertext, ciphertextlen, crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

ssize_t Handler::hs_encrypt_pn(uint8_t *dest, size_t destlen,
                               const uint8_t *ciphertext, size_t ciphertextlen,
                               const uint8_t *key, size_t keylen,
                               const uint8_t *nonce, size_t noncelen) {
  return crypto::encrypt_pn(dest, destlen, ciphertext, ciphertextlen,
                            hs_crypto_ctx_, key, keylen, nonce, noncelen);
}

ssize_t Handler::encrypt_pn(uint8_t *dest, size_t destlen,
                            const uint8_t *ciphertext, size_t ciphertextlen,
                            const uint8_t *key, size_t keylen,
                            const uint8_t *nonce, size_t noncelen) {
  return crypto::encrypt_pn(dest, destlen, ciphertext, ciphertextlen,
                            crypto_ctx_, key, keylen, nonce, noncelen);
}

ssize_t Handler::do_handshake_once(const uint8_t *data, size_t datalen) {
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
    return -1;
  }

  if (nwrite == 0) {
    return 0;
  }

  sendbuf_.push(nwrite);

  auto rv = server_->send_packet(remote_addr_, sendbuf_);
  if (rv == NETWORK_ERR_SEND_NON_FATAL) {
    schedule_retransmit();
    return rv;
  }
  if (rv != NETWORK_ERR_OK) {
    return rv;
  }

  return nwrite;
}

int Handler::do_handshake(const uint8_t *data, size_t datalen) {
  ssize_t nwrite;

  if (sendbuf_.size() > 0) {
    auto rv = server_->send_packet(remote_addr_, sendbuf_);
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

int Handler::feed_data(uint8_t *data, size_t datalen) {
  int rv;

  if (ngtcp2_conn_get_handshake_completed(conn_)) {
    rv = ngtcp2_conn_recv(conn_, data, datalen, util::timestamp(loop_));
    if (rv != 0) {
      std::cerr << "ngtcp2_conn_recv: " << ngtcp2_strerror(rv) << std::endl;
      if (rv == NGTCP2_ERR_DRAINING) {
        start_draining_period();
        return NETWORK_ERR_CLOSE_WAIT;
      }
      if (rv != NGTCP2_ERR_TLS_DECRYPT) {
        return handle_error(rv);
      }
    }
  } else {
    rv = do_handshake(data, datalen);
    if (rv != 0) {
      return handle_error(rv);
    }
  }

  return 0;
}

int Handler::on_read(uint8_t *data, size_t datalen) {
  int rv;

  rv = feed_data(data, datalen);
  if (rv != 0) {
    return rv;
  }

  ev_timer_again(loop_, &timer_);

  return 0;
}

int Handler::on_write(bool retransmit) {
  int rv;

  if (ngtcp2_conn_is_in_closing_period(conn_)) {
    return 0;
  }

  if (sendbuf_.size() > 0) {
    auto rv = server_->send_packet(remote_addr_, sendbuf_);
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }
  }

  assert(sendbuf_.left() >= max_pktlen_);

  if (retransmit) {
    rv = ngtcp2_conn_on_loss_detection_alarm(conn_, util::timestamp(loop_));
    if (rv != 0) {
      std::cerr << "ngtcp2_conn_on_loss_detection_alarm: "
                << ngtcp2_strerror(rv) << std::endl;
      return -1;
    }
  }

  if (!ngtcp2_conn_get_handshake_completed(conn_)) {
    rv = do_handshake(nullptr, 0);
    if (rv == NETWORK_ERR_SEND_NON_FATAL) {
      schedule_retransmit();
    }
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }
  }

  for (auto &p : streams_) {
    auto &stream = p.second;
    rv = on_write_stream(*stream);
    if (rv != 0) {
      if (rv == NETWORK_ERR_SEND_NON_FATAL) {
        schedule_retransmit();
        return rv;
      }
      return rv;
    }
  }

  if (!ngtcp2_conn_get_handshake_completed(conn_)) {
    schedule_retransmit();
    return 0;
  }

  for (;;) {
    auto n = ngtcp2_conn_write_pkt(conn_, sendbuf_.wpos(), max_pktlen_,
                                   util::timestamp(loop_));
    if (n < 0) {
      if (n == NGTCP2_ERR_NOBUF || n == NGTCP2_ERR_CONGESTION) {
        break;
      }
      std::cerr << "ngtcp2_conn_write_pkt: " << ngtcp2_strerror(n) << std::endl;
      return handle_error(n);
    }
    if (n == 0) {
      break;
    }

    sendbuf_.push(n);

    auto rv = server_->send_packet(remote_addr_, sendbuf_);
    if (rv == NETWORK_ERR_SEND_NON_FATAL) {
      schedule_retransmit();
      return rv;
    }
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }
  }

  schedule_retransmit();
  return 0;
}

int Handler::on_write_stream(Stream &stream) {
  if (stream.streambuf_idx == stream.streambuf.size()) {
    if (stream.should_send_fin) {
      auto v = Buffer{};
      if (write_stream_data(stream, 1, v) != 0) {
        return -1;
      }
    }
    return 0;
  }

  for (auto it = std::begin(stream.streambuf) + stream.streambuf_idx;
       it != std::end(stream.streambuf); ++it) {
    auto &v = *it;
    auto fin = stream.should_send_fin &&
               stream.streambuf_idx == stream.streambuf.size() - 1;
    auto rv = write_stream_data(stream, fin, v);
    if (rv != 0) {
      return rv;
    }
    if (v.size() > 0) {
      break;
    }
    ++stream.streambuf_idx;
  }

  return 0;
}

int Handler::write_stream_data(Stream &stream, int fin, Buffer &data) {
  ssize_t ndatalen;

  for (;;) {
    auto n = ngtcp2_conn_write_stream(
        conn_, sendbuf_.wpos(), max_pktlen_, &ndatalen, stream.stream_id, fin,
        data.rpos(), data.size(), util::timestamp(loop_));
    if (n < 0) {
      switch (n) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
      case NGTCP2_ERR_STREAM_SHUT_WR:
      case NGTCP2_ERR_NOBUF:
      case NGTCP2_ERR_CONGESTION:
        return 0;
      }
      std::cerr << "ngtcp2_conn_write_stream: " << ngtcp2_strerror(n)
                << std::endl;
      return handle_error(n);
    }

    if (n == 0) {
      return 0;
    }

    if (ndatalen >= 0) {
      if (fin && static_cast<size_t>(ndatalen) == data.size()) {
        stream.should_send_fin = false;
      }

      data.seek(ndatalen);
    }

    sendbuf_.push(n);

    auto rv = server_->send_packet(remote_addr_, sendbuf_);
    if (rv != NETWORK_ERR_OK) {
      return rv;
    }

    if (data.size() == 0) {
      break;
    }
  }

  return 0;
}

bool Handler::draining() const { return draining_; }

void Handler::start_draining_period() {
  draining_ = true;

  ev_timer_stop(loop_, &rttimer_);

  timer_.repeat = 15.;
  ev_timer_again(loop_, &timer_);

  std::cerr << "Draining period has started" << std::endl;
}

int Handler::start_closing_period(int liberr) {
  if (!conn_ || ngtcp2_conn_is_in_closing_period(conn_)) {
    return 0;
  }

  ev_timer_stop(loop_, &rttimer_);

  timer_.repeat = 15.;
  ev_timer_again(loop_, &timer_);

  std::cerr << "Closing period has started" << std::endl;

  sendbuf_.reset();
  assert(sendbuf_.left() >= max_pktlen_);

  conn_closebuf_ = std::make_unique<Buffer>(NGTCP2_MAX_PKTLEN_IPV4);

  auto n = ngtcp2_conn_write_connection_close(
      conn_, conn_closebuf_->wpos(), max_pktlen_,
      ngtcp2_err_infer_quic_transport_error_code(liberr),
      util::timestamp(loop_));
  if (n < 0) {
    std::cerr << "ngtcp2_conn_write_connection_close: " << ngtcp2_strerror(n)
              << std::endl;
    return -1;
  }

  conn_closebuf_->push(n);

  return 0;
}

int Handler::handle_error(int liberr) {
  int rv;

  rv = start_closing_period(liberr);
  if (rv != 0) {
    return -1;
  }

  rv = send_conn_close();
  if (rv != NETWORK_ERR_OK) {
    return rv;
  }

  return NETWORK_ERR_CLOSE_WAIT;
}

int Handler::send_conn_close() {
  if (!config.quiet) {
    std::cerr << "Closing Period: TX CONNECTION_CLOSE" << std::endl;
  }

  assert(conn_closebuf_ && conn_closebuf_->size());

  if (sendbuf_.size() == 0) {
    std::copy_n(conn_closebuf_->rpos(), conn_closebuf_->size(),
                sendbuf_.wpos());
    sendbuf_.push(conn_closebuf_->size());
  }

  return server_->send_packet(remote_addr_, sendbuf_);
}

void Handler::schedule_retransmit() {
  auto expiry = std::min(ngtcp2_conn_loss_detection_expiry(conn_),
                         ngtcp2_conn_ack_delay_expiry(conn_));
  auto now = util::timestamp(loop_);
  auto t =
      expiry < now ? 1e-9 : static_cast<ev_tstamp>(expiry - now) / 1000000000;
  rttimer_.repeat = t;
  ev_timer_again(loop_, &rttimer_);
}

int Handler::recv_stream_data(uint64_t stream_id, uint8_t fin,
                              const uint8_t *data, size_t datalen) {
  	int rv;
	ssize_t nread_x;

  	if (!config.quiet) {
    		debug::print_stream_data(stream_id, data, datalen);
  	}

  	// codeflow: here we are receiving data

  	auto it = streams_.find(stream_id);
  	if (it == std::end(streams_)) {
    	it = streams_.emplace(stream_id, std::make_unique<Stream>(stream_id)).first;
  	}

  	auto &stream = (*it).second;

  	ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, datalen);
 	ngtcp2_conn_extend_max_offset(conn_, datalen);

	ssize_t nread;
	uint8_t buf[4096];

    printf("Stream ID %d\n", stream_id);

	if(stream_id % 2 == 0 && stream_id % 3 == 0){
		sendto(openflow_sock, (const char *)data, datalen,
                MSG_CONFIRM, (const struct sockaddr *)&ofl_servaddr,
                sizeof(ofl_servaddr));
	}
	else {
		sendto(ovsdb_sock, (const char *)data, datalen,  
        		MSG_CONFIRM, (const struct sockaddr *)&ovsdb_servaddr, 
            	sizeof(ovsdb_servaddr)); 
	}
	return 0;
}

const ngtcp2_cid *Handler::scid() const { return ngtcp2_conn_get_scid(conn_); }

const ngtcp2_cid *Handler::rcid() const { return &rcid_; }

Server *Handler::server() const { return server_; }

const Address &Handler::remote_addr() const { return remote_addr_; }

ngtcp2_conn *Handler::conn() const { return conn_; }

namespace {
size_t remove_tx_stream_data(std::deque<Buffer> &d, size_t &idx,
                             uint64_t &tx_offset, uint64_t offset) {
  size_t len = 0;
  for (; !d.empty() && tx_offset + d.front().bufsize() <= offset;) {
    --idx;
    auto &v = d.front();
    len += v.bufsize();
    tx_offset += v.bufsize();
    d.pop_front();
  }
  return len;
}
} // namespace

void Handler::remove_tx_crypto_data(uint64_t offset, size_t datalen) {
  ::remove_tx_stream_data(shandshake_, shandshake_idx_, tx_crypto_offset_,
                          offset + datalen);
}

int Handler::remove_tx_stream_data(uint64_t stream_id, uint64_t offset,
                                   size_t datalen) {
  int rv;

  auto it = streams_.find(stream_id);
  assert(it != std::end(streams_));
  auto &stream = (*it).second;
  ::remove_tx_stream_data(stream->streambuf, stream->streambuf_idx,
                          stream->tx_stream_offset, offset + datalen);

  if (stream->streambuf.empty() && stream->resp_state == RESP_COMPLETED) {
    rv = ngtcp2_conn_shutdown_stream_read(conn_, stream_id, NGTCP2_APP_NOERROR);
    if (rv != 0 && rv != NGTCP2_ERR_STREAM_NOT_FOUND) {
      std::cerr << "ngtcp2_conn_shutdown_stream_read: " << ngtcp2_strerror(rv)
                << std::endl;
      return -1;
    }
  }

  return 0;
}

int Handler::send_greeting(uint8_t *buf, size_t buf_len, int protocol_type) {
	
	int rv;
	uint64_t stream_id;
	
	if(protocol_type == OFL) {

		printf("Coming to send_greeting OFL\n");

		rv = ngtcp2_conn_open_uni_stream_wrapper(conn_, &stream_id, 
												nullptr, SERVER_TYPE, OFL);
		printf("After stream wrapper %d\n", stream_id);
		auto stream = std::make_unique<Stream>(stream_id);
		stream->streambuf.emplace_back(buf, buf_len);
		stream->should_send_fin = false;
		stream->resp_state = RESP_COMPLETED;
		streams_.emplace(stream_id, std::move(stream));
	}
	else if(protocol_type == OVSDB){

		printf("Coming to send_greeting OVSDB\n");

		rv = ngtcp2_conn_open_uni_stream_wrapper(conn_, &stream_id, 
                                                nullptr, SERVER_TYPE, OVSDB);
        auto stream = std::make_unique<Stream>(stream_id);
        stream->streambuf.emplace_back(buf, buf_len);
        stream->should_send_fin = false;
        stream->resp_state = RESP_COMPLETED;
        streams_.emplace(stream_id, std::move(stream));
	}
	else {
		printf("Unrecognized protocol type\n");
		return 0;
	}
/*
	rv = ngtcp2_conn_open_uni_stream(conn_, &stream_id, nullptr);
	if (rv != 0) {
		return 0;
	}

	auto stream = std::make_unique<Stream>(stream_id);
	//static constexpr uint8_t hw[] = "Hello World!";
	// static constexpr uint8_t hw[] = buf;
	stream->streambuf.emplace_back(buf, buf_len);
	stream->should_send_fin = false;
	stream->resp_state = RESP_COMPLETED;
	streams_.emplace(stream_id, std::move(stream));
*/
	return 0;
}


void Handler::on_stream_close(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  assert(it != std::end(streams_));
  streams_.erase(it);
}

namespace {
void swritecb(struct ev_loop *loop, ev_io *w, int revents) {
  ev_io_stop(loop, w);

  auto s = static_cast<Server *>(w->data);

  auto rv = s->on_write();
  if (rv != 0) {
    if (rv == NETWORK_ERR_SEND_NON_FATAL) {
      s->start_wev();
    }
  }
}
} // namespace

namespace {
void sreadcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto s = static_cast<Server *>(w->data);

  s->on_read();
}
} // namespace

namespace {
void siginthandler(struct ev_loop *loop, ev_signal *watcher, int revents) {
  ev_break(loop, EVBREAK_ALL);
}
} // namespace

Server::Server(struct ev_loop *loop, SSL_CTX *ssl_ctx)
    : loop_(loop), ssl_ctx_(ssl_ctx), fd_(-1) {
  ev_io_init(&wev_, swritecb, 0, EV_WRITE);
  ev_io_init(&rev_, sreadcb, 0, EV_READ);
  wev_.data = this;
  rev_.data = this;
  ev_signal_init(&sigintev_, siginthandler, SIGINT);
}

Server::~Server() {
  disconnect();
  close();
}

void Server::disconnect() { disconnect(0); }

void Server::disconnect(int liberr) {
  config.tx_loss_prob = 0;

  ev_io_stop(loop_, &rev_);

  ev_signal_stop(loop_, &sigintev_);

  while (!handlers_.empty()) {
    auto it = std::begin(handlers_);
    auto &h = (*it).second;

    h->handle_error(0);

    remove(it);
  }
}

void Server::close() {
  ev_io_stop(loop_, &wev_);

  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
}

int Server::init(int fd) {
  fd_ = fd;

  ev_io_set(&wev_, fd_, EV_WRITE);
  ev_io_set(&rev_, fd_, EV_READ);

  ev_io_start(loop_, &rev_);

  ev_signal_start(loop_, &sigintev_);

  return 0;
}

int Server::on_write() {
  for (auto it = std::cbegin(handlers_); it != std::cend(handlers_);) {
    auto h = it->second.get();
    auto rv = h->on_write();
    switch (rv) {
    case 0:
    case NETWORK_ERR_CLOSE_WAIT:
      ++it;
      continue;
    case NETWORK_ERR_SEND_NON_FATAL:
      return NETWORK_ERR_SEND_NON_FATAL;
    }
    it = remove(it);
  }

  return NETWORK_ERR_OK;
}

int Server::on_read() {
  sockaddr_union su;
  socklen_t addrlen = sizeof(su);
  std::array<uint8_t, 64_k> buf;
  int rv;
  ngtcp2_pkt_hd hd;

  while (true) {
    auto nread =
        recvfrom(fd_, buf.data(), buf.size(), MSG_DONTWAIT, &su.sa, &addrlen);
    if (nread == -1) {
      if (!(errno == EAGAIN || errno == ENOTCONN)) {
        std::cerr << "recvfrom: " << strerror(errno) << std::endl;
      }
      return 0;
    }

    if (debug::packet_lost(config.rx_loss_prob)) {
      if (!config.quiet) {
        std::cerr << "** Simulated incoming packet loss **" << std::endl;
      }
      return 0;
    }

    if (nread == 0) {
      continue;
    }

    if (buf[0] & 0x80) {
      rv = ngtcp2_pkt_decode_hd_long(&hd, buf.data(), nread);
    } else {
      // TODO For Short packet, we just need DCID.
      rv =
          ngtcp2_pkt_decode_hd_short(&hd, buf.data(), nread, NGTCP2_SV_SCIDLEN);
    }
    if (rv < 0) {
      std::cerr << "Could not decode QUIC packet header: "
                << ngtcp2_strerror(rv) << std::endl;
      return 0;
    }

    auto dcid_key = util::make_cid_key(&hd.dcid);

    auto handler_it = handlers_.find(dcid_key);
    if (handler_it == std::end(handlers_)) {
      auto ctos_it = ctos_.find(dcid_key);
      if (ctos_it == std::end(ctos_)) {
        constexpr size_t MIN_PKT_SIZE = 1200;
        if (static_cast<size_t>(nread) < MIN_PKT_SIZE) {
          if (!config.quiet) {
            std::cerr << "Initial packet is too short: " << nread << " < "
                      << MIN_PKT_SIZE << std::endl;
          }
          return 0;
        }

        rv = ngtcp2_accept(&hd, buf.data(), nread);
        if (rv == -1) {
          if (!config.quiet) {
            std::cerr << "Unexpected packet received" << std::endl;
          }
          return 0;
        }
        if (rv == 1) {
          if (!config.quiet) {
            std::cerr << "Unsupported version: Send Version Negotiation"
                      << std::endl;
          }
          send_version_negotiation(&hd, &su.sa, addrlen);
          return 0;
        }

        auto h = std::make_unique<Handler>(loop_, ssl_ctx_, this, &hd.dcid);
        h->init(fd_, &su.sa, addrlen, &hd.scid, hd.version);

        if (h->on_read(buf.data(), nread) != 0) {
          return 0;
        }
        rv = h->on_write();
        switch (rv) {
        case 0:
          break;
        case NETWORK_ERR_SEND_NON_FATAL:
          start_wev();
          break;
        default:
          return 0;
        }

        auto scid = h->scid();
        auto scid_key = util::make_cid_key(scid);
        handlers_.emplace(scid_key, std::move(h));
        ctos_.emplace(dcid_key, scid_key);
        return 0;
      }
      if (!config.quiet) {
        std::cerr << "Forward CID=" << util::format_hex((*ctos_it).first)
                  << " to CID=" << util::format_hex((*ctos_it).second)
                  << std::endl;
      }
      handler_it = handlers_.find((*ctos_it).second);
      assert(handler_it != std::end(handlers_));
    }

    auto h = (*handler_it).second.get();
    if (ngtcp2_conn_is_in_closing_period(h->conn())) {
      // TODO do exponential backoff.
      rv = h->send_conn_close();
      switch (rv) {
      case 0:
      case NETWORK_ERR_SEND_NON_FATAL:
        break;
      default:
        remove(handler_it);
      }
      return 0;
    }
    if (h->draining()) {
      return 0;
    }

    rv = h->on_read(buf.data(), nread);
    if (rv != 0) {
      if (rv != NETWORK_ERR_CLOSE_WAIT) {
        remove(handler_it);
      }
      return 0;
    }

    rv = h->on_write();
    switch (rv) {
    case 0:
    case NETWORK_ERR_CLOSE_WAIT:
      break;
    case NETWORK_ERR_SEND_NON_FATAL:
      start_wev();
      break;
    default:
      remove(handler_it);
    }
  }
  return 0;
}

namespace {
uint32_t generate_reserved_version(const sockaddr *sa, socklen_t salen,
                                   uint32_t version) {
  uint32_t h = 0x811C9DC5u;
  const uint8_t *p = (const uint8_t *)sa;
  const uint8_t *ep = p + salen;
  for (; p != ep; ++p) {
    h ^= *p;
    h *= 0x01000193u;
  }
  version = htonl(version);
  p = (const uint8_t *)&version;
  ep = p + sizeof(version);
  for (; p != ep; ++p) {
    h ^= *p;
    h *= 0x01000193u;
  }
  h &= 0xf0f0f0f0u;
  h |= 0x0a0a0a0au;
  return h;
}
} // namespace

int Server::send_version_negotiation(const ngtcp2_pkt_hd *chd,
                                     const sockaddr *sa, socklen_t salen) {
  Buffer buf{NGTCP2_MAX_PKTLEN_IPV4};
  std::array<uint32_t, 2> sv;

  sv[0] = generate_reserved_version(sa, salen, chd->version);
  sv[1] = NGTCP2_PROTO_VER_D13;

  auto nwrite = ngtcp2_pkt_write_version_negotiation(
      buf.wpos(), buf.left(),
      std::uniform_int_distribution<uint8_t>(
          0, std::numeric_limits<uint8_t>::max())(randgen),
      &chd->scid, &chd->dcid, sv.data(), sv.size());
  if (nwrite < 0) {
    std::cerr << "ngtcp2_pkt_write_version_negotiation: "
              << ngtcp2_strerror(nwrite) << std::endl;
    return -1;
  }

  buf.push(nwrite);

  Address remote_addr;
  remote_addr.len = salen;
  memcpy(&remote_addr.su.sa, sa, salen);

  if (send_packet(remote_addr, buf) != NETWORK_ERR_OK) {
    return -1;
  }

  return 0;
}

int Server::send_packet(Address &remote_addr, Buffer &buf) {
  if (debug::packet_lost(config.tx_loss_prob)) {
    if (!config.quiet) {
      std::cerr << "** Simulated outgoing packet loss **" << std::endl;
    }
    buf.reset();
    return NETWORK_ERR_OK;
  }

  int eintr_retries = 5;
  ssize_t nwrite = 0;

  do {
    nwrite = sendto(fd_, buf.rpos(), buf.size(), 0, &remote_addr.su.sa,
                    remote_addr.len);
  } while ((nwrite == -1) && (errno == EINTR) && (eintr_retries-- > 0));

  if (nwrite == -1) {
    switch (errno) {
    case EAGAIN:
    case EINTR:
    case 0:
      return NETWORK_ERR_SEND_NON_FATAL;
    default:
      std::cerr << "sendto: " << strerror(errno) << std::endl;
      return NETWORK_ERR_SEND_FATAL;
    }
  }

  assert(static_cast<size_t>(nwrite) == buf.size());
  buf.reset();

  return NETWORK_ERR_OK;
}

void Server::remove(const Handler *h) {
  ctos_.erase(util::make_cid_key(h->rcid()));
  handlers_.erase(util::make_cid_key(h->scid()));
}

std::map<std::string, std::unique_ptr<Handler>>::const_iterator Server::remove(
    std::map<std::string, std::unique_ptr<Handler>>::const_iterator it) {
  ctos_.erase(util::make_cid_key((*it).second->rcid()));
  return handlers_.erase(it);
}

void Server::start_wev() { ev_io_start(loop_, &wev_); }

namespace {
int alpn_select_proto_cb(SSL *ssl, const unsigned char **out,
                         unsigned char *outlen, const unsigned char *in,
                         unsigned int inlen, void *arg) {
  auto h = static_cast<Handler *>(SSL_get_app_data(ssl));
  const uint8_t *alpn;
  size_t alpnlen;
  auto version = ngtcp2_conn_get_negotiated_version(h->conn());

  switch (version) {
  case NGTCP2_PROTO_VER_D13:
    alpn = reinterpret_cast<const uint8_t *>(NGTCP2_ALPN_D13);
    alpnlen = str_size(NGTCP2_ALPN_D13);
    break;
  default:
    if (!config.quiet) {
      std::cerr << "Unexpected quic protocol version: " << std::hex << "0x"
                << version << std::endl;
    }
    return SSL_TLSEXT_ERR_NOACK;
  }

  for (auto p = in, end = in + inlen; p + alpnlen <= end; p += *p + 1) {
    if (std::equal(alpn, alpn + alpnlen, p)) {
      *out = p + 1;
      *outlen = *p;
      return SSL_TLSEXT_ERR_OK;
    }
  }
  // Just select alpn for now.
  *out = reinterpret_cast<const uint8_t *>(alpn + 1);
  *outlen = alpn[0];

  if (!config.quiet) {
    std::cerr << "Client did not present ALPN " << NGTCP2_ALPN_D13 + 1
              << std::endl;
  }

  return SSL_TLSEXT_ERR_OK;
}
} // namespace

namespace {
int transport_params_add_cb(SSL *ssl, unsigned int ext_type,
                            unsigned int context, const unsigned char **out,
                            size_t *outlen, X509 *x, size_t chainidx, int *al,
                            void *add_arg) {
  int rv;
  auto h = static_cast<Handler *>(SSL_get_app_data(ssl));
  auto conn = h->conn();

  ngtcp2_transport_params params;

  rv = ngtcp2_conn_get_local_transport_params(
      conn, &params, NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS);
  if (rv != 0) {
    *al = SSL_AD_INTERNAL_ERROR;
    return -1;
  }

  params.v.ee.len = 1;
  params.v.ee.supported_versions[0] = NGTCP2_PROTO_VER_D13;

  constexpr size_t bufsize = 512;
  auto buf = std::make_unique<uint8_t[]>(bufsize);

  auto nwrite = ngtcp2_encode_transport_params(
      buf.get(), bufsize, NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS,
      &params);
  if (nwrite < 0) {
    std::cerr << "ngtcp2_encode_transport_params: "
              << ngtcp2_strerror(static_cast<int>(nwrite)) << std::endl;
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
  if (context != SSL_EXT_CLIENT_HELLO) {
    *al = SSL_AD_ILLEGAL_PARAMETER;
    return -1;
  }

  auto h = static_cast<Handler *>(SSL_get_app_data(ssl));
  auto conn = h->conn();

  int rv;

  ngtcp2_transport_params params;

  rv = ngtcp2_decode_transport_params(
      &params, NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO, in, inlen);
  if (rv != 0) {
    std::cerr << "ngtcp2_decode_transport_params: " << ngtcp2_strerror(rv)
              << std::endl;
    *al = SSL_AD_ILLEGAL_PARAMETER;
    return -1;
  }

  rv = ngtcp2_conn_set_remote_transport_params(
      conn, NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO, &params);
  if (rv != 0) {
    *al = SSL_AD_ILLEGAL_PARAMETER;
    return -1;
  }

  return 1;
}
} // namespace

namespace {
SSL_CTX *create_ssl_ctx(const char *private_key_file, const char *cert_file) {
  auto ssl_ctx = SSL_CTX_new(TLS_method());

  constexpr auto ssl_opts = (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                            SSL_OP_SINGLE_ECDH_USE |
                            SSL_OP_CIPHER_SERVER_PREFERENCE;

  SSL_CTX_set_options(ssl_ctx, ssl_opts);
  SSL_CTX_clear_options(ssl_ctx, SSL_OP_ENABLE_MIDDLEBOX_COMPAT);

  if (SSL_CTX_set_cipher_list(ssl_ctx, config.ciphers) != 1) {
    std::cerr << "SSL_CTX_set_cipher_list: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  if (SSL_CTX_set1_groups_list(ssl_ctx, config.groups) != 1) {
    std::cerr << "SSL_CTX_set1_groups_list failed" << std::endl;
    goto fail;
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS | SSL_MODE_QUIC_HACK);

  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);

  SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_proto_cb, nullptr);

  SSL_CTX_set_default_verify_paths(ssl_ctx);

  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, private_key_file,
                                  SSL_FILETYPE_PEM) != 1) {
    std::cerr << "SSL_CTX_use_PrivateKey_file: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file) != 1) {
    std::cerr << "SSL_CTX_use_certificate_file: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    std::cerr << "SSL_CTX_check_private_key: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  if (SSL_CTX_add_custom_ext(
          ssl_ctx, NGTCP2_TLSEXT_QUIC_TRANSPORT_PARAMETERS,
          SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS,
          transport_params_add_cb, transport_params_free_cb, nullptr,
          transport_params_parse_cb, nullptr) != 1) {
    std::cerr << "SSL_CTX_add_custom_ext(NGTCP2_TLSEXT_QUIC_TRANSPORT_"
                 "PARAMETERS) failed: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  SSL_CTX_set_max_early_data(ssl_ctx, std::numeric_limits<uint32_t>::max());

  return ssl_ctx;

fail:
  SSL_CTX_free(ssl_ctx);
  return nullptr;
}
} // namespace

namespace {
int create_sock(const char *addr, const char *port, int family) {
  addrinfo hints{};
  addrinfo *res, *rp;
  int rv;
  int val = 1;

  hints.ai_family = family;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  if (strcmp("addr", "*") == 0) {
    addr = nullptr;
  }

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

    if (rp->ai_family == AF_INET6) {
      if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val,
                     static_cast<socklen_t>(sizeof(val))) == -1) {
        close(fd);
        continue;
      }
    }

    if (bind(fd, rp->ai_addr, rp->ai_addrlen) != -1) {
      break;
    }

    close(fd);
  }

  if (!rp) {
    std::cerr << "Could not bind" << std::endl;
    return -1;
  }

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                 static_cast<socklen_t>(sizeof(val))) == -1) {
    return -1;
  }

  return fd;
}

} // namespace

namespace {
int serve(Server &s, const char *addr, const char *port, int family) {
  auto fd = create_sock(addr, port, family);
  if (fd == -1) {
    return -1;
  }

  if (s.init(fd) != 0) {
    return -1;
  }

  return 0;
}
} // namespace

namespace {
void close(Server &s) {
  s.disconnect();

  s.close();
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
  std::cerr << "Usage: server [OPTIONS] <ADDR> <PORT> <PRIVATE_KEY_FILE> "
               "<CERTIFICATE_FILE>"
            << std::endl;
}
} // namespace

namespace {
void config_set_default(Config &config) {
  config = Config{};
  config.tx_loss_prob = 0.;
  config.rx_loss_prob = 0.;
  config.ciphers = "TLS13-AES-128-GCM-SHA256:TLS13-AES-256-GCM-SHA384:TLS13-"
                   "CHACHA20-POLY1305-SHA256";
  config.groups = "P-256:X25519:P-384:P-521";
  config.timeout = 30;
  {
    auto path = realpath(".", nullptr);
    config.htdocs = path;
    free(path);
  }
}
} // namespace

namespace {
void print_help() {
  print_usage();

  config_set_default(config);

  std::cout << R"(
  <ADDR>      Address to listen to.  '*' binds to any address.
  <PORT>      Port
  <PRIVATE_KEY_FILE>
              Path to private key file
  <CERTIFICATE_FILE>
              Path to certificate file
Options:
  -t, --tx-loss=<P>
              The probability of losing outgoing packets.  <P> must be
              [0.0, 1.0],  inclusive.  0.0 means no  packet loss.  1.0
              means 100% packet loss.
  -r, --rx-loss=<P>
              The probability of losing incoming packets.  <P> must be
              [0.0, 1.0],  inclusive.  0.0 means no  packet loss.  1.0
              means 100% packet loss.
  --ciphers=<CIPHERS>
              Specify the cipher suite list to enable.
              Default: )"
            << config.ciphers << R"(
  --groups=<GROUPS>
              Specify the supported groups.
              Default: )"
            << config.groups << R"(
  -d, --htdocs=<PATH>
              Specify document root.  If this option is not specified,
              the document root is the current working directory.
  -q, --quiet Suppress debug output.
  -s, --show-secret
              Print out secrets unless --quiet is used.
  --timeout=<T>
              Specify idle timeout in seconds.
              Default: )"
            << config.timeout << R"(
  -h, --help  Display this help and exit.
)";
}
} // namespace

int start_server(int ofl_sockfd, int odb_sockfd, 
				const char *addr, const char *port, 
				const char *private_key_file, 
				const char *cert_file, bool quiet, int m) {
  
	config_set_default(config);
	// config.quiet = quiet;
	config.quiet = false;
	mode = m;
	printf("Mode is %d\n", mode);
	
	if(!addr || !port || !private_key_file || !cert_file){
		printf("Check the parameters entered\n");
		return 1;
	}

  	errno = 0;
  	config.port = strtoul(port, nullptr, 10);
  		if (errno != 0) {
    		std::cerr << "port: invalid port number" << std::endl;
    		exit(EXIT_FAILURE);
  		}

  	auto ssl_ctx = create_ssl_ctx(private_key_file, cert_file);
  	if (ssl_ctx == nullptr) {
    	exit(EXIT_FAILURE);
  	}

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

	ovsdb_sock = odb_sockfd;
	openflow_sock = ofl_sockfd;
    ovsdb_fp = fdopen(ovsdb_sock,"r");
	openflow_fp = fdopen(openflow_sock,"r");
  	auto ready = false;

  	Server s4(EV_DEFAULT, ssl_ctx);
  	if (!util::numeric_host(addr, AF_INET6)) {
    	if (serve(s4, addr, port, AF_INET) == 0) {
      		ready = true;
    	}
  	}


  	if (!ready) {
    	exit(EXIT_FAILURE);
  	}

  	ev_run(EV_DEFAULT, 0);

  	close(s4);

  	return EXIT_SUCCESS;
}
