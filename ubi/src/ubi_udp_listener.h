#ifndef UBI_UDP_LISTENER_H_
#define UBI_UDP_LISTENER_H_

#include <cstddef>

#include <uv.h>

#include "node_api.h"

class UbiUdpWrapBase;

class UbiUdpSendWrap {
 public:
  virtual ~UbiUdpSendWrap() = default;
  virtual napi_value object(napi_env env) const = 0;

  uv_udp_send_t req{};
  uv_buf_t* bufs = nullptr;
  size_t nbufs = 0;
  size_t msg_size = 0;
  bool have_callback = false;
};

class UbiUdpListener {
 public:
  virtual ~UbiUdpListener();

  virtual uv_buf_t OnAlloc(size_t suggested_size) = 0;
  virtual void OnRecv(ssize_t nread,
                      const uv_buf_t& buf,
                      const sockaddr* addr,
                      unsigned int flags) = 0;
  virtual UbiUdpSendWrap* CreateSendWrap(size_t msg_size) = 0;
  virtual void OnSendDone(UbiUdpSendWrap* wrap, int status) = 0;
  virtual void OnAfterBind() {}

  UbiUdpWrapBase* udp() const { return wrap_; }

 protected:
  UbiUdpWrapBase* wrap_ = nullptr;

  friend class UbiUdpWrapBase;
};

class UbiUdpWrapBase {
 public:
  virtual ~UbiUdpWrapBase();

  virtual int RecvStart() = 0;
  virtual int RecvStop() = 0;
  virtual ssize_t Send(uv_buf_t* bufs,
                       size_t nbufs,
                       const sockaddr* addr) = 0;

  void set_listener(UbiUdpListener* listener);
  UbiUdpListener* listener() const;

 private:
  UbiUdpListener* listener_ = nullptr;
};

#endif  // UBI_UDP_LISTENER_H_
