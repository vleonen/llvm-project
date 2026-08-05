//===--------- Passes/Golang/go_v1_22.h -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "go_base.h"
#include "go_v1_20.h"

namespace llvm {
namespace bolt {

class Pclntab_v1_22_1 : public Pclntab_v1_20_7 {
public:
  ~Pclntab_v1_22_1() = default;
};

struct GoFunc_v1_22_1 : GoFunc_v1_20_7 {
  enum {
    funcID_normal,
    funcID_abort,
    funcID_asmcgocall,
    funcID_asyncPreempt,
    funcID_cgocallback,
    funcID_corostart,
    funcID_debugCallV2,
    funcID_gcBgMarkWorker,
    funcID_goexit,
    funcID_gogo,
    funcID_gopanic,
    funcID_handleAsyncEvent,
    funcID_mcall,
    funcID_morestack,
    funcID_mstart,
    funcID_panicwrap,
    funcID_rt0_go,
    funcID_runfinq,
    funcID_runtime_main,
    funcID_sigpanic,
    funcID_systemstack,
    funcID_systemstack_switch,
    funcID_wrapper,
  };

  uint32_t getFuncID() const override { return __GoFunc.FuncID; }

  uint32_t getFuncIDForWrapper() const override { return funcID_wrapper; }

  bool hasReservedID(std::string Name) const override {
    return __GoFunc.FuncID != funcID_normal &&
           __GoFunc.FuncID != funcID_wrapper;
  }

  ~GoFunc_v1_22_1() = default;
};

struct Module_v1_22_1 : Module_v1_20_7 {
  ~Module_v1_22_1() = default;
};

struct InlinedCall_v1_22_1 : InlinedCall_v1_20_7 {
  ~InlinedCall_v1_22_1() = default;
};

} // namespace bolt
} // namespace llvm
