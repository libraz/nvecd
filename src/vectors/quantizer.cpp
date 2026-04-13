/**
 * @file quantizer.cpp
 * @brief Scalar quantization implementation
 */

#include "vectors/quantizer.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "vectors/distance_simd.h"

namespace nvecd::vectors {

ScalarQuantizer::Stats ScalarQuantizer::ComputeStats(const float* vectors,
                                                     uint32_t count,
                                                     uint32_t dim) {
  Stats stats;
  stats.min_vals.resize(dim, std::numeric_limits<float>::max());
  stats.max_vals.resize(dim, std::numeric_limits<float>::lowest());

  for (uint32_t i = 0; i < count; ++i) {
    const float* vec = vectors + static_cast<size_t>(i) * dim;
    for (uint32_t d = 0; d < dim; ++d) {
      stats.min_vals[d] = std::min(stats.min_vals[d], vec[d]);
      stats.max_vals[d] = std::max(stats.max_vals[d], vec[d]);
    }
  }

  return stats;
}

void ScalarQuantizer::Quantize(const float* input, uint8_t* output,
                               uint32_t dim, const Stats& stats) {
  for (uint32_t d = 0; d < dim; ++d) {
    float range = stats.max_vals[d] - stats.min_vals[d];
    if (range < 1e-10F) {
      output[d] = 128;  // Midpoint for constant dimensions
    } else {
      float normalized = (input[d] - stats.min_vals[d]) / range;
      normalized = std::clamp(normalized, 0.0F, 1.0F);
      output[d] = static_cast<uint8_t>(normalized * 255.0F + 0.5F);
    }
  }
}

void ScalarQuantizer::Dequantize(const uint8_t* input, float* output,
                                 uint32_t dim, const Stats& stats) {
  for (uint32_t d = 0; d < dim; ++d) {
    float range = stats.max_vals[d] - stats.min_vals[d];
    output[d] = stats.min_vals[d] + (static_cast<float>(input[d]) / 255.0F) * range;
  }
}

void ScalarQuantizer::QuantizeBatch(const float* input, uint8_t* output,
                                    uint32_t count, uint32_t dim,
                                    const Stats& stats) {
  for (uint32_t i = 0; i < count; ++i) {
    Quantize(input + static_cast<size_t>(i) * dim,
             output + static_cast<size_t>(i) * dim, dim, stats);
  }
}

float ScalarQuantizer::QuantizedDotProduct(const uint8_t* a, const uint8_t* b,
                                           uint32_t dim,
                                           const Stats& stats) {
  // Reconstruct approximate dot product from quantized values:
  // a_orig[d] ≈ min[d] + (a[d]/255) * range[d]
  // b_orig[d] ≈ min[d] + (b[d]/255) * range[d]
  // dot = sum_d(a_orig[d] * b_orig[d])

  float result = 0.0F;
  for (uint32_t d = 0; d < dim; ++d) {
    float range = stats.max_vals[d] - stats.min_vals[d];
    float a_val = stats.min_vals[d] + (static_cast<float>(a[d]) / 255.0F) * range;
    float b_val = stats.min_vals[d] + (static_cast<float>(b[d]) / 255.0F) * range;
    result += a_val * b_val;
  }
  return result;
}

float ScalarQuantizer::AsymmetricCosine(const float* query,
                                        const uint8_t* quantized,
                                        uint32_t dim, const Stats& stats) {
  // Asymmetric distance: query stays float32, candidate is dequantized
  float dot = 0.0F;
  float norm_q = 0.0F;
  float norm_c = 0.0F;

  for (uint32_t d = 0; d < dim; ++d) {
    float range = stats.max_vals[d] - stats.min_vals[d];
    float c_val =
        stats.min_vals[d] + (static_cast<float>(quantized[d]) / 255.0F) * range;
    dot += query[d] * c_val;
    norm_q += query[d] * query[d];
    norm_c += c_val * c_val;
  }

  norm_q = std::sqrt(norm_q);
  norm_c = std::sqrt(norm_c);

  constexpr float kEps = 1e-7F;
  if (norm_q < kEps || norm_c < kEps) {
    return 0.0F;
  }
  return dot / (norm_q * norm_c);
}

}  // namespace nvecd::vectors
