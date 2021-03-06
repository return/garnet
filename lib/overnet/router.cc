// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "router.h"
#include <iostream>

namespace overnet {

void Router::Forward(Message message) {
  assert(!message.done.empty());
  // There are three primary cases we care about here, that can be discriminated
  // based on the destination count of the message:
  // 1. If there are zero destinations, this is a malformed message (fail).
  // 2. If there is one destination, forward the message on.
  // 3. If there are multiple destinations, broadcast this message to all
  //    destinations.
  // We separate 2 & 3 as the single forwarding case can be made
  // (much) more efficient.
  switch (message.wire.destinations().size()) {
    case 0:
      // Malformed message, bail
      // TODO(ctiller): Log error: Routing header must have at least one
      // destination
      message.done(Status(StatusCode::INVALID_ARGUMENT,
                          "Routing header must have at least one destination"));
      break;
    case 1: {
      // Single destination... it could be either a local stream or need to be
      // forwarded to a remote node over some link.
      const RoutableMessage::Destination& dst = message.wire.destinations()[0];
      if (dst.dst() == node_id_) {
        streams_[LocalStreamId{message.wire.src(), dst.stream_id()}]
            .HandleMessage(dst.seq(), message.received,
                           std::move(*message.wire.mutable_payload()),
                           std::move(message.done));
      } else {
        links_[dst.dst()].Forward(std::move(message));
      }
    } break;
    default: {
      // Multiple destination:
      // - Handle local streams directly.
      // - For remote forwarding:
      //   1. If we know the next hop, and that next hop is used for multiple of
      //      our destinations, keep the multicast group together for that set.
      //   2. Separate the multicast if next hops are different.
      //   3. Separate the multicast if we do not know about next hops yet.
      std::unordered_map<Link*, std::vector<RoutableMessage::Destination>>
          group_forward;
      class Broadcaster {
       public:
        Broadcaster(StatusCallback cb) : cb_(std::move(cb)) {}
        StatusCallback AddCallback() {
          ++n_;
          return [this](const Status& status) {
            if (!cb_.empty() && !status.is_ok()) {
              cb_(status);
            }
            Step();
          };
        }
        void Step() {
          if (--n_ == 0) {
            if (!cb_.empty()) cb_(Status::Ok());
            delete this;
          }
        }

       private:
        int n_ = 1;
        StatusCallback cb_;
      };
      Broadcaster* b = new Broadcaster(std::move(message.done));
      for (const auto& dst : message.wire.destinations()) {
        if (dst.dst() == node_id_) {
          // Locally handled stream
          streams_[LocalStreamId{message.wire.src(), dst.stream_id()}]
              .HandleMessage(dst.seq(), message.received,
                             message.wire.payload(), b->AddCallback());
        } else {
          // Remote destination
          LinkHolder& h = links_[dst.dst()];
          if (h.link() == nullptr) {
            // We don't know the next link, ask the LinkHolder to forward (which
            // will continue forwarding the message when we know the next hop).
            h.Forward(Message{message.wire.WithDestinations({dst}),
                              message.received, b->AddCallback()});
          } else {
            // We know the next link: gather destinations together by link so
            // that we can (hopefully) keep multicast groups together
            group_forward[h.link()].emplace_back(dst);
          }
        }
      }
      // Forward any grouped messages now that we've examined all destinations
      for (auto& grp : group_forward) {
        grp.first->Forward(
            Message{message.wire.WithDestinations(std::move(grp.second)),
                    message.received, b->AddCallback()});
      }
      b->Step();
    } break;
  }
}

Status Router::RegisterStream(NodeId peer, StreamId stream_id,
                              StreamHandler* stream_handler) {
  return streams_[LocalStreamId{peer, stream_id}].SetHandler(stream_handler);
}

Status Router::RegisterLink(NodeId peer, Link* link) {
  links_[peer].SetLink(link);
  return Status::Ok();
}

void Router::StreamHolder::HandleMessage(Optional<SeqNum> seq,
                                         TimeStamp received, Slice data,
                                         StatusCallback done) {
  assert(!done.empty());
  if (handler_ == nullptr) {
    pending_.emplace_back(
        Pending{seq, received, std::move(data), std::move(done)});
  } else {
    handler_->HandleMessage(seq, received, std::move(data), std::move(done));
  }
}

Status Router::StreamHolder::SetHandler(StreamHandler* handler) {
  if (handler_ != nullptr) {
    return Status(StatusCode::FAILED_PRECONDITION, "Handler already set");
  }
  handler_ = handler;
  std::vector<Pending> pending;
  pending.swap(pending_);
  for (auto& p : pending) {
    handler_->HandleMessage(p.seq, p.received, std::move(p.data),
                            std::move(p.done));
  }
  return Status::Ok();
}

void Router::LinkHolder::Forward(Message message) {
  assert(!message.done.empty());
  if (link_ == nullptr) {
    pending_.emplace_back(std::move(message));
  } else {
    link_->Forward(std::move(message));
  }
}

void Router::LinkHolder::SetLink(Link* link) {
  link_ = link;
  std::vector<Message> pending;
  pending.swap(pending_);
  for (auto& p : pending) {
    link_->Forward(std::move(p));
  }
}

}  // namespace overnet
