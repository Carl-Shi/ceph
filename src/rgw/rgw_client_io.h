// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_H
#define CEPH_RGW_CLIENT_IO_H

#include <exception>
#include <string>
#include <streambuf>
#include <istream>
#include <stdlib.h>
#include <system_error>

#include <boost/utility/string_ref.hpp>

#include "include/types.h"
#include "rgw_common.h"

class RGWClientIO {
protected:
  virtual void init_env(CephContext *cct) = 0;

public:
  virtual ~RGWClientIO() {}

  void init(CephContext *cct);
  virtual RGWEnv& get_env() noexcept = 0;
  virtual int complete_request() = 0;
}; /* RGWClient IO */


class RGWClientIOAccounter {
public:
  virtual ~RGWClientIOAccounter() {}

  virtual void set_account(bool enabled) = 0;

  virtual uint64_t get_bytes_sent() const = 0;
  virtual uint64_t get_bytes_received() const = 0;
};


class RGWRestfulIOEngine : public RGWClientIO {
  template<typename T> friend class RGWDecoratedRestfulIO;

public:
  typedef std::system_error Exception;

  virtual size_t send_status(int status, const char *status_name) = 0;
  virtual size_t send_100_continue() = 0;

  /* Send header to client. On success returns number of bytes sent to the direct
   * client of RadosGW. On failure throws int containing errno. boost::string_ref
   * is being used because of length it internally carries. */
  virtual size_t send_header(const boost::string_ref& name,
                             const boost::string_ref& value) = 0;

  /* Inform a client about a content length. Takes number of bytes supplied in
   * @len XOR one of the alternative modes for dealing with it passed as @mode.
   * On success returns number of bytes sent to the direct client of RadosGW.
   * On failure throws int containing errno.
   *
   * CALL ORDER:
   *  - The method must be called EXACTLY ONE time.
   *  - The method must be preceeded with a call to send_status().
   *  - The method must not be called after complete_header(). */
  virtual size_t send_content_length(uint64_t len) = 0;

  virtual size_t send_chunked_transfer_encoding() {
    /* This is a null implementation. We don't send anything here, even the HTTP
     * header. The intended behaviour should be provided through a decorator or
     * directly by a given front-end. */
    return 0;
  }

  virtual size_t complete_header() = 0;

  /* Receive body. On success Returns number of bytes sent to the direct
   * client of RadosGW. On failure throws int containing errno. */
  virtual size_t recv_body(char* buf, size_t max) = 0;
  virtual size_t send_body(const char* buf, size_t len) = 0;

  virtual void flush() = 0;
};


/* Abstract decorator over any implementation of RGWRestfulIOEngine. */
template <typename DecorateeT>
class RGWDecoratedRestfulIO : public RGWRestfulIOEngine {
  template<typename T> friend class RGWDecoratedRestfulIO;

  typedef typename std::remove_pointer<DecorateeT>::type DerefedDecorateeT;

  static_assert(std::is_base_of<RGWRestfulIOEngine, DerefedDecorateeT>::value,
                "DecorateeT must be a subclass of RGWRestfulIOEngine");

  DecorateeT decoratee;

  /* There is an indirection layer over accessing decoratee to share the same
   * code base between dynamic and static decorators. The difference is about
   * what we store internally: pointer to a decorated object versus the whole
   * object itself. */
  template <typename T = void,
            typename std::enable_if<
    std::is_pointer<DecorateeT>::value, T>::type* = nullptr>
  DerefedDecorateeT& get_decoratee() {
    return *decoratee;
  }

  template <typename T = void,
            typename std::enable_if<
    ! std::is_pointer<DecorateeT>::value, T>::type* = nullptr>
  DerefedDecorateeT& get_decoratee() {
    return decoratee;
  }

protected:
  void init_env(CephContext *cct) override {
    return get_decoratee().init_env(cct);
  }

public:
  RGWDecoratedRestfulIO(DecorateeT&& decoratee)
    : decoratee(std::move(decoratee)) {
  }

  size_t send_status(const int status,
                     const char* const status_name) override {
    return get_decoratee().send_status(status, status_name);
  }

  size_t send_100_continue() override {
    return get_decoratee().send_100_continue();
  }

  size_t send_header(const boost::string_ref& name,
                     const boost::string_ref& value) override {
    return get_decoratee().send_header(name, value);
  }

