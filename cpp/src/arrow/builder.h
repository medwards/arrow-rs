// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef ARROW_BUILDER_H
#define ARROW_BUILDER_H

#include <algorithm>  // IWYU pragma: keep
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "arrow/buffer.h"
#include "arrow/memory_pool.h"
#include "arrow/status.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/bit-util.h"
#include "arrow/util/macros.h"
#include "arrow/util/string_view.h"
#include "arrow/util/type_traits.h"
#include "arrow/util/visibility.h"

namespace arrow {

class Array;
struct ArrayData;
class Decimal128;

constexpr int64_t kBinaryMemoryLimit = std::numeric_limits<int32_t>::max() - 1;
constexpr int64_t kListMaximumElements = std::numeric_limits<int32_t>::max() - 1;

constexpr int64_t kMinBuilderCapacity = 1 << 5;

/// Base class for all data array builders.
///
/// This class provides a facilities for incrementally building the null bitmap
/// (see Append methods) and as a side effect the current number of slots and
/// the null count.
///
/// \note Users are expected to use builders as one of the concrete types below.
/// For example, ArrayBuilder* pointing to BinaryBuilder should be downcast before use.
class ARROW_EXPORT ArrayBuilder {
 public:
  explicit ArrayBuilder(const std::shared_ptr<DataType>& type, MemoryPool* pool)
      : type_(type),
        pool_(pool),
        null_bitmap_(NULLPTR),
        null_count_(0),
        null_bitmap_data_(NULLPTR),
        length_(0),
        capacity_(0) {}

  virtual ~ArrayBuilder() = default;

  /// For nested types. Since the objects are owned by this class instance, we
  /// skip shared pointers and just return a raw pointer
  ArrayBuilder* child(int i) { return children_[i].get(); }

  int num_children() const { return static_cast<int>(children_.size()); }

  int64_t length() const { return length_; }
  int64_t null_count() const { return null_count_; }
  int64_t capacity() const { return capacity_; }

  /// \brief Ensure that enough memory has been allocated to fit the indicated
  /// number of total elements in the builder, including any that have already
  /// been appended. Does not account for reallocations that may be due to
  /// variable size data, like binary values. To make space for incremental
  /// appends, use Reserve instead.
  ///
  /// \param[in] capacity the minimum number of total array values to
  ///            accommodate. Must be greater than the current capacity.
  /// \return Status
  virtual Status Resize(int64_t capacity);

  /// \brief Ensure that there is enough space allocated to add the indicated
  /// number of elements without any further calls to Resize. The memory
  /// allocated is rounded up to the next highest power of 2 similar to memory
  /// allocations in STL containers like std::vector
  /// \param[in] additional_capacity the number of additional array values
  /// \return Status
  Status Reserve(int64_t additional_capacity);

  /// Reset the builder.
  virtual void Reset();

  /// For cases where raw data was memcpy'd into the internal buffers, allows us
  /// to advance the length of the builder. It is your responsibility to use
  /// this function responsibly.
  Status Advance(int64_t elements);

  /// \brief Return result of builder as an internal generic ArrayData
  /// object. Resets builder except for dictionary builder
  ///
  /// \param[out] out the finalized ArrayData object
  /// \return Status
  virtual Status FinishInternal(std::shared_ptr<ArrayData>* out) = 0;

  /// \brief Return result of builder as an Array object.
  ///        Resets the builder except for DictionaryBuilder
  ///
  /// \param[out] out the finalized Array object
  /// \return Status
  Status Finish(std::shared_ptr<Array>* out);

  std::shared_ptr<DataType> type() const { return type_; }

 protected:
  ArrayBuilder() {}

  /// Append to null bitmap
  Status AppendToBitmap(bool is_valid);

  /// Vector append. Treat each zero byte as a null.   If valid_bytes is null
  /// assume all of length bits are valid.
  Status AppendToBitmap(const uint8_t* valid_bytes, int64_t length);

  /// Set the next length bits to not null (i.e. valid).
  Status SetNotNull(int64_t length);

  // Unsafe operations (don't check capacity/don't resize)

  void UnsafeAppendNull() { UnsafeAppendToBitmap(false); }

  // Append to null bitmap, update the length
  void UnsafeAppendToBitmap(bool is_valid) {
    if (is_valid) {
      BitUtil::SetBit(null_bitmap_data_, length_);
    } else {
      ++null_count_;
    }
    ++length_;
  }

  template <typename IterType>
  void UnsafeAppendToBitmap(const IterType& begin, const IterType& end) {
    int64_t byte_offset = length_ / 8;
    int64_t bit_offset = length_ % 8;
    uint8_t bitset = null_bitmap_data_[byte_offset];

    for (auto iter = begin; iter != end; ++iter) {
      if (bit_offset == 8) {
        bit_offset = 0;
        null_bitmap_data_[byte_offset] = bitset;
        byte_offset++;
        // TODO: Except for the last byte, this shouldn't be needed
        bitset = null_bitmap_data_[byte_offset];
      }

      if (*iter) {
        bitset |= BitUtil::kBitmask[bit_offset];
      } else {
        bitset &= BitUtil::kFlippedBitmask[bit_offset];
        ++null_count_;
      }

      bit_offset++;
    }

    if (bit_offset != 0) {
      null_bitmap_data_[byte_offset] = bitset;
    }

    length_ += std::distance(begin, end);
  }

