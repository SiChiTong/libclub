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

#ifndef CLUB_TRANSPORT_TRANSPORT_H
#define CLUB_TRANSPORT_TRANSPORT_H

#include <iostream>
#include <array>
#include <boost/asio/steady_timer.hpp>
#include <transport/transmit_queue.h>
#include <transport/inbound_messages.h>
#include <transport/message_reader.h>
#include <club/debug/ostream_uuid.h>

namespace club { namespace transport {

template<typename UnreliableId>
class Transport {
private:
  static const size_t max_message_size = 65536;

  using udp = boost::asio::ip::udp;
  using OnReceive = std::function<void( const boost::system::error_code&
                                      , boost::asio::const_buffer )>;

  struct SocketState {
    bool                 was_destroyed;
    udp::endpoint        rx_endpoint;

    std::vector<uint8_t> rx_buffer;
    std::vector<uint8_t> tx_buffer;

    SocketState()
      : was_destroyed(false)
      , rx_buffer(max_message_size)
      , tx_buffer(max_message_size)
    {}
  };

public:
  using TransmitQueue    = transport::TransmitQueue<UnreliableId>;
  using OutboundMessages = transport::OutboundMessages<UnreliableId>;
  using InboundMessages  = transport::InboundMessages<UnreliableId>;

public:
  Transport( uuid                              id
           , udp::socket                       socket
           , udp::endpoint                     remote_endpoint
           , std::shared_ptr<OutboundMessages> outbound
           , std::shared_ptr<InboundMessages>  inbound);


  Transport(Transport&&) = delete;
  Transport& operator=(Transport&&) = delete;

  void add_target(const uuid&);

  ~Transport();

private:
  friend class ::club::transport::OutboundMessages<UnreliableId>;

  void insert_message( boost::optional<UnreliableId>
                     , std::shared_ptr<OutMessage> m);

  void start_receiving(std::shared_ptr<SocketState>);

  void on_receive( boost::system::error_code
                 , std::size_t
                 , std::shared_ptr<SocketState>);

  void start_sending(std::shared_ptr<SocketState>);

  void on_send( const boost::system::error_code&
              , std::shared_ptr<SocketState>);

  InboundMessages&  inbound()  { return *_inbound; }
  OutboundMessages& outbound() { return _transmit_queue.outbound_messages(); }

