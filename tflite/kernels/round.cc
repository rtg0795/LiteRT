/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tflite/kernels/internal/reference/round.h"

#include "Eigen/Core"
#include "tflite/core/c/common.h"
#include "tflite/kernels/internal/optimized/optimized_ops.h"
#include "tflite/kernels/internal/tensor.h"
#include "tflite/kernels/internal/tensor_ctypes.h"
#include "tflite/kernels/kernel_util.h"

namespace tflite {
namespace ops {
namespace builtin {
namespace round {

constexpr int kInputTensor = 0;
constexpr int kOutputTensor = 0;

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input;
  TF_LITE_ENSURE_OK(context, GetInputSafe(context, node, kInputTensor, &input));
  TfLiteTensor* output;
  TF_LITE_ENSURE_OK(context,
                    GetOutputSafe(context, node, kOutputTensor, &output));
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 1);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);
  if (input->type != kTfLiteFloat32 && input->type != kTfLiteFloat16 &&
      input->type != kTfLiteBFloat16) {
    TF_LITE_KERNEL_LOG(context, "Type '%s' is not supported by round.",
                       TfLiteTypeGetName(input->type));
    return kTfLiteError;
  }
  output->type = input->type;
  TfLiteIntArray* output_size = TfLiteIntArrayCopy(input->dims);
  return context->ResizeTensor(context, output, output_size);
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input;
  TF_LITE_ENSURE_OK(context, GetInputSafe(context, node, kInputTensor, &input));
  TfLiteTensor* output;
  TF_LITE_ENSURE_OK(context,
                    GetOutputSafe(context, node, kOutputTensor, &output));
  switch (output->type) {
    case kTfLiteFloat32: {
      optimized_ops::Round<float>(
          GetTensorShape(input), GetTensorData<float>(input),
          GetTensorShape(output), GetTensorData<float>(output));
      break;
    }
    case kTfLiteFloat16: {
      optimized_ops::Round<Eigen::half>(
          GetTensorShape(input), GetTensorData<Eigen::half>(input),
          GetTensorShape(output), GetTensorData<Eigen::half>(output));
      break;
    }
    case kTfLiteBFloat16: {
      optimized_ops::Round<Eigen::bfloat16>(
          GetTensorShape(input), GetTensorData<Eigen::bfloat16>(input),
          GetTensorShape(output), GetTensorData<Eigen::bfloat16>(output));
      break;
    }
    default: {
      TF_LITE_KERNEL_LOG(context, "Type '%s' is not supported by round.",
                         TfLiteTypeGetName(output->type));
      return kTfLiteError;
    }
  }

  return kTfLiteOk;
}
}  // namespace round

TfLiteRegistration* Register_ROUND() {
  static TfLiteRegistration r = {/*init=*/nullptr,
                                 /*free=*/nullptr, round::Prepare, round::Eval};
  return &r;
}

}  // namespace builtin
}  // namespace ops
}  // namespace tflite
