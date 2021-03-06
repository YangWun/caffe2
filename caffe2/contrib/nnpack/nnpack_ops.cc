#include <mkl_service.h>
#include "caffe2/core/context.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/operators/conv_pool_op_base.h"
#include "caffe2/utils/math.h"
#include "nnpack.h"

namespace caffe2 {

////////////////////////////////////////////////////////////////////////////////
// Helper Functions
////////////////////////////////////////////////////////////////////////////////

namespace {

nnp_convolution_algorithm get_nnp_convolution_algorithm(
    const std::string& algo) {
  if (algo == "AUTO") {
    return nnp_convolution_algorithm_auto;
  }
  if (algo == "WINOGRAD") {
    return nnp_convolution_algorithm_wt8x8;
  }
  if (algo == "FT16") {
    return nnp_convolution_algorithm_ft16x16;
  }
  if (algo == "FT8") {
    return nnp_convolution_algorithm_ft8x8;
  }
  return nnp_convolution_algorithm_auto;
}

nnp_convolution_transform_strategy get_nnp_convolution_transform_strategy(
    const std::string& kts) {
  if (kts == "BLOCK") {
    return nnp_convolution_transform_strategy_block_based;
  }
  if (kts == "TUPLE") {
    return nnp_convolution_transform_strategy_tuple_based;
  }
  return nnp_convolution_transform_strategy_block_based;
}

////////////////////////////////////////////////////////////////////////////////
// Thread Pool
////////////////////////////////////////////////////////////////////////////////

static pthreadpool_t nnpack_threadpool_ = nullptr;

pthreadpool_t nnpack_threadpool() {
  if (nnpack_threadpool_ == nullptr) {
    enum nnp_status nnpack_status = nnp_initialize();
    CAFFE_ENFORCE(
        nnpack_status == nnp_status_success, "NNPack is not supported here!");
    auto num_mkl_threads = mkl_get_max_threads();
    nnpack_threadpool_ = pthreadpool_create(num_mkl_threads);
  }
  return nnpack_threadpool_;
}
}

////////////////////////////////////////////////////////////////////////////////
// Definitions
////////////////////////////////////////////////////////////////////////////////

class NNPACKConvOp final : public ConvPoolOpBase<CPUContext> {
 public:
  NNPACKConvOp(const OperatorDef& operator_def, Workspace* ws)
      : ConvPoolOpBase<CPUContext>(operator_def, ws),
        algo_(get_nnp_convolution_algorithm(
            OperatorBase::GetSingleArgument<std::string>("algo", "AUTO"))),
        kts_(get_nnp_convolution_transform_strategy(
            OperatorBase::GetSingleArgument<std::string>("kts", "TUPLE"))) {
    OPERATOR_NEEDS_FEATURE(
        this->order_ == StorageOrder::NCHW,
        "NNPack only supports NCHW order. Please consider adding "
        "TransposeOp with axes=[0, 3, 1, 2] before NNPack Conv.");
  }

  bool RunOnDeviceWithOrderNCHW() override;

