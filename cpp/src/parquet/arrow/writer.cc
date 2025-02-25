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

#include "parquet/arrow/writer.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "arrow/array.h"
#include "arrow/buffer-builder.h"
#include "arrow/compute/api.h"
#include "arrow/table.h"
#include "arrow/util/checked_cast.h"
#include "arrow/visitor_inline.h"

#include "arrow/util/logging.h"

#include "parquet/arrow/reader.h"
#include "parquet/arrow/schema.h"
#include "parquet/column_writer.h"
#include "parquet/exception.h"
#include "parquet/file_writer.h"
#include "parquet/platform.h"
#include "parquet/schema.h"

using arrow::Array;
using arrow::BinaryArray;
using arrow::BooleanArray;
using arrow::ChunkedArray;
using arrow::Decimal128Array;
using arrow::Field;
using arrow::FixedSizeBinaryArray;
using Int16BufferBuilder = arrow::TypedBufferBuilder<int16_t>;
using arrow::ListArray;
using arrow::MemoryPool;
using arrow::NumericArray;
using arrow::PrimitiveArray;
using arrow::ResizableBuffer;
using arrow::Status;
using arrow::Table;
using arrow::TimeUnit;

using arrow::compute::Cast;
using arrow::compute::CastOptions;
using arrow::compute::FunctionContext;

using parquet::ParquetFileWriter;
using parquet::ParquetVersion;
using parquet::schema::GroupNode;

namespace parquet {
namespace arrow {

std::shared_ptr<ArrowWriterProperties> default_arrow_writer_properties() {
  static std::shared_ptr<ArrowWriterProperties> default_writer_properties =
      ArrowWriterProperties::Builder().build();
  return default_writer_properties;
}

namespace {

class LevelBuilder {
 public:
  explicit LevelBuilder(MemoryPool* pool) : def_levels_(pool), rep_levels_(pool) {}

  Status VisitInline(const Array& array);

  template <typename T>
  typename std::enable_if<std::is_base_of<::arrow::FlatArray, T>::value, Status>::type
  Visit(const T& array) {
    array_offsets_.push_back(static_cast<int32_t>(array.offset()));
    valid_bitmaps_.push_back(array.null_bitmap_data());
    null_counts_.push_back(array.null_count());
    values_array_ = std::make_shared<T>(array.data());
    return Status::OK();
  }

  Status Visit(const ListArray& array) {
    array_offsets_.push_back(static_cast<int32_t>(array.offset()));
    valid_bitmaps_.push_back(array.null_bitmap_data());
    null_counts_.push_back(array.null_count());
    offsets_.push_back(array.raw_value_offsets());

    // Min offset isn't always zero in the case of sliced Arrays.
    min_offset_idx_ = array.value_offset(min_offset_idx_);
    max_offset_idx_ = array.value_offset(max_offset_idx_);

    return VisitInline(*array.values());
  }

#define NOT_IMPLEMENTED_VISIT(ArrowTypePrefix)                             \
  Status Visit(const ::arrow::ArrowTypePrefix##Array& array) {             \
    return Status::NotImplemented("Level generation for " #ArrowTypePrefix \
                                  " not supported yet");                   \
  }

  NOT_IMPLEMENTED_VISIT(Map)
  NOT_IMPLEMENTED_VISIT(FixedSizeList)
  NOT_IMPLEMENTED_VISIT(Struct)
  NOT_IMPLEMENTED_VISIT(Union)
  NOT_IMPLEMENTED_VISIT(Dictionary)
  NOT_IMPLEMENTED_VISIT(Extension)

  Status GenerateLevels(const Array& array, const std::shared_ptr<Field>& field,
                        int64_t* values_offset, int64_t* num_values, int64_t* num_levels,
                        const std::shared_ptr<ResizableBuffer>& def_levels_scratch,
                        std::shared_ptr<Buffer>* def_levels_out,
                        std::shared_ptr<Buffer>* rep_levels_out,
                        std::shared_ptr<Array>* values_array) {
    // Work downwards to extract bitmaps and offsets
    min_offset_idx_ = 0;
    max_offset_idx_ = array.length();
    RETURN_NOT_OK(VisitInline(array));
    *num_values = max_offset_idx_ - min_offset_idx_;
    *values_offset = min_offset_idx_;
    *values_array = values_array_;

    // Walk downwards to extract nullability
    std::shared_ptr<Field> current_field = field;
    nullable_.push_back(current_field->nullable());
    while (current_field->type()->num_children() > 0) {
      if (current_field->type()->num_children() > 1) {
        return Status::NotImplemented(
            "Fields with more than one child are not supported.");
      } else {
        current_field = current_field->type()->child(0);
      }
      nullable_.push_back(current_field->nullable());
    }

    // Generate the levels.
    if (nullable_.size() == 1) {
      // We have a PrimitiveArray
      *rep_levels_out = nullptr;
      if (nullable_[0]) {
        RETURN_NOT_OK(
            def_levels_scratch->Resize(array.length() * sizeof(int16_t), false));
        auto def_levels_ptr =
            reinterpret_cast<int16_t*>(def_levels_scratch->mutable_data());
        if (array.null_count() == 0) {
          std::fill(def_levels_ptr, def_levels_ptr + array.length(), 1);
        } else if (array.null_count() == array.length()) {
          std::fill(def_levels_ptr, def_levels_ptr + array.length(), 0);
        } else {
          ::arrow::internal::BitmapReader valid_bits_reader(
              array.null_bitmap_data(), array.offset(), array.length());
          for (int i = 0; i < array.length(); i++) {
            def_levels_ptr[i] = valid_bits_reader.IsSet() ? 1 : 0;
            valid_bits_reader.Next();
          }
        }

        *def_levels_out = def_levels_scratch;
      } else {
        *def_levels_out = nullptr;
      }
      *num_levels = array.length();
    } else {
      // Note it is hard to estimate memory consumption due to zero length
      // arrays otherwise we would preallocate.  An upper boun on memory
      // is the sum of the length of each list array  + number of elements
      // but this might be too loose of an upper bound so we choose to use
      // safe methods.
      RETURN_NOT_OK(rep_levels_.Append(0));
      RETURN_NOT_OK(HandleListEntries(0, 0, 0, array.length()));

      RETURN_NOT_OK(def_levels_.Finish(def_levels_out));
      RETURN_NOT_OK(rep_levels_.Finish(rep_levels_out));
      *num_levels = (*rep_levels_out)->size() / sizeof(int16_t);
    }

    return Status::OK();
  }