  // Vector append. Treat each zero byte as a nullzero. If valid_bytes is null
  // assume all of length bits are valid.
  void UnsafeAppendToBitmap(const uint8_t* valid_bytes, int64_t length);

  void UnsafeAppendToBitmap(const std::vector<bool>& is_valid);

  // Set the next length bits to not null (i.e. valid).
  void UnsafeSetNotNull(int64_t length);

  static Status TrimBuffer(const int64_t bytes_filled, ResizableBuffer* buffer);

  static Status CheckCapacity(int64_t new_capacity, int64_t old_capacity) {
    if (new_capacity < 0) {
      return Status::Invalid("Resize capacity must be positive");
    }
    if (new_capacity < old_capacity) {
      return Status::Invalid("Resize cannot downsize");
    }
    return Status::OK();
  }

  std::shared_ptr<DataType> type_;
  MemoryPool* pool_;

  // When null_bitmap are first appended to the builder, the null bitmap is allocated
  std::shared_ptr<ResizableBuffer> null_bitmap_;
  int64_t null_count_;
  uint8_t* null_bitmap_data_;

  // Array length, so far. Also, the index of the next element to be added
  int64_t length_;
  int64_t capacity_;

  // Child value array builders. These are owned by this class
  std::vector<std::unique_ptr<ArrayBuilder>> children_;

 private:
  ARROW_DISALLOW_COPY_AND_ASSIGN(ArrayBuilder);
};

class ARROW_EXPORT NullBuilder : public ArrayBuilder {
 public:
  explicit NullBuilder(MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT)
      : ArrayBuilder(null(), pool) {}

  Status AppendNull() {
    ++null_count_;
    ++length_;
    return Status::OK();
  }

  Status Append(std::nullptr_t value) { return AppendNull(); }

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;
};

template <typename Type>
class ARROW_EXPORT PrimitiveBuilder : public ArrayBuilder {
 public:
  using value_type = typename Type::c_type;

  explicit PrimitiveBuilder(const std::shared_ptr<DataType>& type, MemoryPool* pool)
      : ArrayBuilder(type, pool), data_(NULLPTR), raw_data_(NULLPTR) {}

  using ArrayBuilder::Advance;

  /// Write nulls as uint8_t* (0 value indicates null) into pre-allocated memory
  /// The memory at the corresponding data slot is set to 0 to prevent uninitialized
  /// memory access
  Status AppendNulls(const uint8_t* valid_bytes, int64_t length) {
    ARROW_RETURN_NOT_OK(Reserve(length));
    memset(raw_data_ + length_, 0,
           static_cast<size_t>(TypeTraits<Type>::bytes_required(length)));
    UnsafeAppendToBitmap(valid_bytes, length);
    return Status::OK();
  }

  /// \brief Append a single null element
  Status AppendNull() {
    ARROW_RETURN_NOT_OK(Reserve(1));
    memset(raw_data_ + length_, 0, sizeof(value_type));
    UnsafeAppendToBitmap(false);
    return Status::OK();
  }

  value_type GetValue(int64_t index) const {
    return reinterpret_cast<const value_type*>(data_->data())[index];
  }

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a contiguous C array of values
  /// \param[in] length the number of values to append
  /// \param[in] valid_bytes an optional sequence of bytes where non-zero
  /// indicates a valid (non-null) value
  /// \return Status
  Status AppendValues(const value_type* values, int64_t length,
                      const uint8_t* valid_bytes = NULLPTR);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a contiguous C array of values
  /// \param[in] length the number of values to append
  /// \param[in] is_valid an std::vector<bool> indicating valid (1) or null
  /// (0). Equal in length to values
  /// \return Status
  Status AppendValues(const value_type* values, int64_t length,
                      const std::vector<bool>& is_valid);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a std::vector of values
  /// \param[in] is_valid an std::vector<bool> indicating valid (1) or null
  /// (0). Equal in length to values
  /// \return Status
  Status AppendValues(const std::vector<value_type>& values,
                      const std::vector<bool>& is_valid);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a std::vector of values
  /// \return Status
  Status AppendValues(const std::vector<value_type>& values);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values_begin InputIterator to the beginning of the values
  /// \param[in] values_end InputIterator pointing to the end of the values
  /// \return Status

  template <typename ValuesIter>
  Status AppendValues(ValuesIter values_begin, ValuesIter values_end) {
    int64_t length = static_cast<int64_t>(std::distance(values_begin, values_end));
    ARROW_RETURN_NOT_OK(Reserve(length));

    std::copy(values_begin, values_end, raw_data_ + length_);

    // this updates the length_
    UnsafeSetNotNull(length);
    return Status::OK();
  }

