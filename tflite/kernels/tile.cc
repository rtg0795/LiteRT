/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <stdint.h>

#include <algorithm>
#include <tuple>
#include <utility>

#include "tflite/core/c/common.h"
#include "tflite/kernels/internal/reference/reference_ops.h"
#include "tflite/kernels/internal/tensor.h"
#include "tflite/kernels/internal/tensor_ctypes.h"
#include "tflite/kernels/kernel_util.h"
#include "tflite/string_util.h"

namespace tflite {
namespace ops {
namespace builtin {
namespace tile {

constexpr int kInputTensor = 0;
constexpr int kInputMultipliers = 1;
constexpr int kOutputTensor = 0;

namespace {
struct OpData {
  // Indicates that 'Eval' is a noop as the output as written during 'Prepare'.
  bool noop;
};

template <typename T>
TfLiteIntArray* MultiplyShapeDims(const TfLiteIntArray& shape,
                                  const TfLiteTensor* multipliers,
                                  int num_dimensions) {
  const T* multipliers_v = GetTensorData<T>(multipliers);

  TfLiteIntArray* output_shape = TfLiteIntArrayCreate(num_dimensions);
  for (int i = 0; i < num_dimensions; ++i) {
    output_shape->data[i] = shape.data[i] * multipliers_v[i];
  }
  return output_shape;
}

TfLiteStatus ResizeOutput(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input;
  TF_LITE_ENSURE_OK(context, GetInputSafe(context, node, kInputTensor, &input));
  TfLiteTensor* output;
  TF_LITE_ENSURE_OK(context,
                    GetOutputSafe(context, node, kOutputTensor, &output));
  const TfLiteTensor* multipliers;
  TF_LITE_ENSURE_OK(
      context, GetInputSafe(context, node, kInputMultipliers, &multipliers));

  const int num_dimensions = NumDimensions(input);
  const int num_multipliers = NumElements(multipliers);
  TF_LITE_ENSURE_EQ(context, num_dimensions, num_multipliers);
  switch (multipliers->type) {
    case kTfLiteInt32:
      return context->ResizeTensor(
          context, output,
          MultiplyShapeDims<int32_t>(*input->dims, multipliers,
                                     num_dimensions));
    case kTfLiteInt64:
      return context->ResizeTensor(
          context, output,
          MultiplyShapeDims<int64_t>(*input->dims, multipliers,
                                     num_dimensions));
    default:
      TF_LITE_KERNEL_LOG(context,
                         "Multipliers of type '%s' are not supported by tile.",
                         TfLiteTypeGetName(multipliers->type));
      return kTfLiteError;
  }
}

template <typename T, typename M>
void CopyMultipleTimes(const T* in_data, int32_t in_size, M multiplier,
                       T* out_data) {
  for (M i = 0; i < multiplier; ++i) {
    const T* in_end = in_data + in_size;
    T* new_out_data = std::copy(in_data, in_end, out_data);
    in_data = out_data;
    out_data = new_out_data;
  }
}

template <typename M>
void CopyStringMultipleTimes(const TfLiteTensor* in_data, int in_data_index,
                             const int dimension_size, M multiplier,
                             DynamicBuffer* buffer) {
  for (M i = 0; i < multiplier; ++i) {
    for (int j = 0; j < dimension_size; ++j) {
      const auto string_ref = GetString(in_data, in_data_index + j);
      buffer->AddString(string_ref.str, string_ref.len);
    }
  }
}

template <typename T, typename M>
std::pair<int, int> TileOneDimension(const TfLiteIntArray& in_dimensions,
                                     const T* in_data, const M* multipliers,
                                     T* out_data, int dimension) {
  if (in_dimensions.size == 0) {
    // If input tensor is a scalar, then just copy it to output (no need to
    // multiply).
    *out_data = *in_data;
    return std::make_pair(0, 0);
  }

  const int dimension_size = in_dimensions.data[dimension];
  if (dimension == in_dimensions.size - 1) {
    CopyMultipleTimes(in_data, dimension_size, multipliers[dimension],
                      out_data);
    return std::make_pair(
        dimension_size,
        dimension_size * static_cast<int>(multipliers[dimension]));
  }
  int total_stride_size = 0, total_tiled_stride_size = 0;
  const T* copy_from_data = in_data;
  T* copy_to_data = out_data;
  for (int i = 0; i < dimension_size; ++i) {
    int stride_size = 0, tiled_stride_size = 0;
    std::tie(stride_size, tiled_stride_size) =
        TileOneDimension(in_dimensions, copy_from_data, multipliers,
                         copy_to_data, dimension + 1);
    copy_from_data += stride_size;
    copy_to_data += tiled_stride_size;
    total_stride_size += stride_size;
    total_tiled_stride_size += tiled_stride_size;
  }
  CopyMultipleTimes(out_data, total_tiled_stride_size,
                    multipliers[dimension] - 1,
                    out_data + total_tiled_stride_size);
  return std::make_pair(
      total_stride_size,
      static_cast<int>(total_tiled_stride_size * multipliers[dimension]));
}

template <typename M>
std::pair<int, int> TileStringOneDimension(
    const TfLiteIntArray& in_dimensions, const TfLiteTensor* in_data,
    int in_data_index, const M* multipliers, DynamicBuffer* buffer,
    int buffer_index, int dimension, TfLiteTensor* out_data) {
  const int dimension_size = in_dimensions.data[dimension];
  if (dimension == in_dimensions.size - 1) {
    CopyStringMultipleTimes(in_data, in_data_index, dimension_size,
                            multipliers[dimension], buffer);
    return {dimension_size,
            dimension_size * static_cast<int>(multipliers[dimension])};
  }

  int total_stride_size = 0, total_tiled_stride_size = 0;
  for (int i = 0; i < dimension_size; ++i) {
    int stride_size, tiled_stride_size;
    std::tie(stride_size, tiled_stride_size) = TileStringOneDimension(
        in_dimensions, in_data, in_data_index + total_stride_size, multipliers,
        buffer, buffer_index + total_tiled_stride_size, dimension + 1,
        out_data);
    total_stride_size += stride_size;
    total_tiled_stride_size += tiled_stride_size;
  }

  buffer->WriteToTensor(out_data, /*new_shape=*/nullptr);
  CopyStringMultipleTimes(out_data, buffer_index, total_tiled_stride_size,
                          multipliers[dimension] - 1, buffer);

  return {total_stride_size,
          total_tiled_stride_size * static_cast<int>(multipliers[dimension])};
}

template <typename T>
void Tile(const TfLiteIntArray& in_dimensions, const TfLiteTensor* in_data,
          const TfLiteTensor* multipliers, TfLiteTensor* out_data) {
  // Doing recursively tiling from top to down dimension.
  switch (multipliers->type) {
    case kTfLiteInt32:
      TileOneDimension(in_dimensions, GetTensorData<T>(in_data),
                       GetTensorData<int32_t>(multipliers),
                       GetTensorData<T>(out_data), 0);
      break;
    case kTfLiteInt64:
      TileOneDimension(in_dimensions, GetTensorData<T>(in_data),
                       GetTensorData<int64_t>(multipliers),
                       GetTensorData<T>(out_data), 0);
      break;
    default:
      break;
  }
}

void TileString(const TfLiteIntArray& in_dimensions,
                const TfLiteTensor* in_data, const TfLiteTensor* multipliers,
                DynamicBuffer* buffer, TfLiteTensor* out_data) {
  // Doing recursively tiling from top to down dimension.
  switch (multipliers->type) {
    case kTfLiteInt32:
      TileStringOneDimension(in_dimensions, in_data, 0,
                             GetTensorData<int32_t>(multipliers), buffer, 0, 0,
                             out_data);
      break;
    case kTfLiteInt64:
      TileStringOneDimension(in_dimensions, in_data, 0,
                             GetTensorData<int64_t>(multipliers), buffer, 0, 0,
                             out_data);
      break;
    default:
      break;
  }
}
}  // namespace

TfLiteStatus EvalImpl(TfLiteContext* context, const TfLiteTensor* input,
                      const TfLiteTensor* multipliers, TfLiteTensor* output) {
  if (GetTensorShape(output).FlatSize() == 0) {
    if (output->type == kTfLiteString) {
      // For safety, ensure that we write to the output tensor.
      DynamicBuffer buffer;
      buffer.WriteToTensor(output, /*new_shape=*/nullptr);
    }
    return kTfLiteOk;
  }

  switch (output->type) {
    case kTfLiteInt8:
    case kTfLiteUInt8:
      Tile<int8_t>(*(input->dims), input, multipliers, output);
      break;
    case kTfLiteFloat32:
    case kTfLiteInt32:
      Tile<int32_t>(*(input->dims), input, multipliers, output);
      break;
    case kTfLiteInt64:
      Tile<int64_t>(*(input->dims), input, multipliers, output);
      break;
    case kTfLiteString: {
      DynamicBuffer buffer;
      TileString(*(input->dims), input, multipliers, &buffer, output);
      buffer.WriteToTensor(output, /*new_shape=*/nullptr);
      break;
    }
    case kTfLiteBool:
      Tile<bool>(*(input->dims), input, multipliers, output);
      break;
    default:
      TF_LITE_KERNEL_LOG(context, "Type '%s' is not supported by tile.",
                         TfLiteTypeGetName(output->type));
      return kTfLiteError;
  }
  return kTfLiteOk;
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 2);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);
  OpData* op_data = reinterpret_cast<OpData*>(node->user_data);
  op_data->noop = false;

