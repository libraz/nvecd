/**
 * @file ring_buffer_test.cpp
 * @brief Unit tests for RingBuffer
 */

#include "events/ring_buffer.h"

#include <gtest/gtest.h>

#include <string>

namespace nvecd::events {
namespace {

// ============================================================================
// Basic Operations
// ============================================================================

TEST(RingBufferTest, ConstructEmpty) {
  RingBuffer<int> buf(5);
  EXPECT_EQ(buf.Size(), 0);
  EXPECT_EQ(buf.Capacity(), 5);
  EXPECT_TRUE(buf.GetAll().empty());
}

TEST(RingBufferTest, PushSingleElement) {
  RingBuffer<int> buf(5);
  buf.Push(42);

  EXPECT_EQ(buf.Size(), 1);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 1);
  EXPECT_EQ(all[0], 42);
}

TEST(RingBufferTest, PushMultipleElements) {
  RingBuffer<int> buf(5);
  buf.Push(1);
  buf.Push(2);
  buf.Push(3);

  EXPECT_EQ(buf.Size(), 3);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0], 1);
  EXPECT_EQ(all[1], 2);
  EXPECT_EQ(all[2], 3);
}

TEST(RingBufferTest, PushToCapacity) {
  RingBuffer<int> buf(3);
  buf.Push(1);
  buf.Push(2);
  buf.Push(3);

  EXPECT_EQ(buf.Size(), 3);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0], 1);
  EXPECT_EQ(all[1], 2);
  EXPECT_EQ(all[2], 3);
}

// ============================================================================
// Overwrite Behavior
// ============================================================================

TEST(RingBufferTest, PushAndOverwrite) {
  RingBuffer<int> buf(3);
  buf.Push(1);
  buf.Push(2);
  buf.Push(3);
  buf.Push(4);  // Should overwrite oldest (1)

  EXPECT_EQ(buf.Size(), 3);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0], 2);
  EXPECT_EQ(all[1], 3);
  EXPECT_EQ(all[2], 4);
}

TEST(RingBufferTest, MultipleOverwrites) {
  RingBuffer<int> buf(3);
  for (int i = 1; i <= 10; ++i) {
    buf.Push(i);
  }

  EXPECT_EQ(buf.Size(), 3);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0], 8);
  EXPECT_EQ(all[1], 9);
  EXPECT_EQ(all[2], 10);
}

TEST(RingBufferTest, CompleteOverwriteCycle) {
  RingBuffer<int> buf(3);
  // Fill buffer
  buf.Push(1);
  buf.Push(2);
  buf.Push(3);
  // Overwrite exactly one cycle (3 more elements)
  buf.Push(4);
  buf.Push(5);
  buf.Push(6);

  EXPECT_EQ(buf.Size(), 3);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0], 4);
  EXPECT_EQ(all[1], 5);
  EXPECT_EQ(all[2], 6);
}

// ============================================================================
// Clear Operation
// ============================================================================

TEST(RingBufferTest, ClearEmptyBuffer) {
  RingBuffer<int> buf(5);
  buf.Clear();
  EXPECT_EQ(buf.Size(), 0);
  EXPECT_TRUE(buf.GetAll().empty());
}

TEST(RingBufferTest, ClearPartialBuffer) {
  RingBuffer<int> buf(5);
  buf.Push(1);
  buf.Push(2);
  buf.Clear();

  EXPECT_EQ(buf.Size(), 0);
  EXPECT_EQ(buf.Capacity(), 5);
  EXPECT_TRUE(buf.GetAll().empty());

  // Can reuse after clear
  buf.Push(10);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 1);
  EXPECT_EQ(all[0], 10);
}

TEST(RingBufferTest, ClearFullBuffer) {
  RingBuffer<int> buf(3);
  buf.Push(1);
  buf.Push(2);
  buf.Push(3);
  buf.Clear();

  EXPECT_EQ(buf.Size(), 0);
  EXPECT_TRUE(buf.GetAll().empty());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(RingBufferTest, CapacityOne) {
  RingBuffer<int> buf(1);
  buf.Push(1);
  EXPECT_EQ(buf.Size(), 1);

  buf.Push(2);
  EXPECT_EQ(buf.Size(), 1);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 1);
  EXPECT_EQ(all[0], 2);
}

TEST(RingBufferTest, LargeCapacity) {
  const size_t capacity = 1000;
  RingBuffer<int> buf(capacity);

  for (size_t i = 0; i < capacity; ++i) {
    buf.Push(static_cast<int>(i));
  }

  EXPECT_EQ(buf.Size(), capacity);
  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), capacity);
  for (size_t i = 0; i < capacity; ++i) {
    EXPECT_EQ(all[i], static_cast<int>(i));
  }
}

// ============================================================================
// Non-POD Types
// ============================================================================

TEST(RingBufferTest, StringType) {
  RingBuffer<std::string> buf(3);
  buf.Push("hello");
  buf.Push("world");
  buf.Push("foo");

  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0], "hello");
  EXPECT_EQ(all[1], "world");
  EXPECT_EQ(all[2], "foo");

  buf.Push("bar");  // Overwrite "hello"
  all = buf.GetAll();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0], "world");
  EXPECT_EQ(all[1], "foo");
  EXPECT_EQ(all[2], "bar");
}

struct TestStruct {
  int id;
  std::string name;

  bool operator==(const TestStruct& other) const { return id == other.id && name == other.name; }
};

TEST(RingBufferTest, StructType) {
  RingBuffer<TestStruct> buf(2);
  buf.Push({1, "alice"});
  buf.Push({2, "bob"});

  auto all = buf.GetAll();
  ASSERT_EQ(all.size(), 2);
  EXPECT_EQ(all[0].id, 1);
  EXPECT_EQ(all[0].name, "alice");
  EXPECT_EQ(all[1].id, 2);
  EXPECT_EQ(all[1].name, "bob");

  buf.Push({3, "charlie"});  // Overwrite {1, "alice"}
  all = buf.GetAll();
  ASSERT_EQ(all.size(), 2);
  EXPECT_EQ(all[0].id, 2);
  EXPECT_EQ(all[1].id, 3);
}

}  // namespace
}  // namespace nvecd::events