  /// \brief Append a sequence of elements in one shot, with a specified nullmap
  /// \param[in] values_begin InputIterator to the beginning of the values
  /// \param[in] values_end InputIterator pointing to the end of the values
  /// \param[in] valid_begin InputIterator with elements indication valid(1)
  ///  or null(0) values.
  /// \return Status
  template <typename ValuesIter, typename ValidIter>
  typename std::enable_if<!std::is_pointer<ValidIter>::value, Status>::type AppendValues(
      ValuesIter values_begin, ValuesIter values_end, ValidIter valid_begin) {
    static_assert(!internal::is_null_pointer<ValidIter>::value,
                  "Don't pass a NULLPTR directly as valid_begin, use the 2-argument "
                  "version instead");
    int64_t length = static_cast<int64_t>(std::distance(values_begin, values_end));
    ARROW_RETURN_NOT_OK(Reserve(length));

    std::copy(values_begin, values_end, raw_data_ + length_);

    // this updates the length_
    UnsafeAppendToBitmap(valid_begin, std::next(valid_begin, length));
    return Status::OK();
  }

  // Same as above, with a pointer type ValidIter
  template <typename ValuesIter, typename ValidIter>
  typename std::enable_if<std::is_pointer<ValidIter>::value, Status>::type AppendValues(
      ValuesIter values_begin, ValuesIter values_end, ValidIter valid_begin) {
    int64_t length = static_cast<int64_t>(std::distance(values_begin, values_end));
    ARROW_RETURN_NOT_OK(Reserve(length));

    std::copy(values_begin, values_end, raw_data_ + length_);

    // this updates the length_
    if (valid_begin == NULLPTR) {
      UnsafeSetNotNull(length);
    } else {
      UnsafeAppendToBitmap(valid_begin, std::next(valid_begin, length));
    }

    return Status::OK();
  }

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;
  void Reset() override;

  Status Resize(int64_t capacity) override;

 protected:
  std::shared_ptr<ResizableBuffer> data_;
  value_type* raw_data_;
};

/// Base class for all Builders that emit an Array of a scalar numerical type.
template <typename T>
class ARROW_EXPORT NumericBuilder : public PrimitiveBuilder<T> {
 public:
  using typename PrimitiveBuilder<T>::value_type;
  using PrimitiveBuilder<T>::PrimitiveBuilder;

  template <typename T1 = T>
  explicit NumericBuilder(
      typename std::enable_if<TypeTraits<T1>::is_parameter_free, MemoryPool*>::type pool
          ARROW_MEMORY_POOL_DEFAULT)
      : PrimitiveBuilder<T1>(TypeTraits<T1>::type_singleton(), pool) {}

  using ArrayBuilder::UnsafeAppendNull;
  using PrimitiveBuilder<T>::AppendValues;
  using PrimitiveBuilder<T>::Resize;
  using PrimitiveBuilder<T>::Reserve;

  /// Append a single scalar and increase the size if necessary.
  Status Append(const value_type val) {
    ARROW_RETURN_NOT_OK(ArrayBuilder::Reserve(1));
    UnsafeAppend(val);
    return Status::OK();
  }

  /// Append a single scalar under the assumption that the underlying Buffer is
  /// large enough.
  ///
  /// This method does not capacity-check; make sure to call Reserve
  /// beforehand.
  void UnsafeAppend(const value_type val) {
    BitUtil::SetBit(null_bitmap_data_, length_);
    raw_data_[length_++] = val;
  }

 protected:
  using PrimitiveBuilder<T>::length_;
  using PrimitiveBuilder<T>::null_bitmap_data_;
  using PrimitiveBuilder<T>::raw_data_;
};

// Builders

using UInt8Builder = NumericBuilder<UInt8Type>;
using UInt16Builder = NumericBuilder<UInt16Type>;
using UInt32Builder = NumericBuilder<UInt32Type>;
using UInt64Builder = NumericBuilder<UInt64Type>;

using Int8Builder = NumericBuilder<Int8Type>;
using Int16Builder = NumericBuilder<Int16Type>;
using Int32Builder = NumericBuilder<Int32Type>;
using Int64Builder = NumericBuilder<Int64Type>;
using TimestampBuilder = NumericBuilder<TimestampType>;
using Time32Builder = NumericBuilder<Time32Type>;
using Time64Builder = NumericBuilder<Time64Type>;
using Date32Builder = NumericBuilder<Date32Type>;
using Date64Builder = NumericBuilder<Date64Type>;

using HalfFloatBuilder = NumericBuilder<HalfFloatType>;
using FloatBuilder = NumericBuilder<FloatType>;
using DoubleBuilder = NumericBuilder<DoubleType>;

namespace internal {

class ARROW_EXPORT AdaptiveIntBuilderBase : public ArrayBuilder {
 public:
  explicit AdaptiveIntBuilderBase(MemoryPool* pool);

