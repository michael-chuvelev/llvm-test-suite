// UNSUPPORTED: cuda
// REQUIRES: gpu,linux
// RUN: %clangxx -fsycl %s -o %t.out
// RUN: %GPU_RUN_PLACEHOLDER %t.out

#include "../include/asmhelper.h"
#include <CL/sycl.hpp>

struct KernelFunctor {
  KernelFunctor() {}

  void operator()(cl::sycl::handler &cgh) {
    cgh.parallel_for<KernelFunctor>(
        cl::sycl::range<1>{16}, [=
    ](cl::sycl::id<1> wiID) [[intel::reqd_sub_group_size(8)]] {
#if defined(__SYCL_DEVICE_ONLY__)
          asm volatile(".decl tmp1 v_type=G type=d num_elts=16 align=GRF\n"
                       ".decl tmp2 v_type=G type=d num_elts=16 align=GRF\n"
                       "movi (M1_NM, 8) tmp1(0,1)<1>  tmp2(0,0)\n");
#endif
        });
  }
};

int main() {
  KernelFunctor f;
  launchInlineASMTest(f, /* sg size */ true,
                      /* exception string*/ "syntax error, unexpected IDENT");
  return 0;
}
