// Copyright 2016 Peter Jankuliak
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CLUB_TRANSPORT_SOCKET_H
#define CLUB_TRANSPORT_SOCKET_H

#include <iostream>
#include <array>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/udp.hpp>
#include <transport/transmit_queue.h>
#include <club/debug/ostream_uuid.h>
#include <transport/out_message.h>
#include <async/alarm.h>
#include "error.h"
#include "punch_hole.h"

namespace club { namespace transport {

class SocketImpl {
private:
  enum class SendState { sending, waiting, pending };

public:
  static const size_t packet_size = 1452;

  using OnReceive = std::function<void( const boost::system::error_code&
                                      , boost::asio::const_buffer )>;
  using OnFlush = std::function<void()>;

private:
  using udp = boost::asio::ip::udp;

  struct SocketState {
    bool                 was_destroyed;
    udp::endpoint        rx_endpoint;

    std::vector<uint8_t> rx_buffer;
    std::vector<uint8_t> tx_buffer;

    SocketState()
      : was_destroyed(false)
      , rx_buffer(packet_size)
      , tx_buffer(packet_size)
    {}
  };

  using SocketStatePtr = std::shared_ptr<SocketState>;

public:
  using TransmitQueue = transport::TransmitQueue<OutMessage>;

public:
  SocketImpl(boost::asio::io_service&);
  SocketImpl(udp::socket);

  SocketImpl( udp::socket   socket
            , udp::endpoint remote_endpoint);

  SocketImpl(SocketImpl&&) = delete;
  SocketImpl& operator=(SocketImpl&&) = delete;

  ~SocketImpl();

  udp::endpoint local_endpoint() const;
  boost::optional<udp::endpoint> remote_endpoint() const;

  template<class OnConnect>
  void rendezvous_connect(udp::endpoint, OnConnect);

  void receive_unreliable(OnReceive);
  void receive_reliable(OnReceive);
  void send_unreliable(std::vector<uint8_t>);
  void send_reliable(std::vector<uint8_t>);
  void flush(OnFlush);
  void close();

  udp::socket& get_socket_impl() {
    return _socket;
  }

  // If we don't receive any packet during this duration, the socket
  // close shall be called and handler shall execute with timed_out
  // error.
  async::alarm::duration recv_timeout_duration() const {
    return keepalive_period() * 5;
  }

  boost::asio::io_service& get_io_service() {
    return _socket.get_io_service();
  }

private:
  void handle_error(const boost::system::error_code&);

  void start_receiving(SocketStatePtr);

  void on_receive( boost::system::error_code
                 , std::size_t
                 , SocketStatePtr);

  void start_sending(SocketStatePtr);

  void on_send( const boost::system::error_code&
              , size_t
              , SocketStatePtr);

  void handle_acks(AckSet);
  void handle_message(SocketStatePtr&, InMessagePart);

  bool try_encode(binary::encoder&, OutMessage&) const;

  void encode(binary::encoder&, OutMessage&) const;

  void handle_sync_message(const InMessagePart&);
  void handle_close_message();
  void handle_unreliable_message(SocketStatePtr&, const InMessagePart&);
  void handle_reliable_message(SocketStatePtr&, const InMessagePart&);

  void replay_pending_messages(SocketStatePtr&);

  bool user_handle_reliable_msg(SocketStatePtr&, InMessageFull&);

  template<typename ...Ts>
  void add_message(Ts&&... params) {
    _transmit_queue.emplace(std::forward<Ts>(params)...);
  }

  void on_recv_timeout_alarm();
  void on_send_keepalive_alarm(SocketStatePtr);

  async::alarm::duration keepalive_period() const {
    return std::chrono::milliseconds(200);
  }

  size_t encode_payload(binary::encoder& encoder);
  void sync_send_close_message();

  std::vector<uint8_t>
  construct_packet_with_one_message(OutMessage& m);

  static udp::endpoint sanitize_address(udp::endpoint);
private:
  using PendingMessages = std::map<SequenceNumber, PendingMessage>;

  struct Sync {
    SequenceNumber last_used_reliable_sn;
    SequenceNumber last_used_unreliable_sn;
  };