  const TfLiteTensor* input;
  TF_LITE_ENSURE_OK(context, GetInputSafe(context, node, kInputTensor, &input));

  TfLiteTensor* output;
  TF_LITE_ENSURE_OK(context,
                    GetOutputSafe(context, node, kOutputTensor, &output));
  TF_LITE_ENSURE_TYPES_EQ(context, input->type, output->type);

  const TfLiteTensor* multipliers;
  TF_LITE_ENSURE_OK(
      context, GetInputSafe(context, node, kInputMultipliers, &multipliers));
  // Only int32 and int64 multipliers type is supported.
  if (multipliers->type != kTfLiteInt32 && multipliers->type != kTfLiteInt64) {
    TF_LITE_KERNEL_LOG(context,
                       "Multipliers of type '%s' are not supported by tile.",
                       TfLiteTypeGetName(multipliers->type));
    return kTfLiteError;
  }

  if (IsConstantOrPersistentTensor(multipliers)) {
    if (IsConstantOrPersistentTensor(input)) {
      SetTensorToPersistentRo(output);
      TF_LITE_ENSURE_OK(context, ResizeOutput(context, node));
      op_data->noop = true;
      return EvalImpl(context, input, multipliers, output);
    }
    TF_LITE_ENSURE_OK(context, ResizeOutput(context, node));
  } else {
    SetTensorToDynamic(output);
  }
  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input;
  TF_LITE_ENSURE_OK(context, GetInputSafe(context, node, kInputTensor, &input));
  TfLiteTensor* output;
  TF_LITE_ENSURE_OK(context,
                    GetOutputSafe(context, node, kOutputTensor, &output));
  const TfLiteTensor* multipliers;
  TF_LITE_ENSURE_OK(
      context, GetInputSafe(context, node, kInputMultipliers, &multipliers));

  if (reinterpret_cast<OpData*>(node->user_data)->noop) {
    return kTfLiteOk;
  }
  if (IsDynamicTensor(output)) {
    TF_LITE_ENSURE_OK(context, ResizeOutput(context, node));
  }
  return EvalImpl(context, input, multipliers, output);
}

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  return new OpData;
}

void Free(TfLiteContext* context, void* buffer) {
  delete reinterpret_cast<OpData*>(buffer);
}

}  // namespace tile
TfLiteRegistration* Register_TILE() {
  static TfLiteRegistration r = {tile::Init, tile::Free, tile::Prepare,
                                 tile::Eval};
  return &r;
}
}  // namespace builtin
}  // namespace ops
}  // namespace tflite
