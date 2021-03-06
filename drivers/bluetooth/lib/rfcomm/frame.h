// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstdlib>

#include "garnet/drivers/bluetooth/lib/common/packet_view.h"
#include "garnet/drivers/bluetooth/lib/rfcomm/rfcomm.h"

namespace btlib {
namespace rfcomm {

class UserDataFrame;

// Represents an RFCOMM frame.
// The Frame class has two primary uses:
//  1. Constructing RFCOMM frames with all necessary fields, with the possible
//     exception of the credits field; later, modifying the credits field and
//     writing the frame to a buffer.
//  2. Interpreting an RFCOMM frame written into a buffer, but not modifying the
//     underlying buffer or fields.
// The Frame class is designed to be heavily restricted to the above use cases.
//
// The Frame class hierarchy is as follows:
// Frame
//  - SetAsynchronousBalancedModeCommand
//  - DisconnectCommand
//  - UnnumberedAcknowledgementResponse
//  - DisconnectedModeResponse
//  - UnnumberedInfoHeaderCheckFrame (abstract)
//    - UserDataFrame
//    - MuxCommandFrame (planned)
//
// To use Frame in the first way (to write RFCOMM frames), construct the
// subclass of Frame corresponding to the type of RFCOMM frame you would like.
// If it is a UIH frame, modify the frame's credits field using set_credits() if
// needed. Allocate a buffer using written_size() to determine the size of the
// frame when written. Finally, use Write() to write the frame into a buffer.
//
// To use Frame in the second way (as a frame parser), use Frame::Parse(). If
// needed, cast the returned Frame pointer to a specific subclass (see the
// documentation of Parse()). Read values using the accessors. Finally, if the
// frame contains user data or a multiplexer command, use the Take...()
// functions to take ownership of the frame's information (payload). This
// payload can then be passed to the next layer.
class Frame {
 public:
  // |role| is the local RFCOMM role. This is used to determine how to set the
  // C/R bit. |control| is the control octet. Generally, this will be passed by
  // casting a FrameType to a uint8_t. We take a uint8_t instead of a FrameType
  // because Frame can represent frames of unsupported frame type (whose frame
  // types are not enumerated in FrameType).
  inline Frame(Role role, CommandResponse command_response, DLCI dlci,
               uint8_t control, bool poll_final);

  virtual ~Frame() = default;

  // Parse a frame out of a ByteBuffer. If parsing fails, returns nullptr.
  // Copies the payload from |buffer|. Does not take ownership of |buffer|.
  // |credit_based_flow| and |role| are RFCOMM session parameters necessary for
  // fully parsing the frame.
  //
  // The Frame returned by Parse should first have its type inspected. If it is
  // not a UIH frame, it can be used as-is. No casting needs to be done to get
  // all of the useful information out of the Frame; simply use the Frame
  // accessors to read information about the frame. If the frame type is UIH,
  // then the DLCI should be inspected. If the DLCI is a user data DLCI, the
  // Frame should be converted to a UserDataFrame using ToUserDataFrame.
  //
  // TODO(gusss): Add this snipped of documentation when MuxCommandFrames are
  // added:
  // Otherwise, if the DLCI is 0, the frame should be cast to a MuxCommandFrame
  // using ToMuxCommandFrame.
  //
  // For UIH frames, this function will copy from |buffer|.
  static std::unique_ptr<Frame> Parse(bool credit_based_flow, Role role,
                                      const common::ByteBuffer& buffer);

  // Write this into a buffer. The base implementation of Write() will simply
  // write the address, control, length(=0), and FCS octets into a buffer. This
  // is adequate for non-UIH frames.
  virtual void Write(common::MutableBufferView buffer) const;

  // The amount of space this frame takes up when written. Used to allocate the
  // correct size for Write().
  virtual inline size_t written_size() const {
    // Address, control, length, FCS octets.
    return 4 * sizeof(uint8_t);
  }

  // See GSM 5.2 to understand how we extract different fields from the frame.

  // Returns Data Link Connection Identifier (DLCI) used to identify the
  // specific DLC/channel which this frame pertains to.
  inline DLCI dlci() const { return dlci_; }

  // Returns whether this is a Command or a Response frame.
  inline CommandResponse command_response() const { return command_response_; }

  // Returns Control field with the Poll/Final bit set to 0. See GSM Table 2.
  // The Control field encodes the frame type. This octet can be cast to a
  // FrameType; however, this octet may not correspond to any of the octets in
  // the FrameType enum, if the peer sent an unrecognized/unsupported frame
  // type.
  inline uint8_t control() const { return control_; }

  // Returns the Poll/Final (P/F) bit. See RFCOMM 5.1.2, which indicates the
  // various uses of the P/F bit in RFCOMM.
  inline bool poll_final() const { return poll_final_; }

  // Returns the length of the information field of this frame. This is the
  // value which will be encoded in the length field of the frame, when the
  // frame is written. For the default Frame implementation, returns 0, as there
  // is no payload. This is overridden for UIH frames.
  inline virtual InformationLength length() const { return 0; }