  Status HandleList(int16_t def_level, int16_t rep_level, int64_t index) {
    if (nullable_[rep_level]) {
      if (null_counts_[rep_level] == 0 ||
          BitUtil::GetBit(valid_bitmaps_[rep_level], index + array_offsets_[rep_level])) {
        return HandleNonNullList(static_cast<int16_t>(def_level + 1), rep_level, index);
      } else {
        return def_levels_.Append(def_level);
      }
    } else {
      return HandleNonNullList(def_level, rep_level, index);
    }
  }

  Status HandleNonNullList(int16_t def_level, int16_t rep_level, int64_t index) {
    const int32_t inner_offset = offsets_[rep_level][index];
    const int32_t inner_length = offsets_[rep_level][index + 1] - inner_offset;
    const int64_t recursion_level = rep_level + 1;
    if (inner_length == 0) {
      return def_levels_.Append(def_level);
    }
    if (recursion_level < static_cast<int64_t>(offsets_.size())) {
      return HandleListEntries(static_cast<int16_t>(def_level + 1),
                               static_cast<int16_t>(rep_level + 1), inner_offset,
                               inner_length);
    }
    // We have reached the leaf: primitive list, handle remaining nullables
    const bool nullable_level = nullable_[recursion_level];
    const int64_t level_null_count = null_counts_[recursion_level];
    const uint8_t* level_valid_bitmap = valid_bitmaps_[recursion_level];

    if (inner_length >= 1) {
      RETURN_NOT_OK(
          rep_levels_.Append(inner_length - 1, static_cast<int16_t>(rep_level + 1)));
    }

    // Special case: this is a null array (all elements are null)
    if (level_null_count && level_valid_bitmap == nullptr) {
      return def_levels_.Append(inner_length, static_cast<int16_t>(def_level + 1));
    }
    for (int64_t i = 0; i < inner_length; i++) {
      if (nullable_level &&
          ((level_null_count == 0) ||
           BitUtil::GetBit(level_valid_bitmap,
                           inner_offset + i + array_offsets_[recursion_level]))) {
        // Non-null element in a null level
        RETURN_NOT_OK(def_levels_.Append(static_cast<int16_t>(def_level + 2)));
      } else {
        // This can be produced in two cases:
        //  * elements are nullable and this one is null
        //   (i.e. max_def_level = def_level + 2)
        //  * elements are non-nullable (i.e. max_def_level = def_level + 1)
        RETURN_NOT_OK(def_levels_.Append(static_cast<int16_t>(def_level + 1)));
      }
    }
    return Status::OK();
  }

  Status HandleListEntries(int16_t def_level, int16_t rep_level, int64_t offset,
                           int64_t length) {
    for (int64_t i = 0; i < length; i++) {
      if (i > 0) {
        RETURN_NOT_OK(rep_levels_.Append(rep_level));
      }
      RETURN_NOT_OK(HandleList(def_level, rep_level, offset + i));
    }
    return Status::OK();
  }

 private:
  Int16BufferBuilder def_levels_;
  Int16BufferBuilder rep_levels_;

  std::vector<int64_t> null_counts_;
  std::vector<const uint8_t*> valid_bitmaps_;
  std::vector<const int32_t*> offsets_;
  std::vector<int32_t> array_offsets_;
  std::vector<bool> nullable_;

  int64_t min_offset_idx_;
  int64_t max_offset_idx_;
  std::shared_ptr<Array> values_array_;
};

Status LevelBuilder::VisitInline(const Array& array) {
  return VisitArrayInline(array, this);
}

struct ColumnWriterContext {
  ColumnWriterContext(MemoryPool* memory_pool, ArrowWriterProperties* properties)
      : memory_pool(memory_pool), properties(properties) {
    this->data_buffer = AllocateBuffer(memory_pool);
    this->def_levels_buffer = AllocateBuffer(memory_pool);
  }

  template <typename T>
  Status GetScratchData(const int64_t num_values, T** out) {
    RETURN_NOT_OK(this->data_buffer->Resize(num_values * sizeof(T), false));
    *out = reinterpret_cast<T*>(this->data_buffer->mutable_data());
    return Status::OK();
  }

  MemoryPool* memory_pool;
  ArrowWriterProperties* properties;

  // Buffer used for storing the data of an array converted to the physical type
  // as expected by parquet-cpp.
  std::shared_ptr<ResizableBuffer> data_buffer;

  // We use the shared ownership of this buffer
  std::shared_ptr<ResizableBuffer> def_levels_buffer;
};

Status GetLeafType(const ::arrow::DataType& type, ::arrow::Type::type* leaf_type) {
  if (type.id() == ::arrow::Type::LIST || type.id() == ::arrow::Type::STRUCT) {
    if (type.num_children() != 1) {
      return Status::Invalid("Nested column branch had multiple children: ", type);
    }
    return GetLeafType(*type.child(0)->type(), leaf_type);
  } else {
    *leaf_type = type.id();
    return Status::OK();
  }
}

class ArrowColumnWriter {
 public:
  ArrowColumnWriter(ColumnWriterContext* ctx, ColumnWriter* column_writer,
                    const std::shared_ptr<Field>& field)
      : ctx_(ctx), writer_(column_writer), field_(field) {}

  Status Write(const Array& data);

  Status Write(const ChunkedArray& data, int64_t offset, const int64_t size) {
    if (data.length() == 0) {
      return Status::OK();
    }

    int64_t absolute_position = 0;
    int chunk_index = 0;
    int64_t chunk_offset = 0;
    while (chunk_index < data.num_chunks() && absolute_position < offset) {
      const int64_t chunk_length = data.chunk(chunk_index)->length();
      if (absolute_position + chunk_length > offset) {
        // Relative offset into the chunk to reach the desired start offset for
        // writing
        chunk_offset = offset - absolute_position;
        break;
      } else {
        ++chunk_index;
        absolute_position += chunk_length;
      }
    }

    if (absolute_position >= data.length()) {
      return Status::Invalid("Cannot write data at offset past end of chunked array");
    }

    int64_t values_written = 0;
    while (values_written < size) {
      const Array& chunk = *data.chunk(chunk_index);
      const int64_t available_values = chunk.length() - chunk_offset;
      const int64_t chunk_write_size = std::min(size - values_written, available_values);

      // The chunk offset here will be 0 except for possibly the first chunk
      // because of the advancing logic above
      std::shared_ptr<Array> array_to_write = chunk.Slice(chunk_offset, chunk_write_size);
      RETURN_NOT_OK(Write(*array_to_write));

      if (chunk_write_size == available_values) {
        chunk_offset = 0;
        ++chunk_index;
      }
      values_written += chunk_write_size;
    }

    return Status::OK();
  }