  /// Write nulls as uint8_t* (0 value indicates null) into pre-allocated memory
  Status AppendNulls(const uint8_t* valid_bytes, int64_t length) {
    ARROW_RETURN_NOT_OK(CommitPendingData());
    ARROW_RETURN_NOT_OK(Reserve(length));
    memset(data_->mutable_data() + length_ * int_size_, 0, int_size_ * length);
    UnsafeAppendToBitmap(valid_bytes, length);
    return Status::OK();
  }

  Status AppendNull() {
    pending_data_[pending_pos_] = 0;
    pending_valid_[pending_pos_] = 0;
    pending_has_nulls_ = true;
    ++pending_pos_;

    if (ARROW_PREDICT_FALSE(pending_pos_ >= pending_size_)) {
      return CommitPendingData();
    }
    return Status::OK();
  }

  void Reset() override;
  Status Resize(int64_t capacity) override;

 protected:
  virtual Status CommitPendingData() = 0;

  std::shared_ptr<ResizableBuffer> data_;
  uint8_t* raw_data_;
  uint8_t int_size_;

  static constexpr int32_t pending_size_ = 1024;
  uint8_t pending_valid_[pending_size_];
  uint64_t pending_data_[pending_size_];
  int32_t pending_pos_;
  bool pending_has_nulls_;
};

}  // namespace internal

class ARROW_EXPORT AdaptiveUIntBuilder : public internal::AdaptiveIntBuilderBase {
 public:
  explicit AdaptiveUIntBuilder(MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT);

  using ArrayBuilder::Advance;
  using internal::AdaptiveIntBuilderBase::Reset;

  /// Scalar append
  Status Append(const uint64_t val) {
    pending_data_[pending_pos_] = val;
    pending_valid_[pending_pos_] = 1;
    ++pending_pos_;

    if (ARROW_PREDICT_FALSE(pending_pos_ >= pending_size_)) {
      return CommitPendingData();
    }
    return Status::OK();
  }

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a contiguous C array of values
  /// \param[in] length the number of values to append
  /// \param[in] valid_bytes an optional sequence of bytes where non-zero
  /// indicates a valid (non-null) value
  /// \return Status
  Status AppendValues(const uint64_t* values, int64_t length,
                      const uint8_t* valid_bytes = NULLPTR);

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

 protected:
  Status CommitPendingData() override;
  Status ExpandIntSize(uint8_t new_int_size);

  Status AppendValuesInternal(const uint64_t* values, int64_t length,
                              const uint8_t* valid_bytes);

  template <typename new_type, typename old_type>
  typename std::enable_if<sizeof(old_type) >= sizeof(new_type), Status>::type
  ExpandIntSizeInternal();
#define __LESS(a, b) (a) < (b)
  template <typename new_type, typename old_type>
  typename std::enable_if<__LESS(sizeof(old_type), sizeof(new_type)), Status>::type
  ExpandIntSizeInternal();
#undef __LESS

  template <typename new_type>
  Status ExpandIntSizeN();
};

class ARROW_EXPORT AdaptiveIntBuilder : public internal::AdaptiveIntBuilderBase {
 public:
  explicit AdaptiveIntBuilder(MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT);

  using ArrayBuilder::Advance;
  using internal::AdaptiveIntBuilderBase::Reset;

  /// Scalar append
  Status Append(const int64_t val) {
    auto v = static_cast<uint64_t>(val);

    pending_data_[pending_pos_] = v;
    pending_valid_[pending_pos_] = 1;
    ++pending_pos_;

    if (ARROW_PREDICT_FALSE(pending_pos_ >= pending_size_)) {
      return CommitPendingData();
    }
    return Status::OK();
  }

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a contiguous C array of values
  /// \param[in] length the number of values to append
  /// \param[in] valid_bytes an optional sequence of bytes where non-zero
  /// indicates a valid (non-null) value
  /// \return Status
  Status AppendValues(const int64_t* values, int64_t length,
                      const uint8_t* valid_bytes = NULLPTR);

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

 protected:
  Status CommitPendingData() override;
  Status ExpandIntSize(uint8_t new_int_size);

  Status AppendValuesInternal(const int64_t* values, int64_t length,
                              const uint8_t* valid_bytes);

  template <typename new_type, typename old_type>
  typename std::enable_if<sizeof(old_type) >= sizeof(new_type), Status>::type
  ExpandIntSizeInternal();
#define __LESS(a, b) (a) < (b)
  template <typename new_type, typename old_type>
  typename std::enable_if<__LESS(sizeof(old_type), sizeof(new_type)), Status>::type
  ExpandIntSizeInternal();
#undef __LESS

  template <typename new_type>
  Status ExpandIntSizeN();
};

class ARROW_EXPORT BooleanBuilder : public ArrayBuilder {
 public:
  using value_type = bool;
  explicit BooleanBuilder(MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT);

  explicit BooleanBuilder(const std::shared_ptr<DataType>& type, MemoryPool* pool);

  using ArrayBuilder::Advance;
  using ArrayBuilder::UnsafeAppendNull;

  /// Write nulls as uint8_t* (0 value indicates null) into pre-allocated memory
  Status AppendNulls(const uint8_t* valid_bytes, int64_t length) {
    ARROW_RETURN_NOT_OK(Reserve(length));
    UnsafeAppendToBitmap(valid_bytes, length);

    return Status::OK();
  }

