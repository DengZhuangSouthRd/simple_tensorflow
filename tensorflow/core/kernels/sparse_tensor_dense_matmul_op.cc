/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

// See docs in ../ops/math_ops.cc.

#define EIGEN_USE_THREADS

#include "tensorflow/core/kernels/sparse_tensor_dense_matmul_op.h"

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/kernels/fill_functor.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

template <typename Device, typename T>
class SparseTensorDenseMatMulOp : public OpKernel {
 public:
  explicit SparseTensorDenseMatMulOp(OpKernelConstruction* ctx)
      : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("adjoint_a", &adjoint_a_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("adjoint_b", &adjoint_b_));
  }

  void Compute(OpKernelContext* ctx) override {
    const Tensor* a_indices;
    const Tensor* a_values;
    const Tensor* a_shape;
    const Tensor* b;
    OP_REQUIRES_OK(ctx, ctx->input("a_indices", &a_indices));
    OP_REQUIRES_OK(ctx, ctx->input("a_values", &a_values));
    OP_REQUIRES_OK(ctx, ctx->input("a_shape", &a_shape));
    OP_REQUIRES_OK(ctx, ctx->input("b", &b));

    // Check that the dimensions of the two matrices are valid.
    OP_REQUIRES(ctx, TensorShapeUtils::IsMatrix(b->shape()),
                errors::InvalidArgument("Tensor 'b' is not a matrix"));

    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(a_shape->shape()),
                errors::InvalidArgument("Tensor 'a_shape' is not a vector"));

    OP_REQUIRES(
        ctx, a_shape->NumElements() == 2,
        errors::InvalidArgument("Tensor 'a_shape' must have 2 elements"));

    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(a_values->shape()),
                errors::InvalidArgument("Tensor 'a_values' is not a vector"));

    OP_REQUIRES(ctx, TensorShapeUtils::IsMatrix(a_indices->shape()),
                errors::InvalidArgument("Tensor 'a_indices' is not a matrix"));

    OP_REQUIRES(ctx, a_indices->shape().dim_size(0) == a_values->NumElements(),
                errors::InvalidArgument("Number of rows of a_indices does not "
                                        "match number of entries in a_values"));

    OP_REQUIRES(
        ctx, a_indices->shape().dim_size(1) == a_shape->NumElements(),
        errors::InvalidArgument("Number of columns of a_indices does not match "
                                "number of entries in a_shape"));

    auto a_shape_t = a_shape->vec<int64>();
    const int64 outer_left = (adjoint_a_) ? a_shape_t(1) : a_shape_t(0);
    const int64 outer_right =
        (adjoint_b_) ? b->shape().dim_size(0) : b->shape().dim_size(1);
    const int64 inner_left = (adjoint_a_) ? a_shape_t(0) : a_shape_t(1);
    const int64 inner_right =
        (adjoint_b_) ? b->shape().dim_size(1) : b->shape().dim_size(0);

    OP_REQUIRES(
        ctx, inner_right == inner_left,
        errors::InvalidArgument(
            "Cannot multiply A and B because inner dimension does not match: ",
            inner_left, " vs. ", inner_right,
            ".  Did you forget a transpose?  "
            "Dimensions of A: [",
            a_shape_t(0), ", ", a_shape_t(1), ").  Dimensions of B: ",
            b->shape().DebugString()));

    TensorShape out_shape({outer_left, outer_right});
    Tensor* out = nullptr;
    OP_REQUIRES_OK(ctx, ctx->allocate_output(0, out_shape, &out));

    if (out->NumElements() == 0) {
      // If a has shape [0, x] or b has shape [x, 0], the output shape
      // is a 0-element matrix, so there is nothing to do.
      return;
    }

    if (a_values->NumElements() == 0 || b->NumElements() == 0) {
      // If a has shape [x, 0] and b has shape [0, y], the
      // output shape is [x, y] where x and y are non-zero, so we fill
      // the output with zeros.
      functor::SetZeroFunctor<Device, T> f;
      f(ctx->eigen_device<Device>(), out->flat<T>());
      return;
    }

    Tensor scratch;

    if (std::is_same<Device, GPUDevice>::value) {
      // The GPU implementation is optimized to use 32 bit indexing, so
      // give a friendly error to the programmer early on if they exceed.
      OP_REQUIRES(
          ctx,
          FastBoundsCheck(inner_left, std::numeric_limits<int>::max()) &&
              FastBoundsCheck(inner_right, std::numeric_limits<int>::max()) &&
              FastBoundsCheck(outer_left, std::numeric_limits<int>::max()) &&
              FastBoundsCheck(outer_right, std::numeric_limits<int>::max()) &&
              FastBoundsCheck(b->NumElements(),
                              std::numeric_limits<int>::max()) &&
              FastBoundsCheck(out->NumElements(),
                              std::numeric_limits<int>::max()) &&
              FastBoundsCheck(a_values->NumElements(),
                              std::numeric_limits<int>::max()),
          errors::InvalidArgument("Cannot use GPU for > 2^31 entry inputs"));
      const int nnz = static_cast<const int>(a_values->NumElements());
      // Need nnz length vec scratch space on the GPU.
      OP_REQUIRES_OK(ctx, ctx->allocate_temp(DataTypeToEnum<T>::value,
                                             TensorShape({nnz}), &scratch));
    } else {
      // We don't need scratch space on the CPU.
      OP_REQUIRES_OK(ctx, ctx->allocate_temp(DataTypeToEnum<T>::value,
                                             TensorShape({0}), &scratch));
    }