  SendState                        _send_state;
  udp::socket                      _socket;
  udp::endpoint                    _remote_endpoint;
  TransmitQueue                    _transmit_queue;
  boost::asio::steady_timer        _timer;
  SocketStatePtr                   _socket_state;
  // If this is nonet, then we haven't yet received sync
  boost::optional<Sync>            _sync;
  PendingMessages                  _pending_reliable_messages;
  boost::optional<PendingMessage>  _pending_unreliable_message;
  bool                             _schedule_sending_acks = false;
  AckSet _received_message_ids_by_peer;
  AckSet _received_message_ids;

  SequenceNumber _next_reliable_sn   = 0;
  SequenceNumber _next_unreliable_sn = 1;

  OnReceive _on_receive_reliable;
  OnReceive _on_receive_unreliable;

  OnFlush _on_flush;

  async::alarm                     _recv_timeout_alarm;
  async::alarm                     _send_keepalive_alarm;
};

//------------------------------------------------------------------------------
// Implementation
//------------------------------------------------------------------------------
inline
SocketImpl::SocketImpl(boost::asio::io_service& ios)
  : _send_state(SendState::pending)
  , _socket(ios, udp::endpoint(udp::v4(), 0))
  , _timer(_socket.get_io_service())
  , _socket_state(std::make_shared<SocketState>())
  , _recv_timeout_alarm(_socket.get_io_service(), [this]() { on_recv_timeout_alarm(); })
  , _send_keepalive_alarm(_socket.get_io_service(), [=]() { on_send_keepalive_alarm(_socket_state); })
{
}

inline
SocketImpl::SocketImpl(udp::socket udp_socket)
  : _send_state(SendState::pending)
  , _socket(std::move(udp_socket))
  , _timer(_socket.get_io_service())
  , _socket_state(std::make_shared<SocketState>())
  , _recv_timeout_alarm(_socket.get_io_service(), [this]() { on_recv_timeout_alarm(); })
  , _send_keepalive_alarm(_socket.get_io_service(), [=]() { on_send_keepalive_alarm(_socket_state); })
{
}

//------------------------------------------------------------------------------
template<class OnConnect>
inline
void SocketImpl::rendezvous_connect(udp::endpoint remote_ep, OnConnect on_connect) {
  using std::move;
  using boost::system::error_code;

  namespace asio = boost::asio;

  remote_ep = sanitize_address(remote_ep);

#if 0 // Simple connect without hole punching
  _remote_endpoint = remote_ep;
  add_message(true, MessageType::sync, _next_reliable_sn++, std::vector<uint8_t>());
  start_sending(_socket_state);
  start_receiving(_socket_state);

  auto state = _socket_state;

  _socket.get_io_service().post([h = std::move(on_connect), state]() {
      if (state->was_destroyed) {
        h(boost::asio::error::operation_aborted);
      }
      else {
        h(error_code());
      }
    });
#else
  auto state = _socket_state;

  auto syn_message = OutMessage( true
                               , MessageType::sync
                               , _next_reliable_sn++
                               , std::vector<uint8_t>());

  auto packet = construct_packet_with_one_message(syn_message);

  // TODO: Optimization: When hole punch receives a package from the remote
  //       we should add it's sequence number into _received_message_ids_by_peer
  //       so that we can acknowledge it asap.
  auto on_punch = [ this
                  , on_connect = move(on_connect)
                  , state = move(state)
                  , syn_message = move(syn_message)]
                  (error_code error, udp::endpoint remote_ep) mutable {
    if (error) return on_connect(error);
    _remote_endpoint = remote_ep;
    _transmit_queue.insert(std::move(syn_message));
    start_sending(_socket_state);
    start_receiving(_socket_state);
    return on_connect(error);
  };

  punch_hole(_socket, remote_ep, std::move(packet), std::move(on_punch));
#endif
}

//------------------------------------------------------------------------------
inline
boost::asio::ip::udp::endpoint SocketImpl::local_endpoint() const {
  return _socket.local_endpoint();
}

//------------------------------------------------------------------------------
inline
boost::optional<boost::asio::ip::udp::endpoint>
SocketImpl::remote_endpoint() const {
  return _remote_endpoint;
}

//------------------------------------------------------------------------------
inline
SocketImpl::~SocketImpl() {
  _socket_state->was_destroyed = true;
}

//------------------------------------------------------------------------------
inline
void SocketImpl::receive_unreliable(OnReceive on_receive) {
  _on_receive_unreliable = std::move(on_receive);
}

//------------------------------------------------------------------------------
inline
void SocketImpl::receive_reliable(OnReceive on_receive) {
  _on_receive_reliable = std::move(on_receive);
}

//------------------------------------------------------------------------------
inline
void SocketImpl::send_unreliable(std::vector<uint8_t> data) {
  add_message(false, MessageType::unreliable, _next_unreliable_sn++, std::move(data));
  start_sending(_socket_state);
}

//------------------------------------------------------------------------------
inline
void SocketImpl::send_reliable(std::vector<uint8_t> data) {
  add_message(true, MessageType::reliable, _next_reliable_sn++, std::move(data));
  start_sending(_socket_state);
}

//------------------------------------------------------------------------------
inline
void SocketImpl::start_receiving(SocketStatePtr state)
{
  using boost::system::error_code;
  using std::move;

  auto s = state.get();

  _recv_timeout_alarm.start(recv_timeout_duration());

  _socket.async_receive_from( boost::asio::buffer(s->rx_buffer)
                            , s->rx_endpoint
                            , [this, state = move(state)]
                              (const error_code& e, std::size_t size) {
                                on_receive(e, size, move(state));
                              }
                            );
}

//------------------------------------------------------------------------------
inline void SocketImpl::flush(OnFlush on_flush) {
  _on_flush = on_flush;
  // TODO: If there is nothing to flush, post _on_flush for execution.
}

//------------------------------------------------------------------------------
inline void SocketImpl::close() {
  _timer.cancel();
  if (_socket.is_open()) {
    sync_send_close_message();
    _socket.close();
  }
  _recv_timeout_alarm.stop();
  _send_keepalive_alarm.stop();
}

//------------------------------------------------------------------------------
inline
void SocketImpl::handle_error(const boost::system::error_code& err) {
  auto state = _socket_state;

  close();

  auto r1 = std::move(_on_receive_unreliable);
  auto r2 = std::move(_on_receive_reliable);
  if (r1) r1(err, boost::asio::const_buffer());
  if (r2) r2(err, boost::asio::const_buffer());
}

//------------------------------------------------------------------------------
inline
void SocketImpl::on_receive( boost::system::error_code error
                           , std::size_t               size
                           , SocketStatePtr            state)
{
  using namespace std;
  namespace asio = boost::asio;

  if (state->was_destroyed) return;

  _recv_timeout_alarm.stop();

  if (error) {
    auto r1 = std::move(_on_receive_unreliable);
    auto r2 = std::move(_on_receive_reliable);
    if (r1) r1(error, asio::const_buffer());
    if (r2) r2(error, asio::const_buffer());
    return;
  }

  // Ignore packets from unknown sources.
  if (!_remote_endpoint.address().is_unspecified()) {
    if (state->rx_endpoint != _remote_endpoint) {
      return start_receiving(move(state));
    }
  }

  binary::decoder decoder(state->rx_buffer);

  auto ack_set = decoder.get<AckSet>();

  if (decoder.error()) return handle_error(transport::error::parse_error);

  handle_acks(ack_set);

  auto message_count = decoder.get<uint16_t>();
  assert(!decoder.error());

  for (uint16_t i = 0; i < message_count; ++i) {
    auto m = decoder.get<InMessagePart>();
    if (decoder.error()) {
      return handle_error(transport::error::parse_error);
    }
    handle_message(state, std::move(m));
    if (state->was_destroyed) return;
    if (!_socket.is_open()) return;
  }

  start_receiving(move(state));
}

//------------------------------------------------------------------------------
inline
void SocketImpl::handle_acks(AckSet acks) {
  // TODO: If we receive an older packet than we've already received, this
  // is going to reduce our information.
  _received_message_ids_by_peer = acks;
}

//------------------------------------------------------------------------------
inline
void SocketImpl::handle_message(SocketStatePtr& state, InMessagePart msg) {
  switch (msg.type) {
    case MessageType::sync:       handle_sync_message(msg); break;
    case MessageType::keep_alive: break;
    case MessageType::unreliable: handle_unreliable_message(state, msg); break;
    case MessageType::reliable:   handle_reliable_message(state, msg); break;
    case MessageType::close:      handle_close_message(); break;
    default: return handle_error(error::parse_error);
  }

  if (!state->was_destroyed) {
    start_sending(_socket_state);
  }
}

//------------------------------------------------------------------------------
inline
void SocketImpl::handle_close_message() {
  _socket.close();
  handle_error(boost::asio::error::connection_reset);
}

//------------------------------------------------------------------------------
inline
void SocketImpl::handle_sync_message(const InMessagePart& msg) {
  _schedule_sending_acks = true;
  if (!_sync) {
    _received_message_ids.try_add(msg.sequence_number);
    _sync = Sync{msg.sequence_number, msg.sequence_number};
  }
}

//------------------------------------------------------------------------------
inline
void SocketImpl::handle_reliable_message( SocketStatePtr& state
                                        , const InMessagePart& msg) {
  _schedule_sending_acks = true;
  if (!_sync) return;
  if (!_received_message_ids.can_add(msg.sequence_number)) return;

  if (msg.sequence_number == _sync->last_used_reliable_sn + 1) {
    if (auto full_msg = msg.get_complete_message()) {
      if (!user_handle_reliable_msg(state, *full_msg)) return;
      return replay_pending_messages(state);
    }
  }

  auto i = _pending_reliable_messages.find(msg.sequence_number);

  if (i == _pending_reliable_messages.end()) {
    i = _pending_reliable_messages.emplace(msg.sequence_number, msg).first;
  }
  else {
    i->second.update_payload(msg.chunk_start, msg.payload);
    replay_pending_messages(state);
  }
}

//------------------------------------------------------------------------------
inline
void SocketImpl::replay_pending_messages(SocketStatePtr& state) {
  auto& pms = _pending_reliable_messages;

  for (auto i = pms.begin(); i != pms.end();) {
    auto& pm = i->second;

    if (pm.sequence_number == _sync->last_used_reliable_sn + 1) {
      auto full_message = pm.get_complete_message();
      if (!full_message) return;
      if (!user_handle_reliable_msg(state, *full_message)) return;
      i = pms.erase(i);
    }
    else {
      ++i;
    }
  }
}

//------------------------------------------------------------------------------
inline
bool SocketImpl::user_handle_reliable_msg( SocketStatePtr& state
                                         , InMessageFull& msg) {
  if (!_on_receive_reliable) return false;
  // The callback may hold a shared_ptr to this, so I placed the scope
  // here so that 'f' would get destroyed and thus state->was_destroyed
  // would be relevant in the line below.
  {
    auto f = std::move(_on_receive_reliable);
    f(boost::system::error_code(), msg.payload);
  }
  if (state->was_destroyed) return false;
  _received_message_ids.try_add(msg.sequence_number);
  _sync->last_used_reliable_sn = msg.sequence_number;
  return true;
}

//------------------------------------------------------------------------------
inline
void SocketImpl::handle_unreliable_message( SocketStatePtr& state
                                          , const InMessagePart& msg) {
  if (!_on_receive_unreliable) return;
  if (!_sync) return;
  if (msg.sequence_number <= _sync->last_used_unreliable_sn) return;

  auto& opm = _pending_unreliable_message;

  if (msg.is_complete()) {
    auto r = std::move(_on_receive_unreliable);
    r(boost::system::error_code(), msg.payload);
    if (state->was_destroyed) return;
    _sync->last_used_unreliable_sn = msg.sequence_number;
    opm = boost::none;
    return;
  }

  if (!opm || opm->sequence_number < msg.sequence_number) {
    opm.emplace(msg);
    return;
  }

  auto& pm = *_pending_unreliable_message;

  if (pm.sequence_number > msg.sequence_number) {
    return;
  }

  assert(pm.sequence_number == msg.sequence_number);

  pm.update_payload(msg.chunk_start, msg.payload);

  if (pm.is_complete()) {
    auto r = std::move(_on_receive_unreliable);
    r(boost::system::error_code(), pm.payload);
    if (state->was_destroyed) return;
    _sync->last_used_unreliable_sn = msg.sequence_number;
    opm = boost::none;
  }
}

//------------------------------------------------------------------------------
inline
void SocketImpl::start_sending(SocketStatePtr state) {
  using boost::system::error_code;
  using boost::asio::buffer;

  if (!_socket.is_open()) return;
  if (_send_state != SendState::pending) return;

  binary::encoder encoder(state->tx_buffer);

  // Encode acks
  encoder.put(_received_message_ids);

  assert(!encoder.error());

  size_t count = encode_payload(encoder);

  if (count == 0 && !_schedule_sending_acks) {
    // If no payload was encoded and there is no need to
    // re-send acks, then flush end return.
    if (_on_flush) {
      auto f = std::move(_on_flush);
      f();
      if (state->was_destroyed) return;
      if (!_socket.is_open()) return;
    }
    _send_keepalive_alarm.start(keepalive_period());
    return;
  }

  _schedule_sending_acks = false;

  assert(encoder.written());

  _send_state = SendState::sending;

  // Get the pointer here because `state` is moved from in arguments below
  // (and order of argument evaluation is undefined).
  auto s = state.get();

  _socket.async_send_to( buffer(s->tx_buffer.data(), encoder.written())
                       , _remote_endpoint
                       , [this, state = std::move(state)]
                         (const error_code& error, std::size_t size) {
                           on_send(error, size, std::move(state));
                         });
}

//------------------------------------------------------------------------------
inline
std::vector<uint8_t>
SocketImpl::construct_packet_with_one_message(OutMessage& m) {
  std::vector<uint8_t> data(packet_size);
  binary::encoder encoder(data);

  encoder.put(_received_message_ids);
  encoder.put<uint16_t>(1); // We're sending just one message.

  if (!try_encode(encoder, m)) {
    assert(0 && "Shouldn't happen");
  }

  data.resize(encoder.written());

  return data;
}

//------------------------------------------------------------------------------
inline
void SocketImpl::sync_send_close_message() {
  OutMessage m( false
              , MessageType::close
              , 0
              , std::vector<uint8_t>());

  auto data = construct_packet_with_one_message(m);

  _socket.send_to(boost::asio::buffer(data), _remote_endpoint);
}

//------------------------------------------------------------------------------
inline
size_t SocketImpl::encode_payload(binary::encoder& encoder) {
  size_t count = 0;

  auto count_encoder = encoder;
  encoder.put<uint16_t>(0);

  auto cycle = _transmit_queue.cycle();

  for (auto mi = cycle.begin(); mi != cycle.end();) {
    if (mi->resend_until_acked && _received_message_ids_by_peer.is_in(mi->sequence_number())) {
      mi.erase();
      continue;
    }

    if (!try_encode(encoder, *mi)) {
      break;
    }

    ++count;

    if (mi->bytes_already_sent != mi->payload_size()) {
      // It means we've exhausted the buffer in encoder.
      break;
    }

    // Unreliable entries are sent only once.
    if (!mi->resend_until_acked) {
      mi.erase();
      continue;
    }
    
    ++mi;
  }

  count_encoder.put<uint16_t>(count);

  return count;
}

//------------------------------------------------------------------------------
inline
void SocketImpl::on_send( const boost::system::error_code& error
                        , size_t                           size
                        , SocketStatePtr                   state)
{
  using std::move;
  using boost::system::error_code;

  if (state->was_destroyed) return;

  _send_state = SendState::pending;

  if (error) {
    if (error == boost::asio::error::operation_aborted) {
      return;
    }
    assert(0);
  }

  _send_state = SendState::waiting;

  /*
   * Wikipedia says [1] that in practice 2G/GPRS capacity is 40kbit/s.
   * [1] https://en.wikipedia.org/wiki/2G
   *
   * We calculate delay:
   *   delay_s  = size / (40000/8)
   *   delay_us = 1000000 * size / (40000/8)
   *   delay_us = 200 * size
   *
   * TODO: Proper congestion control
   */
  if (_remote_endpoint.address().is_loopback()) {
    // No need to wait when we're on the same PC. Would have been nicer
    // if we didn't use timer at all in this case, but this is gonna
    // have to be refactored due to proper congestion control.
    _timer.expires_from_now(std::chrono::microseconds(0));
  }
  else {
    _timer.expires_from_now(std::chrono::microseconds(200*size));
  }

  _timer.async_wait([this, state = move(state)]
                    (const error_code error) {
                      if (state->was_destroyed) return;
                      _send_state = SendState::pending;
                      if (error) return;
                      start_sending(move(state));
                    });
}

//------------------------------------------------------------------------------
inline
bool
SocketImpl::try_encode(binary::encoder& encoder, OutMessage& message) const {

  auto minimal_encoded_size =
      OutMessage::header_size
      // We'd want to send at least one byte of the payload,
      // otherwise what's the point.
      + std::min<size_t>(1, message.payload_size()) ;

  if (minimal_encoded_size > encoder.remaining_size()) {
    return false;
  }

  encode(encoder, message);

  assert(!encoder.error());

  return true;
}

//------------------------------------------------------------------------------
inline
void
SocketImpl::encode( binary::encoder& encoder, OutMessage& message) const {
  auto& m = message;

  if (m.bytes_already_sent == m.payload_size()) {
    m.bytes_already_sent = 0;
  }

  uint16_t payload_size = m.encode_header_and_payload( encoder
                                                     , m.bytes_already_sent);

  if (encoder.error()) {
    assert(0);
    return;
  }

  m.bytes_already_sent += payload_size;
}

inline void SocketImpl::on_recv_timeout_alarm() {
  handle_error(error::timed_out);
}

inline void SocketImpl::on_send_keepalive_alarm(SocketStatePtr state) {
  add_message(false, MessageType::keep_alive, 0, std::vector<uint8_t>());
  start_sending(std::move(state));
}

inline
boost::asio::ip::udp::endpoint
SocketImpl::sanitize_address(udp::endpoint ep) {
  namespace ip = boost::asio::ip;

  if (ep.address().is_unspecified()) {
    if (ep.address().is_v4()) {
      ep = udp::endpoint(ip::address_v4::loopback(), ep.port());
    }
    else {
      ep = udp::endpoint(ip::address_v6::loopback(), ep.port());
    }
  }

  return ep;
}

//------------------------------------------------------------------------------
// Socket
//------------------------------------------------------------------------------
class Socket {
private:
  using udp = boost::asio::ip::udp;