  Status Close() {
    PARQUET_CATCH_NOT_OK(writer_->Close());
    return Status::OK();
  }

 private:
  template <typename ParquetType, typename ArrowType>
  Status TypedWriteBatch(const Array& data, int64_t num_levels, const int16_t* def_levels,
                         const int16_t* rep_levels);

  Status WriteTimestamps(const Array& data, int64_t num_levels, const int16_t* def_levels,
                         const int16_t* rep_levels);

  Status WriteTimestampsCoerce(const Array& data, int64_t num_levels,
                               const int16_t* def_levels, const int16_t* rep_levels,
                               const ArrowWriterProperties& properties);

  template <typename ParquetType, typename ArrowType>
  Status WriteNonNullableBatch(const ArrowType& type, int64_t num_values,
                               int64_t num_levels, const int16_t* def_levels,
                               const int16_t* rep_levels,
                               const typename ArrowType::c_type* values);

  template <typename ParquetType, typename ArrowType>
  Status WriteNullableBatch(const ArrowType& type, int64_t num_values, int64_t num_levels,
                            const int16_t* def_levels, const int16_t* rep_levels,
                            const uint8_t* valid_bits, int64_t valid_bits_offset,
                            const typename ArrowType::c_type* values);

  template <typename ParquetType>
  Status WriteBatch(int64_t num_levels, const int16_t* def_levels,
                    const int16_t* rep_levels,
                    const typename ParquetType::c_type* values) {
    auto typed_writer =
        ::arrow::internal::checked_cast<TypedColumnWriter<ParquetType>*>(writer_);
    // WriteBatch was called with type mismatching the writer_'s type. This
    // could be a schema conversion problem.
    DCHECK(typed_writer);
    PARQUET_CATCH_NOT_OK(
        typed_writer->WriteBatch(num_levels, def_levels, rep_levels, values));
    return Status::OK();
  }

  template <typename ParquetType>
  Status WriteBatchSpaced(int64_t num_levels, const int16_t* def_levels,
                          const int16_t* rep_levels, const uint8_t* valid_bits,
                          int64_t valid_bits_offset,
                          const typename ParquetType::c_type* values) {
    auto typed_writer =
        ::arrow::internal::checked_cast<TypedColumnWriter<ParquetType>*>(writer_);
    // WriteBatchSpaced was called with type mismatching the writer_'s type. This
    // could be a schema conversion problem.
    DCHECK(typed_writer);
    PARQUET_CATCH_NOT_OK(typed_writer->WriteBatchSpaced(
        num_levels, def_levels, rep_levels, valid_bits, valid_bits_offset, values));
    return Status::OK();
  }

  ColumnWriterContext* ctx_;
  ColumnWriter* writer_;
  std::shared_ptr<Field> field_;
};

template <typename ParquetType, typename ArrowType>
Status ArrowColumnWriter::TypedWriteBatch(const Array& array, int64_t num_levels,
                                          const int16_t* def_levels,
                                          const int16_t* rep_levels) {
  using ArrowCType = typename ArrowType::c_type;

  const auto& data = static_cast<const PrimitiveArray&>(array);
  const ArrowCType* values = nullptr;
  // The values buffer may be null if the array is empty (ARROW-2744)
  if (data.values() != nullptr) {
    values = reinterpret_cast<const ArrowCType*>(data.values()->data()) + data.offset();
  } else {
    DCHECK_EQ(data.length(), 0);
  }

  if (writer_->descr()->schema_node()->is_required() || (data.null_count() == 0)) {
    // no nulls, just dump the data
    RETURN_NOT_OK((WriteNonNullableBatch<ParquetType, ArrowType>(
        static_cast<const ArrowType&>(*array.type()), array.length(), num_levels,
        def_levels, rep_levels, values)));
  } else {
    const uint8_t* valid_bits = data.null_bitmap_data();
    RETURN_NOT_OK((WriteNullableBatch<ParquetType, ArrowType>(
        static_cast<const ArrowType&>(*array.type()), data.length(), num_levels,
        def_levels, rep_levels, valid_bits, data.offset(), values)));
  }
  return Status::OK();
}

template <typename ParquetType, typename ArrowType>
Status ArrowColumnWriter::WriteNonNullableBatch(
    const ArrowType& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels,
    const typename ArrowType::c_type* values) {
  using ParquetCType = typename ParquetType::c_type;
  ParquetCType* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<ParquetCType>(num_values, &buffer));

  std::copy(values, values + num_values, buffer);

  return WriteBatch<ParquetType>(num_levels, def_levels, rep_levels, buffer);
}

template <>
Status ArrowColumnWriter::WriteNonNullableBatch<Int32Type, ::arrow::Date64Type>(
    const ::arrow::Date64Type& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels, const int64_t* values) {
  int32_t* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<int32_t>(num_levels, &buffer));

  for (int i = 0; i < num_values; i++) {
    buffer[i] = static_cast<int32_t>(values[i] / 86400000);
  }

  return WriteBatch<Int32Type>(num_levels, def_levels, rep_levels, buffer);
}

template <>
Status ArrowColumnWriter::WriteNonNullableBatch<Int32Type, ::arrow::Time32Type>(
    const ::arrow::Time32Type& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels, const int32_t* values) {
  int32_t* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<int32_t>(num_levels, &buffer));
  if (type.unit() == TimeUnit::SECOND) {
    for (int i = 0; i < num_values; i++) {
      buffer[i] = values[i] * 1000;
    }
  } else {
    std::copy(values, values + num_values, buffer);
  }
  return WriteBatch<Int32Type>(num_levels, def_levels, rep_levels, buffer);
}

#define NONNULLABLE_BATCH_FAST_PATH(ParquetType, ArrowType, CType)                 \
  template <>                                                                      \
  Status ArrowColumnWriter::WriteNonNullableBatch<ParquetType, ArrowType>(         \
      const ArrowType& type, int64_t num_values, int64_t num_levels,               \
      const int16_t* def_levels, const int16_t* rep_levels, const CType* buffer) { \
    return WriteBatch<ParquetType>(num_levels, def_levels, rep_levels, buffer);    \
  }

