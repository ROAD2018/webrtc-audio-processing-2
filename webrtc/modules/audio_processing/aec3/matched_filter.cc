/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "webrtc/modules/audio_processing/aec3/matched_filter.h"

#include "webrtc/typedefs.h"
#if defined(WEBRTC_ARCH_X86_FAMILY)
#include <emmintrin.h>
#endif
#include <algorithm>
#include <numeric>

#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/audio_processing/logging/apm_data_dumper.h"

namespace webrtc {
namespace aec3 {

#if defined(WEBRTC_ARCH_X86_FAMILY)

void MatchedFilterCore_SSE2(size_t x_start_index,
                            float x2_sum_threshold,
                            rtc::ArrayView<const float> x,
                            rtc::ArrayView<const float> y,
                            rtc::ArrayView<float> h,
                            bool* filters_updated,
                            float* error_sum) {
  const int h_size = static_cast<int>(h.size());
  const int x_size = static_cast<int>(x.size());
  RTC_DCHECK_EQ(0, h_size % 4);

  // Process for all samples in the sub-block.
  for (size_t i = 0; i < kSubBlockSize; ++i) {
    // Apply the matched filter as filter * x, and compute x * x.

    RTC_DCHECK_GT(x_size, x_start_index);
    const float* x_p = &x[x_start_index];
    const float* h_p = &h[0];

    // Initialize values for the accumulation.
    __m128 s_128 = _mm_set1_ps(0);
    __m128 x2_sum_128 = _mm_set1_ps(0);
    float x2_sum = 0.f;
    float s = 0;

    // Compute loop chunk sizes until, and after, the wraparound of the circular
    // buffer for x.
    const int chunk1 =
        std::min(h_size, static_cast<int>(x_size - x_start_index));

    // Perform the loop in two chunks.
    const int chunk2 = h_size - chunk1;
    for (int limit : {chunk1, chunk2}) {
      // Perform 128 bit vector operations.
      const int limit_by_4 = limit >> 2;
      for (int k = limit_by_4; k > 0; --k, h_p += 4, x_p += 4) {
        // Load the data into 128 bit vectors.
        const __m128 x_k = _mm_loadu_ps(x_p);
        const __m128 h_k = _mm_loadu_ps(h_p);
        const __m128 xx = _mm_mul_ps(x_k, x_k);
        // Compute and accumulate x * x and h * x.
        x2_sum_128 = _mm_add_ps(x2_sum_128, xx);
        const __m128 hx = _mm_mul_ps(h_k, x_k);
        s_128 = _mm_add_ps(s_128, hx);
      }

      // Perform non-vector operations for any remaining items.
      for (int k = limit - limit_by_4 * 4; k > 0; --k, ++h_p, ++x_p) {
        const float x_k = *x_p;
        x2_sum += x_k * x_k;
        s += *h_p * x_k;
      }

      x_p = &x[0];
    }

    // Combine the accumulated vector and scalar values.
    float* v = reinterpret_cast<float*>(&x2_sum_128);
    x2_sum += v[0] + v[1] + v[2] + v[3];
    v = reinterpret_cast<float*>(&s_128);
    s += v[0] + v[1] + v[2] + v[3];

    // Compute the matched filter error.
    const float e = std::min(32767.f, std::max(-32768.f, y[i] - s));
    *error_sum += e * e;

    // Update the matched filter estimate in an NLMS manner.
    if (x2_sum > x2_sum_threshold) {
      RTC_DCHECK_LT(0.f, x2_sum);
      const float alpha = 0.7f * e / x2_sum;
      const __m128 alpha_128 = _mm_set1_ps(alpha);

      // filter = filter + 0.7 * (y - filter * x) / x * x.
      float* h_p = &h[0];
      x_p = &x[x_start_index];

      // Perform the loop in two chunks.
      for (int limit : {chunk1, chunk2}) {
        // Perform 128 bit vector operations.
        const int limit_by_4 = limit >> 2;
        for (int k = limit_by_4; k > 0; --k, h_p += 4, x_p += 4) {
          // Load the data into 128 bit vectors.
          __m128 h_k = _mm_loadu_ps(h_p);
          const __m128 x_k = _mm_loadu_ps(x_p);

          // Compute h = h + alpha * x.
          const __m128 alpha_x = _mm_mul_ps(alpha_128, x_k);
          h_k = _mm_add_ps(h_k, alpha_x);

          // Store the result.
          _mm_storeu_ps(h_p, h_k);
        }

        // Perform non-vector operations for any remaining items.
        for (int k = limit - limit_by_4 * 4; k > 0; --k, ++h_p, ++x_p) {
          *h_p += alpha * *x_p;
        }

        x_p = &x[0];
      }

      *filters_updated = true;
    }

    x_start_index = x_start_index > 0 ? x_start_index - 1 : x_size - 1;
  }
}
#endif

void MatchedFilterCore(size_t x_start_index,
                       float x2_sum_threshold,
                       rtc::ArrayView<const float> x,
                       rtc::ArrayView<const float> y,
                       rtc::ArrayView<float> h,
                       bool* filters_updated,
                       float* error_sum) {
  // Process for all samples in the sub-block.
  for (size_t i = 0; i < kSubBlockSize; ++i) {
    // Apply the matched filter as filter * x, and compute x * x.
    float x2_sum = 0.f;
    float s = 0;
    size_t x_index = x_start_index;
    for (size_t k = 0; k < h.size(); ++k) {
      x2_sum += x[x_index] * x[x_index];
      s += h[k] * x[x_index];
      x_index = x_index < (x.size() - 1) ? x_index + 1 : 0;
    }

    // Compute the matched filter error.
    const float e = std::min(32767.f, std::max(-32768.f, y[i] - s));
    (*error_sum) += e * e;

    // Update the matched filter estimate in an NLMS manner.
    if (x2_sum > x2_sum_threshold) {
      RTC_DCHECK_LT(0.f, x2_sum);
      const float alpha = 0.7f * e / x2_sum;

      // filter = filter + 0.7 * (y - filter * x) / x * x.
      size_t x_index = x_start_index;
      for (size_t k = 0; k < h.size(); ++k) {
        h[k] += alpha * x[x_index];
        x_index = x_index < (x.size() - 1) ? x_index + 1 : 0;
      }
      *filters_updated = true;
    }

    x_start_index = x_start_index > 0 ? x_start_index - 1 : x.size() - 1;
  }
}

}  // namespace aec3

MatchedFilter::MatchedFilter(ApmDataDumper* data_dumper,
                             Aec3Optimization optimization,
                             size_t window_size_sub_blocks,
                             int num_matched_filters,
                             size_t alignment_shift_sub_blocks)
    : data_dumper_(data_dumper),
      optimization_(optimization),
      filter_intra_lag_shift_(alignment_shift_sub_blocks * kSubBlockSize),
      filters_(num_matched_filters,
               std::vector<float>(window_size_sub_blocks * kSubBlockSize, 0.f)),
      lag_estimates_(num_matched_filters) {
  RTC_DCHECK(data_dumper);
  RTC_DCHECK_LT(0, window_size_sub_blocks);
}

MatchedFilter::~MatchedFilter() = default;

void MatchedFilter::Reset() {
  for (auto& f : filters_) {
    std::fill(f.begin(), f.end(), 0.f);
  }

  for (auto& l : lag_estimates_) {
    l = MatchedFilter::LagEstimate();
  }
}

void MatchedFilter::Update(const DownsampledRenderBuffer& render_buffer,
                           const std::array<float, kSubBlockSize>& capture) {
  const std::array<float, kSubBlockSize>& y = capture;

  const float x2_sum_threshold = filters_[0].size() * 150.f * 150.f;

  // Apply all matched filters.
  size_t alignment_shift = 0;
  for (size_t n = 0; n < filters_.size(); ++n) {
    float error_sum = 0.f;
    bool filters_updated = false;

    size_t x_start_index =
        (render_buffer.position + alignment_shift + kSubBlockSize - 1) %
        render_buffer.buffer.size();

    switch (optimization_) {
#if defined(WEBRTC_ARCH_X86_FAMILY)
      case Aec3Optimization::kSse2:
        aec3::MatchedFilterCore_SSE2(x_start_index, x2_sum_threshold,
                                     render_buffer.buffer, y, filters_[n],
                                     &filters_updated, &error_sum);
        break;
#endif
      default:
        aec3::MatchedFilterCore(x_start_index, x2_sum_threshold,
                                render_buffer.buffer, y, filters_[n],
                                &filters_updated, &error_sum);
    }

    // Compute anchor for the matched filter error.
    const float error_sum_anchor =
        std::inner_product(y.begin(), y.end(), y.begin(), 0.f);

    // Estimate the lag in the matched filter as the distance to the portion in
    // the filter that contributes the most to the matched filter output. This
    // is detected as the peak of the matched filter.
    const size_t lag_estimate = std::distance(
        filters_[n].begin(),
        std::max_element(
            filters_[n].begin(), filters_[n].end(),
            [](float a, float b) -> bool { return a * a < b * b; }));

    // Update the lag estimates for the matched filter.
    const float kMatchingFilterThreshold = 0.1f;
    lag_estimates_[n] = LagEstimate(
        error_sum_anchor - error_sum,
        (lag_estimate > 2 && lag_estimate < (filters_[n].size() - 10) &&
         error_sum < kMatchingFilterThreshold * error_sum_anchor),
        lag_estimate + alignment_shift, filters_updated);

    // TODO(peah): Remove once development of EchoCanceller3 is fully done.
    RTC_DCHECK_EQ(4, filters_.size());
    switch (n) {
      case 0:
        data_dumper_->DumpRaw("aec3_correlator_0_h", filters_[0]);
        break;
      case 1:
        data_dumper_->DumpRaw("aec3_correlator_1_h", filters_[1]);
        break;
      case 2:
        data_dumper_->DumpRaw("aec3_correlator_2_h", filters_[2]);
        break;
      case 3:
        data_dumper_->DumpRaw("aec3_correlator_3_h", filters_[3]);
        break;
      default:
        RTC_DCHECK(false);
    }

    alignment_shift += filter_intra_lag_shift_;
  }
}

}  // namespace webrtc