 private:
  const nnp_convolution_algorithm algo_;
  const nnp_convolution_transform_strategy kts_;
};

////////////////////////////////////////////////////////////////////////////////
// Implementations
////////////////////////////////////////////////////////////////////////////////

bool NNPACKConvOp::RunOnDeviceWithOrderNCHW() {
  auto& X = Input(0);
  auto& filter = Input(1);
  auto& bias = Input(2);
  auto* Y = Output(0);
  CAFFE_ENFORCE(X.ndim() == 4, "Input dim should be 4");
  const int N = X.dim32(0), C = X.dim32(1);
  CAFFE_ENFORCE(filter.ndim(), 4);
  const int M = filter.dim32(0);
  CAFFE_ENFORCE(filter.dim32(1) == C, "");
  CAFFE_ENFORCE(filter.dim32(2) == this->kernel_h_, "");
  CAFFE_ENFORCE(filter.dim32(3) == this->kernel_w_, "");
  CAFFE_ENFORCE(bias.ndim() == 1, "");
  CAFFE_ENFORCE(bias.dim32(0) == M, "");
  ConvPoolOpBase<CPUContext>::SetOutputSize(X, Y, filter.dim32(0));
  if (N > 1) {
    // NNPack only supports stride = 1 when doing batch feedforward
    CAFFE_ENFORCE(this->stride_h_ == 1, "");
    CAFFE_ENFORCE(this->stride_w_ == 1, "");
  }
  std::vector<int> pads(
      {this->pad_t_, this->pad_b_, this->pad_l_, this->pad_r_});
  std::vector<int> stride({this->stride_h_, this->stride_w_});

  const size_t batch_size = X.dim32(0);
  const size_t input_channels = X.dim32(1);
  const size_t output_channels = Y->dim32(1);
  const nnp_size input_size = {.width = static_cast<size_t>(X.dim32(3)),
                               .height = static_cast<size_t>(X.dim32(2))};
  // filter is MCHW
  const nnp_size kernel_size = {.width = static_cast<size_t>(filter.dim32(3)),
                                .height = static_cast<size_t>(filter.dim32(2))};
  // pad is tblr
  const nnp_padding padding = {.top = static_cast<size_t>(pads[0]),
                               .right = static_cast<size_t>(pads[3]),
                               .bottom = static_cast<size_t>(pads[1]),
                               .left = static_cast<size_t>(pads[2])};

  const nnp_size output_subsample = {.width = static_cast<size_t>(stride[1]),
                                     .height = static_cast<size_t>(stride[0])};
  if (batch_size == 1) {
    VLOG(1) << "Running inference mode";
    const auto status = nnp_convolution_inference(
        algo_,
        kts_,
        input_channels,
        output_channels,
        input_size,
        padding,
        kernel_size,
        output_subsample,
        X.template data<float>(),
        filter.template data<float>(),
        bias.template data<float>(),
        Y->template mutable_data<float>(),
        nnpack_threadpool(),
        nullptr);
    CAFFE_ENFORCE(nnp_status_success == status, "");
  } else {
    VLOG(1) << "Running batched mode";
    const auto status = nnp_convolution_output(
        algo_,
        batch_size,
        input_channels,
        output_channels,
        input_size,
        padding,
        kernel_size,
        X.template data<float>(),
        filter.template data<float>(),
        bias.template data<float>(),
        Y->template mutable_data<float>(),
        nnpack_threadpool(),
        nullptr);
    CAFFE_ENFORCE(nnp_status_success == status, "");
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// Definitions
////////////////////////////////////////////////////////////////////////////////

class NNPACKMaxPoolOp final : public ConvPoolOpBase<CPUContext> {
 public:
  NNPACKMaxPoolOp(const OperatorDef& operator_def, Workspace* ws)
      : ConvPoolOpBase<CPUContext>(operator_def, ws) {
    OPERATOR_NEEDS_FEATURE(
        this->order_ == StorageOrder::NCHW,
        "NNPack only supports NCHW order. Please consider add "
        "TransposeOp with axes=[0, 3, 1, 2] before NNPack Conv.");
    OPERATOR_NEEDS_FEATURE(
        this->kernel_h_ == 2, "NNPack only supports MaxPool kernel size 2*2!");
    OPERATOR_NEEDS_FEATURE(
        this->kernel_w_ == 2, "NNPack only supports MaxPool kernel size 2*2!");
    OPERATOR_NEEDS_FEATURE(
        this->stride_h_ == 2, "NNPack only supports MaxPool stride size 2*2!");
    OPERATOR_NEEDS_FEATURE(
        this->stride_w_ == 2, "NNPack only supports MaxPool stride size 2*2!");
    OPERATOR_NEEDS_FEATURE(
        this->pad_t_ == 0,
        "NNPack Pooling differs from Caffe2 Pooling when pad > 0!");
    OPERATOR_NEEDS_FEATURE(
        this->pad_l_ == 0,
        "NNPack Pooling differs from Caffe2 Pooling when pad > 0!");
    OPERATOR_NEEDS_FEATURE(
        this->pad_r_ == 0,
        "NNPack Pooling differs from Caffe2 Pooling when pad > 0!");
    OPERATOR_NEEDS_FEATURE(
        this->pad_b_ == 0,
        "NNPack Pooling differs from Caffe2 Pooling when pad > 0!");
  }
  bool RunOnDeviceWithOrderNCHW() override;

 private:
};

////////////////////////////////////////////////////////////////////////////////
// Implementations
////////////////////////////////////////////////////////////////////////////////

bool NNPACKMaxPoolOp::RunOnDeviceWithOrderNCHW() {
  auto& X = Input(0);
  auto* Y = Output(0);
  CAFFE_ENFORCE(X.ndim() == 4, "");
  const int H = X.dim32(2), W = X.dim32(3);
  CAFFE_ENFORCE(
      H % 2 == 0,
      "NNPack MaxPool differs from Caffe2 when Input Size is not even!");
  CAFFE_ENFORCE(
      W % 2 == 0,
      "NNPack MaxPool differs from Caffe2 when Input Size is not even!");
  ConvPoolOpBase<CPUContext>::SetOutputSize(X, Y, X.dim32(1));
  std::vector<int> pads(
      {this->pad_t_, this->pad_b_, this->pad_l_, this->pad_r_});
  std::vector<int> stride({this->stride_h_, this->stride_w_});
  std::vector<int> pooling({this->kernel_h_, this->kernel_w_});

  // Input X is in NCHW order
  const size_t batch_size = X.dim32(0);
  const size_t input_channels = X.dim32(1);
  const nnp_size input_size = {.width = static_cast<size_t>(X.dim32(3)),
                               .height = static_cast<size_t>(X.dim32(2))};
  // pooling kernel
  const nnp_size pooling_size = {.width = static_cast<size_t>(pooling[1]),
                                 .height = static_cast<size_t>(pooling[0])};
  // pad is tblr
  const nnp_padding padding = {.top = static_cast<size_t>(pads[0]),
                               .right = static_cast<size_t>(pads[3]),
                               .bottom = static_cast<size_t>(pads[1]),
                               .left = static_cast<size_t>(pads[2])};

  const nnp_size pooling_stride = {.width = static_cast<size_t>(stride[1]),
                                   .height = static_cast<size_t>(stride[0])};
  const auto status = nnp_max_pooling_output(
      batch_size,
      input_channels,
      input_size,
      padding,
      pooling_size,
      pooling_stride,
      X.template data<float>(),
      Y->template mutable_data<float>(),
      nnpack_threadpool());
  CAFFE_ENFORCE(nnp_status_success == status, "");
  return true;
}

REGISTER_CPU_OPERATOR_WITH_ENGINE(Conv, NNPACK, NNPACKConvOp);
REGISTER_CPU_OPERATOR_WITH_ENGINE(MaxPool, NNPACK, NNPACKMaxPoolOp);

} // namespace caffe2