NONNULLABLE_BATCH_FAST_PATH(Int32Type, ::arrow::Int32Type, int32_t)
NONNULLABLE_BATCH_FAST_PATH(Int32Type, ::arrow::Date32Type, int32_t)
NONNULLABLE_BATCH_FAST_PATH(Int64Type, ::arrow::Int64Type, int64_t)
NONNULLABLE_BATCH_FAST_PATH(Int64Type, ::arrow::Time64Type, int64_t)
NONNULLABLE_BATCH_FAST_PATH(FloatType, ::arrow::FloatType, float)
NONNULLABLE_BATCH_FAST_PATH(DoubleType, ::arrow::DoubleType, double)

template <typename ParquetType, typename ArrowType>
Status ArrowColumnWriter::WriteNullableBatch(
    const ArrowType& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels, const uint8_t* valid_bits,
    int64_t valid_bits_offset, const typename ArrowType::c_type* values) {
  using ParquetCType = typename ParquetType::c_type;

  ParquetCType* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<ParquetCType>(num_values, &buffer));
  for (int i = 0; i < num_values; i++) {
    buffer[i] = static_cast<ParquetCType>(values[i]);
  }

  return WriteBatchSpaced<ParquetType>(num_levels, def_levels, rep_levels, valid_bits,
                                       valid_bits_offset, buffer);
}

template <>
Status ArrowColumnWriter::WriteNullableBatch<Int32Type, ::arrow::Date64Type>(
    const ::arrow::Date64Type& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels, const uint8_t* valid_bits,
    int64_t valid_bits_offset, const int64_t* values) {
  int32_t* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<int32_t>(num_values, &buffer));

  for (int i = 0; i < num_values; i++) {
    // Convert from milliseconds into days since the epoch
    buffer[i] = static_cast<int32_t>(values[i] / 86400000);
  }

  return WriteBatchSpaced<Int32Type>(num_levels, def_levels, rep_levels, valid_bits,
                                     valid_bits_offset, buffer);
}

template <>
Status ArrowColumnWriter::WriteNullableBatch<Int32Type, ::arrow::Time32Type>(
    const ::arrow::Time32Type& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels, const uint8_t* valid_bits,
    int64_t valid_bits_offset, const int32_t* values) {
  int32_t* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<int32_t>(num_values, &buffer));

  if (type.unit() == TimeUnit::SECOND) {
    for (int i = 0; i < num_values; i++) {
      buffer[i] = values[i] * 1000;
    }
  } else {
    for (int i = 0; i < num_values; i++) {
      buffer[i] = values[i];
    }
  }
  return WriteBatchSpaced<Int32Type>(num_levels, def_levels, rep_levels, valid_bits,
                                     valid_bits_offset, buffer);
}

#define NULLABLE_BATCH_FAST_PATH(ParquetType, ArrowType, CType)                          \
  template <>                                                                            \
  Status ArrowColumnWriter::WriteNullableBatch<ParquetType, ArrowType>(                  \
      const ArrowType& type, int64_t num_values, int64_t num_levels,                     \
      const int16_t* def_levels, const int16_t* rep_levels, const uint8_t* valid_bits,   \
      int64_t valid_bits_offset, const CType* values) {                                  \
    return WriteBatchSpaced<ParquetType>(num_levels, def_levels, rep_levels, valid_bits, \
                                         valid_bits_offset, values);                     \
  }

NULLABLE_BATCH_FAST_PATH(Int32Type, ::arrow::Int32Type, int32_t)
NULLABLE_BATCH_FAST_PATH(Int32Type, ::arrow::Date32Type, int32_t)
NULLABLE_BATCH_FAST_PATH(Int64Type, ::arrow::Int64Type, int64_t)
NULLABLE_BATCH_FAST_PATH(Int64Type, ::arrow::Time64Type, int64_t)
NULLABLE_BATCH_FAST_PATH(FloatType, ::arrow::FloatType, float)
NULLABLE_BATCH_FAST_PATH(DoubleType, ::arrow::DoubleType, double)
NULLABLE_BATCH_FAST_PATH(Int64Type, ::arrow::TimestampType, int64_t)
NONNULLABLE_BATCH_FAST_PATH(Int64Type, ::arrow::TimestampType, int64_t)

#define CONV_CASE_LOOP(ConversionFunction) \
  for (int64_t i = 0; i < num_values; i++) \
    ConversionFunction(arrow_values[i], &output[i]);

static void ConvertArrowTimestampToParquetInt96(const int64_t* arrow_values,
                                                int64_t num_values,
                                                ::arrow::TimeUnit ::type unit_type,
                                                Int96* output) {
  switch (unit_type) {
    case TimeUnit::NANO:
      CONV_CASE_LOOP(internal::NanosecondsToImpalaTimestamp);
      break;
    case TimeUnit::MICRO:
      CONV_CASE_LOOP(internal::MicrosecondsToImpalaTimestamp);
      break;
    case TimeUnit::MILLI:
      CONV_CASE_LOOP(internal::MillisecondsToImpalaTimestamp);
      break;
    case TimeUnit::SECOND:
      CONV_CASE_LOOP(internal::SecondsToImpalaTimestamp);
      break;
  }
}

#undef CONV_CASE_LOOP

template <>
Status ArrowColumnWriter::WriteNullableBatch<Int96Type, ::arrow::TimestampType>(
    const ::arrow::TimestampType& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels, const uint8_t* valid_bits,
    int64_t valid_bits_offset, const int64_t* values) {
  Int96* buffer = nullptr;
  RETURN_NOT_OK(ctx_->GetScratchData<Int96>(num_values, &buffer));

  ConvertArrowTimestampToParquetInt96(values, num_values, type.unit(), buffer);

  return WriteBatchSpaced<Int96Type>(num_levels, def_levels, rep_levels, valid_bits,
                                     valid_bits_offset, buffer);
}

template <>
Status ArrowColumnWriter::WriteNonNullableBatch<Int96Type, ::arrow::TimestampType>(
    const ::arrow::TimestampType& type, int64_t num_values, int64_t num_levels,
    const int16_t* def_levels, const int16_t* rep_levels, const int64_t* values) {
  Int96* buffer = nullptr;
  RETURN_NOT_OK(ctx_->GetScratchData<Int96>(num_values, &buffer));

  ConvertArrowTimestampToParquetInt96(values, num_values, type.unit(), buffer);

  return WriteBatch<Int96Type>(num_levels, def_levels, rep_levels, buffer);
}

