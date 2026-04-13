/**
 * @file id_mapper.cpp
 * @brief Bidirectional ID mapping implementation
 */

#include "vectors/id_mapper.h"

namespace nvecd::vectors {

uint32_t IdMapper::GetOrCreate(const std::string& external_id) {
  auto it = external_to_slot_.find(external_id);
  if (it != external_to_slot_.end()) {
    return it->second;
  }

  uint32_t slot;
  if (!free_slots_.empty()) {
    // Reuse a freed slot
    slot = free_slots_.back();
    free_slots_.pop_back();
    slot_to_external_[slot] = external_id;
    slot_active_[slot] = true;
  } else {
    // Allocate a new slot
    slot = static_cast<uint32_t>(slot_to_external_.size());
    slot_to_external_.push_back(external_id);
    slot_active_.push_back(true);
  }

  external_to_slot_[external_id] = slot;
  ++active_count_;
  return slot;
}

std::optional<uint32_t> IdMapper::Get(const std::string& external_id) const {
  auto it = external_to_slot_.find(external_id);
  if (it == external_to_slot_.end()) {
    return std::nullopt;
  }
  return it->second;
}

const std::string& IdMapper::GetExternal(uint32_t slot) const {
  return slot_to_external_[slot];
}

bool IdMapper::Delete(const std::string& external_id) {
  auto it = external_to_slot_.find(external_id);
  if (it == external_to_slot_.end()) {
    return false;
  }

  uint32_t slot = it->second;
  external_to_slot_.erase(it);
  slot_active_[slot] = false;
  slot_to_external_[slot].clear();
  free_slots_.push_back(slot);
  --active_count_;
  return true;
}

bool IdMapper::DeleteBySlot(uint32_t slot) {
  if (slot >= slot_to_external_.size() || !slot_active_[slot]) {
    return false;
  }

  external_to_slot_.erase(slot_to_external_[slot]);
  slot_active_[slot] = false;
  slot_to_external_[slot].clear();
  free_slots_.push_back(slot);
  --active_count_;
  return true;
}

bool IdMapper::Has(const std::string& external_id) const {
  return external_to_slot_.count(external_id) > 0;
}

bool IdMapper::IsActive(uint32_t slot) const {
  return slot < slot_active_.size() && slot_active_[slot];
}

void IdMapper::Clear() {
  external_to_slot_.clear();
  slot_to_external_.clear();
  slot_active_.clear();
  free_slots_.clear();
  active_count_ = 0;
}

}  // namespace nvecd::vectors
