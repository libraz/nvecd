/**
 * @file id_mapper_test.cpp
 * @brief Tests for IdMapper
 */

#include "vectors/id_mapper.h"

#include <gtest/gtest.h>

#include <set>

namespace nvecd::vectors {
namespace {

TEST(IdMapperTest, EmptyMapper) {
  IdMapper mapper;
  EXPECT_EQ(mapper.Size(), 0U);
  EXPECT_EQ(mapper.TotalSlots(), 0U);
  EXPECT_EQ(mapper.FreeSlots(), 0U);
  EXPECT_FALSE(mapper.Has("anything"));
  EXPECT_EQ(mapper.Get("anything"), std::nullopt);
}

TEST(IdMapperTest, GetOrCreateNewId) {
  IdMapper mapper;
  uint32_t slot = mapper.GetOrCreate("item1");
  EXPECT_EQ(slot, 0U);
  EXPECT_EQ(mapper.Size(), 1U);
  EXPECT_TRUE(mapper.Has("item1"));
  EXPECT_EQ(mapper.GetExternal(slot), "item1");
}

TEST(IdMapperTest, GetOrCreateExistingId) {
  IdMapper mapper;
  uint32_t slot1 = mapper.GetOrCreate("item1");
  uint32_t slot2 = mapper.GetOrCreate("item1");
  EXPECT_EQ(slot1, slot2);
  EXPECT_EQ(mapper.Size(), 1U);
}

TEST(IdMapperTest, MultipleIds) {
  IdMapper mapper;
  uint32_t s0 = mapper.GetOrCreate("a");
  uint32_t s1 = mapper.GetOrCreate("b");
  uint32_t s2 = mapper.GetOrCreate("c");
  EXPECT_EQ(s0, 0U);
  EXPECT_EQ(s1, 1U);
  EXPECT_EQ(s2, 2U);
  EXPECT_EQ(mapper.Size(), 3U);
  EXPECT_EQ(mapper.TotalSlots(), 3U);
}

TEST(IdMapperTest, Get) {
  IdMapper mapper;
  mapper.GetOrCreate("item1");
  EXPECT_EQ(mapper.Get("item1"), 0U);
  EXPECT_EQ(mapper.Get("nonexistent"), std::nullopt);
}

TEST(IdMapperTest, DeleteByExternalId) {
  IdMapper mapper;
  mapper.GetOrCreate("item1");
  EXPECT_TRUE(mapper.Delete("item1"));
  EXPECT_FALSE(mapper.Has("item1"));
  EXPECT_EQ(mapper.Size(), 0U);
  EXPECT_EQ(mapper.FreeSlots(), 1U);
}

TEST(IdMapperTest, DeleteNonExistent) {
  IdMapper mapper;
  EXPECT_FALSE(mapper.Delete("nonexistent"));
}

TEST(IdMapperTest, DeleteBySlot) {
  IdMapper mapper;
  uint32_t slot = mapper.GetOrCreate("item1");
  EXPECT_TRUE(mapper.DeleteBySlot(slot));
  EXPECT_FALSE(mapper.Has("item1"));
  EXPECT_EQ(mapper.Size(), 0U);
}

TEST(IdMapperTest, DeleteBySlotInvalid) {
  IdMapper mapper;
  EXPECT_FALSE(mapper.DeleteBySlot(0));
  EXPECT_FALSE(mapper.DeleteBySlot(999));
}

TEST(IdMapperTest, DeleteBySlotAlreadyFreed) {
  IdMapper mapper;
  uint32_t slot = mapper.GetOrCreate("item1");
  mapper.DeleteBySlot(slot);
  EXPECT_FALSE(mapper.DeleteBySlot(slot));
}

TEST(IdMapperTest, SlotReuse) {
  IdMapper mapper;
  uint32_t s0 = mapper.GetOrCreate("a");
  mapper.GetOrCreate("b");
  mapper.GetOrCreate("c");

  // Delete slot 0
  mapper.Delete("a");
  EXPECT_EQ(mapper.FreeSlots(), 1U);

  // New ID should reuse slot 0
  uint32_t s_new = mapper.GetOrCreate("d");
  EXPECT_EQ(s_new, s0);
  EXPECT_EQ(mapper.GetExternal(s_new), "d");
  EXPECT_EQ(mapper.FreeSlots(), 0U);
  EXPECT_EQ(mapper.Size(), 3U);
}

TEST(IdMapperTest, MultipleSlotReuse) {
  IdMapper mapper;
  mapper.GetOrCreate("a");
  mapper.GetOrCreate("b");
  mapper.GetOrCreate("c");

  mapper.Delete("a");
  mapper.Delete("c");
  EXPECT_EQ(mapper.FreeSlots(), 2U);

  // Reuse happens LIFO from free_slots_
  uint32_t s1 = mapper.GetOrCreate("x");
  uint32_t s2 = mapper.GetOrCreate("y");

  // Both reused slots should be valid
  std::set<uint32_t> reused = {s1, s2};
  EXPECT_TRUE(reused.count(0) > 0 || reused.count(2) > 0);
  EXPECT_EQ(mapper.FreeSlots(), 0U);
  EXPECT_EQ(mapper.Size(), 3U);
}

TEST(IdMapperTest, IsActive) {
  IdMapper mapper;
  uint32_t slot = mapper.GetOrCreate("item1");
  EXPECT_TRUE(mapper.IsActive(slot));

  mapper.Delete("item1");
  EXPECT_FALSE(mapper.IsActive(slot));

  // Out-of-range slot
  EXPECT_FALSE(mapper.IsActive(999));
}

TEST(IdMapperTest, Clear) {
  IdMapper mapper;
  mapper.GetOrCreate("a");
  mapper.GetOrCreate("b");
  mapper.Delete("a");

  mapper.Clear();
  EXPECT_EQ(mapper.Size(), 0U);
  EXPECT_EQ(mapper.TotalSlots(), 0U);
  EXPECT_EQ(mapper.FreeSlots(), 0U);
  EXPECT_FALSE(mapper.Has("b"));
}

TEST(IdMapperTest, AddAfterClear) {
  IdMapper mapper;
  mapper.GetOrCreate("a");
  mapper.Clear();

  uint32_t slot = mapper.GetOrCreate("b");
  EXPECT_EQ(slot, 0U);
  EXPECT_EQ(mapper.Size(), 1U);
  EXPECT_EQ(mapper.GetExternal(slot), "b");
}

TEST(IdMapperTest, LargeScale) {
  IdMapper mapper;
  constexpr uint32_t kCount = 10000;

  for (uint32_t i = 0; i < kCount; ++i) {
    uint32_t slot = mapper.GetOrCreate("item_" + std::to_string(i));
    EXPECT_EQ(slot, i);
  }
  EXPECT_EQ(mapper.Size(), kCount);

  // Delete every other
  for (uint32_t i = 0; i < kCount; i += 2) {
    mapper.Delete("item_" + std::to_string(i));
  }
  EXPECT_EQ(mapper.Size(), kCount / 2);
  EXPECT_EQ(mapper.FreeSlots(), kCount / 2);

  // Re-add: should reuse freed slots
  for (uint32_t i = 0; i < kCount / 2; ++i) {
    uint32_t slot = mapper.GetOrCreate("new_" + std::to_string(i));
    EXPECT_LT(slot, kCount);  // Reused, not new
  }
  EXPECT_EQ(mapper.Size(), kCount);
  EXPECT_EQ(mapper.FreeSlots(), 0U);
  EXPECT_EQ(mapper.TotalSlots(), kCount);  // No growth
}

}  // namespace
}  // namespace nvecd::vectors