Status ArrowColumnWriter::WriteTimestamps(const Array& values, int64_t num_levels,
                                          const int16_t* def_levels,
                                          const int16_t* rep_levels) {
  const auto& source_type = static_cast<const ::arrow::TimestampType&>(*values.type());

  if (ctx_->properties->support_deprecated_int96_timestamps()) {
    // User explicitly requested Int96 timestamps
    return TypedWriteBatch<Int96Type, ::arrow::TimestampType>(values, num_levels,
                                                              def_levels, rep_levels);
  } else if (ctx_->properties->coerce_timestamps_enabled()) {
    // User explicitly requested coercion to specific unit
    if (source_type.unit() == ctx_->properties->coerce_timestamps_unit()) {
      // No data conversion necessary
      return TypedWriteBatch<Int64Type, ::arrow::TimestampType>(values, num_levels,
                                                                def_levels, rep_levels);
    } else {
      return WriteTimestampsCoerce(values, num_levels, def_levels, rep_levels,
                                   *(ctx_->properties));
    }
  } else if (writer_->properties()->version() == ParquetVersion::PARQUET_1_0 &&
             source_type.unit() == TimeUnit::NANO) {
    // Absent superseding user instructions, when writing Parquet version 1.0 files,
    // timestamps in nanoseconds are coerced to microseconds
    std::shared_ptr<ArrowWriterProperties> properties =
        (ArrowWriterProperties::Builder())
            .coerce_timestamps(TimeUnit::MICRO)
            ->disallow_truncated_timestamps()
            ->build();
    return WriteTimestampsCoerce(values, num_levels, def_levels, rep_levels, *properties);
  } else if (source_type.unit() == TimeUnit::SECOND) {
    // Absent superseding user instructions, timestamps in seconds are coerced to
    // milliseconds
    std::shared_ptr<ArrowWriterProperties> properties =
        (ArrowWriterProperties::Builder()).coerce_timestamps(TimeUnit::MILLI)->build();
    return WriteTimestampsCoerce(values, num_levels, def_levels, rep_levels, *properties);
  } else {
    // No data conversion necessary
    return TypedWriteBatch<Int64Type, ::arrow::TimestampType>(values, num_levels,
                                                              def_levels, rep_levels);
  }
}

#define COERCE_DIVIDE -1
#define COERCE_INVALID 0
#define COERCE_MULTIPLY +1

static std::pair<int, int64_t> kTimestampCoercionFactors[4][4] = {
    // from seconds ...
    {{COERCE_INVALID, 0},                      // ... to seconds
     {COERCE_MULTIPLY, 1000},                  // ... to millis
     {COERCE_MULTIPLY, 1000000},               // ... to micros
     {COERCE_MULTIPLY, INT64_C(1000000000)}},  // ... to nanos
    // from millis ...
    {{COERCE_INVALID, 0},
     {COERCE_MULTIPLY, 1},
     {COERCE_MULTIPLY, 1000},
     {COERCE_MULTIPLY, 1000000}},
    // from micros ...
    {{COERCE_INVALID, 0},
     {COERCE_DIVIDE, 1000},
     {COERCE_MULTIPLY, 1},
     {COERCE_MULTIPLY, 1000}},
    // from nanos ...
    {{COERCE_INVALID, 0},
     {COERCE_DIVIDE, 1000000},
     {COERCE_DIVIDE, 1000},
     {COERCE_MULTIPLY, 1}}};

Status ArrowColumnWriter::WriteTimestampsCoerce(const Array& array, int64_t num_levels,
                                                const int16_t* def_levels,
                                                const int16_t* rep_levels,
                                                const ArrowWriterProperties& properties) {
  int64_t* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<int64_t>(num_levels, &buffer));

  const auto& data = static_cast<const ::arrow::TimestampArray&>(array);
  auto values = data.raw_values();

  const auto& source_type = static_cast<const ::arrow::TimestampType&>(*array.type());
  auto source_unit = source_type.unit();

  TimeUnit::type target_unit = properties.coerce_timestamps_unit();
  auto target_type = ::arrow::timestamp(target_unit);
  bool truncation_allowed = properties.truncated_timestamps_allowed();

  auto DivideBy = [&](const int64_t factor) {
    for (int64_t i = 0; i < array.length(); i++) {
      if (!truncation_allowed && !data.IsNull(i) && (values[i] % factor != 0)) {
        return Status::Invalid("Casting from ", source_type.ToString(), " to ",
                               target_type->ToString(), " would lose data: ", values[i]);
      }
      buffer[i] = values[i] / factor;
    }
    return Status::OK();
  };

  auto MultiplyBy = [&](const int64_t factor) {
    for (int64_t i = 0; i < array.length(); i++) {
      buffer[i] = values[i] * factor;
    }
    return Status::OK();
  };

  const auto& coercion = kTimestampCoercionFactors[static_cast<int>(source_unit)]
                                                  [static_cast<int>(target_unit)];
  // .first -> coercion operation; .second -> scale factor
  DCHECK_NE(coercion.first, COERCE_INVALID);
  RETURN_NOT_OK(coercion.first == COERCE_DIVIDE ? DivideBy(coercion.second)
                                                : MultiplyBy(coercion.second));

  if (writer_->descr()->schema_node()->is_required() || (data.null_count() == 0)) {
    // no nulls, just dump the data
    RETURN_NOT_OK((WriteNonNullableBatch<Int64Type, ::arrow::TimestampType>(
        static_cast<const ::arrow::TimestampType&>(*target_type), array.length(),
        num_levels, def_levels, rep_levels, buffer)));
  } else {
    const uint8_t* valid_bits = data.null_bitmap_data();
    RETURN_NOT_OK((WriteNullableBatch<Int64Type, ::arrow::TimestampType>(
        static_cast<const ::arrow::TimestampType&>(*target_type), array.length(),
        num_levels, def_levels, rep_levels, valid_bits, data.offset(), buffer)));
  }

  return Status::OK();
}

#undef COERCE_DIVIDE
#undef COERCE_INVALID
#undef COERCE_MULTIPLY

// This specialization seems quite similar but it significantly differs in two points:
// * offset is added at the most latest time to the pointer as we have sub-byte access
// * Arrow data is stored bitwise thus we cannot use std::copy to transform from
//   ArrowType::c_type to ParquetType::c_type

template <>
Status ArrowColumnWriter::TypedWriteBatch<BooleanType, ::arrow::BooleanType>(
    const Array& array, int64_t num_levels, const int16_t* def_levels,
    const int16_t* rep_levels) {
  bool* buffer = nullptr;
  RETURN_NOT_OK(ctx_->GetScratchData<bool>(array.length(), &buffer));

  const auto& data = static_cast<const BooleanArray&>(array);
  const uint8_t* values = nullptr;
  // The values buffer may be null if the array is empty (ARROW-2744)
  if (data.values() != nullptr) {
    values = reinterpret_cast<const uint8_t*>(data.values()->data());
  } else {
    DCHECK_EQ(data.length(), 0);
  }

  int buffer_idx = 0;
  int64_t offset = array.offset();
  for (int i = 0; i < data.length(); i++) {
    if (!data.IsNull(i)) {
      buffer[buffer_idx++] = BitUtil::GetBit(values, offset + i);
    }
  }

  return WriteBatch<BooleanType>(num_levels, def_levels, rep_levels, buffer);
}

