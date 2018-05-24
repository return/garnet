// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AMLOGIC_VIDEO_H_
#define AMLOGIC_VIDEO_H_

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/platform-defs.h>
#include <ddk/protocol/platform-device.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zx/handle.h>

class AmlogicVideo {
 public:
  ~AmlogicVideo();

  zx_status_t Init(zx_device_t* parent);

 private:
  zx_device_t* parent_ = nullptr;
  zx_device_t* device_ = nullptr;
  platform_device_protocol_t pdev_;
  io_buffer_t mmio_cbus_ = {};
  io_buffer_t mmio_dosbus_ = {};
  io_buffer_t mmio_hiubus_ = {};
  io_buffer_t mmio_aobus_ = {};
  io_buffer_t mmio_dmc_ = {};

  zx::handle bti_;

  zx::handle parser_interrupt_handle_;
  zx::handle vdec1_interrupt_handle_;
};

#endif  // AMLOGIC_VIDEO_H_