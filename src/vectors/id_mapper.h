/**
 * @file id_mapper.h
 * @brief Bidirectional mapping between external string IDs and internal uint32_t slots
 *
 * IdMapper replaces expensive unordered_map<string, size_t> with a compact
 * slot-based scheme. Deleted slots are recycled via a free list, reducing
 * the need for full defragmentation.
 *
 * Thread-safety: NOT thread-safe. Caller must provide external synchronization.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nvecd::vectors {

/**
 * @brief Bidirectional ID mapping with slot reuse
 */
class IdMapper {
 public:
  static constexpr uint32_t kInvalidSlot = UINT32_MAX;

  IdMapper() = default;

  /**
   * @brief Get existing slot or create a new one for the given external ID
   * @param external_id External string identifier
   * @return Internal slot index (reuses freed slots when available)
   */
  uint32_t GetOrCreate(const std::string& external_id);

  /**
   * @brief Look up the internal slot for an external ID
   * @param external_id External string identifier
   * @return Slot index, or nullopt if not mapped
   */
  std::optional<uint32_t> Get(const std::string& external_id) const;

  /**
   * @brief Get the external ID for an internal slot
   * @param slot Internal slot index
   * @return External string ID (undefined behavior if slot is invalid or freed)
   */
  const std::string& GetExternal(uint32_t slot) const;

  /**
   * @brief Delete a mapping by external ID and free the slot
   * @param external_id External string identifier
   * @return True if deleted, false if not found
   */
  bool Delete(const std::string& external_id);

  /**
   * @brief Delete a mapping by slot and free it
   * @param slot Internal slot index
   * @return True if deleted, false if slot is invalid or already freed
   */
  bool DeleteBySlot(uint32_t slot);

  /**
   * @brief Check if an external ID is mapped
   */
  bool Has(const std::string& external_id) const;

  /**
   * @brief Check if a slot is active (mapped, not freed)
   */
  bool IsActive(uint32_t slot) const;

  /**
   * @brief Number of active mappings
   */
  size_t Size() const { return active_count_; }

  /**
   * @brief Total slots allocated (including freed)
   */
  size_t TotalSlots() const { return slot_to_external_.size(); }

  /**
   * @brief Number of free slots available for reuse
   */
  size_t FreeSlots() const { return free_slots_.size(); }

  /**
   * @brief Clear all mappings
   */
  void Clear();

 private:
  std::unordered_map<std::string, uint32_t> external_to_slot_;
  std::vector<std::string> slot_to_external_;
  std::vector<bool> slot_active_;
  std::vector<uint32_t> free_slots_;
  size_t active_count_ = 0;
};

}  // namespace nvecd::vectors
