// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "pairing_state.h"

#include "lib/fxl/random/rand.h"

#include "util.h"

namespace btlib {

using common::ByteBuffer;
using common::DeviceAddress;
using common::HostError;
using common::MutableBufferView;
using common::UInt128;

namespace sm {

namespace {

SecurityProperties FeaturesToProperties(const PairingFeatures& features) {
  return SecurityProperties(features.method == PairingMethod::kJustWorks
                                ? SecurityLevel::kEncrypted
                                : SecurityLevel::kAuthenticated,
                            features.encryption_key_size,
                            features.secure_connections);
}

}  // namespace

PairingState::LegacyState::LegacyState(uint64_t id)
    : id(id),
      stk_encrypted(false),
      ltk_encrypted(false),
      obtained_remote_keys(0u),
      has_tk(false),
      has_peer_confirm(false),
      has_peer_rand(false),
      sent_local_confirm(false),
      sent_local_rand(false),
      has_ltk(false) {}

bool PairingState::LegacyState::InPhase1() const {
  return !features && !stk_encrypted;
}

bool PairingState::LegacyState::InPhase2() const {
  return features && has_tk && !stk_encrypted;
}

bool PairingState::LegacyState::InPhase3() const {
  return features && stk_encrypted && !RequestedKeysObtained();
}

bool PairingState::LegacyState::IsComplete() const {
  return features && stk_encrypted && RequestedKeysObtained() &&
         !WaitingForEncryptionWithLTK();
}

bool PairingState::LegacyState::WaitingForTK() const {
  return features && !has_tk && !stk_encrypted;
}

bool PairingState::LegacyState::RequestedKeysObtained() const {
  FXL_DCHECK(features);

  // Return true if we expect no keys from the remote.
  return !features->remote_key_distribution ||
         (features->remote_key_distribution == obtained_remote_keys);
}

bool PairingState::LegacyState::ShouldReceiveLTK() const {
  FXL_DCHECK(features);
  return (features->remote_key_distribution & KeyDistGen::kEncKey);
}

bool PairingState::LegacyState::ShouldSendLTK() const {
  FXL_DCHECK(features);
  return (features->local_key_distribution & KeyDistGen::kEncKey);
}

bool PairingState::LegacyState::WaitingForEncryptionWithLTK() const {
  return (ShouldReceiveLTK() || ShouldSendLTK()) && !ltk_encrypted;
}

PairingState::PendingRequest::PendingRequest(SecurityLevel level,
                                             PairingCallback callback)
    : level(level), callback(std::move(callback)) {}

PairingState::PairingState(fxl::WeakPtr<hci::Connection> link,
                           fbl::RefPtr<l2cap::Channel> smp,
                           IOCapability io_capability)
    : next_pairing_id_(0), le_link_(link), weak_ptr_factory_(this) {
  FXL_DCHECK(link);
  FXL_DCHECK(smp);
  FXL_DCHECK(link->handle() == smp->link_handle());
  FXL_DCHECK(link->ll_type() == hci::Connection::LinkType::kLE);
  FXL_DCHECK(smp->id() == l2cap::kLESMPChannelId);

  // Set up SMP data bearer.
  // TODO(armansito): Enable SC when we support it.
  le_smp_ = std::make_unique<Bearer>(
      std::move(smp), link->role(), false /* sc */, io_capability,
      fit::bind_member(this, &PairingState::OnLEPairingFailed),
      fit::bind_member(this, &PairingState::OnLEPairingFeatures));
  le_smp_->set_confirm_value_callback(
      fit::bind_member(this, &PairingState::OnLEPairingConfirm));
  le_smp_->set_random_value_callback(
      fit::bind_member(this, &PairingState::OnLEPairingRandom));
  le_smp_->set_long_term_key_callback(
      fit::bind_member(this, &PairingState::OnLELongTermKey));
  le_smp_->set_master_id_callback(
      fit::bind_member(this, &PairingState::OnLEMasterIdentification));

  // Set up HCI encryption event.
  le_link_->set_encryption_change_callback(
      fit::bind_member(this, &PairingState::OnLEEncryptionChange));
}

PairingState::~PairingState() {
  if (le_link_) {
    le_link_->set_encryption_change_callback({});
  }
}

void PairingState::Reset(IOCapability io_capability) {
  Abort();
  le_smp_->set_io_capability(io_capability);
}

void PairingState::UpdateSecurity(SecurityLevel level,
                                  PairingCallback callback) {
  // If pairing is in progress then we queue the request.
  if (legacy_state_) {
    FXL_VLOG(2) << "sm: LE legacy pairing in progress; request queued";
    FXL_DCHECK(le_smp_->pairing_started());
    request_queue_.emplace(level, std::move(callback));
    return;
  }

  if (level <= le_sec_.level()) {
    callback(Status(), le_sec_);
    return;
  }

  // TODO(armansito): Support initiating a security upgrade as slave (Bearer
  // needs to support the SMP Security Request).
  if (le_smp_->role() == hci::Connection::Role::kSlave) {
    callback(Status(HostError::kNotSupported), SecurityProperties());
    return;
  }

  request_queue_.emplace(level, std::move(callback));
  BeginLegacyPairingPhase1(level);
}

void PairingState::AbortLegacyPairing(ErrorCode error_code) {
  FXL_DCHECK(legacy_state_);
  FXL_DCHECK(le_smp_->pairing_started());

  le_smp_->Abort(error_code);

  // "Abort" should trigger OnLEPairingFailed.
}

void PairingState::BeginLegacyPairingPhase1(SecurityLevel level) {
  FXL_DCHECK(le_smp_->role() == hci::Connection::Role::kMaster);
  FXL_DCHECK(!legacy_state_) << "Already pairing!";

  if (level == SecurityLevel::kAuthenticated) {
    le_smp_->set_mitm_required(true);
  }

  legacy_state_ = std::make_unique<LegacyState>(next_pairing_id_++);
  le_smp_->InitiateFeatureExchange();
}

void PairingState::Abort() {
  if (!le_smp_->pairing_started())
    return;

  FXL_VLOG(1) << "sm: Abort pairing";
  if (legacy_state_) {
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
  }
}

void PairingState::BeginLegacyPairingPhase2(const ByteBuffer& preq,
                                            const ByteBuffer& pres) {
  FXL_DCHECK(legacy_state_);
  FXL_DCHECK(legacy_state_->WaitingForTK());
  FXL_DCHECK(!legacy_state_->features->secure_connections);
  FXL_DCHECK(!legacy_state_->has_tk);
  FXL_DCHECK(!legacy_state_->has_peer_confirm);
  FXL_DCHECK(!legacy_state_->has_peer_rand);
  FXL_DCHECK(!legacy_state_->sent_local_confirm);
  FXL_DCHECK(!legacy_state_->sent_local_rand);

  // Cache |preq| and |pres|. These are used for confirm value generation.
  FXL_DCHECK(preq.size() == legacy_state_->preq.size());
  FXL_DCHECK(pres.size() == legacy_state_->pres.size());
  preq.Copy(&legacy_state_->preq);
  pres.Copy(&legacy_state_->pres);

  auto self = weak_ptr_factory_.GetWeakPtr();
  auto tk_callback = [self, id = legacy_state_->id](bool success, uint32_t tk) {
    if (!self) {
      return;
    }

    auto* state = self->legacy_state_.get();
    if (!state || id != state->id) {
      FXL_VLOG(1) << "sm: Ignoring TK callback for expired pairing: (id = "
                  << id << ")";
      return;
    }

    if (!success) {
      FXL_VLOG(1) << "sm: TK delegate responded with error; aborting";
      if (state->features->method == PairingMethod::kPasskeyEntryInput) {
        self->AbortLegacyPairing(ErrorCode::kPasskeyEntryFailed);
      } else {
        self->AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
      }
      return;
    }

    FXL_DCHECK(state->WaitingForTK());

    // Set the lower bits to |tk|.
    tk = htole32(tk);
    state->tk.fill(0);
    std::memcpy(state->tk.data(), &tk, sizeof(tk));
    state->has_tk = true;

    FXL_DCHECK(state->InPhase2());

    // We have TK so we can generate the confirm value now.
    const DeviceAddress *ia, *ra;
    self->LEPairingAddresses(&ia, &ra);
    fxl::RandBytes(state->local_rand.data(), state->local_rand.size());
    util::C1(state->tk, state->local_rand, state->preq, state->pres, *ia, *ra,
             &state->local_confirm);

    // If we are the initiator then we just generated the "Mconfirm" value. We
    // start the exchange by sending this value to the peer. Otherwise this is
    // the "Sconfirm" value and we either:
    //    a. send it now if the peer has sent us its confirm value while we were
    //    waiting for the TK.
    //    b. send it later when we receive Mconfirm.
    if (state->features->initiator || state->has_peer_confirm) {
      self->LegacySendConfirmValue();
    }
  };

  FXL_DCHECK(le_tk_delegate_);
  le_tk_delegate_(legacy_state_->features->method, std::move(tk_callback));
}

void PairingState::LegacySendConfirmValue() {
  FXL_DCHECK(legacy_state_);
  FXL_DCHECK(legacy_state_->InPhase2());
  FXL_DCHECK(!legacy_state_->sent_local_confirm);

  legacy_state_->sent_local_confirm = true;
  le_smp_->SendConfirmValue(legacy_state_->local_confirm);
}

void PairingState::LegacySendRandomValue() {
  FXL_DCHECK(legacy_state_);
  FXL_DCHECK(legacy_state_->InPhase2());
  FXL_DCHECK(!legacy_state_->sent_local_rand);

  legacy_state_->sent_local_rand = true;
  le_smp_->SendRandomValue(legacy_state_->local_rand);
}

void PairingState::EndLegacyPairingPhase2() {
  FXL_DCHECK(legacy_state_);
  FXL_DCHECK(legacy_state_->InPhase2());

  // Update the current security level. Even though the link is encrypted with
  // the STK (i.e. a temporary key) it provides a level of security.
  le_sec_ = FeaturesToProperties(*legacy_state_->features);
  legacy_state_->stk_encrypted = true;

  // If the slave has no keys to send then we're done with pairing. Since we're
  // only encrypted with the STK, the pairing will be short-term (this is the
  // case if the "bonding" flag was not set).
  if (legacy_state_->IsComplete()) {
    CompleteLegacyPairing();

    // TODO(NET-1088): Make sure IsComplete() returns false if we're the slave
    // and have keys to distribute.
    return;
  }

  FXL_DCHECK(legacy_state_->InPhase3());

  if (legacy_state_->features->initiator) {
    FXL_DCHECK(le_smp_->role() == hci::Connection::Role::kMaster);
    FXL_VLOG(1) << "sm: Waiting to receive keys from the slave.";
  } else {
    FXL_DCHECK(le_smp_->role() == hci::Connection::Role::kSlave);

    // TODO(NET-1088): Distribute the slave's (local) keys now.
  }
}

void PairingState::CompleteLegacyPairing() {
  FXL_DCHECK(legacy_state_);
  FXL_DCHECK(legacy_state_->IsComplete());
  FXL_DCHECK(le_smp_->pairing_started());

  le_smp_->StopTimer();

  // Notify that we got a LTK. The security properties of |ltk| are defined by
  // the security properties of the link when LTK was distributed (i.e. the
  // properties of the STK). This is reflected by |le_sec_|.
  if (legacy_state_->ltk) {
    FXL_DCHECK(le_ltk_callback_);
    le_ltk_callback_(LTK(le_sec_, *legacy_state_->ltk));
  }

  FXL_VLOG(1) << "sm: Legacy pairing complete";
  legacy_state_ = nullptr;

  // Separate out the requests that are satisifed by the current security level
  // from the ones that require a higher level. We'll retry pairing for the
  // latter.
  std::queue<PendingRequest> satisfied;
  std::queue<PendingRequest> unsatisfied;
  while (!request_queue_.empty()) {
    auto& request = request_queue_.front();
    if (request.level <= le_sec_.level()) {
      satisfied.push(std::move(request));
    } else {
      unsatisfied.push(std::move(request));
    }
    request_queue_.pop();
  }

  request_queue_ = std::move(unsatisfied);

  // Notify the satisfied requests with success.
  while (!satisfied.empty()) {
    satisfied.front().callback(Status(), le_sec_);
    satisfied.pop();
  }

  if (!unsatisfied.empty()) {
    BeginLegacyPairingPhase1(unsatisfied.front().level);
  }
}

void PairingState::OnLEPairingFeatures(const PairingFeatures& features,
                                       const ByteBuffer& preq,
                                       const ByteBuffer& pres) {
  FXL_VLOG(2) << "sm: Obtained LE Pairing features";

  if (!features.initiator) {
    if (legacy_state_) {
      FXL_DCHECK(legacy_state_->features);

      // Reject if the peer sent a new pairing request while pairing is already
      // in progress.
      AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
      return;
    }

    legacy_state_ = std::make_unique<LegacyState>(next_pairing_id_++);
  }

  FXL_DCHECK(legacy_state_);
  legacy_state_->features = features;
  BeginLegacyPairingPhase2(preq, pres);
}

void PairingState::OnLEPairingFailed(Status status) {
  FXL_LOG(ERROR) << "sm: LE Pairing failed: " << status.ToString();

  // TODO(armansito): implement "waiting interval" to prevent repeated attempts
  // as described in Vol 3, Part H, 2.3.6.

  auto requests = std::move(request_queue_);
  while (!requests.empty()) {
    requests.front().callback(status, le_sec_);
    requests.pop();
  }

  if (legacy_state_) {
    FXL_DCHECK(le_link_);
    le_link_->set_link_key(hci::LinkKey());
    legacy_state_ = nullptr;
  }
}

void PairingState::OnLEPairingConfirm(const UInt128& confirm) {
  // TODO(armansito): Have separate subroutines to handle this event for legacy
  // and secure connections.
  if (!legacy_state_) {
    FXL_VLOG(1) << "sm: Ignoring confirm value received while not pairing";
    return;
  }

  // Allow this command if:
  //    a. we are in Phase 2, or
  //    b. we are the responder but still waiting for a TK.
  // Reject pairing if neither of these is true.
  if (!legacy_state_->InPhase2() &&
      (!legacy_state_->WaitingForTK() || legacy_state_->features->initiator)) {
    FXL_LOG(ERROR)
        << "sm: Abort pairing due to confirm value received outside phase 2";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Abort pairing if we received a second confirm value from the peer. The
  // specification defines a certain order for the phase 2 commands.
  if (legacy_state_->has_peer_confirm) {
    FXL_LOG(ERROR) << "sm: Already received confirm value! Aborting";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // The confirm value shouldn't be sent after the random value. (See Vol 3,
  // Part H, 2.3.5.5 and Appendix C.2.1.1 for the specific order of events.
  if (legacy_state_->has_peer_rand) {
    FXL_LOG(ERROR)
        << "sm: \"Pairing Confirm\" expected before \"Pairing Random\"";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  legacy_state_->peer_confirm = confirm;
  legacy_state_->has_peer_confirm = true;

  if (legacy_state_->features->initiator) {
    // We MUST have a TK and have previously generated an Mconfirm.
    FXL_DCHECK(legacy_state_->has_tk);

    // We are the master and have previously sent Mconfirm and just received
    // Sconfirm. We now send Mrand for the slave to compare.
    FXL_DCHECK(le_smp_->role() == hci::Connection::Role::kMaster);
    LegacySendRandomValue();
  } else {
    // We are the slave and have just received Mconfirm.
    FXL_DCHECK(le_smp_->role() == hci::Connection::Role::kSlave);

    if (!legacy_state_->WaitingForTK()) {
      LegacySendConfirmValue();
    }
  }
}

void PairingState::OnLEPairingRandom(const UInt128& random) {
  // TODO(armansito): Have separate subroutines to handle this event for legacy
  // and secure connections.
  if (!legacy_state_) {
    FXL_VLOG(1) << "sm: Ignoring confirm value received while not pairing";
    return;
  }

  if (!legacy_state_->InPhase2()) {
    FXL_LOG(ERROR)
        << "sm: Abort pairing due to confirm value received outside phase 2";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // We must have a TK and sent a confirm value by now (this is implied by
  // InPhase2() above).
  FXL_DCHECK(legacy_state_->has_tk);

  // Abort pairing if we received a second random value from the peer. The
  // specification defines a certain order for the phase 2 commands.
  if (legacy_state_->has_peer_rand) {
    FXL_LOG(ERROR) << "sm: Already received random value! Aborting";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // The random value shouldn't be sent before the confirm value. (See Vol 3,
  // Part H, 2.3.5.5 and Appendix C.2.1.1 for the specific order of events.
  if (!legacy_state_->has_peer_confirm) {
    FXL_LOG(ERROR) << "sm: \"Pairing Rand\" expected after \"Pairing Confirm\"";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Check that the order of the SMP commands is correct.
  if (legacy_state_->features->initiator) {
    FXL_DCHECK(le_smp_->role() == hci::Connection::Role::kMaster);

    // The master distributes both values before the slave sends Srandom.
    if (!legacy_state_->sent_local_rand || !legacy_state_->sent_local_confirm) {
      FXL_LOG(ERROR) << "sm: \"Pairing Random\" received in wrong order!";
      AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
      return;
    }
  } else {
    FXL_DCHECK(le_smp_->role() == hci::Connection::Role::kSlave);

    // We cannot have sent the Srand without receiving Mrand first.
    FXL_DCHECK(!legacy_state_->sent_local_rand);

    // We need to send Sconfirm before the master sends Mrand.
    if (!legacy_state_->sent_local_confirm) {
      FXL_LOG(ERROR) << "sm: \"Pairing Random\" received in wrong order!";
      AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
      return;
    }
  }

  legacy_state_->peer_rand = random;
  legacy_state_->has_peer_rand = true;

  // We have both confirm and rand values from the peer. Generate it locally and
  // compare.
  const DeviceAddress *ia, *ra;
  LEPairingAddresses(&ia, &ra);
  UInt128 peer_confirm;
  util::C1(legacy_state_->tk, legacy_state_->peer_rand, legacy_state_->preq,
           legacy_state_->pres, *ia, *ra, &peer_confirm);
  if (peer_confirm != legacy_state_->peer_confirm) {
    FXL_LOG(ERROR) << "sm: " << (legacy_state_->features->initiator ? "S" : "M")
                   << "confirm value does not match!";
    AbortLegacyPairing(ErrorCode::kConfirmValueFailed);
    return;
  }

  FXL_DCHECK(le_link_);

  // Generate the STK.
  UInt128 stk;
  util::S1(legacy_state_->tk, legacy_state_->peer_rand,
           legacy_state_->local_rand, &stk);

  // Mask the key based on the requested encryption key size.
  uint8_t key_size = legacy_state_->features->encryption_key_size;
  if (key_size < 16) {
    MutableBufferView view(stk.data() + key_size, 16 - key_size);
    view.SetToZeros();
  }

  // EDiv and Rand values are set to 0 to generate the STK (Vol 3, Part H,
  // 2.4.4.1).
  le_link_->set_link_key(hci::LinkKey(stk, 0, 0));

  if (legacy_state_->features->initiator) {
    // Initiate link layer encryption with STK.
    if (!le_link_->StartEncryption()) {
      FXL_LOG(ERROR) << "sm: Failed to start encryption";
      AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    }
  } else {
    // Send Srand and wait for the master to encrypt the link with the STK.
    // |le_link_| will respond to the LE LTK request event with the STK that it
    // got assigned above.
    LegacySendRandomValue();
  }
}

void PairingState::OnLELongTermKey(const common::UInt128& ltk) {
  if (!legacy_state_) {
    FXL_VLOG(1) << "sm: Ignoring LTK received while not in legacy pairing";
    return;
  }

  if (!legacy_state_->InPhase3()) {
    // The link MUST be encrypted with the STK for the transfer of the LTK to be
    // secure.
    FXL_LOG(ERROR) << "sm: Abort pairing due to LTK received outside phase 3";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  if (!legacy_state_->ShouldReceiveLTK()) {
    FXL_LOG(ERROR) << "sm: Received unexpected LTK";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Abort pairing if we received a second LTK from the peer.
  if (legacy_state_->has_ltk) {
    FXL_LOG(ERROR) << "sm: Already received LTK! Aborting";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  FXL_DCHECK(!(legacy_state_->obtained_remote_keys & KeyDistGen::kEncKey));
  legacy_state_->ltk_bytes = ltk;
  legacy_state_->has_ltk = true;

  // Wait to receive EDiv and Rand
}

void PairingState::OnLEMasterIdentification(uint16_t ediv, uint64_t random) {
  if (!legacy_state_) {
    FXL_VLOG(1)
        << "sm: Ignoring ediv/rand received while not in legacy pairing";
    return;
  }

  if (!legacy_state_->InPhase3()) {
    // The link MUST be encrypted with the STK for the transfer of the LTK to be
    // secure.
    FXL_LOG(ERROR)
        << "sm: Abort pairing due to ediv/rand received outside phase 3";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  FXL_DCHECK(legacy_state_->stk_encrypted);

  if (!legacy_state_->ShouldReceiveLTK()) {
    FXL_LOG(ERROR) << "sm: Received unexpected ediv/rand";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // EDIV and Rand must be sent AFTER the LTK (Vol 3, Part H, 3.6.1).
  if (!legacy_state_->has_ltk) {
    FXL_LOG(ERROR) << "sm: Received EDIV and Rand before LTK!";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  if (legacy_state_->obtained_remote_keys & KeyDistGen::kEncKey) {
    FXL_LOG(ERROR) << "sm: Already received EDIV and Rand!";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Store the LTK. We'll notify it when pairing is complete.
  hci::LinkKey ltk(legacy_state_->ltk_bytes, random, ediv);
  legacy_state_->obtained_remote_keys |= KeyDistGen::kEncKey;
  legacy_state_->ltk = ltk;

  // TODO(armansito): Move this to a subroutine called "MaybeCompletePhase3" and
  // call after each received key.
  FXL_DCHECK(!legacy_state_->ltk_encrypted);
  if (legacy_state_->RequestedKeysObtained()) {
    // We're no longer in Phase 3.
    FXL_DCHECK(!legacy_state_->InPhase3());

    // TODO(armansito): Distribute local keys if we are the master.

    // We're done. Encrypt the link with the LTK.
    le_link_->set_link_key(ltk);
    if (!le_link_->StartEncryption()) {
      FXL_LOG(ERROR) << "sm: Failed to start encryption";
      AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    }
  }
}

void PairingState::OnLEEncryptionChange(hci::Status status, bool enabled) {
  // TODO(armansito): Have separate subroutines to handle this event for legacy
  // and secure connections.
  if (!legacy_state_) {
    FXL_VLOG(2) << "sm: Ignoring encryption change while not pairing";
    return;
  }

  if (!status || !enabled) {
    FXL_LOG(ERROR) << "sm: Failed to encrypt link";
    AbortLegacyPairing(ErrorCode::kUnspecifiedReason);
    return;
  }

  FXL_DCHECK(le_smp_->pairing_started());

  if (legacy_state_->InPhase2()) {
    FXL_VLOG(1) << "sm: Link encrypted with STK";
    EndLegacyPairingPhase2();
    return;
  }

  // If encryption was enabled after Phase 3 then this completes the pairing
  // procedure.
  if (legacy_state_->RequestedKeysObtained() &&
      legacy_state_->WaitingForEncryptionWithLTK()) {
    FXL_VLOG(1) << "sm: Link encrypted with LTK";
    legacy_state_->ltk_encrypted = true;
    CompleteLegacyPairing();
  }
}

void PairingState::LEPairingAddresses(const DeviceAddress** out_initiator,
                                      const DeviceAddress** out_responder) {
  FXL_DCHECK(legacy_state_);
  FXL_DCHECK(legacy_state_->features);

  if (legacy_state_->features->initiator) {
    *out_initiator = &le_link_->local_address();
    *out_responder = &le_link_->peer_address();
  } else {
    *out_initiator = &le_link_->peer_address();
    *out_responder = &le_link_->local_address();
  }
}

}  // namespace sm
}  // namespace btlib
