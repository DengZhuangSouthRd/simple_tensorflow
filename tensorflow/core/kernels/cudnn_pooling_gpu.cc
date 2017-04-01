/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#define USE_EIGEN_TENSOR
#define EIGEN_USE_THREADS

#include <array>

#include "tensorflow/core/kernels/cudnn_pooling_gpu.h"
#include "tensorflow/core/kernels/conv_2d.h"
#include "tensorflow/core/kernels/conv_3d.h"
#include "tensorflow/core/kernels/conv_ops_gpu.h"

typedef Eigen::GpuDevice GPUDevice;

namespace tensorflow {

#if GOOGLE_CUDA

template <typename T>
void DnnPooling3dOp<T>::Compute(
    OpKernelContext* context,
    perftools::gputools::dnn::PoolingMode pooling_mode,
    const std::array<int64, 3>& window, const std::array<int64, 3>& stride,
    const std::array<int64, 3>& padding, const Tensor& tensor_in,
    Tensor* output) {
  const auto in_shape = tensor_in.shape();
  const auto out_shape = output->shape();

  const int64 in_batch = in_shape.dim_size(0);
  const int64 in_features = in_shape.dim_size(4);

  Tensor transformed_input;
  OP_REQUIRES_OK(context, context->allocate_temp(
                              DataTypeToEnum<T>::value,
                              {in_shape.dim_size(0), in_shape.dim_size(4),
                               in_shape.dim_size(1), in_shape.dim_size(2),
                               in_shape.dim_size(3)},
                              &transformed_input));
  functor::NHWCToNCHW<GPUDevice, T, 5>()(context->eigen_device<GPUDevice>(),
                                         tensor_in.tensor<T, 5>(),
                                         transformed_input.tensor<T, 5>());
  Tensor transformed_output;
  OP_REQUIRES_OK(context, context->allocate_temp(
                              DataTypeToEnum<T>::value,
                              {out_shape.dim_size(0), out_shape.dim_size(4),
                               out_shape.dim_size(1), out_shape.dim_size(2),
                               out_shape.dim_size(3)},
                              &transformed_output));

  perftools::gputools::dnn::PoolingDescriptor pooling_desc(3);
  pooling_desc.set_pooling_mode(pooling_mode);
  perftools::gputools::dnn::BatchDescriptor input_desc(3);
  input_desc.set_count(in_batch)
      .set_feature_map_count(in_features)
      .set_layout(perftools::gputools::dnn::DataLayout::kBatchDepthYX);
  perftools::gputools::dnn::BatchDescriptor output_desc(3);
  output_desc.set_count(in_batch)
      .set_feature_map_count(in_features)
      .set_layout(perftools::gputools::dnn::DataLayout::kBatchDepthYX);
  for (size_t i = 0; i < window.size(); ++i) {
    const auto dim_i = static_cast<perftools::gputools::dnn::DimIndex>(i);
    pooling_desc.set_window(dim_i, window[i]);
    pooling_desc.set_stride(dim_i, stride[i]);
    pooling_desc.set_padding(dim_i, padding[i]);
    input_desc.set_spatial_dim(dim_i, in_shape.dim_size(3 - i));
    output_desc.set_spatial_dim(dim_i, out_shape.dim_size(3 - i));
  }

  auto input_data = AsDeviceMemory(transformed_input.template flat<T>().data(),
                                   transformed_input.template flat<T>().size());
  auto output_data =
      AsDeviceMemory(transformed_output.template flat<T>().data(),
                     transformed_output.template flat<T>().size());

  auto* stream = context->op_device_context()->stream();
  OP_REQUIRES(context, stream, errors::Internal("No GPU stream available."));

  bool status = stream
                    ->ThenPoolForward(pooling_desc, input_desc, input_data,
                                      output_desc, &output_data)
                    .ok();
  OP_REQUIRES(context, status,
              errors::Internal("cudnn PoolForward launch failed"));

  auto toConstTensor = [](const Tensor& x) -> const Tensor { return x; };
  functor::NCHWToNHWC<GPUDevice, T, 5>()(
      context->eigen_device<GPUDevice>(),
      toConstTensor(transformed_output).template tensor<T, 5>(),
      output->tensor<T, 5>());
}

template <typename T>
void DnnPooling3dGradOp<T>::Compute(
    OpKernelContext* context,
    perftools::gputools::dnn::PoolingMode pooling_mode,
    const std::array<int64, 3>& window, const std::array<int64, 3>& stride,
    const std::array<int64, 3>& padding,
    const std::array<int64, 3>& output_size, const Tensor& out_backprop,
    const TensorShape& tensor_in_shape, const Tensor* tensor_in,
    const Tensor* tensor_out, Tensor* input_backprop) {
  CHECK((pooling_mode != perftools::gputools::dnn::PoolingMode::kMaximum) ||
        (tensor_in && tensor_out))
      << "For MaxPoolGrad, both tensor_in and tensor_out needs to be "
         "specified";

  const int64 in_batch = tensor_in_shape.dim_size(0);
  const int64 in_features = tensor_in_shape.dim_size(4);

  Tensor transformed_input;
  TensorShape transformed_input_shape = {
      in_batch, in_features, tensor_in_shape.dim_size(1),
      tensor_in_shape.dim_size(2), tensor_in_shape.dim_size(3)};
  OP_REQUIRES_OK(context, context->allocate_temp(DataTypeToEnum<T>::value,
                                                 transformed_input_shape,
                                                 &transformed_input));
  Tensor transformed_output;
  TensorShape transformed_output_shape = {
      out_backprop.dim_size(0), out_backprop.dim_size(4),
      out_backprop.dim_size(1), out_backprop.dim_size(2),
      out_backprop.dim_size(3)};
  OP_REQUIRES_OK(context, context->allocate_temp(DataTypeToEnum<T>::value,
                                                 transformed_output_shape,
                                                 &transformed_output));
  Tensor transformed_input_backprop;
  OP_REQUIRES_OK(context, context->allocate_temp(DataTypeToEnum<T>::value,
                                                 transformed_input_shape,
                                                 &transformed_input_backprop));
  Tensor transformed_output_backprop;
  OP_REQUIRES_OK(context, context->allocate_temp(DataTypeToEnum<T>::value,
                                                 transformed_output_shape,
                                                 &transformed_output_backprop));
  if (tensor_in != nullptr) {
    functor::NHWCToNCHW<GPUDevice, T, 5>()(context->eigen_device<GPUDevice>(),
                                           tensor_in->tensor<T, 5>(),
                                           transformed_input.tensor<T, 5>());
  }
  if (tensor_out != nullptr) {
    functor::NHWCToNCHW<GPUDevice, T, 5>()(context->eigen_device<GPUDevice>(),
                                           tensor_out->tensor<T, 5>(),
                                           transformed_output.tensor<T, 5>());
  }
  functor::NHWCToNCHW<GPUDevice, T, 5>()(
      context->eigen_device<GPUDevice>(), out_backprop.tensor<T, 5>(),
      transformed_output_backprop.tensor<T, 5>());

  perftools::gputools::dnn::PoolingDescriptor pooling_desc(3);
  pooling_desc.set_pooling_mode(pooling_mode);

  perftools::gputools::dnn::BatchDescriptor orig_output_desc(3);
  orig_output_desc.set_count(in_batch)
      .set_feature_map_count(in_features)
      .set_layout(perftools::gputools::dnn::DataLayout::kBatchDepthYX);

  perftools::gputools::dnn::BatchDescriptor orig_input_desc(3);
  orig_input_desc.set_count(in_batch)
      .set_feature_map_count(in_features)
      .set_layout(perftools::gputools::dnn::DataLayout::kBatchDepthYX);

  for (size_t i = 0; i < window.size(); ++i) {
    const auto dim_i = static_cast<perftools::gputools::dnn::DimIndex>(i);
    pooling_desc.set_window(dim_i, window[i]);
    pooling_desc.set_stride(dim_i, stride[i]);
    pooling_desc.set_padding(dim_i, padding[i]);
    orig_input_desc.set_spatial_dim(dim_i, tensor_in_shape.dim_size(3 - i));
    orig_output_desc.set_spatial_dim(dim_i, output_size[i]);
  }

  auto orig_output_data =
      AsDeviceMemory(transformed_output.template flat<T>().data(),
                     transformed_output.template flat<T>().size());
  auto orig_input_data =
      AsDeviceMemory(transformed_input.template flat<T>().data(),
                     transformed_input.template flat<T>().size());
  auto output_backprop_data =
      AsDeviceMemory(transformed_output_backprop.template flat<T>().data(),
                     transformed_output_backprop.template flat<T>().size());
  auto input_backprop_data =
      AsDeviceMemory(transformed_input_backprop.template flat<T>().data(),
                     transformed_input_backprop.template flat<T>().size());

  auto* stream = context->op_device_context()->stream();
  OP_REQUIRES(context, stream, errors::Internal("No GPU stream available."));

  bool status =
      stream
          ->ThenPoolBackward(pooling_desc, orig_input_desc, orig_input_data,
                             orig_output_desc, orig_output_data,
                             output_backprop_data, &input_backprop_data)
          .ok();
  OP_REQUIRES(context, status,
              errors::Internal("cudnn PoolBackward launch failed"));

  auto toConstTensor = [](const Tensor& x) -> const Tensor { return x; };
  functor::NCHWToNHWC<GPUDevice, T, 5>()(
      context->eigen_device<GPUDevice>(),
      toConstTensor(transformed_input_backprop).template tensor<T, 5>(),
      input_backprop->tensor<T, 5>());
}

template class DnnPooling3dOp<float>;
template class DnnPooling3dGradOp<float>;

#endif  // GOOGLE_CUDA

}  // namespace tensorflow
