/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/delegates/gpu/metal/selectors/operation_selector.h"

#include <vector>

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/common/gpu_info.h"
#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/task/tensor_desc.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/add.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/concat_xy.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/concat_z.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/elementwise.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/lstm.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/prelu.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/quantize_and_dequantize.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/relu.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/space_to_depth.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/strided_slice.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/transpose.h"
#include "tensorflow/lite/delegates/gpu/common/util.h"
#include "tensorflow/lite/delegates/gpu/common/winograd_util.h"
#include "tensorflow/lite/delegates/gpu/metal/compute_task_descriptor.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/conv.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/depthwise_conv.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/fully_connected.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/max_unpooling.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/mean.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/padding.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/pooling.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/reshape.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/resize.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/softmax.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/transpose_conv.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/winograd.h"
#include "tensorflow/lite/delegates/gpu/metal/selectors/default_selector.h"
#include "tensorflow/lite/delegates/gpu/metal/selectors/subgraph.h"

namespace tflite {
namespace gpu {
namespace metal {
namespace {

std::unique_ptr<ComputeTaskDescriptor> SelectDepthWiseConv(
    const OperationDef& op_def, const DepthwiseConvolution2DAttributes& attr) {
  if (CheckDepthWiseConv3x3Stride1x1Support(attr)) {
    auto gpu_op = DepthWiseConv3x3Stride1x1(op_def, attr);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  } else if (CheckDepthWiseConv3x3Stride2Support(attr)) {
    auto gpu_op = DepthWiseConv3x3Stride2(op_def, attr);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  } else {
    auto gpu_op = DepthWiseConvolution(op_def, attr);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  }
}

absl::Status SelectConcat(const ConcatAttributes& attr,
                          const std::vector<int>& channels,
                          const OperationDef& op_def, const GpuInfo& gpu_info,
                          std::unique_ptr<GPUOperation>* ptr) {
  switch (attr.axis) {
    case Axis::CHANNELS: {
      GPUOperation operation = CreateConcatZ(op_def, channels, gpu_info);
      *ptr = absl::make_unique<GPUOperation>(std::move(operation));
      return absl::OkStatus();
    }
    case Axis::BATCH:
    case Axis::DEPTH:
    case Axis::HEIGHT:
    case Axis::WIDTH: {
      GPUOperation operation = CreateConcatXY(op_def, attr);
      *ptr = absl::make_unique<GPUOperation>(std::move(operation));
      return absl::OkStatus();
    }
    default:
      return absl::UnimplementedError("No concat for this axis.");
  }
}

std::unique_ptr<ComputeTaskDescriptor> SelectConvolutionTransposed(
    const OperationDef& op_def, const ConvolutionTransposedAttributes& attr,
    const GpuInfo& gpu_info) {
  if (CheckConvolutionTransposed4x4Support(attr)) {
    auto gpu_op = ConvolutionTransposed4x4(op_def, attr, gpu_info);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  } else {
    auto gpu_op = ConvolutionTransposed(op_def, attr, gpu_info);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  }
}

std::unique_ptr<GPUOperation> SelectLSTM(const OperationDef& op_def,
                                         const GpuInfo& gpu_info) {
  return absl::make_unique<GPUOperation>(CreateLSTM(op_def, gpu_info));
}

std::unique_ptr<ComputeTaskDescriptor> SelectReshape(
    const OperationDef& op_def, const BHWC& src_shape,
    const ReshapeAttributes& attr) {
  if (src_shape.c % 4 == 0 && attr.new_shape.c % 4 == 0) {
    auto gpu_op = Reshapex4(op_def);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  } else {
    auto gpu_op = Reshape(op_def);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  }
}

std::unique_ptr<ComputeTaskDescriptor> SelectSoftmax(const OperationDef& op_def,
                                                     const BHWC& src_shape,
                                                     const GpuInfo& gpu_info) {
  if (src_shape.w == 1 && src_shape.h == 1) {
    auto gpu_op = Softmax1x1(op_def, gpu_info);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  } else {
    auto gpu_op = Softmax(op_def);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  }
}

void SelectSpaceToDepth(const SpaceToDepthAttributes& attr,
                        const OperationDef& op_def,
                        std::unique_ptr<GPUOperation>* ptr) {
  GPUOperation operation = CreateSpaceToDepth(op_def, attr);
  *ptr = absl::make_unique<GPUOperation>(std::move(operation));
}

void SelectStridedSlice(const SliceAttributes& attr, const OperationDef& op_def,
                        std::unique_ptr<GPUOperation>* ptr) {
  StridedSlice operation = CreateStridedSlice(op_def, attr);
  *ptr = absl::make_unique<StridedSlice>(std::move(operation));
}

void SelectTranspose(const TransposeAttributes& attr,
                     const OperationDef& op_def,
                     std::unique_ptr<GPUOperation>* ptr) {
  GPUOperation operation = CreateTranspose(op_def, attr);
  *ptr = absl::make_unique<GPUOperation>(std::move(operation));
}

std::unique_ptr<ComputeTaskDescriptor> SelectWinograd4x4To36(
    const OperationDef& op_def, const Winograd4x4To36Attributes& attr,
    const GpuInfo& gpu_info) {
  if (gpu_info.IsApple()) {
    auto gpu_op = Winograd4x4To36(op_def, attr);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  } else {
    auto gpu_op = Winograd4x4To36TileX6(op_def, attr);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  }
}

std::unique_ptr<ComputeTaskDescriptor> SelectWinograd36To4x4(
    const OperationDef& op_def, const Winograd36To4x4Attributes& attr,
    const GpuInfo& gpu_info) {
  if (gpu_info.IsApple()) {
    auto gpu_op = Winograd36To4x4(op_def, attr);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  } else {
    auto gpu_op = Winograd36To4x4Tile4x1(op_def, attr);
    return absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  }
}

bool IsRecommendedForWinograd4x4To6x6(const Convolution2DAttributes& attr,
                                      const GpuInfo& gpu_info,
                                      const BHWC& dst_shape) {
  const int tiles_x = DivideRoundUp(dst_shape.w, 4);
  const int tiles_y = DivideRoundUp(dst_shape.h, 4);
  const int total_tiles = tiles_x * tiles_y;
  const int src_depth = DivideRoundUp(attr.weights.shape.i, 4);
  const int dst_depth = DivideRoundUp(attr.weights.shape.o, 4);
  int min_depth = 16;
  const int min_tiles = 32;
  if (total_tiles >= min_tiles * 8) {
    min_depth /= 4;
    min_depth = std::max(min_depth, 8);
  } else if (total_tiles >= min_tiles * 4) {
    min_depth /= 2;
    min_depth = std::max(min_depth, 8);
  }
  const bool recommended_channels =
      src_depth >= min_depth && dst_depth >= min_depth;
  const bool recommended_hw = total_tiles >= min_tiles;
  return recommended_channels && recommended_hw;
}

absl::Status WinogradFromNode(const GpuInfo& gpu_info,
                              const std::vector<Value*>& inputs,
                              const std::vector<Value*>& outputs,
                              const OperationDef& op_def,
                              const BHWC& input_shape, const BHWC& output_shape,
                              const Convolution2DAttributes& attr,
                              GPUOperationsSubgraph* gpu_subgraph) {
  if (!IsSuitableForWinograd4x4To6x6(attr)) {
    return absl::UnimplementedError("No implementation for this case.");
  }
  if (!IsRecommendedForWinograd4x4To6x6(attr, gpu_info, output_shape)) {
    return absl::UnimplementedError("Not recommended for this case.");
  }

  const int tiles_x = DivideRoundUp(output_shape.w, 4);
  const int tiles_y = DivideRoundUp(output_shape.h, 4);
  const BHWC shape_0{input_shape.b, 36, tiles_x * tiles_y, input_shape.c};
  const BHWC shape_1{input_shape.b, 36, tiles_x * tiles_y, output_shape.c};
  TensorDescriptor tensor_desc = op_def.src_tensors[0];
  gpu_subgraph->new_tensors = {{shape_0, tensor_desc}, {shape_1, tensor_desc}};
  gpu_subgraph->operations.clear();
  gpu_subgraph->operations.resize(3);

  OperationDef winograd_up_def;
  winograd_up_def.precision = op_def.precision;
  winograd_up_def.src_tensors.push_back(op_def.src_tensors[0]);
  winograd_up_def.dst_tensors.push_back(op_def.src_tensors[0]);
  auto& winograd_up = gpu_subgraph->operations[0];
  Winograd4x4To36Attributes wino_up_attr;
  wino_up_attr.padding = attr.padding;
  winograd_up.task_desc =
      SelectWinograd4x4To36(winograd_up_def, wino_up_attr, gpu_info);
  winograd_up.input_ids = {static_cast<int>(inputs[0]->id)};
  winograd_up.output_ids = {-1};

  OperationDef conv_def;
  conv_def.precision = op_def.precision;
  conv_def.src_tensors.push_back(op_def.src_tensors[0]);
  conv_def.dst_tensors.push_back(op_def.src_tensors[0]);
  auto& conv = gpu_subgraph->operations[1];
  conv.input_ids = {-1};
  conv.output_ids = {-2};
  auto gpu_op = ConvolutionWino4x4To6x6(conv_def, shape_1, attr, gpu_info);
  conv.task_desc = absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
  OperationDef winograd_down_def;
  winograd_down_def.precision = op_def.precision;
  winograd_down_def.src_tensors.push_back(op_def.src_tensors[0]);
  winograd_down_def.dst_tensors.push_back(op_def.dst_tensors[0]);
  auto& winograd_down = gpu_subgraph->operations[2];
  winograd_down.input_ids = {-2};
  winograd_down.output_ids = {static_cast<int>(outputs[0]->id)};
  Winograd36To4x4Attributes wino_down_attr;
  wino_down_attr.output_shape = outputs[0]->tensor.shape;
  wino_down_attr.biases = attr.bias;
  winograd_down.task_desc =
      SelectWinograd36To4x4(winograd_down_def, wino_down_attr, gpu_info);
  return absl::OkStatus();
}

}  // namespace

absl::Status GPUOperationFromNode(const GpuInfo& gpu_info,
                                  const OperationDef& op_def,
                                  const std::vector<Value*>& inputs,
                                  const std::vector<Value*>& outputs,
                                  const Node& node,
                                  GPUOperationsSubgraph* gpu_subgraph) {
  GPUOperationWithRefs* gpu_operation =
      InitSingleOpSubgraph(inputs, outputs, gpu_subgraph);
  auto op_type = OperationTypeFromString(node.operation.type);
  switch (op_type) {
    case OperationType::ADD: {
      if (inputs.size() == 2 &&
          (inputs[0]->tensor.shape.c == inputs[1]->tensor.shape.c ||
           inputs[1]->tensor.shape.c == 1)) {
        GPUOperation operation =
            CreateElementwiseTwoInput(op_def, op_type, inputs[1]->tensor.shape);
        gpu_operation->operation =
            absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      } else if (inputs.size() >= 2) {
        auto output = outputs[0];
        std::vector<int> channels(inputs.size());
        for (int i = 0; i < inputs.size(); ++i) {
          channels[i] = inputs[i]->tensor.shape.c;
        }
        GPUOperation operation =
            CreateAdd(op_def, channels, output->tensor.shape.c);
        gpu_operation->operation =
            absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      } else if (inputs.size() == 1 && node.operation.attributes.has_value()) {
        auto attr =
            absl::any_cast<ElementwiseAttributes>(node.operation.attributes);
        GPUOperation operation =
            CreateElementwise(gpu_info, op_def, op_type, attr);
        gpu_operation->operation =
            absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      }
      return absl::UnimplementedError(absl::StrCat(
          "No support of ", node.operation.type, " with this parameters"));
    }
    case OperationType::CONCAT: {
      auto attr = absl::any_cast<ConcatAttributes>(node.operation.attributes);
      std::vector<int> channels(inputs.size());
      for (int i = 0; i < inputs.size(); ++i) {
        channels[i] = inputs[i]->tensor.shape.c;
      }
      return SelectConcat(attr, channels, op_def, gpu_info,
                          &gpu_operation->operation);
    }
    case OperationType::CONVOLUTION_2D: {
      if (inputs.size() != 1) {
        return absl::UnimplementedError(
            "Convolution does not support more than 1 runtime tensor");
      }
      auto attr =
          absl::any_cast<Convolution2DAttributes>(node.operation.attributes);
      auto input_shape = inputs[0]->tensor.shape;
      auto output_shape = outputs[0]->tensor.shape;
      if (WinogradFromNode(gpu_info, inputs, outputs, op_def, input_shape,
                           output_shape, attr, gpu_subgraph)
              .ok()) {
        return absl::OkStatus();
      } else {
        auto gpu_op = ConvolutionGeneric(op_def, output_shape, attr, gpu_info);
        gpu_operation->task_desc =
            absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
      }
      break;
    }
    case OperationType::CONVOLUTION_TRANSPOSED:
      if (inputs.size() != 1) {
        return absl::UnimplementedError(
            "Convolution Transposed does not support more than 1 runtime "
            "tensor");
      }
      gpu_operation->task_desc = SelectConvolutionTransposed(
          op_def,
          absl::any_cast<ConvolutionTransposedAttributes>(
              node.operation.attributes),
          gpu_info);
      break;
    case OperationType::DEPTHWISE_CONVOLUTION:
      if (inputs.size() != 1) {
        return absl::UnimplementedError(
            "DepthWise Convolution does not support more than 1 runtime "
            "tensor");
      }
      gpu_operation->task_desc = SelectDepthWiseConv(
          op_def, absl::any_cast<DepthwiseConvolution2DAttributes>(
                      node.operation.attributes));
      break;
    case OperationType::FULLY_CONNECTED: {
      auto gpu_op = FullyConnected(
          op_def,
          absl::any_cast<FullyConnectedAttributes>(node.operation.attributes),
          gpu_info);
      gpu_operation->task_desc =
          absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
      break;
    }
    case OperationType::LSTM: {
      gpu_operation->operation = SelectLSTM(op_def, gpu_info);
      return absl::OkStatus();
    }
    case OperationType::MAX_UNPOOLING_2D: {
      auto gpu_op = MaxUnpooling(
          op_def,
          absl::any_cast<MaxUnpooling2DAttributes>(node.operation.attributes));
      gpu_operation->task_desc =
          absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
      break;
    }
    case OperationType::MEAN: {
      auto attr = absl::any_cast<MeanAttributes>(node.operation.attributes);
      if (attr.dims != std::set<Axis>({Axis::HEIGHT, Axis::WIDTH})) {
        return absl::UnimplementedError("Mean supports HW axis only in Metal");
      }
      auto gpu_op = Mean(op_def, attr);
      gpu_operation->task_desc =
          absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
      break;
    }
    case OperationType::PAD: {
      auto attr = absl::any_cast<PadAttributes>(node.operation.attributes);
      if (attr.appended.b != 0 || attr.prepended.b != 0) {
        return absl::UnimplementedError("Padding for BATCH is not supported.");
      }
      auto gpu_op = Padding(op_def, attr);
      gpu_operation->task_desc =
          absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
      break;
    }
    case OperationType::POOLING_2D: {
      auto attr =
          absl::any_cast<Pooling2DAttributes>(node.operation.attributes);
      auto pooling_op_def = op_def;
      pooling_op_def.dst_tensors = {op_def.dst_tensors[0]};
      auto gpu_op = Pooling(op_def, attr, false);
      gpu_subgraph->operations[0].task_desc =
          absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
      gpu_subgraph->operations[0].input_ids = {static_cast<int>(inputs[0]->id)};
      gpu_subgraph->operations[0].output_ids = {
          static_cast<int>(outputs[0]->id)};
      if (attr.type == PoolingType::MAX && attr.output_indices) {
        gpu_subgraph->operations.push_back({});
        auto gpu_ind_op = Pooling(op_def, attr, true);
        gpu_subgraph->operations[1].task_desc =
            absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_ind_op));
        gpu_subgraph->operations[1].input_ids = {
            static_cast<int>(inputs[0]->id)};
        gpu_subgraph->operations[1].output_ids = {
            static_cast<int>(outputs[1]->id)};
      }
      break;
    }
    case OperationType::PRELU: {
      auto attr = absl::any_cast<PReLUAttributes>(node.operation.attributes);
      gpu_operation->operation =
          absl::make_unique<GPUOperation>(CreatePReLU(gpu_info, op_def, attr));
      return absl::OkStatus();
    }
    case OperationType::RELU: {
      auto attr = absl::any_cast<ReLUAttributes>(node.operation.attributes);
      gpu_operation->operation =
          absl::make_unique<GPUOperation>(CreateReLU(op_def, attr));
      return absl::OkStatus();
    }
    case OperationType::QUANTIZE_AND_DEQUANTIZE: {
      auto attr = absl::any_cast<QuantizeAndDequantizeAttributes>(
          node.operation.attributes);
      gpu_operation->operation = absl::make_unique<GPUOperation>(
          CreateQuantizeAndDequantize(op_def, attr));
      return absl::OkStatus();
    }
    case OperationType::RESHAPE: {
      const auto src_shape = inputs[0]->tensor.shape;
      gpu_operation->task_desc = SelectReshape(
          op_def, src_shape,
          absl::any_cast<ReshapeAttributes>(node.operation.attributes));
      break;
    }
    case OperationType::RESIZE: {
      auto gpu_op =
          Resize(op_def,
                 absl::any_cast<Resize2DAttributes>(node.operation.attributes));
      gpu_operation->task_desc =
          absl::make_unique<ComputeTaskDescriptor>(std::move(gpu_op));
      break;
    }
    case OperationType::SLICE: {
      auto attr = absl::any_cast<SliceAttributes>(node.operation.attributes);
      SelectStridedSlice(attr, op_def, &gpu_operation->operation);
      return absl::OkStatus();
    }
    case OperationType::SOFTMAX: {
      auto attr = absl::any_cast<SoftmaxAttributes>(node.operation.attributes);
      if (attr.axis != Axis::CHANNELS) {
        return absl::UnimplementedError(
            "Softmax supports only CHANNELS dimension");
      }
      const auto src_shape = inputs[0]->tensor.shape;
      gpu_operation->task_desc = SelectSoftmax(op_def, src_shape, gpu_info);
      break;
    }
    case OperationType::SPACE_TO_DEPTH: {
      auto attr =
          absl::any_cast<SpaceToDepthAttributes>(node.operation.attributes);
      SelectSpaceToDepth(attr, op_def, &gpu_operation->operation);
      return absl::OkStatus();
    }
    case OperationType::TRANSPOSE: {
      auto attr =
          absl::any_cast<TransposeAttributes>(node.operation.attributes);
      SelectTranspose(attr, op_def, &gpu_operation->operation);
      return absl::OkStatus();
    }
    case OperationType::ABS:
    case OperationType::COPY:
    case OperationType::COS:
    case OperationType::ELU:
    case OperationType::EXP:
    case OperationType::HARD_SWISH:
    case OperationType::LOG:
    case OperationType::NEG:
    case OperationType::RSQRT:
    case OperationType::SIGMOID:
    case OperationType::SIN:
    case OperationType::SQRT:
    case OperationType::SQUARE:
    case OperationType::TANH: {
      GPUOperation operation =
          CreateElementwiseOneInput(gpu_info, op_def, op_type);
      gpu_operation->operation =
          absl::make_unique<GPUOperation>(std::move(operation));
      return absl::OkStatus();
    }
    case OperationType::DIV:
    case OperationType::EQUAL:
    case OperationType::GREATER:
    case OperationType::GREATER_EQUAL:
    case OperationType::LESS:
    case OperationType::LESS_EQUAL:
    case OperationType::MAXIMUM:
    case OperationType::MINIMUM:
    case OperationType::MUL:
    case OperationType::NOT_EQUAL:
    case OperationType::POW:
    case OperationType::SQUARED_DIFF:
    case OperationType::SUB: {
      if (inputs.size() == 2) {
        GPUOperation operation =
            CreateElementwiseTwoInput(op_def, op_type, inputs[1]->tensor.shape);
        gpu_operation->operation =
            absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      } else if (inputs.size() == 1 && node.operation.attributes.has_value()) {
        auto attr =
            absl::any_cast<ElementwiseAttributes>(node.operation.attributes);
        GPUOperation operation =
            CreateElementwise(gpu_info, op_def, op_type, attr);
        gpu_operation->operation =
            absl::make_unique<GPUOperation>(std::move(operation));
        return absl::OkStatus();
      }
      return absl::UnimplementedError(absl::StrCat(
          "No support of ", node.operation.type, " with this parameters"));
    }
    case OperationType::BATCH_NORMALIZATION:
    case OperationType::BATCH_TO_SPACE:
    case OperationType::BATCHED_MATMUL:
    case OperationType::CONSTANT:
    // TODO(b/162763635): implement MeanStddevNormalization for Metal.
    case OperationType::MEAN_STDDEV_NORMALIZATION:
    case OperationType::REDUCE_MAXIMUM:
    case OperationType::REDUCE_MINIMUM:
    case OperationType::REDUCE_PRODUCT:
    case OperationType::REDUCE_SUM:
    case OperationType::SPACE_TO_BATCH:
      return absl::UnimplementedError("Unsupported op: " + node.operation.type);
    default:
      return SelectDefault(gpu_info, op_def, inputs, outputs, node,
                           gpu_subgraph);
  }
  return absl::OkStatus();
}

}  // namespace metal
}  // namespace gpu
}  // namespace tflite
