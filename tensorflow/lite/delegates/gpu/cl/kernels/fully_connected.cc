/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/delegates/gpu/cl/kernels/fully_connected.h"

#include <string>
#include <utility>

#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"

namespace tflite {
namespace gpu {
namespace cl {
namespace {

// We split vec vec dot (every thread do vec vec dot product in basic
// vec mat mult) on 4 parts to create more threads
// tid.y thread process every 4-th element in vec vec dot
// Good results for ~1024 x 1024 sizes, for other can be written more
// optimized shaders

std::string GetFullyConnectedKernelCode(
    const OperationDef& op_def, const LinearStorage& biases,
    const std::vector<ElementwiseOperation*>& linked_operations,
    const int3& work_group_size) {
  TensorCodeGenerator src_tensor("src_data", WHSPoint{"1", "1", "depthes.x"},
                                 op_def.src_tensors[0]);
  TensorCodeGenerator dst_tensor("dst_data", WHSPoint{"1", "1", "depthes.y"},
                                 op_def.dst_tensors[0]);

  std::string c = GetCommonDefines(op_def.precision);
  switch (op_def.precision) {
    case CalculationsPrecision::F32:
      c += "#define FLT16 float16\n";
      break;
    case CalculationsPrecision::F32_F16:
    case CalculationsPrecision::F16:
      c += "#define FLT16 half16\n";
      break;
  }

  const std::string wg_x = std::to_string(work_group_size.x);
  const std::string wg_y = std::to_string(work_group_size.y);
  c += "__kernel void main_function(\n";
  c += src_tensor.GetDeclaration(AccessType::READ) + ",\n";
  c += "    __global FLT16* filters,      \n";
  c += biases.GetDeclaration();
  c += GetArgsDeclaration(linked_operations);
  c += dst_tensor.GetDeclaration(AccessType::WRITE) + ",\n";
  c += "    int2 depthes                  \n";
  c += ") {\n";
  c += "  int gid = get_global_id(0);\n";
  c += "  bool inside = gid < depthes.y;\n";
  c += "  gid = min(gid, depthes.y - 1);\n";
  c += "  int2 tid = (int2)(get_local_id(0), get_local_id(1));\n";
  c += "  ACCUM_FLT4 s = (ACCUM_FLT4)(0.0f);\n";
  c += "  for (uint c = tid.y; c < depthes.x; c += " + wg_y + ") {\n";
  c += "    FLT4 v = " + src_tensor.ReadWHS("0", "0", "c") + ";\n";
  c += "    FLT16 w = filters[c * depthes.y + gid];\n";
  c += "    s.x += dot(v, w.s0123);\n";
  c += "    s.y += dot(v, w.s4567);\n";
  c += "    s.z += dot(v, w.s89ab);\n";
  c += "    s.w += dot(v, w.scdef);\n";
  c += "  }\n";
  c += "  __local ACCUM_FLT4 temp[" + wg_x + "][" + wg_y + "];\n";
  c += "  temp[tid.x][tid.y] = s;\n";
  c += "  barrier(CLK_LOCAL_MEM_FENCE);\n";
  c += "  if (tid.y == 0 && inside) {\n";
  for (int i = 1; i < work_group_size.y; ++i) {
    c += "    s += temp[tid.x][" + std::to_string(i) + "];\n";
  }
  c += "    FLT4 r0 = TO_FLT4(s) + " + biases.ReadLinearFLT4("gid") + ";\n";
  const LinkingContext context{"r0", "0", "0", "gid"};
  c += PostProcess(linked_operations, context);
  c += "  " + dst_tensor.WriteWHS("r0", "0", "0", "gid") + "\n";
  c += "  }\n";
  c += "}\n";

  return c;
}
}  // namespace

FullyConnected::FullyConnected(const OperationDef& definition)
    : GPUOperation(definition) {}

FullyConnected::FullyConnected(FullyConnected&& kernel)
    : GPUOperation(std::move(kernel)),
      weights_(std::move(kernel.weights_)),
      biases_(std::move(kernel.biases_)),
      kernel_(std::move(kernel.kernel_)),
      work_group_size_(kernel.work_group_size_) {}

FullyConnected& FullyConnected::operator=(FullyConnected&& kernel) {
  if (this != &kernel) {
    weights_ = std::move(kernel.weights_);
    biases_ = std::move(kernel.biases_);
    kernel_ = std::move(kernel.kernel_);
    std::swap(work_group_size_, kernel.work_group_size_);
    GPUOperation::operator=(std::move(kernel));
  }
  return *this;
}

Status FullyConnected::Compile(const CreationContext& creation_context) {
  int wg_width = 32;
  int wg_height = 4;
  int work_items;
  do {
    work_group_size_ = {wg_width, wg_height, 1};
    wg_width /= 2;
    const auto code = GetFullyConnectedKernelCode(
        definition_, biases_, linked_operations_, work_group_size_);
    auto status = creation_context.cache->GetOrCreateCLKernel(
        code, "main_function", *creation_context.context,
        *creation_context.device, &kernel_);
    if (!status.ok()) {
      if (work_group_size_.x == 1) {
        return status;
      } else {
        continue;
      }
    }
    work_items = work_group_size_.x * work_group_size_.y * work_group_size_.z;
  } while (work_items > kernel_.GetMaxWorkGroupSize());
  return OkStatus();
}

Status FullyConnected::AddToQueue(CLCommandQueue* queue) {
  kernel_.ResetBindingCounter();
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(src_[0]->GetMemoryPtr()));
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(weights_.GetMemoryPtr()));
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(biases_.GetMemoryPtr()));
  RETURN_IF_ERROR(BindArgs(&kernel_, linked_operations_));
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(dst_[0]->GetMemoryPtrForWriting()));
  RETURN_IF_ERROR(
      kernel_.SetBytesAuto(int2(src_[0]->Slices(), dst_[0]->Slices())));

  return queue->DispatchImplicit(kernel_, {dst_[0]->Slices(), 1, 1},
                                 work_group_size_);
}

Status CreateFullyConnected(const CreationContext& creation_context,
                            const OperationDef& definition,
                            const FullyConnectedAttributes& attr,
                            FullyConnected* result) {
  *result = FullyConnected(definition);
  RETURN_IF_ERROR(
      result->UploadWeights(attr.weights, creation_context.context));
  LinearStorageCreateInfo create_info;
  create_info.storage_type = LinearStorageType::TEXTURE_2D;
  create_info.data_type = definition.GetDataType();
  create_info.name = "biases";
  create_info.aligned_size = attr.weights.shape.o;
  RETURN_IF_ERROR(CreateLinearStorage(
      create_info, attr.bias, creation_context.context, &result->biases_));
  return OkStatus();
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