  Status AppendNull() {
    ARROW_RETURN_NOT_OK(Reserve(1));
    UnsafeAppendToBitmap(false);

    return Status::OK();
  }

  /// Scalar append
  Status Append(const bool val) {
    ARROW_RETURN_NOT_OK(Reserve(1));
    UnsafeAppend(val);
    return Status::OK();
  }

  Status Append(const uint8_t val) { return Append(val != 0); }

  /// Scalar append, without checking for capacity
  void UnsafeAppend(const bool val) {
    BitUtil::SetBit(null_bitmap_data_, length_);
    if (val) {
      BitUtil::SetBit(raw_data_, length_);
    } else {
      BitUtil::ClearBit(raw_data_, length_);
    }
    ++length_;
  }

  void UnsafeAppend(const uint8_t val) { UnsafeAppend(val != 0); }

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a contiguous array of bytes (non-zero is 1)
  /// \param[in] length the number of values to append
  /// \param[in] valid_bytes an optional sequence of bytes where non-zero
  /// indicates a valid (non-null) value
  /// \return Status
  Status AppendValues(const uint8_t* values, int64_t length,
                      const uint8_t* valid_bytes = NULLPTR);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a contiguous C array of values
  /// \param[in] length the number of values to append
  /// \param[in] is_valid an std::vector<bool> indicating valid (1) or null
  /// (0). Equal in length to values
  /// \return Status
  Status AppendValues(const uint8_t* values, int64_t length,
                      const std::vector<bool>& is_valid);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a std::vector of bytes
  /// \param[in] is_valid an std::vector<bool> indicating valid (1) or null
  /// (0). Equal in length to values
  /// \return Status
  Status AppendValues(const std::vector<uint8_t>& values,
                      const std::vector<bool>& is_valid);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values a std::vector of bytes
  /// \return Status
  Status AppendValues(const std::vector<uint8_t>& values);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values an std::vector<bool> indicating true (1) or false
  /// \param[in] is_valid an std::vector<bool> indicating valid (1) or null
  /// (0). Equal in length to values
  /// \return Status
  Status AppendValues(const std::vector<bool>& values, const std::vector<bool>& is_valid);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values an std::vector<bool> indicating true (1) or false
  /// \return Status
  Status AppendValues(const std::vector<bool>& values);

  /// \brief Append a sequence of elements in one shot
  /// \param[in] values_begin InputIterator to the beginning of the values
  /// \param[in] values_end InputIterator pointing to the end of the values
  ///  or null(0) values
  /// \return Status
  template <typename ValuesIter>
  Status AppendValues(ValuesIter values_begin, ValuesIter values_end) {
    int64_t length = static_cast<int64_t>(std::distance(values_begin, values_end));
    ARROW_RETURN_NOT_OK(Reserve(length));
    auto iter = values_begin;
    internal::GenerateBitsUnrolled(raw_data_, length_, length,
                                   [&iter]() -> bool { return *(iter++); });

    // this updates length_
    UnsafeSetNotNull(length);
    return Status::OK();
  }

  /// \brief Append a sequence of elements in one shot, with a specified nullmap
  /// \param[in] values_begin InputIterator to the beginning of the values
  /// \param[in] values_end InputIterator pointing to the end of the values
  /// \param[in] valid_begin InputIterator with elements indication valid(1)
  ///  or null(0) values
  /// \return Status
  template <typename ValuesIter, typename ValidIter>
  typename std::enable_if<!std::is_pointer<ValidIter>::value, Status>::type AppendValues(
      ValuesIter values_begin, ValuesIter values_end, ValidIter valid_begin) {
    static_assert(!internal::is_null_pointer<ValidIter>::value,
                  "Don't pass a NULLPTR directly as valid_begin, use the 2-argument "
                  "version instead");
    int64_t length = static_cast<int64_t>(std::distance(values_begin, values_end));
    ARROW_RETURN_NOT_OK(Reserve(length));

    auto iter = values_begin;
    internal::GenerateBitsUnrolled(raw_data_, length_, length,
                                   [&iter]() -> bool { return *(iter++); });

    // this updates length_
    ArrayBuilder::UnsafeAppendToBitmap(valid_begin, std::next(valid_begin, length));
    return Status::OK();
  }

  // Same as above, for a pointer type ValidIter
  template <typename ValuesIter, typename ValidIter>
  typename std::enable_if<std::is_pointer<ValidIter>::value, Status>::type AppendValues(
      ValuesIter values_begin, ValuesIter values_end, ValidIter valid_begin) {
    int64_t length = static_cast<int64_t>(std::distance(values_begin, values_end));
    ARROW_RETURN_NOT_OK(Reserve(length));

    auto iter = values_begin;
    internal::GenerateBitsUnrolled(raw_data_, length_, length,
                                   [&iter]() -> bool { return *(iter++); });

    // this updates the length_
    if (valid_begin == NULLPTR) {
      UnsafeSetNotNull(length);
    } else {
      UnsafeAppendToBitmap(valid_begin, std::next(valid_begin, length));
    }

    return Status::OK();
  }

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;
  void Reset() override;
  Status Resize(int64_t capacity) override;