  size_t send_content_length(const uint64_t len) override {
    return get_decoratee().send_content_length(len);
  }

  size_t send_chunked_transfer_encoding() override {
    return get_decoratee().send_chunked_transfer_encoding();
  }

  size_t complete_header() override {
    return get_decoratee().complete_header();
  }

  size_t recv_body(char* const buf, const size_t max) override {
    return get_decoratee().recv_body(buf, max);
  }

  size_t send_body(const char* const buf,
                   const size_t len) override {
    return get_decoratee().send_body(buf, len);
  }

  void flush() override {
    return get_decoratee().flush();
  }

  RGWEnv& get_env() noexcept override {
    return get_decoratee().get_env();
  }

  int complete_request() override {
    return get_decoratee().complete_request();
  }
};


/* We're doing this nasty thing only because of extensive usage of templates
 * to implement the static decorator pattern. C++ templates de facto enforce
 * mixing interfaces with implementation. Additionally, those classes derive
 * from RGWRestfulIO defined here. I believe that including in the middle of
 * file is still better than polluting it directly. */
#include "rgw_client_io_decoimpl.h"


/* RGWRestfulIO: high level interface to interact with RESTful clients. What
 * differentiates it from RGWRestfulIOEngine is providing more specific APIs
 * like RGWClientIOAccounter or the AWS Auth v4 stuff implemented by filters
 * while hiding the pipelined architecture from clients.
 *
 * RGWClientIOAccounter came in as a part of RGWRestfulIOAccountingEngine. */
class RGWRestfulIO : public RGWRestfulIOAccountingEngine<RGWRestfulIOEngine*> {
  SHA256 *sha256_hash;

public:
  virtual ~RGWRestfulIO() {}

  RGWRestfulIO(RGWRestfulIOEngine* engine)
    : RGWRestfulIOAccountingEngine<RGWRestfulIOEngine*>(std::move(engine)),
      sha256_hash(nullptr) {
  }

  using RGWDecoratedRestfulIO<RGWRestfulIOEngine*>::recv_body;
  virtual int recv_body(char* buf, size_t max, bool calculate_hash);
  std::string grab_aws4_sha256_hash();
}; /* RGWRestfulIO */


/* Type conversions to work around lack of req_state type hierarchy matching
 * (e.g.) REST backends (may be replaced w/dynamic typed req_state). */
static inline RGWRestfulIO* RESTFUL_IO(struct req_state* s) {
  return static_cast<RGWRestfulIO*>(s->cio);
}

static inline RGWClientIOAccounter* ACCOUNTING_IO(struct req_state* s) {
  return dynamic_cast<RGWClientIOAccounter*>(s->cio);
}


class RGWClientIOStreamBuf : public std::streambuf {
protected:
  RGWRestfulIO &rio;
  size_t const window_size;
  size_t const putback_size;
  std::vector<char> buffer;

public:
  RGWClientIOStreamBuf(RGWRestfulIO &rio, size_t ws, size_t ps = 1)
    : rio(rio),
      window_size(ws),
      putback_size(ps),
      buffer(ws + ps)
  {
    setg(nullptr, nullptr, nullptr);
  }

  std::streambuf::int_type underflow() {
    if (gptr() < egptr()) {
      return traits_type::to_int_type(*gptr());
    }

    char * const base = buffer.data();
    char * start;

    if (nullptr != eback()) {
      /* We need to skip moving bytes on first underflow. In such case
       * there is simply no previous data we should preserve for unget()
       * or something similar. */
      std::memmove(base, egptr() - putback_size, putback_size);
      start = base + putback_size;
    } else {
      start = base;
    }

    const int read_len = rio.recv_body(base, window_size, false);
    if (read_len < 0 || 0 == read_len) {
      return traits_type::eof();
    }

    setg(base, start, start + read_len);

    return traits_type::to_int_type(*gptr());
  }
};

class RGWClientIOStream : private RGWClientIOStreamBuf, public std::istream {
/* Inheritance from RGWClientIOStreamBuf is a kind of shadow, undirect
 * form of composition here. We cannot do that explicitly because istream
 * ctor is being called prior to construction of any member of this class. */

public:
  explicit RGWClientIOStream(RGWRestfulIO &s)
    : RGWClientIOStreamBuf(s, 1, 2),
      istream(static_cast<RGWClientIOStreamBuf *>(this)) {
  }
};

#endif /* CEPH_RGW_CLIENT_IO_H */