#define MAYBE_ADJOINT(ADJ_A, ADJ_B)                                            \
  if (adjoint_a_ == ADJ_A && adjoint_b_ == ADJ_B) {                            \
    functor::SparseTensorDenseMatMulFunctor<Device, T, ADJ_A, ADJ_B>::Compute( \
        ctx->eigen_device<Device>(), out->matrix<T>(),                         \
        a_indices->matrix<int64>(), a_values->vec<T>(), b->matrix<T>(),        \
        scratch.vec<T>());                                                     \
  }

    MAYBE_ADJOINT(false, false);
    MAYBE_ADJOINT(false, true);
    MAYBE_ADJOINT(true, false);
    MAYBE_ADJOINT(true, true);

#undef MAYBE_ADJOINT
  }

 private:
  bool adjoint_a_;
  bool adjoint_b_;
};

#define REGISTER_CPU(T)                                   \
  REGISTER_KERNEL_BUILDER(Name("SparseTensorDenseMatMul") \
                              .Device(DEVICE_CPU)         \
                              .TypeConstraint<T>("T")     \
                              .HostMemory("a_shape"),     \
                          SparseTensorDenseMatMulOp<CPUDevice, T>);

REGISTER_CPU(float);
REGISTER_CPU(double);
REGISTER_CPU(int32);
REGISTER_CPU(complex64);
REGISTER_CPU(complex128);

#if GOOGLE_CUDA

namespace functor {
#define DECLARE_GPU_SPEC(T, ADJ_A, ADJ_B)                                    \
  template <>                                                                \
  void SparseTensorDenseMatMulFunctor<GPUDevice, T, ADJ_A, ADJ_B>::Compute(  \
      const GPUDevice& d, typename TTypes<T>::Matrix out,                    \
      TTypes<int64>::ConstMatrix a_indices,                                  \
      typename TTypes<T>::ConstVec a_values,                                 \
      typename TTypes<T>::ConstMatrix b, typename TTypes<T>::Vec scratch);   \
  extern template struct SparseTensorDenseMatMulFunctor<GPUDevice, T, ADJ_A, \
                                                        ADJ_B>;

#define DECLARE_ADJOINT_GPU_SPEC(T) \
  DECLARE_GPU_SPEC(T, false, false) \
  DECLARE_GPU_SPEC(T, false, true)  \
  DECLARE_GPU_SPEC(T, true, false)  \
  DECLARE_GPU_SPEC(T, true, true)

DECLARE_ADJOINT_GPU_SPEC(float);
#undef DECLARE_ADJOINT_GPU_SPEC
#undef DECLARE_GPU_SPEC

}  // namespace functor