template <>
Status ArrowColumnWriter::TypedWriteBatch<Int32Type, ::arrow::NullType>(
    const Array& array, int64_t num_levels, const int16_t* def_levels,
    const int16_t* rep_levels) {
  return WriteBatch<Int32Type>(num_levels, def_levels, rep_levels, nullptr);
}

template <>
Status ArrowColumnWriter::TypedWriteBatch<ByteArrayType, ::arrow::BinaryType>(
    const Array& array, int64_t num_levels, const int16_t* def_levels,
    const int16_t* rep_levels) {
  ByteArray* buffer = nullptr;
  RETURN_NOT_OK(ctx_->GetScratchData<ByteArray>(num_levels, &buffer));

  const auto& data = static_cast<const BinaryArray&>(array);

  // In the case of an array consisting of only empty strings or all null,
  // data.data() points already to a nullptr, thus data.data()->data() will
  // segfault.
  const uint8_t* values = nullptr;
  if (data.value_data()) {
    values = reinterpret_cast<const uint8_t*>(data.value_data()->data());
    DCHECK(values != nullptr);
  }

  // Slice offset is accounted for in raw_value_offsets
  const int32_t* value_offset = data.raw_value_offsets();

  if (writer_->descr()->schema_node()->is_required() || (data.null_count() == 0)) {
    // no nulls, just dump the data
    for (int64_t i = 0; i < data.length(); i++) {
      buffer[i] =
          ByteArray(value_offset[i + 1] - value_offset[i], values + value_offset[i]);
    }
  } else {
    int buffer_idx = 0;
    for (int64_t i = 0; i < data.length(); i++) {
      if (!data.IsNull(i)) {
        buffer[buffer_idx++] =
            ByteArray(value_offset[i + 1] - value_offset[i], values + value_offset[i]);
      }
    }
  }

  return WriteBatch<ByteArrayType>(num_levels, def_levels, rep_levels, buffer);
}

template <>
Status ArrowColumnWriter::TypedWriteBatch<FLBAType, ::arrow::FixedSizeBinaryType>(
    const Array& array, int64_t num_levels, const int16_t* def_levels,
    const int16_t* rep_levels) {
  const auto& data = static_cast<const FixedSizeBinaryArray&>(array);
  const int64_t length = data.length();

  FLBA* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<FLBA>(num_levels, &buffer));

  if (writer_->descr()->schema_node()->is_required() || data.null_count() == 0) {
    // no nulls, just dump the data
    // todo(advancedxy): use a writeBatch to avoid this step
    for (int64_t i = 0; i < length; i++) {
      buffer[i] = FixedLenByteArray(data.GetValue(i));
    }
  } else {
    int buffer_idx = 0;
    for (int64_t i = 0; i < length; i++) {
      if (!data.IsNull(i)) {
        buffer[buffer_idx++] = FixedLenByteArray(data.GetValue(i));
      }
    }
  }

  return WriteBatch<FLBAType>(num_levels, def_levels, rep_levels, buffer);
}

template <>
Status ArrowColumnWriter::TypedWriteBatch<FLBAType, ::arrow::Decimal128Type>(
    const Array& array, int64_t num_levels, const int16_t* def_levels,
    const int16_t* rep_levels) {
  const auto& data = static_cast<const Decimal128Array&>(array);
  const int64_t length = data.length();

  FLBA* buffer;
  RETURN_NOT_OK(ctx_->GetScratchData<FLBA>(num_levels, &buffer));

  const auto& decimal_type = static_cast<const ::arrow::Decimal128Type&>(*data.type());
  const int32_t offset =
      decimal_type.byte_width() - DecimalSize(decimal_type.precision());

  const bool does_not_have_nulls =
      writer_->descr()->schema_node()->is_required() || data.null_count() == 0;

  const auto valid_value_count = static_cast<size_t>(length - data.null_count()) * 2;
  std::vector<uint64_t> big_endian_values(valid_value_count);

  // TODO(phillipc): Look into whether our compilers will perform loop unswitching so we
  // don't have to keep writing two loops to handle the case where we know there are no
  // nulls
  if (does_not_have_nulls) {
    // no nulls, just dump the data
    // todo(advancedxy): use a writeBatch to avoid this step
    for (int64_t i = 0, j = 0; i < length; ++i, j += 2) {
      auto unsigned_64_bit = reinterpret_cast<const uint64_t*>(data.GetValue(i));
      big_endian_values[j] = ::arrow::BitUtil::ToBigEndian(unsigned_64_bit[1]);
      big_endian_values[j + 1] = ::arrow::BitUtil::ToBigEndian(unsigned_64_bit[0]);
      buffer[i] = FixedLenByteArray(
          reinterpret_cast<const uint8_t*>(&big_endian_values[j]) + offset);
    }
  } else {
    for (int64_t i = 0, buffer_idx = 0, j = 0; i < length; ++i) {
      if (!data.IsNull(i)) {
        auto unsigned_64_bit = reinterpret_cast<const uint64_t*>(data.GetValue(i));
        big_endian_values[j] = ::arrow::BitUtil::ToBigEndian(unsigned_64_bit[1]);
        big_endian_values[j + 1] = ::arrow::BitUtil::ToBigEndian(unsigned_64_bit[0]);
        buffer[buffer_idx++] = FixedLenByteArray(
            reinterpret_cast<const uint8_t*>(&big_endian_values[j]) + offset);
        j += 2;
      }
    }
  }

  return WriteBatch<FLBAType>(num_levels, def_levels, rep_levels, buffer);
}

