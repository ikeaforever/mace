//
// Copyright (c) 2017 XiaoMi All rights reserved.
//
#ifndef MACE_KERNELS_RESIZE_BILINEAR_H_
#define MACE_KERNELS_RESIZE_BILINEAR_H_

#include "mace/core/future.h"
#include "mace/core/runtime/opencl/cl2_header.h"
#include "mace/core/tensor.h"

namespace mace {
namespace kernels {

namespace {
struct CachedInterpolation {
  index_t lower;  // Lower source index used in the interpolation
  index_t upper;  // Upper source index used in the interpolation
  // 1-D linear iterpolation scale (see:
  // https://en.wikipedia.org/wiki/Bilinear_interpolation)
  float lerp;
};

inline float CalculateResizeScale(index_t in_size,
                                  index_t out_size,
                                  bool align_corners) {
  return (align_corners && out_size > 1)
             ? (in_size - 1) / static_cast<float>(out_size - 1)
             : in_size / static_cast<float>(out_size);
}

inline void ComputeInterpolationWeights(const index_t out_size,
                                        const index_t in_size,
                                        const float scale,
                                        CachedInterpolation *interpolation) {
  interpolation[out_size].lower = 0;
  interpolation[out_size].upper = 0;
  for (index_t i = out_size - 1; i >= 0; --i) {
    const float in = i * scale;
    interpolation[i].lower = static_cast<index_t>(in);
    interpolation[i].upper = std::min(interpolation[i].lower + 1, in_size - 1);
    interpolation[i].lerp = in - interpolation[i].lower;
  }
}

inline float ComputeLerp(const float top_left,
                         const float top_right,
                         const float bottom_left,
                         const float bottom_right,
                         const float x_lerp,
                         const float y_lerp) {
  const float top = top_left + (top_right - top_left) * x_lerp;
  const float bottom = bottom_left + (bottom_right - bottom_left) * x_lerp;
  return top + (bottom - top) * y_lerp;
}

template <typename T>
void ResizeImage(const T *images,
                 const index_t batch_size,
                 const index_t in_height,
                 const index_t in_width,
                 const index_t out_height,
                 const index_t out_width,
                 const index_t channels,
                 const std::vector<CachedInterpolation> &xs_vec,
                 const std::vector<CachedInterpolation> &ys,
                 T *output) {
  const index_t in_batch_num_values = channels * in_height * in_width;
  const index_t out_batch_num_values = channels * out_height * out_width;
  const CachedInterpolation *xs = xs_vec.data();

#pragma omp parallel for collapse(2)
  for (index_t b = 0; b < batch_size; ++b) {
    for (index_t y = 0; y < out_height; ++y) {
      const T *batch_input_ptr = images + in_batch_num_values * b;
      T *batch_output_ptr = output + out_batch_num_values * b;
      const T *y_lower_input_ptr =
          batch_input_ptr + ys[y].lower * in_width * channels;
      const T *y_upper_input_ptr =
          batch_input_ptr + ys[y].upper * in_width * channels;
      T *y_output_ptr = batch_output_ptr + y * out_width * channels;
      const float ys_lerp = ys[y].lerp;

      for (index_t x = 0; x < out_width; ++x) {
        const float xs_lerp = xs[x].lerp;
        const T *top_left_ptr = y_lower_input_ptr + xs[x].lower * channels;
        const T *top_right_ptr = y_lower_input_ptr + xs[x].upper * channels;
        const T *bottom_left_ptr = y_upper_input_ptr + xs[x].lower * channels;
        const T *bottom_right_ptr = y_upper_input_ptr + xs[x].upper * channels;
        T *output_ptr = y_output_ptr + x * channels;

        for (index_t c = 0; c < channels; ++c) {
          const T top_left = top_left_ptr[c];
          const T top_right = top_right_ptr[c];
          const T bottom_left = bottom_left_ptr[c];
          const T bottom_right = bottom_right_ptr[c];

          output_ptr[c] = ComputeLerp(top_left, top_right, bottom_left,
                                      bottom_right, xs_lerp, ys_lerp);
        }
      }
    }
  }
}
}

struct ResizeBilinearFunctorBase {
  ResizeBilinearFunctorBase(const std::vector<index_t> &size,
                            bool align_corners)
      : align_corners_(align_corners) {
    MACE_CHECK(size.size() == 2);
    out_height_ = size[0];
    out_width_ = size[1];
  }

 protected:
  bool align_corners_;
  index_t out_height_;
  index_t out_width_;
};

template <DeviceType D, typename T>
struct ResizeBilinearFunctor : ResizeBilinearFunctorBase {
  ResizeBilinearFunctor(const std::vector<index_t> &size, bool align_corners)
      : ResizeBilinearFunctorBase(size, align_corners) {}

  void operator()(const Tensor *input, Tensor *output, StatsFuture *future) {
    const index_t batch = input->dim(0);
    const index_t in_height = input->dim(1);
    const index_t in_width = input->dim(2);
    const index_t channels = input->dim(3);

    index_t out_height = out_height_;
    index_t out_width = out_width_;
    MACE_CHECK(out_height > 0 && out_width > 0);
    std::vector<index_t> out_shape{batch, out_height, out_width, channels};
    output->Resize(out_shape);

    Tensor::MappingGuard input_mapper(input);
    Tensor::MappingGuard output_mapper(output);
    const T *input_data = input->data<T>();
    T *output_data = output->mutable_data<T>();

    if (out_height == in_height && out_width == in_width) {
      std::copy(input_data, input_data + channels * in_height * in_width,
                output_data);
      return;
    }

    float height_scale =
        CalculateResizeScale(in_height, out_height, align_corners_);
    float width_scale =
        CalculateResizeScale(in_width, out_width, align_corners_);

    std::vector<CachedInterpolation> ys(out_height + 1);
    std::vector<CachedInterpolation> xs(out_width + 1);

    // Compute the cached interpolation weights on the x and y dimensions.
    ComputeInterpolationWeights(out_height, in_height, height_scale, ys.data());
    ComputeInterpolationWeights(out_width, in_width, width_scale, xs.data());

    ResizeImage(input_data, batch, in_height, in_width, out_height, out_width,
                channels, xs, ys, output_data);
  }
};

template <typename T>
struct ResizeBilinearFunctor<DeviceType::OPENCL, T>
    : ResizeBilinearFunctorBase {
  ResizeBilinearFunctor(const std::vector<index_t> &size, bool align_corners)
      : ResizeBilinearFunctorBase(size, align_corners) {}

  void operator()(const Tensor *input, Tensor *output, StatsFuture *future);

  cl::Kernel kernel_;
};

}  // namespace kernels
}  // namespace mace

#endif  // MACE_KERNELS_RESIZE_BILINEAR_H_