 protected:
  std::shared_ptr<ResizableBuffer> data_;
  uint8_t* raw_data_;
};

// ----------------------------------------------------------------------
// List builder

/// \class ListBuilder
/// \brief Builder class for variable-length list array value types
///
/// To use this class, you must append values to the child array builder and use
/// the Append function to delimit each distinct list value (once the values
/// have been appended to the child array) or use the bulk API to append
/// a sequence of offests and null values.
///
/// A note on types.  Per arrow/type.h all types in the c++ implementation are
/// logical so even though this class always builds list array, this can
/// represent multiple different logical types.  If no logical type is provided
/// at construction time, the class defaults to List<T> where t is taken from the
/// value_builder/values that the object is constructed with.
class ARROW_EXPORT ListBuilder : public ArrayBuilder {
 public:
  /// Use this constructor to incrementally build the value array along with offsets and
  /// null bitmap.
  ListBuilder(MemoryPool* pool, std::shared_ptr<ArrayBuilder> const& value_builder,
              const std::shared_ptr<DataType>& type = NULLPTR);

  Status Resize(int64_t capacity) override;
  void Reset() override;
  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

  /// \brief Vector append
  ///
  /// If passed, valid_bytes is of equal length to values, and any zero byte
  /// will be considered as a null for that slot
  Status AppendValues(const int32_t* offsets, int64_t length,
                      const uint8_t* valid_bytes = NULLPTR);

  /// \brief Start a new variable-length list slot
  ///
  /// This function should be called before beginning to append elements to the
  /// value builder
  Status Append(bool is_valid = true);

  Status AppendNull() { return Append(false); }

  ArrayBuilder* value_builder() const;

 protected:
  TypedBufferBuilder<int32_t> offsets_builder_;
  std::shared_ptr<ArrayBuilder> value_builder_;
  std::shared_ptr<Array> values_;

  Status AppendNextOffset();
};

// ----------------------------------------------------------------------
// Binary and String

/// \class BinaryBuilder
/// \brief Builder class for variable-length binary data
class ARROW_EXPORT BinaryBuilder : public ArrayBuilder {
 public:
  explicit BinaryBuilder(MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT);

  BinaryBuilder(const std::shared_ptr<DataType>& type, MemoryPool* pool);

  Status Append(const uint8_t* value, int32_t length);

  Status Append(const char* value, int32_t length) {
    return Append(reinterpret_cast<const uint8_t*>(value), length);
  }

  Status Append(util::string_view value) {
    return Append(value.data(), static_cast<int32_t>(value.size()));
  }

  Status AppendNull();

  /// \brief Append without checking capacity
  ///
  /// Offsets and data should have been presized using Reserve() and
  /// ReserveData(), respectively.
  void UnsafeAppend(const uint8_t* value, int32_t length) {
    UnsafeAppendNextOffset();
    value_data_builder_.UnsafeAppend(value, length);
    UnsafeAppendToBitmap(true);
  }

  void UnsafeAppend(const char* value, int32_t length) {
    UnsafeAppend(reinterpret_cast<const uint8_t*>(value), length);
  }

  void UnsafeAppend(const std::string& value) {
    UnsafeAppend(value.c_str(), static_cast<int32_t>(value.size()));
  }

  void UnsafeAppendNull() {
    const int64_t num_bytes = value_data_builder_.length();
    offsets_builder_.UnsafeAppend(static_cast<int32_t>(num_bytes));
    UnsafeAppendToBitmap(false);
  }

  void Reset() override;
  Status Resize(int64_t capacity) override;

  /// \brief Ensures there is enough allocated capacity to append the indicated
  /// number of bytes to the value data buffer without additional allocations
  Status ReserveData(int64_t elements);

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

  /// \return size of values buffer so far
  int64_t value_data_length() const { return value_data_builder_.length(); }
  /// \return capacity of values buffer
  int64_t value_data_capacity() const { return value_data_builder_.capacity(); }

  /// Temporary access to a value.
  ///
  /// This pointer becomes invalid on the next modifying operation.
  const uint8_t* GetValue(int64_t i, int32_t* out_length) const;

  /// Temporary access to a value.
  ///
  /// This view becomes invalid on the next modifying operation.
  util::string_view GetView(int64_t i) const;

 protected:
  TypedBufferBuilder<int32_t> offsets_builder_;
  TypedBufferBuilder<uint8_t> value_data_builder_;

  Status AppendNextOffset();

  void UnsafeAppendNextOffset() {
    const int64_t num_bytes = value_data_builder_.length();
    offsets_builder_.UnsafeAppend(static_cast<int32_t>(num_bytes));
  }
};

/// \class StringBuilder
/// \brief Builder class for UTF8 strings
class ARROW_EXPORT StringBuilder : public BinaryBuilder {
 public:
  using BinaryBuilder::BinaryBuilder;
  explicit StringBuilder(MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT);

