// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_SWAPCHAIN_MAGMA_BUFFER_H_
#define GARNET_LIB_UI_GFX_SWAPCHAIN_MAGMA_BUFFER_H_

#include <memory>

#include <lib/zx/event.h>
#include <lib/zx/vmo.h>

#include "garnet/lib/magma/include/magma_abi/magma.h"
#include "garnet/lib/ui/gfx/swapchain/magma_connection.h"
#include "lib/fxl/macros.h"

namespace scenic {
namespace gfx {

// Wraps a magma_buffer_t and takes care of releasing it on object destruction.
class MagmaBuffer {
 public:
  MagmaBuffer();
  MagmaBuffer(MagmaConnection* magma_connection, magma_buffer_t buffer);
  MagmaBuffer(MagmaBuffer&& rhs);
  MagmaBuffer& operator=(MagmaBuffer&& rhs);

  ~MagmaBuffer();
  static MagmaBuffer NewFromVmo(MagmaConnection* magma_connection,
                                const zx::vmo& vmo_handle);
  const magma_buffer_t& get() const { return buffer_; }

 private:
  MagmaConnection* magma_connection_;
  magma_buffer_t buffer_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MagmaBuffer);
};

}  // namespace gfx
}  // namespace scenic

#endif  // GARNET_LIB_UI_GFX_SWAPCHAIN_MAGMA_BUFFER_H_