Status ArrowColumnWriter::Write(const Array& data) {
  if (data.length() == 0) {
    // Write nothing when length is 0
    return Status::OK();
  }

  ::arrow::Type::type values_type;
  RETURN_NOT_OK(GetLeafType(*data.type(), &values_type));

  std::shared_ptr<Array> _values_array;
  int64_t values_offset = 0;
  int64_t num_levels = 0;
  int64_t num_values = 0;
  LevelBuilder level_builder(ctx_->memory_pool);

  std::shared_ptr<Buffer> def_levels_buffer, rep_levels_buffer;
  RETURN_NOT_OK(level_builder.GenerateLevels(
      data, field_, &values_offset, &num_values, &num_levels, ctx_->def_levels_buffer,
      &def_levels_buffer, &rep_levels_buffer, &_values_array));
  const int16_t* def_levels = nullptr;
  if (def_levels_buffer) {
    def_levels = reinterpret_cast<const int16_t*>(def_levels_buffer->data());
  }
  const int16_t* rep_levels = nullptr;
  if (rep_levels_buffer) {
    rep_levels = reinterpret_cast<const int16_t*>(rep_levels_buffer->data());
  }
  std::shared_ptr<Array> values_array = _values_array->Slice(values_offset, num_values);

#define WRITE_BATCH_CASE(ArrowEnum, ArrowType, ParquetType)                            \
  case ::arrow::Type::ArrowEnum:                                                       \
    return TypedWriteBatch<ParquetType, ::arrow::ArrowType>(*values_array, num_levels, \
                                                            def_levels, rep_levels);

  switch (values_type) {
    case ::arrow::Type::UINT32: {
      if (writer_->properties()->version() == ParquetVersion::PARQUET_1_0) {
        // Parquet 1.0 reader cannot read the UINT_32 logical type. Thus we need
        // to use the larger Int64Type to store them lossless.
        return TypedWriteBatch<Int64Type, ::arrow::UInt32Type>(*values_array, num_levels,
                                                               def_levels, rep_levels);
      } else {
        return TypedWriteBatch<Int32Type, ::arrow::UInt32Type>(*values_array, num_levels,
                                                               def_levels, rep_levels);
      }
    }
      WRITE_BATCH_CASE(NA, NullType, Int32Type)
    case ::arrow::Type::TIMESTAMP:
      return WriteTimestamps(*values_array, num_levels, def_levels, rep_levels);
      WRITE_BATCH_CASE(BOOL, BooleanType, BooleanType)
      WRITE_BATCH_CASE(INT8, Int8Type, Int32Type)
      WRITE_BATCH_CASE(UINT8, UInt8Type, Int32Type)
      WRITE_BATCH_CASE(INT16, Int16Type, Int32Type)
      WRITE_BATCH_CASE(UINT16, UInt16Type, Int32Type)
      WRITE_BATCH_CASE(INT32, Int32Type, Int32Type)
      WRITE_BATCH_CASE(INT64, Int64Type, Int64Type)
      WRITE_BATCH_CASE(UINT64, UInt64Type, Int64Type)
      WRITE_BATCH_CASE(FLOAT, FloatType, FloatType)
      WRITE_BATCH_CASE(DOUBLE, DoubleType, DoubleType)
      WRITE_BATCH_CASE(BINARY, BinaryType, ByteArrayType)
      WRITE_BATCH_CASE(STRING, BinaryType, ByteArrayType)
      WRITE_BATCH_CASE(FIXED_SIZE_BINARY, FixedSizeBinaryType, FLBAType)
      WRITE_BATCH_CASE(DECIMAL, Decimal128Type, FLBAType)
      WRITE_BATCH_CASE(DATE32, Date32Type, Int32Type)
      WRITE_BATCH_CASE(DATE64, Date64Type, Int32Type)
      WRITE_BATCH_CASE(TIME32, Time32Type, Int32Type)
      WRITE_BATCH_CASE(TIME64, Time64Type, Int64Type)
    default:
      break;
  }
  return Status::NotImplemented("Data type not supported as list value: ",
                                values_array->type()->ToString());
}

}  // namespace

// ----------------------------------------------------------------------
// FileWriter implementation

class FileWriter::Impl {
 public:
  Impl(MemoryPool* pool, std::unique_ptr<ParquetFileWriter> writer,
       const std::shared_ptr<ArrowWriterProperties>& arrow_properties)
      : writer_(std::move(writer)),
        row_group_writer_(nullptr),
        column_write_context_(pool, arrow_properties.get()),
        arrow_properties_(arrow_properties),
        closed_(false) {}

  Status NewRowGroup(int64_t chunk_size) {
    if (row_group_writer_ != nullptr) {
      PARQUET_CATCH_NOT_OK(row_group_writer_->Close());
    }
    PARQUET_CATCH_NOT_OK(row_group_writer_ = writer_->AppendRowGroup());
    return Status::OK();
  }

  Status Close() {
    if (!closed_) {
      // Make idempotent
      closed_ = true;
      if (row_group_writer_ != nullptr) {
        PARQUET_CATCH_NOT_OK(row_group_writer_->Close());
      }
      PARQUET_CATCH_NOT_OK(writer_->Close());
    }
    return Status::OK();
  }

  Status WriteColumnChunk(const Array& data) {
    // A bit awkward here since cannot instantiate ChunkedArray from const Array&
    ::arrow::ArrayVector chunks = {::arrow::MakeArray(data.data())};
    auto chunked_array = std::make_shared<::arrow::ChunkedArray>(chunks);
    return WriteColumnChunk(chunked_array, 0, data.length());
  }

  Status WriteColumnChunk(const std::shared_ptr<ChunkedArray>& data, int64_t offset,
                          const int64_t size) {
    // DictionaryArrays are not yet handled with a fast path. To still support
    // writing them as a workaround, we convert them back to their non-dictionary
    // representation.
    if (data->type()->id() == ::arrow::Type::DICTIONARY) {
      const ::arrow::DictionaryType& dict_type =
          static_cast<const ::arrow::DictionaryType&>(*data->type());

      // TODO(ARROW-1648): Remove this special handling once we require an Arrow
      // version that has this fixed.
      if (dict_type.value_type()->id() == ::arrow::Type::NA) {
        auto null_array = std::make_shared<::arrow::NullArray>(data->length());
        return WriteColumnChunk(*null_array);
      }

      FunctionContext ctx(this->memory_pool());
      ::arrow::compute::Datum cast_input(data);
      ::arrow::compute::Datum cast_output;
      RETURN_NOT_OK(
          Cast(&ctx, cast_input, dict_type.value_type(), CastOptions(), &cast_output));
      return WriteColumnChunk(cast_output.chunked_array(), offset, size);
    }

    ColumnWriter* column_writer;
    PARQUET_CATCH_NOT_OK(column_writer = row_group_writer_->NextColumn());

    // TODO(wesm): This trick to construct a schema for one Parquet root node
    // will not work for arbitrary nested data
    int current_column_idx = row_group_writer_->current_column();
    std::shared_ptr<::arrow::Schema> arrow_schema;
    RETURN_NOT_OK(FromParquetSchema(writer_->schema(), {current_column_idx - 1},
                                    default_arrow_reader_properties(),
                                    writer_->key_value_metadata(), &arrow_schema));

    ArrowColumnWriter arrow_writer(&column_write_context_, column_writer,
                                   arrow_schema->field(0));

    RETURN_NOT_OK(arrow_writer.Write(*data, offset, size));
    return arrow_writer.Close();
  }