  // Convert this frame into a MuxCommandFrame or UserDataFrame. If the
  // underlying frame is not a MuxCommandFrame or UserDataFrame, these functions
  // return nullptr, and the passed pointer will not be modified. If the
  // function is successful, then the passed pointer will be set to nullptr.
  //
  // TODO(gusss): add ToMuxCommandFrame when mux commands are added.
  static std::unique_ptr<UserDataFrame> ToUserDataFrame(
      std::unique_ptr<Frame>* frame);

 protected:
  // The size of the header. We consider the header to be the address octet,
  // control octet, and and length octet(s).
  virtual size_t header_size() const;

  // Write the header of this frame into a buffer.
  virtual void WriteHeader(common::MutableBufferView buffer) const;

  // RFCOMM session parameters.
  Role role_;

  // Frame fields.
  CommandResponse command_response_;
  DLCI dlci_;
  uint8_t control_;
  bool poll_final_;
};

// Set Asynchronous Balanced Mode (SABM) command, described in GSM 5.3.1. Used
// to start up channels.
class SetAsynchronousBalancedModeCommand : public Frame {
 public:
  SetAsynchronousBalancedModeCommand(Role role, DLCI dlci);
};

// Disconnect (DISC) command, described in GSM 5.3.3. Used to close down
// channels, or the multiplexer session as a whole.
class DisconnectCommand : public Frame {
 public:
  DisconnectCommand(Role role, DLCI dlci);
};

// Unnumbered Acknowledgement (UA) response, described in GSM 5.3.2. Used as an
// acknowledgement to SABM and DISC commands.
class UnnumberedAcknowledgementResponse : public Frame {
 public:
  UnnumberedAcknowledgementResponse(Role role, DLCI dlci);
};

// Disconnected Mode (DM) response, described in GSM 5.3.3. This response is
// sent when commands are sent along a disconnected channel.
class DisconnectedModeResponse : public Frame {
 public:
  DisconnectedModeResponse(Role role, DLCI dlci);
};

// Unnumbered Information with Header Check frame. This abstract class is the
// superclass of both MuxCommandFrame (sent along DLCI 0) and UserDataFrame
// (sent along DLCIs 2-61).
//
// |credit_based_flow| is a session parameter specifying whether credit-based
// flow control is turned on or off for this session. It determines whether a
// credit field can appear in this frame; if it is set to true and set_credits()
// is used to set the credits to a nonzero amount, a credits field will appear.
// Note that, with this class, we cannot encode a frame with a credits field
// equal to 0. This isn't an issue, as sending a frame with credits=0 is
// functionally equivalent to a frame without a credits field.
class UnnumberedInfoHeaderCheckFrame : public Frame {
 public:
  UnnumberedInfoHeaderCheckFrame(Role role, bool credit_based_flow, DLCI dlci);

  virtual ~UnnumberedInfoHeaderCheckFrame() = default;

  // Frame overrides
  virtual void Write(common::MutableBufferView buffer) const override = 0;
  virtual size_t written_size() const override = 0;
  virtual InformationLength length() const override = 0;

  // Returns the number of credits contained in the credits field of this frame.
  // See RFCOMM 6.5.2. If this frame does not contain a credits field, returns
  // 0.
  inline uint8_t credits() const { return has_credit_octet() ? credits_ : 0; }

  // Sets the credits. This is the only field that may need to be changed
  // after frame creation. This function should not be called if credit-based
  // flow is off; if credit-based flow is off, then |credits_| should remain 0.
  // This function also changes the poll/final bit to reflect the new amount of
  // credits; P/F=0 iff credits=0. See RFCOMM 6.5.2.
  void set_credits(uint8_t credits);

 protected:
  // Whether or not this frame contains the optional credit octet.
  inline bool has_credit_octet() const {
    return credit_based_flow_ && credits_;
  }

  // The size of the header. In this case, we define the header to be the
  // address octet, control octet, length octet(s), and optional credits octets.
  virtual size_t header_size() const override;

  // Write the header of this frame, including the optional credits octet.
  virtual void WriteHeader(common::MutableBufferView buffer) const override;

  bool credit_based_flow_;
  uint8_t credits_;
};

class UserDataFrame : public UnnumberedInfoHeaderCheckFrame {
 public:
  // |information| is the payload; "information" is RFCOMM/GSM's term for the
  // payload of a frame. Frame takes ownership of |information|.
  UserDataFrame(Role role, bool credit_based_flow, DLCI dlci,
                common::ByteBufferPtr information);

  // UnnumberedInfoHeaderCheckFrame overrides
  void Write(common::MutableBufferView buffer) const override;
  size_t written_size() const override;
  inline InformationLength length() const override {
    return information_->size();
  }

  // Transfers ownership of the information field (aka the payload) from this
  // Frame to the caller. Future calls to TakeInformation() will return nullptr.
  // It is expected that the Frame will be destructed soon after this call.
  common::ByteBufferPtr TakeInformation();

 private:
  common::ByteBufferPtr information_;
};

}  // namespace rfcomm
}  // namespace btlib
