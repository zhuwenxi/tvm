#include <tvm/runtime/registry.h>
#include <tvm/runtime/device_api.h>
#include <libxsmm.h>
#include <libxsmm_typedefs.h>

#include "../json/json_runtime.h"
#include "../json/json_node.h"


namespace tvm {
namespace runtime {
namespace contrib {

class LibxsmmJSONRuntime : public json::JSONRuntimeBase {

public:
  LibxsmmJSONRuntime(const std::string& symbol_name, const std::string& graph_json, const Array<String> const_names)
    : JSONRuntimeBase(symbol_name, graph_json, const_names) {}

  const char* type_key() const { return "libxsmm_json"; }

  void Init(const Array<NDArray>& consts) override {
    for (size_t nid = 0; nid < nodes_.size(); ++ nid) {
      auto& node = nodes_[nid];
      if (node.GetOpType() == "kernel") {
        auto op_name = node.GetOpName();
        has_bias_ = op_name.find("_bias") != std::string::npos;
        has_relu_ = op_name.find("_relu") != std::string::npos;

        std::cout << "op name: " << node.GetOpName() << std::endl;
        auto data_entry = node.GetInputs()[0];
        auto weight_entry = node.GetInputs()[1];
        json::JSONGraphNodeEntry out_entry(nid, 0);
        std::vector<int64_t> input_shape = nodes_[data_entry.id_].GetOpShape()[data_entry.index_];
        std::vector<int64_t> weight_shape = nodes_[weight_entry.id_].GetOpShape()[weight_entry.index_];
        std::vector<int64_t> out_shape = nodes_[out_entry.id_].GetOpShape()[out_entry.index_];
        
        M = input_shape[0];
        N = weight_shape[0];
        K = input_shape[1];

        relu_mask_.resize(M * N, 0);

        int lda = N;
        int ldb = K;
        int ldc = N;
        libxsmm_datatype dtype = LIBXSMM_DATATYPE_F32;

        libxsmm_bitfield l_flags = LIBXSMM_GEMM_FLAG_NONE | LIBXSMM_GEMM_FLAG_BETA_0;
        libxsmm_bitfield l_prefetch_flags = LIBXSMM_GEMM_PREFETCH_NONE;
        libxsmm_gemm_shape l_shape = libxsmm_create_gemm_shape(N, M, K, lda, ldb, ldc, dtype, dtype, dtype, dtype);
        libxsmm_blasint stride_a = N * K * sizeof(float);
        libxsmm_blasint stride_b = K * M * sizeof(float);
        libxsmm_gemm_batch_reduce_config l_brconfig = libxsmm_create_gemm_batch_reduce_config(LIBXSMM_GEMM_BATCH_REDUCE_STRIDE, stride_a, stride_b, 0 /*br_unrool_hint*/); 

        libxsmm_gemm_ext_unary_argops l_argops;
        libxsmm_gemm_ext_binary_postops l_postops;
        memset( &l_argops, 0, sizeof(libxsmm_gemm_ext_unary_argops) );
        memset( &l_postops, 0, sizeof(libxsmm_gemm_ext_binary_postops) );

        if (has_bias_) {
          l_postops.d_in_type = dtype;
          l_postops.d_binary_flags = LIBXSMM_MELTW_FLAG_BINARY_BCAST_COL_IN_0;
          l_postops.d_binary_type = LIBXSMM_MELTW_TYPE_BINARY_ADD;
          l_postops.ldd = ldc;
        }
        
        if (has_relu_) {
          l_argops.cp_unary_flags = LIBXSMM_MELTW_FLAG_UNARY_NONE;
          l_argops.cp_unary_type    = LIBXSMM_MELTW_TYPE_UNARY_RELU;
          l_argops.ldcp             = ldc;
        }

        if (has_bias_ || has_relu_) {
          gemm_fusion_kernel_ = libxsmm_dispatch_brgemm_ext_v2(l_shape, l_flags, l_prefetch_flags, l_brconfig, l_argops, l_postops);
        } else {
          gemm_kernel_ = libxsmm_dispatch_brgemm_v2(l_shape, l_flags, l_prefetch_flags, l_brconfig);
        }
      }
    } 
  }

  void Run() override {
    std::cout << "Run()" << std::endl;
    std::cout << "has_relu_:" << has_relu_ << std::endl;
    auto data_eid = EntryID(input_nodes_[0], 0);
    auto filter_eid = EntryID(input_nodes_[1], 0);
    auto output_eid = EntryID(outputs_[0]);

    void *data_handle = data_entry_[data_eid]->data;
    void *filter_handle = data_entry_[filter_eid]->data;
    void *output_handle = data_entry_[output_eid]->data;

    if (!transposed_filter_handle_) {
      TVMDeviceAllocDataSpace(dev, K * N * sizeof(float), kAllocAlignment, type_hint, &transposed_filter_handle_);
      for (int k = 0; k < K; ++ k) {
        for (int n = 0; n < N; ++ n) {
          static_cast<float*>(transposed_filter_handle_)[k * N + n] = static_cast<float*>(filter_handle)[n * K + k];      
        }
      }
    }

    unsigned long long int blocks = 1;
    if (has_bias_ || has_relu_) {
      libxsmm_gemm_ext_param gemm_param_ext;
      gemm_param_ext.a.secondary = NULL;
      gemm_param_ext.b.secondary = NULL;

      gemm_param_ext.a.primary = transposed_filter_handle_;
      gemm_param_ext.b.primary = data_handle;
      gemm_param_ext.c.primary = output_handle;
      if (has_bias_) {
        auto bias_eid = EntryID(input_nodes_[2], 0);
        void *bias_handle = data_entry_[bias_eid]->data;

        gemm_param_ext.d.primary = bias_handle;
      }
      if (has_relu_) {
        gemm_param_ext.c.secondary = relu_mask_.data();
      }
      gemm_param_ext.op.tertiary = &blocks;

      std::cout << "Before gemm_fusion_kernel_" << std::endl;
      gemm_fusion_kernel_(&gemm_param_ext);
      std::cout << "After gemm_fusion_kernel_" << std::endl;
    } else {
      libxsmm_gemm_param gemm_param;
      gemm_param.a.secondary = NULL;
      gemm_param.b.secondary = NULL;

      gemm_param.a.primary = transposed_filter_handle_;
      gemm_param.b.primary = data_handle;
      gemm_param.c.primary = output_handle;
      gemm_param.op.tertiary = &blocks;

      gemm_kernel_(&gemm_param);
    }
  }

  ~LibxsmmJSONRuntime()
  {
    TVMDeviceFreeDataSpace(dev, transposed_filter_handle_);
  }

private:
  libxsmm_gemmfunction gemm_kernel_;
  libxsmm_gemmfunction_ext gemm_fusion_kernel_;

  void *transposed_filter_handle_{nullptr};
  DLDevice dev {kDLCPU, 0};
  DLDataType type_hint {2, 32, 1};

  int64_t M;
  int64_t K;
  int64_t N;

  bool has_bias_{false};
  bool has_relu_{false};

  std::vector<unsigned char> relu_mask_;
};

runtime::Module LibxsmmJSONRuntimeCreate(String symbol_name, String graph_json, const Array<String>& const_names) {
  auto n = make_object<LibxsmmJSONRuntime>(symbol_name, graph_json, const_names);
  return runtime::Module(n);
}

TVM_REGISTER_GLOBAL("runtime.LibxsmmJSONRuntimeCreate").set_body_typed(LibxsmmJSONRuntimeCreate);

} // contrib
} // namespace runtime
} // namespace tvm
