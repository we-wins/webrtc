/*
 *  Copyright 2015 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef WEBRTC_P2P_STUNPROBER_STUNPROBER_H_
#define WEBRTC_P2P_STUNPROBER_STUNPROBER_H_

#include <set>
#include <string>
#include <vector>

#include "webrtc/base/basictypes.h"
#include "webrtc/base/bytebuffer.h"
#include "webrtc/base/callback.h"
#include "webrtc/base/ipaddress.h"
#include "webrtc/base/scoped_ptr.h"
#include "webrtc/base/socketaddress.h"
#include "webrtc/base/thread_checker.h"
#include "webrtc/typedefs.h"

namespace stunprober {

static const int kMaxUdpBufferSize = 1200;

typedef rtc::Callback1<void, int> AsyncCallback;

class HostNameResolverInterface {
 public:
  HostNameResolverInterface() {}

  // Resolve should allow re-entry as |callback| could trigger another
  // Resolve().
  virtual void Resolve(const rtc::SocketAddress& addr,
                       std::vector<rtc::SocketAddress>* addresses,
                       AsyncCallback callback) = 0;

  virtual ~HostNameResolverInterface() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(HostNameResolverInterface);
};

// Chrome has client and server socket. Client socket supports Connect but not
// Bind. Server is opposite.
class SocketInterface {
 public:
  enum {
    IO_PENDING = -1,
    FAILED = -2,
  };
  SocketInterface() {}
  virtual int GetLocalAddress(rtc::SocketAddress* local_address) = 0;
  virtual void Close() = 0;
  virtual ~SocketInterface() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(SocketInterface);
};

class ClientSocketInterface : public SocketInterface {
 public:
  ClientSocketInterface() {}
  // Even though we have SendTo and RecvFrom, if Connect is not called first,
  // getsockname will only return 0.0.0.0.
  virtual int Connect(const rtc::SocketAddress& addr) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(ClientSocketInterface);
};

class ServerSocketInterface : public SocketInterface {
 public:
  ServerSocketInterface() {}
  virtual int Bind(const rtc::SocketAddress& addr) = 0;

  virtual int SendTo(const rtc::SocketAddress& addr,
                     char* buf,
                     size_t buf_len,
                     AsyncCallback callback) = 0;

  // If the returned value is positive, it means that buf has been
  // sent. Otherwise, it should return IO_PENDING. Callback will be invoked
  // after the data is successfully read into buf.
  virtual int RecvFrom(char* buf,
                       size_t buf_len,
                       rtc::SocketAddress* addr,
                       AsyncCallback callback) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(ServerSocketInterface);
};

class SocketFactoryInterface {
 public:
  SocketFactoryInterface() {}
  virtual ClientSocketInterface* CreateClientSocket() = 0;
  virtual ServerSocketInterface* CreateServerSocket(
      size_t send_buffer_size,
      size_t receive_buffer_size) = 0;
  virtual ~SocketFactoryInterface() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(SocketFactoryInterface);
};

class TaskRunnerInterface {
 public:
  TaskRunnerInterface() {}
  virtual void PostTask(rtc::Callback0<void>, uint32_t delay_ms) = 0;
  virtual ~TaskRunnerInterface() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(TaskRunnerInterface);
};

class StunProber {
 public:
  enum Status {       // Used in UMA_HISTOGRAM_ENUMERATION.
    SUCCESS,          // Successfully received bytes from the server.
    GENERIC_FAILURE,  // Generic failure.
    RESOLVE_FAILED,   // Host resolution failed.
    WRITE_FAILED,     // Sending a message to the server failed.
    READ_FAILED,      // Reading the reply from the server failed.
  };

  struct Stats {
    Stats() {}
    int num_request_sent = 0;
    int num_response_received = 0;
    bool behind_nat = false;
    int average_rtt_ms = -1;
    int success_percent = 0;
    int target_request_interval_ns = 0;
    int actual_request_interval_ns = 0;

    // Also report whether this trial can't be considered truly as shared
    // mode. Share mode only makes sense when we have multiple IP resolved and
    // successfully probed.
    bool shared_socket_mode = false;

    std::string host_ip;

    // If the srflx_addrs has more than 1 element, the NAT is symmetric.
    std::set<std::string> srflx_addrs;

    bool symmetric_nat() { return srflx_addrs.size() > 1; }
  };

  // StunProber is not thread safe. It's task_runner's responsibility to ensure
  // all calls happen sequentially.
  StunProber(HostNameResolverInterface* host_name_resolver,
             SocketFactoryInterface* socket_factory,
             TaskRunnerInterface* task_runner);
  virtual ~StunProber();

  // Begin performing the probe test against the |servers|. If
  // |shared_socket_mode| is false, each request will be done with a new socket.
  // Otherwise, a unique socket will be used for a single round of requests
  // against all resolved IPs. No single socket will be used against a given IP
  // more than once.  The interval of requests will be as close to the requested
  // inter-probe interval |stun_ta_interval_ms| as possible. After sending out
  // the last scheduled request, the probe will wait |timeout_ms| for request
  // responses and then call |finish_callback|.  |requests_per_ip| indicates how
  // many requests should be tried for each resolved IP address. In shared mode,
  // (the number of sockets to be created) equals to |requests_per_ip|. In
  // non-shared mode, (the number of sockets) equals to requests_per_ip * (the
  // number of resolved IP addresses).
  bool Start(const std::vector<rtc::SocketAddress>& servers,
             bool shared_socket_mode,
             int stun_ta_interval_ms,
             int requests_per_ip,
             int timeout_ms,
             const AsyncCallback finish_callback);

  // Method to retrieve the Stats once |finish_callback| is invoked. Returning
  // false when the result is inconclusive, for example, whether it's behind a
  // NAT or not.
  bool GetStats(Stats* stats);

 private:
  // A requester tracks the requests and responses from a single socket to many
  // STUN servers
  class Requester {
   public:
    // Each Request maps to a request and response.
    struct Request {
      // Actual time the STUN bind request was sent.
      int64 sent_time_ms = 0;
      // Time the response was received.
      int64 received_time_ms = 0;

      // See whether the observed address returned matches the
      // local address as in StunProber.local_addr_.
      bool behind_nat = false;

      // Server reflexive address from STUN response for this given request.
      rtc::SocketAddress srflx_addr;

      rtc::IPAddress server_addr;

      int64 rtt() { return received_time_ms - sent_time_ms; }
      void ProcessResponse(rtc::ByteBuffer* message,
                           int buf_len,
                           const rtc::IPAddress& local_addr);
    };

    // StunProber provides |server_ips| for Requester to probe. For shared
    // socket mode, it'll be all the resolved IP addresses. For non-shared mode,
    // it'll just be a single address.
    Requester(StunProber* prober,
              ServerSocketInterface* socket,
              const std::vector<rtc::SocketAddress>& server_ips);
    virtual ~Requester();

    // There is no callback for SendStunRequest as the underneath socket send is
    // expected to be completed immediately. Otherwise, it'll skip this request
    // and move to the next one.
    void SendStunRequest();

    void ReadStunResponse();

    // |result| is the positive return value from RecvFrom when data is
    // available.
    void OnStunResponseReceived(int result);

    const std::vector<Request*>& requests() { return requests_; }

    // Whether this Requester has completed all requests.
    bool Done() {
      return static_cast<size_t>(num_request_sent_) == server_ips_.size();
    }

   private:
    Request* GetRequestByAddress(const rtc::IPAddress& ip);

    StunProber* prober_;

    // The socket for this session.
    rtc::scoped_ptr<ServerSocketInterface> socket_;

    // Temporary SocketAddress and buffer for RecvFrom.
    rtc::SocketAddress addr_;
    rtc::scoped_ptr<rtc::ByteBuffer> response_packet_;

    std::vector<Request*> requests_;
    std::vector<rtc::SocketAddress> server_ips_;
    int16 num_request_sent_ = 0;
    int16 num_response_received_ = 0;

    rtc::ThreadChecker& thread_checker_;

    DISALLOW_COPY_AND_ASSIGN(Requester);
  };

 private:
  void OnServerResolved(int index, int result);

  bool Done() {
    return num_request_sent_ >= requests_per_ip_ * all_servers_ips_.size();
  }

  bool SendNextRequest();

  // Will be invoked in 1ms intervals and schedule the next request from the
  // |current_requester_| if the time has passed for another request.
  void MaybeScheduleStunRequests();

  // End the probe with the given |status|.  Invokes |fininsh_callback|, which
  // may destroy the class.
  void End(StunProber::Status status, int result);

  // Create a socket, connect to the first resolved server, and return the
  // result of getsockname().  All Requesters will bind to this name. We do this
  // because if a socket is not bound nor connected, getsockname will return
  // 0.0.0.0. We can't connect to a single STUN server IP either as that will
  // fail subsequent requests in shared mode.
  int GetLocalAddress(rtc::IPAddress* addr);

  Requester* CreateRequester();

  Requester* current_requester_ = nullptr;

  // The time when the next request should go out.
  uint64 next_request_time_ms_ = 0;

  // Total requests sent so far.
  uint32 num_request_sent_ = 0;

  bool shared_socket_mode_ = false;

  // How many requests should be done against each resolved IP.
  uint32 requests_per_ip_ = 0;

  // Milliseconds to pause between each STUN request.
  int interval_ms_;

  // Timeout period after the last request is sent.
  int timeout_ms_;

  // STUN server name to be resolved.
  std::vector<rtc::SocketAddress> servers_;

  // The local address that each probing socket will be bound to.
  rtc::IPAddress local_addr_;

  // Owned pointers.
  rtc::scoped_ptr<SocketFactoryInterface> socket_factory_;
  rtc::scoped_ptr<HostNameResolverInterface> resolver_;
  rtc::scoped_ptr<TaskRunnerInterface> task_runner_;

  // Addresses filled out by HostNameResolver for a single server.
  std::vector<rtc::SocketAddress> resolved_ips_;

  // Accumulate all resolved IPs.
  std::vector<rtc::SocketAddress> all_servers_ips_;

  // Caller-supplied callback executed when testing is completed, called by
  // End().
  AsyncCallback finished_callback_;

  // The set of STUN probe sockets and their state.
  std::vector<Requester*> requesters_;

  rtc::ThreadChecker thread_checker_;

  DISALLOW_COPY_AND_ASSIGN(StunProber);
};

}  // namespace stunprober

#endif  // WEBRTC_P2P_STUNPROBER_STUNPROBER_H_
