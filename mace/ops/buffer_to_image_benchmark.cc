// Copyright 2018 Xiaomi, Inc.  All rights reserved.
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

#include "mace/core/operator.h"
#include "mace/core/runtime/opencl/opencl_runtime.h"
#include "mace/core/testing/test_benchmark.h"
#include "mace/ops/ops_test_util.h"

namespace mace {
namespace ops {
namespace test {

namespace {
template <DeviceType D, typename T>
void FilterBufferToImage(int iters,
                         int out_channel, int in_channel,
                         int height, int width) {
  mace::testing::StopTiming();

  OpsTestNet net;

  // Add input data
  net.AddRandomInput<D, T>("Input",
                           {out_channel, in_channel, height, width});

  OpDefBuilder("BufferToImage", "BufferToImageBM")
      .Input("Input")
      .Output("Output")
      .Finalize(net.NewOperatorDef());

  // Warm-up
  net.Setup(D);
  for (int i = 0; i < 5; ++i) {
    net.Run();
  }
  net.Sync();

  mace::testing::StartTiming();
  while (iters--) {
    net.Run();
  }
  net.Sync();
}
}  // namespace

#define BM_B2I_MACRO(O, I, H, W, TYPE, DEVICE)                  \
  static void BM_B2I_##O##_##I##_##H##_##W##_##TYPE##_##DEVICE( \
      int iters) {                                                   \
    const int64_t tot = static_cast<int64_t>(iters) * O * I * H * W; \
    mace::testing::MaccProcessed(tot);                               \
    mace::testing::BytesProcessed(tot *(sizeof(TYPE)));              \
    FilterBufferToImage<DEVICE, TYPE>(iters, O, I, H, W);            \
  }                                                                  \
  BENCHMARK(BM_B2I_##O##_##I##_##H##_##W##_##TYPE##_##DEVICE)

#define BM_B2I(O, I, H, W)              \
  BM_B2I_MACRO(O, I, H, W, float, GPU); \
  BM_B2I_MACRO(O, I, H, W, half, GPU);

BM_B2I(5, 3, 3, 3);
BM_B2I(5, 3, 7, 7);
BM_B2I(32, 16, 1, 1);
BM_B2I(32, 16, 3, 3);
BM_B2I(32, 16, 5, 5);
BM_B2I(32, 16, 7, 7);
BM_B2I(64, 32, 1, 1);
BM_B2I(64, 32, 3, 3);
BM_B2I(64, 32, 5, 5);
BM_B2I(64, 32, 7, 7);
BM_B2I(128, 64, 1, 1);
BM_B2I(128, 64, 3, 3);
BM_B2I(128, 32, 1, 1);
BM_B2I(128, 32, 3, 3);
BM_B2I(256, 32, 1, 1);
BM_B2I(256, 32, 3, 3);

}  // namespace test
}  // namespace ops
}  // namespace mace