#define REGISTER_GPU(T)                                   \
  REGISTER_KERNEL_BUILDER(Name("SparseTensorDenseMatMul") \
                              .Device(DEVICE_GPU)         \
                              .TypeConstraint<T>("T")     \
                              .HostMemory("a_shape"),     \
                          SparseTensorDenseMatMulOp<GPUDevice, T>);

REGISTER_GPU(float);
#undef REGISTER_GPU
#endif  // GOOGLE_CUDA

namespace functor {

template <typename T, bool ADJ_A, bool ADJ_B>
struct SparseTensorDenseMatMulFunctor<CPUDevice, T, ADJ_A, ADJ_B> {
  // Vectorize certain operations above this size.
  static const std::size_t kNumVectorize = 32;

  static void Compute(const CPUDevice& d, typename TTypes<T>::Matrix out,
                      TTypes<int64>::ConstMatrix a_indices,
                      typename TTypes<T>::ConstVec a_values,
                      typename TTypes<T>::ConstMatrix b,
                      typename TTypes<T>::Vec scratch) {
    const std::size_t nnz = a_values.size();
    const std::size_t rhs_right = (ADJ_B ? b.dimension(0) : b.dimension(1));
    const std::size_t lhs_right = (ADJ_B ? b.dimension(1) : b.dimension(0));
    const int lhs_index_a = ADJ_A ? 1 : 0;
    const int rhs_index_a = ADJ_A ? 0 : 1;

    out.setZero();

    // TODO(ebrevdo): After many failed experiments, can't find a multi-threaded
    // approach that achieves the performance of the single threaded
    // one.  Perhaps Eigen threadpool implementation is just too slow?

    if (rhs_right < kNumVectorize) {
      // Disable vectorization if the RHS of output is too small
      auto maybe_adjoint_b = MaybeAdjoint<decltype(b), ADJ_B>(b);
      for (std::size_t i = 0; i < nnz; ++i) {
        const int64 m = internal::SubtleMustCopy(a_indices(i, lhs_index_a));
        const int64 k = internal::SubtleMustCopy(a_indices(i, rhs_index_a));
        CHECK_LT(k, lhs_right);
        CHECK_LT(m, out.dimension(0));
        const T a_value = ADJ_A ? MaybeConj(a_values(i)) : a_values(i);
        for (std::size_t n = 0; n < rhs_right; ++n) {
          const T b_value = maybe_adjoint_b(k, n);
          out(m, n) += a_value * b_value;
        }
      }
    } else {
      // Vectorization via Eigen.
      const int b_chip_index = ADJ_B ? 1 : 0;

#define LOOP_NNZ(b_passed)                                               \
  for (std::size_t i = 0; i < nnz; ++i) {                                \
    const int64 m = internal::SubtleMustCopy(a_indices(i, lhs_index_a)); \
    const int64 k = internal::SubtleMustCopy(a_indices(i, rhs_index_a)); \
    const T a_value = (ADJ_A) ? MaybeConj(a_values(i)) : a_values(i);    \
    CHECK_LT(m, out.dimension(0));                                       \
    CHECK_LT(k, lhs_right);                                              \
    out.template chip<0>(m) +=                                           \
        b_passed.template chip<b_chip_index>(k) * a_value;               \
  }

      if (ADJ_B) {
        // Perform transpose and conjugation on B once, since we chip out B's
        // columns in the nnz loop.
        Eigen::array<int, 2> shuffle(1, 0);  // preserve dimension order
        Eigen::Tensor<T, 2, Eigen::ColMajor> col_major_conj_b =
            b.swap_layout().shuffle(shuffle).conjugate();
        LOOP_NNZ(col_major_conj_b);
      } else {
        LOOP_NNZ(b);
      }
#undef LOOP_NNZ
    }
  }
};

}  // namespace functor

}  // namespace tensorflow