  std::unique_ptr<SocketImpl> _impl;

public:
  static const size_t packet_size = SocketImpl::packet_size;

  using OnReceive = SocketImpl::OnReceive;
  using OnFlush = SocketImpl::OnFlush;

public:
  Socket(boost::asio::io_service& ios)
    : _impl(std::make_unique<SocketImpl>(ios))
  {}

  Socket(udp::socket udp_socket)
    : _impl(std::make_unique<SocketImpl>(std::move(udp_socket)))
  {}

  Socket(Socket&& other)
    : _impl(std::move(other._impl)) {}

  Socket& operator = (Socket&& other) {
    _impl = std::move(other._impl);
    return *this;
  }

  udp::endpoint local_endpoint() const {
    return _impl->local_endpoint();
  }

  boost::optional<udp::endpoint> remote_endpoint() const {
    return _impl->remote_endpoint();
  }

  template<class OnConnect>
  void rendezvous_connect(udp::endpoint remote_ep, OnConnect on_connect) {
    _impl->rendezvous_connect(std::move(remote_ep), std::move(on_connect));
  }

  void receive_unreliable(OnReceive _1) {
    _impl->receive_unreliable(std::move(_1));
  }

  void receive_reliable(OnReceive _1) {
    _impl->receive_reliable(std::move(_1));
  }

  void send_unreliable(std::vector<uint8_t> _1) {
    _impl->send_unreliable(std::move(_1));
  }

  void send_reliable(std::vector<uint8_t> _1) {
    _impl->send_reliable(std::move(_1));
  }

  void flush(OnFlush _1) {
    _impl->flush(std::move(_1));
  }

  void close() {
    _impl->close();
  }

  udp::socket& get_socket_impl() {
    return _impl->get_socket_impl();
  }

  async::alarm::duration recv_timeout_duration() const {
    return _impl->recv_timeout_duration();
  }

  boost::asio::io_service& get_io_service() {
    return _impl->get_io_service();
  }
};

}} // club::transport namespace

#endif // ifndef CLUB_TRANSPORT_SOCKET_H