  using BinaryBuilder::Append;
  using BinaryBuilder::Reset;
  using BinaryBuilder::UnsafeAppend;

  /// \brief Append a sequence of strings in one shot.
  ///
  /// \param[in] values a vector of strings
  /// \param[in] valid_bytes an optional sequence of bytes where non-zero
  /// indicates a valid (non-null) value
  /// \return Status
  Status AppendValues(const std::vector<std::string>& values,
                      const uint8_t* valid_bytes = NULLPTR);

  /// \brief Append a sequence of nul-terminated strings in one shot.
  ///        If one of the values is NULL, it is processed as a null
  ///        value even if the corresponding valid_bytes entry is 1.
  ///
  /// \param[in] values a contiguous C array of nul-terminated char *
  /// \param[in] length the number of values to append
  /// \param[in] valid_bytes an optional sequence of bytes where non-zero
  /// indicates a valid (non-null) value
  /// \return Status
  Status AppendValues(const char** values, int64_t length,
                      const uint8_t* valid_bytes = NULLPTR);
};

// ----------------------------------------------------------------------
// FixedSizeBinaryBuilder

class ARROW_EXPORT FixedSizeBinaryBuilder : public ArrayBuilder {
 public:
  FixedSizeBinaryBuilder(const std::shared_ptr<DataType>& type,
                         MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT);

  Status Append(const uint8_t* value) {
    ARROW_RETURN_NOT_OK(Reserve(1));
    UnsafeAppendToBitmap(true);
    return byte_builder_.Append(value, byte_width_);
  }

  Status Append(const char* value) {
    return Append(reinterpret_cast<const uint8_t*>(value));
  }

  Status Append(const util::string_view& view) {
#ifndef NDEBUG
    CheckValueSize(static_cast<int64_t>(view.size()));
#endif
    return Append(reinterpret_cast<const uint8_t*>(view.data()));
  }

  Status Append(const std::string& s) {
#ifndef NDEBUG
    CheckValueSize(static_cast<int64_t>(s.size()));
#endif
    return Append(reinterpret_cast<const uint8_t*>(s.data()));
  }

  template <size_t NBYTES>
  Status Append(const std::array<uint8_t, NBYTES>& value) {
    ARROW_RETURN_NOT_OK(Reserve(1));
    UnsafeAppendToBitmap(true);
    return byte_builder_.Append(value);
  }

  Status AppendValues(const uint8_t* data, int64_t length,
                      const uint8_t* valid_bytes = NULLPTR);
  Status AppendNull();

  void Reset() override;
  Status Resize(int64_t capacity) override;
  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

  /// \return size of values buffer so far
  int64_t value_data_length() const { return byte_builder_.length(); }

  int32_t byte_width() const { return byte_width_; }

  /// Temporary access to a value.
  ///
  /// This pointer becomes invalid on the next modifying operation.
  const uint8_t* GetValue(int64_t i) const;

  /// Temporary access to a value.
  ///
  /// This view becomes invalid on the next modifying operation.
  util::string_view GetView(int64_t i) const;

 protected:
  int32_t byte_width_;
  BufferBuilder byte_builder_;

#ifndef NDEBUG
  void CheckValueSize(int64_t size);
#endif
};

class ARROW_EXPORT Decimal128Builder : public FixedSizeBinaryBuilder {
 public:
  explicit Decimal128Builder(const std::shared_ptr<DataType>& type,
                             MemoryPool* pool ARROW_MEMORY_POOL_DEFAULT);

  using FixedSizeBinaryBuilder::Append;
  using FixedSizeBinaryBuilder::AppendValues;
  using FixedSizeBinaryBuilder::Reset;

  Status Append(const Decimal128& val);

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;
};

using DecimalBuilder = Decimal128Builder;

// ----------------------------------------------------------------------
// Struct

// ---------------------------------------------------------------------------------
// StructArray builder
/// Append, Resize and Reserve methods are acting on StructBuilder.
/// Please make sure all these methods of all child-builders' are consistently
/// called to maintain data-structure consistency.
class ARROW_EXPORT StructBuilder : public ArrayBuilder {
 public:
  StructBuilder(const std::shared_ptr<DataType>& type, MemoryPool* pool,
                std::vector<std::shared_ptr<ArrayBuilder>>&& field_builders);

  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

  /// Null bitmap is of equal length to every child field, and any zero byte
  /// will be considered as a null for that field, but users must using app-
  /// end methods or advance methods of the child builders' independently to
  /// insert data.
  Status AppendValues(int64_t length, const uint8_t* valid_bytes) {
    ARROW_RETURN_NOT_OK(Reserve(length));
    UnsafeAppendToBitmap(valid_bytes, length);
    return Status::OK();
  }

  /// Append an element to the Struct. All child-builders' Append method must
  /// be called independently to maintain data-structure consistency.
  Status Append(bool is_valid = true) {
    ARROW_RETURN_NOT_OK(Reserve(1));
    UnsafeAppendToBitmap(is_valid);
    return Status::OK();
  }

