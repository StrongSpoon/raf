/*!
 * Copyright (c) 2020 by Contributors
 * \file src/op/op_utils.h
 * \brief Useful classes of storing op metadata.
 */
#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <limits>
#include "./op.h"
#include "./value.h"

namespace mnm {
namespace op {

#define MNM_APPEND_BYTES(type, nbytes, value)                               \
  {                                                                         \
    constexpr int NUM_BYTES = nbytes;                                       \
    union UNION {                                                           \
      type v;                                                               \
      struct {                                                              \
        uint8_t bytes[NUM_BYTES];                                           \
      };                                                                    \
    } u;                                                                    \
    u.v = value;                                                            \
    static_assert(sizeof(UNION) == sizeof(uint8_t) * NUM_BYTES, "invalid"); \
    static_assert(sizeof(UNION) == sizeof(type), "invalid");                \
    for (int i = 0; i < NUM_BYTES; ++i) {                                   \
      byte_vector.push_back(u.bytes[i]);                                    \
    }                                                                       \
  }

#define MNM_DEF_PRIMITIVE(type_code, type, nbytes)                              \
  inline HashKey& operator<<(const type& v) {                                   \
    static_assert(0 <= type_code, "invalid");                                   \
    static_assert(type_code <= std::numeric_limits<uint8_t>::max(), "invalid"); \
    MNM_APPEND_BYTES(type, nbytes, v);                                          \
    return *this;                                                               \
  }

class HashKey {
 public:
  MNM_DEF_PRIMITIVE(0, bool, 1);
  MNM_DEF_PRIMITIVE(1, int8_t, 1);
  MNM_DEF_PRIMITIVE(2, int16_t, 2);
  MNM_DEF_PRIMITIVE(3, int32_t, 4);
  MNM_DEF_PRIMITIVE(4, int64_t, 8);
  MNM_DEF_PRIMITIVE(5, uint8_t, 1);
  MNM_DEF_PRIMITIVE(6, uint16_t, 2);
  MNM_DEF_PRIMITIVE(7, uint32_t, 4);
  MNM_DEF_PRIMITIVE(8, uint64_t, 8);
  MNM_DEF_PRIMITIVE(9, float, 4);
  MNM_DEF_PRIMITIVE(10, double, 8);
  MNM_DEF_PRIMITIVE(11, DLDataType, 4);
  MNM_DEF_PRIMITIVE(12, DLContext, 8);

  inline HashKey& operator<<(const std::vector<int64_t>& v) {
    byte_vector.push_back(13);
    for (int i = 0, n = v.size(); i < n; ++i) {
      MNM_APPEND_BYTES(int64_t, 8, v[i]);
    }
    MNM_APPEND_BYTES(int64_t, 8, 0);
    return *this;
  }

  inline HashKey& operator<<(const ir::TensorType& v) {
    byte_vector.push_back(14);
    MNM_APPEND_BYTES(DLDataType, 4, v->dtype);
    for (int i = 0, n = v->shape.size(); i < n; ++i) {
      int64_t dim_i;
      if (v->shape.as<ir::AnyNode>()) {
        dim_i = -1;
      } else {
        dim_i = ir::Downcast<ir::Integer>(v->shape[i]);
      }
      MNM_APPEND_BYTES(int64_t, 8, dim_i);
    }
    MNM_APPEND_BYTES(int64_t, 8, 0);
    return *this;
  }

  inline HashKey& operator<<(const DLTensor& v) {
    // N.B.: stride and ctx are not taken into consideration
    byte_vector.push_back(15);
    MNM_APPEND_BYTES(DLDataType, 4, v.dtype);
    for (int i = 0, n = v.ndim; i < n; ++i) {
      MNM_APPEND_BYTES(int64_t, 8, v.shape[i]);
    }
    MNM_APPEND_BYTES(int64_t, 8, 0);
    return *this;
  }

  inline HashKey& operator<<(const value::TensorValue& v) {
    DLTensor* t = v;
    return operator<<(*t);
  }

  HashKey() {
    byte_vector.reserve(1024);
  }

  std::vector<uint8_t> byte_vector;
};

#undef MNM_DEF_PRIMITIVE
#undef MNM_APPEND_BYTES

template <typename T>
class MetaCache {
 public:
  std::unordered_map<std::string, T> cached_;
  std::mutex mu;

  ~MetaCache() = default;

  bool Has(const std::vector<uint8_t>& key) {
    const std::string s(key.begin(), key.end());
    return cached_.count(s);
  }

  const T* Get(const std::vector<uint8_t>& key) {
    const std::string s(key.begin(), key.end());
    auto iter = cached_.find(s);
    if (iter == cached_.end()) {
      return nullptr;
    }
    return &iter->second;
  }

  void Set(const std::vector<uint8_t>& key, T val) {
    const std::string s(key.begin(), key.end());
    auto iter = cached_.find(s);
    if (iter != cached_.end()) {
      LOG(FATAL) << "KeyError: The key is already cached!";
      throw;
    }
    cached_.emplace(s, val);
  }
};

template <int n>
static std::vector<int64_t> Pad(const std::vector<int64_t>& a) {
  int size = a.size();
  CHECK(size == 1 || size == n);
  return size == 1 ? std::vector<int64_t>(n, a[0]) : a;
}

}  // namespace op
}  // namespace mnm