  void handle_ack_entry(AckEntry);
  void handle_message(std::shared_ptr<SocketState>&, InMessage);

private:
  uuid                             _id;
  bool                             _is_sending;
  udp::socket                      _socket;
  udp::endpoint                    _remote_endpoint;
  TransmitQueue                    _transmit_queue;
  std::shared_ptr<InboundMessages> _inbound;
  MessageReader                    _message_reader;
  boost::asio::steady_timer        _timer;
  std::shared_ptr<SocketState>     _socket_state;

};

//------------------------------------------------------------------------------
// Implementation
//------------------------------------------------------------------------------
template<typename UnreliableId>
Transport<UnreliableId>
::Transport( uuid                              id
           , udp::socket                       socket
           , udp::endpoint                     remote_endpoint
           , std::shared_ptr<OutboundMessages> outbound
           , std::shared_ptr<InboundMessages>  inbound)
  : _id(std::move(id))
  , _is_sending(false)
  , _socket(std::move(socket))
  , _remote_endpoint(std::move(remote_endpoint))
  , _transmit_queue(std::move(outbound))
  , _inbound(std::move(inbound))
  , _timer(_socket.get_io_service())
  , _socket_state(std::make_shared<SocketState>())
{
  this->inbound().register_transport(this);
  this->outbound().register_transport(this);

  start_receiving(_socket_state);
}

//------------------------------------------------------------------------------
template<typename UnreliableId>
Transport<UnreliableId>::~Transport() {
  inbound().deregister_transport(this);
  outbound().deregister_transport(this);
  _socket_state->was_destroyed = true;
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::add_target(const uuid& id)
{
  _transmit_queue.add_target(id);
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::start_receiving(std::shared_ptr<SocketState> state)
{
  using boost::system::error_code;
  using std::move;

  auto s = state.get();

  _socket.async_receive_from( boost::asio::buffer(s->rx_buffer)
                            , s->rx_endpoint
                            , [this, state = move(state)]
                              (const error_code& e, std::size_t size) {
                                on_receive(e, size, move(state));
                              }
                            );
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::on_receive( boost::system::error_code    error
                              , std::size_t                  size
                              , std::shared_ptr<SocketState> state)
{
  using namespace std;
  namespace asio = boost::asio;

  if (state->was_destroyed) return;

  if (error) {
    return inbound().on_receive(error, nullptr);
  }

  // Ignore packets from unknown sources.
  if (!_remote_endpoint.address().is_unspecified()) {
    if (state->rx_endpoint != _remote_endpoint) {
      return start_receiving(move(state));
    }
  }

  _message_reader.set_data(state->rx_buffer.data(), size);

  // Parse Acks
  while (auto opt_ack_entry = _message_reader.read_one_ack_entry()) {
    handle_ack_entry(std::move(*opt_ack_entry));
  }

  // Parse messages
  while (auto opt_msg = _message_reader.read_one_message()) {
    handle_message(state, std::move(*opt_msg));
    if (state->was_destroyed) return;
  }

  start_receiving(move(state));
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::handle_ack_entry(AckEntry entry) {
  if (entry.to == _id) {
    assert(entry.from != _id);
    outbound().on_receive_acks(entry.from, entry.acks);
  }
  else {
    outbound().add_ack_entry(std::move(entry));
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::handle_message( std::shared_ptr<SocketState>& state
                                  , InMessage msg) {
  if (msg.source == _id) {
    assert(0 && "Our message was returned back");
    return;
  }

  // Notify user only if we're one of the targets.
  if (msg.targets.count(_id)) {
    msg.targets.erase(_id);

    outbound().acknowledge(msg.source, msg.sequence_number);

    inbound().on_receive(boost::system::error_code(), &msg);

    if (state->was_destroyed) return;

    start_sending(_socket_state);
  }

  if (!msg.targets.empty()) {
    outbound().forward_message(std::move(msg));
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::start_sending(std::shared_ptr<SocketState> state) {
  using boost::system::error_code;
  using std::move;
  using boost::asio::buffer;

  if (_is_sending) return;

  binary::encoder encoder(state->tx_buffer);

  size_t count = 0;

  // TODO: Should we limit the number of acks we encode here to guarantee
  //       some space for messages?
  count += outbound().encode_acks(encoder);
  count += _transmit_queue.encode_few(encoder);

  if (count == 0) {
    _is_sending = false;
    return;
  }

  // Get the pointer here because `state` is moved from in arguments below
  // (and order of argument evaluation is undefined).
  auto s = state.get();

  _socket.async_send_to( buffer(s->tx_buffer.data(), encoder.written())
                       , _remote_endpoint
                       , [this, state = move(state)]
                         (const error_code& error, std::size_t) {
                           on_send(error, move(state));
                         });
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::on_send( const boost::system::error_code& error
                           , std::shared_ptr<SocketState>     state)
{
  using std::move;
  using boost::system::error_code;

  if (state->was_destroyed) return;

  if (error) {
    if (error == boost::asio::error::operation_aborted) {
      return;
    }
    assert(0);
  }

  // TODO: Proper congestion control
  _timer.expires_from_now(std::chrono::milliseconds(100));
  _timer.async_wait([this, state = move(state)]
                    (const error_code error) {
                      if (state->was_destroyed) return;
                      if (error) return;
                      start_sending(move(state));
                    });
}

//------------------------------------------------------------------------------
template<class Id>
void Transport<Id>::insert_message( boost::optional<Id> unreliable_id
                                  , std::shared_ptr<OutMessage> m) {
  _transmit_queue.insert_message(std::move(unreliable_id), std::move(m));
  start_sending(_socket_state);
}

//------------------------------------------------------------------------------

}} // club::transport namespace

#endif // ifndef CLUB_TRANSPORT_TRANSPORT_H