  Status AppendNull() { return Append(false); }

  void Reset() override;

  ArrayBuilder* field_builder(int i) const { return field_builders_[i].get(); }

  int num_fields() const { return static_cast<int>(field_builders_.size()); }

 protected:
  std::vector<std::shared_ptr<ArrayBuilder>> field_builders_;
};

// ----------------------------------------------------------------------
// Dictionary builder

namespace internal {

template <typename T>
struct DictionaryScalar {
  using type = typename T::c_type;
};

template <>
struct DictionaryScalar<BinaryType> {
  using type = util::string_view;
};

template <>
struct DictionaryScalar<StringType> {
  using type = util::string_view;
};

template <>
struct DictionaryScalar<FixedSizeBinaryType> {
  using type = util::string_view;
};

}  // namespace internal

/// \brief Array builder for created encoded DictionaryArray from dense array
///
/// Unlike other builders, dictionary builder does not completely reset the state
/// on Finish calls. The arrays built after the initial Finish call will reuse
/// the previously created encoding and build a delta dictionary when new terms
/// occur.
///
/// data
template <typename T>
class ARROW_EXPORT DictionaryBuilder : public ArrayBuilder {
 public:
  using Scalar = typename internal::DictionaryScalar<T>::type;

  // WARNING: the type given below is the value type, not the DictionaryType.
  // The DictionaryType is instantiated on the Finish() call.
  DictionaryBuilder(const std::shared_ptr<DataType>& type, MemoryPool* pool);

  template <typename T1 = T>
  explicit DictionaryBuilder(
      typename std::enable_if<TypeTraits<T1>::is_parameter_free, MemoryPool*>::type pool)
      : DictionaryBuilder<T1>(TypeTraits<T1>::type_singleton(), pool) {}

  ~DictionaryBuilder() override;

  /// \brief Append a scalar value
  Status Append(const Scalar& value);

  /// \brief Append a fixed-width string (only for FixedSizeBinaryType)
  template <typename T1 = T>
  Status Append(typename std::enable_if<std::is_base_of<FixedSizeBinaryType, T1>::value,
                                        const uint8_t*>::type value) {
    return Append(util::string_view(reinterpret_cast<const char*>(value), byte_width_));
  }

  /// \brief Append a fixed-width string (only for FixedSizeBinaryType)
  template <typename T1 = T>
  Status Append(typename std::enable_if<std::is_base_of<FixedSizeBinaryType, T1>::value,
                                        const char*>::type value) {
    return Append(util::string_view(value, byte_width_));
  }

  /// \brief Append a scalar null value
  Status AppendNull();

  /// \brief Append a whole dense array to the builder
  Status AppendArray(const Array& array);

  void Reset() override;
  Status Resize(int64_t capacity) override;
  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

  /// is the dictionary builder in the delta building mode
  bool is_building_delta() { return delta_offset_ > 0; }

 protected:
  class MemoTableImpl;
  std::unique_ptr<MemoTableImpl> memo_table_;

  int32_t delta_offset_;
  // Only used for FixedSizeBinaryType
  int32_t byte_width_;

  AdaptiveIntBuilder values_builder_;
};

template <>
class ARROW_EXPORT DictionaryBuilder<NullType> : public ArrayBuilder {
 public:
  DictionaryBuilder(const std::shared_ptr<DataType>& type, MemoryPool* pool);
  explicit DictionaryBuilder(MemoryPool* pool);

  /// \brief Append a scalar null value
  Status AppendNull();

  /// \brief Append a whole dense array to the builder
  Status AppendArray(const Array& array);

  Status Resize(int64_t capacity) override;
  Status FinishInternal(std::shared_ptr<ArrayData>* out) override;

 protected:
  AdaptiveIntBuilder values_builder_;
};

class ARROW_EXPORT BinaryDictionaryBuilder : public DictionaryBuilder<BinaryType> {
 public:
  using DictionaryBuilder::Append;
  using DictionaryBuilder::DictionaryBuilder;

  Status Append(const uint8_t* value, int32_t length) {
    return Append(reinterpret_cast<const char*>(value), length);
  }

  Status Append(const char* value, int32_t length) {
    return Append(util::string_view(value, length));
  }
};

/// \brief Dictionary array builder with convenience methods for strings
class ARROW_EXPORT StringDictionaryBuilder : public DictionaryBuilder<StringType> {
 public:
  using DictionaryBuilder::Append;
  using DictionaryBuilder::DictionaryBuilder;

  Status Append(const uint8_t* value, int32_t length) {
    return Append(reinterpret_cast<const char*>(value), length);
  }

  Status Append(const char* value, int32_t length) {
    return Append(util::string_view(value, length));
  }
};

// ----------------------------------------------------------------------
// Helper functions

ARROW_EXPORT
Status MakeBuilder(MemoryPool* pool, const std::shared_ptr<DataType>& type,
                   std::unique_ptr<ArrayBuilder>* out);

}  // namespace arrow

#endif  // ARROW_BUILDER_H_
