/**
 * @file quantizer.h
 * @brief Scalar quantization (SQ8) for memory-efficient vector storage
 *
 * Reduces memory footprint by encoding float32 vectors as uint8 (75% reduction).
 * Uses per-dimension min/max statistics for linear quantization.
 *
 * Typical workflow:
 * 1. ComputeStats() over the full dataset
 * 2. Quantize() each vector to uint8
 * 3. QuantizedDotProduct() for approximate distance computation
 * 4. Re-rank top candidates with original float32 for precision
 */

#pragma once

#include <cstdint>
#include <vector>

namespace nvecd::vectors {

/**
 * @brief Scalar quantization (float32 -> uint8)
 */
class ScalarQuantizer {
 public:
  /**
   * @brief Per-dimension statistics for quantization
   */
  struct Stats {
    std::vector<float> min_vals;  ///< Per-dimension minimum values
    std::vector<float> max_vals;  ///< Per-dimension maximum values

    uint32_t Dimension() const { return static_cast<uint32_t>(min_vals.size()); }
  };

  /**
   * @brief Compute per-dimension min/max statistics from a vector matrix
   * @param vectors Contiguous [count x dim] float matrix
   * @param count Number of vectors
   * @param dim Vector dimension
   * @return Stats with per-dimension min/max values
   */
  static Stats ComputeStats(const float* vectors, uint32_t count, uint32_t dim);

  /**
   * @brief Quantize a single float32 vector to uint8
   * @param input Float32 vector (dim elements)
   * @param output Uint8 output buffer (dim elements, must be pre-allocated)
   * @param dim Vector dimension
   * @param stats Quantization statistics
   */
  static void Quantize(const float* input, uint8_t* output, uint32_t dim, const Stats& stats);

  /**
   * @brief Dequantize a uint8 vector back to float32 (approximate)
   * @param input Uint8 vector (dim elements)
   * @param output Float32 output buffer (dim elements, must be pre-allocated)
   * @param dim Vector dimension
   * @param stats Quantization statistics
   */
  static void Dequantize(const uint8_t* input, float* output, uint32_t dim, const Stats& stats);

  /**
   * @brief Batch quantize a matrix of vectors
   * @param input Contiguous [count x dim] float matrix
   * @param output Contiguous [count x dim] uint8 matrix (pre-allocated)
   * @param count Number of vectors
   * @param dim Vector dimension
   * @param stats Quantization statistics
   */
  static void QuantizeBatch(const float* input, uint8_t* output, uint32_t count, uint32_t dim, const Stats& stats);

  /**
   * @brief Approximate dot product between two quantized vectors
   *
   * Computes an approximate dot product using integer arithmetic,
   * then scales back to float. Faster than dequantize + float dot product.
   *
   * @param a Quantized vector (dim uint8 elements)
   * @param b Quantized vector (dim uint8 elements)
   * @param dim Vector dimension
   * @param stats Quantization statistics (for scale/offset recovery)
   * @return Approximate dot product value
   */
  static float QuantizedDotProduct(const uint8_t* a, const uint8_t* b, uint32_t dim, const Stats& stats);

  /**
   * @brief Approximate cosine similarity between a float32 query and quantized vector
   *
   * Dequantizes the stored vector on-the-fly and computes cosine similarity.
   * This is used in the asymmetric quantization pattern (ADC):
   * query is kept in float32, database vectors are quantized.
   *
   * @param query Float32 query vector
   * @param quantized Quantized database vector
   * @param dim Vector dimension
   * @param stats Quantization statistics
   * @return Approximate cosine similarity
   */
  static float AsymmetricCosine(const float* query, const uint8_t* quantized, uint32_t dim, const Stats& stats);
};

}  // namespace nvecd::vectors
