/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_MODULES_XAM_H_
#define XENIA_KERNEL_MODULES_XAM_H_

#include <xenia/common.h>
#include <xenia/core.h>

#include <xenia/kernel/export.h>
#include <xenia/kernel/kernel_module.h>
#include <xenia/kernel/modules/xam/xam_ordinals.h>

// All of the exported functions:
#include <xenia/kernel/modules/xam/xam_info.h>


namespace xe {
namespace kernel {
namespace xam {

class XamState;


class XamModule : public KernelModule {
public:
  XamModule(Runtime* runtime);
  virtual ~XamModule();

private:
  auto_ptr<XamState> xam_state;
};


}  // namespace xam
}  // namespace kernel
}  // namespace xe


#endif  // XENIA_KERNEL_MODULES_XAM_H_