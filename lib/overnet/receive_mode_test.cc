// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "receive_mode.h"
#include "gtest/gtest.h"
#include "receive_mode_fuzzer_helpers.h"

namespace overnet {
namespace receive_mode {
namespace receive_mode_test {

// Wrapper to allow testing parameterized modes without needing to add
// constructor variants everywhere
template <ReliabilityAndOrdering reliability_and_ordering>
class ParameterizedWrapper final : public ReceiveMode {
 public:
  ParameterizedWrapper() : mode_(reliability_and_ordering) {}

  bool Begin(uint64_t seq, StatusCallback ready) override {
    return mode_.Begin(seq, std::move(ready));
  }
  bool Completed(uint64_t seq, const Status& status) override {
    return mode_.Completed(seq, status);
  }
  AckFrame GenerateAck() const override { return mode_.GenerateAck(); }

 private:
  ParameterizedReceiveMode mode_;
};

template <class Type>
class ReceiveModeTest : public ::testing::Test {
 public:
  bool Begin(uint64_t seq) {
    auto cb = StatusCallback(ALLOCATED_CALLBACK, [=](const Status& status) {
      auto it = expected_begin_cbs_.find(seq);
      if (it == expected_begin_cbs_.end()) {
        FAIL() << "Unexpected Begin callback: seq=" << seq
               << " status=" << status;
      }
      if (status.code() != it->second) {
        FAIL() << "Unexpected status for Begin callback: seq=" << seq
               << " expected " << StatusCodeString(it->second) << " but got "
               << status;
      }
      expected_begin_cbs_.erase(it);
    });
    return type_.Begin(seq, std::move(cb));
  }

  void ExpectBegin(uint64_t seq, StatusCode expect) {
    EXPECT_EQ(0u, expected_begin_cbs_.count(seq));
    expected_begin_cbs_[seq] = expect;
  }

  bool Completed(uint64_t seq, const Status& status) {
    return type_.Completed(seq, status);
  }

  AckFrame GenerateAck() { return type_.GenerateAck(); }

  ~ReceiveModeTest() {
    for (auto exp : expected_begin_cbs_) {
      ADD_FAILURE() << "Expected seq " << exp.first
                    << " to complete with status "
                    << StatusCodeString(exp.second) << " but got nothing";
    }
  }

 private:
  Type type_;