  const WriterProperties& properties() const { return *writer_->properties(); }

  ::arrow::MemoryPool* memory_pool() const { return column_write_context_.memory_pool; }

  virtual ~Impl() {}

  const std::shared_ptr<FileMetaData> metadata() const { return writer_->metadata(); }

 private:
  friend class FileWriter;

  std::unique_ptr<ParquetFileWriter> writer_;
  RowGroupWriter* row_group_writer_;
  ColumnWriterContext column_write_context_;
  std::shared_ptr<ArrowWriterProperties> arrow_properties_;
  bool closed_;
};

Status FileWriter::NewRowGroup(int64_t chunk_size) {
  return impl_->NewRowGroup(chunk_size);
}

Status FileWriter::WriteColumnChunk(const ::arrow::Array& data) {
  return impl_->WriteColumnChunk(data);
}

Status FileWriter::WriteColumnChunk(const std::shared_ptr<::arrow::ChunkedArray>& data,
                                    const int64_t offset, const int64_t size) {
  return impl_->WriteColumnChunk(data, offset, size);
}

Status FileWriter::WriteColumnChunk(const std::shared_ptr<::arrow::ChunkedArray>& data) {
  return WriteColumnChunk(data, 0, data->length());
}

Status FileWriter::Close() { return impl_->Close(); }

MemoryPool* FileWriter::memory_pool() const { return impl_->memory_pool(); }

const std::shared_ptr<FileMetaData> FileWriter::metadata() const {
  return impl_->metadata();
}

FileWriter::~FileWriter() {}

FileWriter::FileWriter(MemoryPool* pool, std::unique_ptr<ParquetFileWriter> writer,
                       const std::shared_ptr<::arrow::Schema>& schema,
                       const std::shared_ptr<ArrowWriterProperties>& arrow_properties)
    : impl_(new FileWriter::Impl(pool, std::move(writer), arrow_properties)),
      schema_(schema) {}

Status FileWriter::Open(const ::arrow::Schema& schema, ::arrow::MemoryPool* pool,
                        const std::shared_ptr<::arrow::io::OutputStream>& sink,
                        const std::shared_ptr<WriterProperties>& properties,
                        std::unique_ptr<FileWriter>* writer) {
  return Open(schema, pool, sink, properties, default_arrow_writer_properties(), writer);
}

Status FileWriter::Open(const ::arrow::Schema& schema, ::arrow::MemoryPool* pool,
                        const std::shared_ptr<::arrow::io::OutputStream>& sink,
                        const std::shared_ptr<WriterProperties>& properties,
                        const std::shared_ptr<ArrowWriterProperties>& arrow_properties,
                        std::unique_ptr<FileWriter>* writer) {
  std::shared_ptr<SchemaDescriptor> parquet_schema;
  RETURN_NOT_OK(
      ToParquetSchema(&schema, *properties, *arrow_properties, &parquet_schema));

  auto schema_node = std::static_pointer_cast<GroupNode>(parquet_schema->schema_root());

  std::unique_ptr<ParquetFileWriter> base_writer =
      ParquetFileWriter::Open(sink, schema_node, properties, schema.metadata());

  auto schema_ptr = std::make_shared<::arrow::Schema>(schema);
  writer->reset(
      new FileWriter(pool, std::move(base_writer), schema_ptr, arrow_properties));
  return Status::OK();
}

Status WriteFileMetaData(const FileMetaData& file_metadata,
                         ::arrow::io::OutputStream* sink) {
  PARQUET_CATCH_NOT_OK(::parquet::WriteFileMetaData(file_metadata, sink));
  return Status::OK();
}

Status WriteMetaDataFile(const FileMetaData& file_metadata,
                         ::arrow::io::OutputStream* sink) {
  PARQUET_CATCH_NOT_OK(::parquet::WriteMetaDataFile(file_metadata, sink));
  return Status::OK();
}

Status FileWriter::WriteTable(const Table& table, int64_t chunk_size) {
  RETURN_NOT_OK(table.Validate());

  if (chunk_size <= 0 && table.num_rows() > 0) {
    return Status::Invalid("chunk size per row_group must be greater than 0");
  } else if (!table.schema()->Equals(*schema_, false)) {
    return Status::Invalid("table schema does not match this writer's. table:'",
                           table.schema()->ToString(), "' this:'", schema_->ToString(),
                           "'");
  } else if (chunk_size > impl_->properties().max_row_group_length()) {
    chunk_size = impl_->properties().max_row_group_length();
  }

  auto WriteRowGroup = [&](int64_t offset, int64_t size) {
    RETURN_NOT_OK(NewRowGroup(size));
    for (int i = 0; i < table.num_columns(); i++) {
      RETURN_NOT_OK(WriteColumnChunk(table.column(i), offset, size));
    }
    return Status::OK();
  };

  if (table.num_rows() == 0) {
    // Append a row group with 0 rows
    RETURN_NOT_OK_ELSE(WriteRowGroup(0, 0), PARQUET_IGNORE_NOT_OK(Close()));
    return Status::OK();
  }

  for (int chunk = 0; chunk * chunk_size < table.num_rows(); chunk++) {
    int64_t offset = chunk * chunk_size;
    RETURN_NOT_OK_ELSE(
        WriteRowGroup(offset, std::min(chunk_size, table.num_rows() - offset)),
        PARQUET_IGNORE_NOT_OK(Close()));
  }
  return Status::OK();
}

Status WriteTable(const ::arrow::Table& table, ::arrow::MemoryPool* pool,
                  const std::shared_ptr<::arrow::io::OutputStream>& sink,
                  int64_t chunk_size, const std::shared_ptr<WriterProperties>& properties,
                  const std::shared_ptr<ArrowWriterProperties>& arrow_properties) {
  std::unique_ptr<FileWriter> writer;
  RETURN_NOT_OK(FileWriter::Open(*table.schema(), pool, sink, properties,
                                 arrow_properties, &writer));
  RETURN_NOT_OK(writer->WriteTable(table, chunk_size));
  return writer->Close();
}

}  // namespace arrow
}  // namespace parquet