  std::unordered_map<uint64_t, StatusCode> expected_begin_cbs_;
};

///////////////////////////////////////////////////////////////////////////////
// Generic tests

typedef ::testing::Types<
    ReliableOrdered, ReliableUnordered, UnreliableOrdered, UnreliableUnordered,
    TailReliable, ParameterizedWrapper<ReliabilityAndOrdering::ReliableOrdered>,
    ParameterizedWrapper<ReliabilityAndOrdering::ReliableUnordered>,
    ParameterizedWrapper<ReliabilityAndOrdering::UnreliableOrdered>,
    ParameterizedWrapper<ReliabilityAndOrdering::UnreliableUnordered>,
    ParameterizedWrapper<ReliabilityAndOrdering::TailReliable>>
    ReceiveModeTypes;
TYPED_TEST_CASE(ReceiveModeTest, ReceiveModeTypes);

TYPED_TEST(ReceiveModeTest, InitialAck) {
  EXPECT_EQ(AckFrame(1, 0, 0, {}), this->GenerateAck());
}

TYPED_TEST(ReceiveModeTest, SimpleInSeq) {
  for (uint64_t i = 1u; i <= 10u; i++) {
    this->ExpectBegin(i, StatusCode::OK);
    this->Begin(i);
    this->Completed(i, Status::Ok());
  }
  EXPECT_EQ(AckFrame(11, 0, 0, {}), this->GenerateAck());
}

TYPED_TEST(ReceiveModeTest, SmallBatchInSeq) {
  for (uint64_t i = 1u; i <= 10u; i++) {
    this->ExpectBegin(i, StatusCode::OK);
  }
  for (uint64_t i = 1u; i <= 10u; i++) {
    this->Begin(i);
  }
  for (uint64_t i = 1u; i <= 10u; i++) {
    this->Completed(i, Status::Ok());
  }
}

TYPED_TEST(ReceiveModeTest, RepeatedFailure) {
  for (uint64_t i = 1u; i <= 10u; i++) {
    this->ExpectBegin(1, StatusCode::OK);
    this->Begin(1);
    this->Completed(1, Status::Cancelled());
  }
}

TYPED_TEST(ReceiveModeTest, DuplicateBegin) {
  this->ExpectBegin(1, StatusCode::OK);
  this->Begin(1);
  // get dup while processing
  this->ExpectBegin(1, StatusCode::CANCELLED);
  this->Begin(1);
  this->Completed(1, StatusCode::OK);
  // get dup after processing
  this->ExpectBegin(1, StatusCode::CANCELLED);
  this->Begin(1);
}

///////////////////////////////////////////////////////////////////////////////
// ReliableOrdered specific tests

template <class T>
using ReliableOrderedTest = ReceiveModeTest<T>;
typedef ::testing::Types<
    ReliableOrdered,
    ParameterizedWrapper<ReliabilityAndOrdering::ReliableOrdered>>
    ReliableOrderedTypes;
TYPED_TEST_CASE(ReliableOrderedTest, ReliableOrderedTypes);

TYPED_TEST(ReliableOrderedTest, MissedOneThenGotIt) {
  EXPECT_TRUE(this->Begin(2));
  EXPECT_EQ(AckFrame(1, 0, 0, {1}), this->GenerateAck());
  this->ExpectBegin(1, StatusCode::OK);
  this->Begin(1);
  this->ExpectBegin(2, StatusCode::OK);
  this->Completed(1, Status::Ok());
  this->Completed(2, Status::Ok());
}

///////////////////////////////////////////////////////////////////////////////
// ReliableUnordered specific tests

template <class T>
using ReliableUnorderedTest = ReceiveModeTest<T>;
typedef ::testing::Types<
    ReliableUnordered,
    ParameterizedWrapper<ReliabilityAndOrdering::ReliableUnordered>>
    ReliableUnorderedTypes;
TYPED_TEST_CASE(ReliableUnorderedTest, ReliableUnorderedTypes);

TYPED_TEST(ReliableUnorderedTest, MissedOneThenGotIt) {
  this->ExpectBegin(2, StatusCode::OK);
  EXPECT_TRUE(this->Begin(2));
  EXPECT_EQ(AckFrame(1, 0, 0, {1}), this->GenerateAck());
  this->ExpectBegin(1, StatusCode::OK);
  this->Begin(1);
  this->Completed(1, Status::Ok());
  this->Completed(2, Status::Ok());
}

///////////////////////////////////////////////////////////////////////////////
// UnreliableOrdered specific tests

template <class T>
using UnreliableOrderedTest = ReceiveModeTest<T>;
typedef ::testing::Types<
    UnreliableOrdered,
    ParameterizedWrapper<ReliabilityAndOrdering::UnreliableOrdered>>
    UnreliableOrderedTypes;
TYPED_TEST_CASE(UnreliableOrderedTest, UnreliableOrderedTypes);

TYPED_TEST(UnreliableOrderedTest, Skippy) {
  this->ExpectBegin(1, StatusCode::OK);
  this->Begin(1);
  this->Completed(1, Status::Ok());

  this->ExpectBegin(2, StatusCode::OK);
  this->Begin(2);
  this->Begin(4);
  this->ExpectBegin(4, StatusCode::OK);
  this->Completed(2, Status::Ok());

  this->ExpectBegin(3, StatusCode::CANCELLED);
  this->Begin(3);

  this->Begin(70000000);
  this->ExpectBegin(70000000, StatusCode::OK);
  this->Completed(4, StatusCode::CANCELLED);
  this->Completed(70000000, Status::Ok());
}

///////////////////////////////////////////////////////////////////////////////
// UnreliableUnordered specific tests

template <class T>
using UnreliableUnorderedTest = ReceiveModeTest<T>;
typedef ::testing::Types<
    UnreliableUnordered,
    ParameterizedWrapper<ReliabilityAndOrdering::UnreliableUnordered>>
    UnreliableUnorderedTypes;
TYPED_TEST_CASE(UnreliableUnorderedTest, UnreliableUnorderedTypes);

TYPED_TEST(UnreliableUnorderedTest, AnythingGoesReally) {
  this->ExpectBegin(3, StatusCode::OK);
  this->Begin(3);
  this->ExpectBegin(2, StatusCode::OK);
  this->Begin(2);
  this->Completed(2, Status::Ok());
  this->Completed(3, Status::Cancelled());
  this->ExpectBegin(1, StatusCode::OK);
  this->Begin(1);
  this->Completed(1, Status::Cancelled());
}

///////////////////////////////////////////////////////////////////////////////
// Error specific tests

template <class T>
using ErrorTest = ReceiveModeTest<T>;
typedef ::testing::Types<
    Error, ParameterizedWrapper<static_cast<ReliabilityAndOrdering>(255)>>
    ErrorTypes;
TYPED_TEST_CASE(ErrorTest, ErrorTypes);

TYPED_TEST(ErrorTest, BeginAlwaysFails) {
  for (uint64_t i = 0; i < 20; i++) {
    this->ExpectBegin(i, StatusCode::CANCELLED);
    this->Begin(i);
  }
}

TYPED_TEST(ErrorTest, ErrorInitialAck) {
  EXPECT_EQ(AckFrame(1, 0, 0, {}), this->GenerateAck());
}

///////////////////////////////////////////////////////////////////////////////
// Fuzzer found failures
//
// Procedure: run the receive_mode_fuzzer
//            use ./receive_mode_fuzzer_corpus_to_code.py corpus_entry
//            copy paste test here, leave a comment about what failed
//            (also, fix it)

// Originally led to an OOM generating nacks
TEST(ReceiveModeFuzzed, _18dd797d7734fe115b89e72e79a26713c480096c) {
  receive_mode::ParameterizedReceiveMode m(
      static_cast<ReliabilityAndOrdering>(1));
  m.GenerateAck();
  m.Begin(112ull, StatusCallback([](const Status&) {}));
  m.GenerateAck();
  m.Begin(1125899905794047ull, StatusCallback([](const Status&) {}));
  m.GenerateAck();
}

// Originally led to an out of range index into a bitset
TEST(ReceiveModeFuzzed, _0ea3e010446b0d18d1a08efc4e7f028372140b81) {
  receive_mode::ParameterizedReceiveMode m(
      static_cast<ReliabilityAndOrdering>(3));
  m.GenerateAck();
  m.Begin(450ull, StatusCallback([](const Status&) {}));
  m.GenerateAck();
}

// Originally led to an OOM generating nacks
TEST(ReceiveModeFuzzed, _feac8b7a55c39b0d70f86f40d340888a25ea69b8) {
  receive_mode::ParameterizedReceiveMode m(
      static_cast<ReliabilityAndOrdering>(4));
  m.GenerateAck();
  m.Begin(1ull, StatusCallback([](const Status&) {}));
  m.GenerateAck();
  m.Begin(143547839805374333ull, StatusCallback([](const Status&) {}));
  m.GenerateAck();
  m.Begin(139044205818265471ull, StatusCallback([](const Status&) {}));
  m.GenerateAck();
  m.Begin(0ull, StatusCallback([](const Status&) {}));
  m.GenerateAck();
}

// Originally led to a crash
TEST(ReceiveModeFuzzed, _75079e04acd10ecdf49c97c714f344c811e870dd) {
  receive_mode::Fuzzer m(3);
  m.Step();
  if (!m.Begin(3ull)) return;
  m.Step();
  if (!m.Begin(33ull)) return;
  m.Step();
  if (!m.Begin(255ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(89ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(2ull)) return;
  m.Step();
  if (!m.Completed(1ull, 0)) return;
  m.Step();
  if (!m.Begin(112ull)) return;
  m.Step();
  if (!m.Begin(0ull)) return;
  m.Step();
  if (!m.Begin(3ull)) return;
  m.Step();
  if (!m.Begin(3ull)) return;
  m.Step();
  if (!m.Begin(33ull)) return;  // Crash occurred here, rest of test clipped.
}

TEST(ReceiveModeFuzzed, _6b1d71bc2330430a1719c9774a7408bbd3aa7f29) {
  receive_mode::Fuzzer m(2);
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Completed(1ull, 1)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(2ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(3ull)) return;
  m.Step();
  if (!m.Begin(3ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Begin(112ull)) return;
  m.Step();
  if (!m.Begin(0ull)) return;
  m.Step();
  if (!m.Begin(1ull)) return;
  m.Step();
  if (!m.Completed(1ull, 0)) return;
  m.Step();
  if (!m.Begin(30331092862ull)) return;
  m.Step();
  if (!m.Begin(68ull)) return;
  m.Step();
  if (!m.Begin(10737418235ull)) return;
  m.Step();
  if (!m.Begin(112ull)) return;
  m.Step();
  if (!m.Begin(0ull)) return;
  m.Step();
  if (!m.Begin(34357641211ull)) return;
  m.Step();
  if (!m.Begin(268435451ull)) return;
  m.Step();
  if (!m.Begin(10737418235ull)) return;
  m.Step();
  if (!m.Begin(112ull)) return;  // Crash occurred here, rest of test clipped.
}

}  // namespace receive_mode_test
}  // namespace receive_mode
}  // namespace overnet