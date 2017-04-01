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

#include <unordered_set>

#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/grappler/clusters/cluster.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/optimizers/layout_optimizer.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/strcat.h"

namespace tensorflow {
namespace grappler {

const char kConcatConst[] = "LayoutOptimizerConcatConst";
const char kPermNHWCToNCHW[] = "LayoutOptimizerPermConstNHWCToNCHW";
const char kPermNCHWToNHWC[] = "LayoutOptimizerPermConstNCHWToNHWC";
const char kTransposeNHWCToNCHW[] = "LayoutOptimizerTransposeNHWCToNCHW";
const char kTransposeNCHWToNHWC[] = "LayoutOptimizerTransposeNCHWToNHWC";
const char kPermVecNHWCToNCHW[] = "LayoutOptimizerPermVecNHWCToNCHW";
const char kReshapeNHWCToNCHW[] = "LayoutOptimizerReshapeNHWCToNCHW";
const char kReshapeConst[] = "LayoutOptimizerReshapeConst";
const char kReductionConst[] = "LayoutOptimizerReductionConst";

std::set<string> GetOpsFormatSupported() {
  std::set<string> ops_format_supported = {"AvgPool",
                                           "AvgPoolGrad",
                                           "Conv2D",
                                           "Conv2DBackpropFilter",
                                           "Conv2DBackpropInput",
                                           "BiasAdd",
                                           "BiasAddGrad",
                                           "FusedBatchNorm",
                                           "FusedBatchNormGrad",
                                           "MaxPool",
                                           "MaxPoolGrad"};
  return ops_format_supported;
}

std::set<string> GetOpsFormatAgnostic() {
  std::set<string> ops_format_agnostic = {"Add",
                                          "AddN",
                                          "Concat",
                                          "ConcatV2",
                                          "Floor",
                                          "Identity",
                                          "Mul",
                                          "Neg",
                                          "RealDiv",
                                          "Relu",
                                          "ReluGrad",
                                          "Slice",
                                          "SquaredDifference",
                                          "Squeeze",
                                          "Sub",
                                          "Sum"};
  return ops_format_agnostic;
}

class NodeMap {
 public:
  explicit NodeMap(GraphDef* graph) : graph_(graph) {
    for (int i = 0; i < graph_->node_size(); i++) {
      auto node = graph_->mutable_node(i);
      nodes_.insert(std::make_pair(node->name(), node));
      for (const auto& input : node->input()) {
        outputs_[input].insert(nodes_[node->name()]);
      }
    }
  }

  NodeDef* GetNode(const string& name) {
    string node_name = NodeName(name);
    return nodes_[node_name];
  }

  std::set<NodeDef*> GetOutputs(const string& name) { return outputs_[name]; }

  void AddNode(const string& name, NodeDef* node) {
    nodes_.insert(std::make_pair(name, node));
  }

  void AddOutput(const string& node, const string& output) {
    outputs_[node].insert(nodes_[output]);
  }

  void UpdateOutput(const string& node, const string& old_output,
                    const string& new_output) {
    outputs_[node].erase(nodes_[old_output]);
    outputs_[node].insert(nodes_[new_output]);
  }

 private:
  GraphDef* graph_;
  std::unordered_map<string, NodeDef*> nodes_;
  std::unordered_map<string, std::set<NodeDef*>> outputs_;
};

bool IsNodeNHWCToNCHW(const string& node_name) {
  const string transpose_node_prefix = kTransposeNHWCToNCHW;
  string prefix = node_name.substr(0, transpose_node_prefix.length());
  if (prefix.compare(transpose_node_prefix) == 0) {
    return true;
  }
  return false;
}

bool IsNodeNCHWToNHWC(const string& node_name) {
  const string transpose_node_prefix = kTransposeNCHWToNHWC;
  string prefix = node_name.substr(0, transpose_node_prefix.length());
  if (prefix.compare(transpose_node_prefix) == 0) {
    return true;
  }
  return false;
}

class NodeProcessor {
 public:
  NodeProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : graph_(graph), node_(node), node_map_(node_map) {}
  virtual ~NodeProcessor() {}
  virtual void ConvertNode() {
    if (ShouldProcess()) {
      UpdateAttrDataFormat();
      UpdateAttrKSize();
      UpdateAttrStrides();
      UpdateAttrShape();
      AddLayoutTransposeToInputs();
      AddLayoutTransposeToOutputs();
      CustomizedProcessing();
    }
  }

 protected:
  bool IsDimsN(NodeDef* node, int n) const {
    if (node->attr().find("_output_shapes") != node->attr().end()) {
      auto shape = node->attr().at("_output_shapes").list().shape(0);
      if (shape.dim_size() == n) {
        return true;
      }
    }
    return false;
  }

  bool IsDimsFour(NodeDef* node) const { return IsDimsN(node, 4); }

  bool IsNHWC() const {
    if (node_->attr().find("data_format") != node_->attr().end()) {
      if (node_->attr().at("data_format").s().compare("NHWC") == 0) {
        return true;
      }
    }
    return false;
  }

  bool HasOutputs() const {
    auto outputs = node_map_->GetOutputs(node_->name());
    return !outputs.empty();
  }

  virtual bool ShouldProcess() const {
    return IsNHWC() && IsDimsFour(node_) && HasOutputs();
  }

  void UpdateAttrDataFormat() {
    if (node_->attr().find("data_format") != node_->attr().end()) {
      if (node_->attr().at("data_format").s().compare("NHWC") == 0) {
        string* data_format =
            node_->mutable_attr()->at("data_format").mutable_s();
        *data_format = "NCHW";
      }
    }
  }

  virtual void UpdateAttrShape() {
    if (node_->attr().find("_output_shapes") != node_->attr().end()) {
      auto shape = node_->mutable_attr()
                       ->at("_output_shapes")
                       .mutable_list()
                       ->mutable_shape(0);
      if (shape->dim_size() == 4) {
        int64 h = shape->dim(1).size();
        int64 w = shape->dim(2).size();
        int64 c = shape->dim(3).size();
        shape->mutable_dim(1)->set_size(c);
        shape->mutable_dim(2)->set_size(h);
        shape->mutable_dim(3)->set_size(w);
      }
    }
  }

  void UpdateAttrKSize() {
    if (node_->attr().find("ksize") != node_->attr().end()) {
      auto list = node_->mutable_attr()->at("ksize").mutable_list();
      UpdateTuple(list);
    }
  }

  void UpdateAttrStrides() {
    if (node_->attr().find("strides") != node_->attr().end()) {
      auto list = node_->mutable_attr()->at("strides").mutable_list();
      UpdateTuple(list);
    }
  }

  void UpdateAttrValue(const string& name) {
    NodeDef* node = node_map_->GetNode(name);
    Tensor tensor;
    auto success =
        tensor.FromProto(node->mutable_attr()->at({"value"}).tensor());
    if (!success) {
      LOG(ERROR) << "Failed to parse TensorProto.";
    }
    int c = tensor.flat<int>()(3);
    tensor.flat<int>()(3) = tensor.flat<int>()(2);
    tensor.flat<int>()(2) = tensor.flat<int>()(1);
    tensor.flat<int>()(1) = c;
    tensor.AsProtoTensorContent(
        node->mutable_attr()->at({"value"}).mutable_tensor());
  }

  virtual std::vector<int> GetInputPos() const {
    std::vector<int> input_pos = {0};
    return input_pos;
  }

  void AddNodeTranspose(const string& node_name, const string& input_name,
                        DataType data_type, const TensorShapeProto& input_shape,
                        bool NHWCToNCHW) {
    NodeDef* node = graph_->add_node();
    node_map_->AddNode(node_name, node);
    node->set_name(node_name);
    *node->add_input() = input_name;
    *node->add_input() = NHWCToNCHW ? kPermNHWCToNCHW : kPermNCHWToNHWC;
    node->set_op("Transpose");
    AttrValue attr_data_type;
    attr_data_type.set_type(data_type);
    node->mutable_attr()->insert({"T", attr_data_type});
    AttrValue attr_data_type_perm;
    attr_data_type_perm.set_type(DT_INT32);
    node->mutable_attr()->insert({"Tperm", attr_data_type_perm});
    AttrValue attr_output_shape;
    auto output_shape = attr_output_shape.mutable_list()->add_shape();
    if (NHWCToNCHW) {
      output_shape->add_dim()->set_size(input_shape.dim(0).size());
      output_shape->add_dim()->set_size(input_shape.dim(3).size());
      output_shape->add_dim()->set_size(input_shape.dim(1).size());
      output_shape->add_dim()->set_size(input_shape.dim(2).size());
    } else {
      output_shape->add_dim()->set_size(input_shape.dim(0).size());
      output_shape->add_dim()->set_size(input_shape.dim(2).size());
      output_shape->add_dim()->set_size(input_shape.dim(3).size());
      output_shape->add_dim()->set_size(input_shape.dim(1).size());
    }
    node->mutable_attr()->insert({"_output_shapes", attr_output_shape});
  }

  virtual void AddLayoutTransposeToInputs() {
    std::vector<int> input_pos = GetInputPos();
    for (const auto& pos : input_pos) {
      string node_name_NHWCToNCHW = strings::StrCat(
          kTransposeNHWCToNCHW, "-", node_->name(), "-", node_->input(pos));
      auto input_node = node_map_->GetNode(node_->input(pos));
      int output_pos = NodePosition(node_->input(pos));
      AddNodeTranspose(
          node_name_NHWCToNCHW, node_->input(pos), node_->attr().at("T").type(),
          input_node->attr().at("_output_shapes").list().shape(output_pos),
          true);
      node_map_->UpdateOutput(node_->input(pos), node_->name(),
                              node_name_NHWCToNCHW);
      node_map_->AddOutput(node_name_NHWCToNCHW, node_->name());
      *node_->mutable_input(pos) = node_name_NHWCToNCHW;
    }
  }

  virtual void AddLayoutTransposeToOutputs() {
    auto outputs = node_map_->GetOutputs(node_->name());
    for (const auto& output : outputs) {
      string node_name_NCHWToNHWC = strings::StrCat(
          kTransposeNCHWToNHWC, "-", node_->name(), "-", output->name());
      auto it = std::find_if(output->mutable_input()->begin(),
                             output->mutable_input()->end(),
                             [this](const string& input) {
                               return input.compare(node_->name()) == 0;
                             });
      int output_pos = NodePosition(*it);
      AddNodeTranspose(
          node_name_NCHWToNHWC, node_->name(), node_->attr().at("T").type(),
          node_->attr().at("_output_shapes").list().shape(output_pos), false);
      *it = node_name_NCHWToNHWC;
      node_map_->UpdateOutput(node_->name(), output->name(),
                              node_name_NCHWToNHWC);
      node_map_->AddOutput(node_name_NCHWToNHWC, output->name());
    }
  }

  virtual void CustomizedProcessing() {}

  GraphDef* graph_;
  NodeDef* node_;
  NodeMap* node_map_;

 private:
  void UpdateTuple(AttrValue_ListValue* list) {
    int64 h = list->i(1);
    int64 w = list->i(2);
    int64 c = list->i(3);
    list->set_i(1, c);
    list->set_i(2, h);
    list->set_i(3, w);
  }
};

class AvgPoolGradProcessor : public NodeProcessor {
 public:
  AvgPoolGradProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : NodeProcessor(graph, node, node_map) {}

 protected:
  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos = {1};
    return input_pos;
  }
  void CustomizedProcessing() override { UpdateAttrValue(node_->input(0)); }
};

class BiasAddGradProcessor : public NodeProcessor {
 public:
  BiasAddGradProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : NodeProcessor(graph, node, node_map) {}

 protected:
  bool ShouldProcess() const override {
    auto input = node_map_->GetNode(node_->input(0));
    if (input) {
      if ((IsNHWC() && IsDimsFour(input)) || IsNodeNCHWToNHWC(input->name())) {
        return true;
      }
    }
    return false;
  }

  void AddLayoutTransposeToOutputs() override {}
};

class Conv2DBackpropFilterProcessor : public NodeProcessor {
 public:
  Conv2DBackpropFilterProcessor(GraphDef* graph, NodeDef* node,
                                NodeMap* node_map)
      : NodeProcessor(graph, node, node_map) {}

 protected:
  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos = {0, 2};
    return input_pos;
  }

  void AddLayoutTransposeToOutputs() override {}
  // No need to update output shape, as it is always of shape
  // [filter_height, filter_width, in_channels, out_channels], regardless of
  // whether NCHW or NHWC is used.
  void UpdateAttrShape() override {}
};

class Conv2DBackpropInputProcessor : public NodeProcessor {
 public:
  Conv2DBackpropInputProcessor(GraphDef* graph, NodeDef* node,
                               NodeMap* node_map)
      : NodeProcessor(graph, node, node_map) {}

 protected:
  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos = {2};
    return input_pos;
  }
  void CustomizedProcessing() override { UpdateAttrValue(node_->input(0)); }
};

class FusedBatchNormGradProcessor : public NodeProcessor {
 public:
  FusedBatchNormGradProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : NodeProcessor(graph, node, node_map) {}

 protected:
  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos = {0, 1};
    return input_pos;
  }
};

class MaxPoolGradProcessor : public NodeProcessor {
 public:
  MaxPoolGradProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : NodeProcessor(graph, node, node_map) {}

 protected:
  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos = {0, 1, 2};
    return input_pos;
  }
};

class AgnosticNodeProcessor : public NodeProcessor {
 public:
  AgnosticNodeProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : NodeProcessor(graph, node, node_map) {}

 protected:
  bool ShouldProcess() const override {
    return IsDimsFour(node_) && HasOutputs() && IsNodeAfterNCHWToNHWC();
  }

  bool IsNodeAfterNCHWToNHWC() const {
    std::set<string> ops_format_agnostic = GetOpsFormatAgnostic();
    auto node = node_map_->GetNode(node_->name());
    while (node->input_size() > 0) {
      int data_input_pos = 0;
      if (node->op().compare("Concat") == 0) {
        data_input_pos = 1;
      }
      node = node_map_->GetNode(node->input(data_input_pos));
      if (IsNodeNCHWToNHWC(node->name())) {
        return true;
      }
      bool connected =
          ops_format_agnostic.find(node->name()) != ops_format_agnostic.end();
      if (!connected) {
        return false;
      }
    }
    return false;
  }
};

class AddNProcessor : public AgnosticNodeProcessor {
 public:
  AddNProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {}

 protected:
  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos;
    for (int i = 0; i < node_->input_size(); i++) {
      input_pos.push_back(i);
    }
    return input_pos;
  }
};

class BinaryOpProcessor : public AgnosticNodeProcessor {
 public:
  BinaryOpProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {
    is_4d_with_vector_ = Is4DOperateWithVector();
  }

 protected:
  bool ShouldProcess() const override {
    return IsDimsFour(node_) && HasOutputs() && IsNodeAfterNCHWToNHWC() &&
           (Is4DOperateWithND(4) || Is4DOperateWithScalar() ||
            Is4DOperateWithVector());
  }

  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos = {0};
    if (Is4DOperateWithND(4)) {
      input_pos.push_back(1);
    }
    return input_pos;
  }

  bool Is4DOperateWithND(int n) const {
    auto input0 = node_map_->GetNode(node_->input(0));
    auto input1 = node_map_->GetNode(node_->input(1));
    if (input0 && input1) {
      return (IsDimsFour(input0) || IsNodeNCHWToNHWC(input0->name())) &&
             ((n == 4)
                  ? (IsDimsFour(input1) || IsNodeNCHWToNHWC(input1->name()))
                  : IsDimsN(input1, n));
    }
    return false;
  }

  bool Is4DOperateWithScalar() const { return Is4DOperateWithND(0); }

  bool Is4DOperateWithVector() const { return Is4DOperateWithND(1); }

  void AddNodeShapeConst(const string& name, int num_channels) {
    NodeDef* node = graph_->add_node();
    node_map_->AddNode(name, node);
    node->set_name(name);
    node->set_op("Const");
    AttrValue attr_data_type;
    attr_data_type.set_type(DT_INT32);
    node->mutable_attr()->insert({"dtype", attr_data_type});

    AttrValue attr_tensor;
    Tensor tensor(DT_INT32, TensorShape({4}));
    std::vector<int> shape = {1, num_channels, 1, 1};
    for (int i = 0; i < shape.size(); i++) {
      tensor.flat<int>()(i) = shape[i];
    }
    tensor.AsProtoTensorContent(attr_tensor.mutable_tensor());
    node->mutable_attr()->insert({"value", attr_tensor});
  }

  void AddNodeReshape(const string& node_name, const string& input_name,
                      const string& shape_const_node_name, DataType data_type) {
    NodeDef* node = graph_->add_node();
    node_map_->AddNode(node_name, node);
    node->set_name(node_name);
    *node->add_input() = input_name;
    *node->add_input() = shape_const_node_name;
    node->set_op("Reshape");

    AttrValue attr_type_indices;
    attr_type_indices.set_type(DT_INT32);
    node->mutable_attr()->insert({"Tshape", attr_type_indices});

    AttrValue attr_type_params;
    attr_type_params.set_type(data_type);
    node->mutable_attr()->insert({"T", attr_type_params});
  }

  void CustomizedProcessing() override {
    if (is_4d_with_vector_) {
      string suffix = strings::StrCat("-", node_->name(), "-", node_->input(1));
      string reshape_node_name = strings::StrCat(kReshapeNHWCToNCHW, suffix);
      string shape_const_node_name = strings::StrCat(kReshapeConst, suffix);
      int vector_size = node_map_->GetNode(node_->input(1))
                            ->attr()
                            .at("_output_shapes")
                            .list()
                            .shape(0)
                            .dim(0)
                            .size();
      AddNodeShapeConst(shape_const_node_name, vector_size);
      AddNodeReshape(reshape_node_name, node_->input(1), shape_const_node_name,
                     node_->attr().at("T").type());
      node_map_->AddOutput(shape_const_node_name, reshape_node_name);
      node_map_->UpdateOutput(node_->input(1), node_->name(),
                              reshape_node_name);
      node_map_->AddOutput(reshape_node_name, node_->name());
      *node_->mutable_input(1) = reshape_node_name;
    }
  }

 private:
  bool is_4d_with_vector_;
};

class ConcatProcessor : public AgnosticNodeProcessor {
 public:
  ConcatProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {
    // For Concat,  the concat axis is the first input; for ConcatV2,
    // the last input.
    axis_node_pos_ =
        (node_->op().compare("Concat") == 0) ? 0 : (node_->input_size() - 1);
  }

 protected:
  bool ShouldProcess() const override {
    return IsDimsFour(node_) && HasOutputs() && IsNodeAfterNCHWToNHWC() &&
           IsAlongDimC();
  }

  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos;
    int start = (node_->op().compare("Concat") == 0) ? 1 : 0;
    int end = (node_->op().compare("Concat") == 0) ? node_->input_size()
                                                   : (node_->input_size() - 1);
    for (int i = start; i < end; i++) {
      input_pos.push_back(i);
    }
    return input_pos;
  }

  void CustomizedProcessing() override {
    node_map_->AddOutput(kConcatConst, node_->name());
    *node_->mutable_input(axis_node_pos_) = kConcatConst;
  }

  bool IsAlongDimC() const {
    auto axis_node = node_map_->GetNode(node_->input(axis_node_pos_));
    if (axis_node->attr().find("value") != axis_node->attr().end()) {
      return axis_node->attr().at("value").tensor().int_val(0) == 3;
    }
    return false;
  }

  int axis_node_pos_;
};

class ReluGradProcessor : public AgnosticNodeProcessor {
 public:
  ReluGradProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {}

 protected:
  std::vector<int> GetInputPos() const override {
    std::vector<int> input_pos = {0, 1};
    return input_pos;
  }
};

// This is the older, less optimized gather-based SliceProcessor. We keep it as
// a test case for constant propagation optimization.
class SliceProcessorGatherBased : public AgnosticNodeProcessor {
 public:
  SliceProcessorGatherBased(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {}

 protected:
  void CustomizedProcessing() override {
    // Skip the first input, which is the data to be sliced.
    for (int i = 1; i < node_->input_size(); i++) {
      string node_name_NHWCToNCHW =
          strings::StrCat(kPermVecNHWCToNCHW, "-", node_->name(), "-input", i);
      AddNodePermVec(node_name_NHWCToNCHW, node_->input(i),
                     node_->attr().at("Index").type(), true);
      node_map_->UpdateOutput(node_->input(i), node_->name(),
                              node_name_NHWCToNCHW);
      node_map_->AddOutput(node_name_NHWCToNCHW, node_->name());
      *node_->mutable_input(i) = node_name_NHWCToNCHW;
    }
  }

 private:
  void AddNodePermVec(const string& node_name, const string& input_name,
                      DataType data_type, bool NHWCToNCHW) {
    NodeDef* node = graph_->add_node();
    node_map_->AddNode(node_name, node);
    node->set_name(node_name);
    *node->add_input() = input_name;
    *node->add_input() = NHWCToNCHW ? kPermNHWCToNCHW : kPermNCHWToNHWC;
    node->set_op("Gather");

    AttrValue attr_type_indices;
    attr_type_indices.set_type(DT_INT32);
    node->mutable_attr()->insert({"Tindices", attr_type_indices});

    AttrValue attr_type_params;
    attr_type_params.set_type(data_type);
    node->mutable_attr()->insert({"Tparams", attr_type_params});

    AttrValue attr_validate;
    attr_validate.set_b(true);
    node->mutable_attr()->insert({"validate_indices", attr_validate});
  }
};

class SliceProcessor : public AgnosticNodeProcessor {
 public:
  SliceProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {}

 protected:
  void CustomizedProcessing() override {
    auto maybe_concatoffset_node =
        node_map_->GetNode(NodeName(node_->input(1)));
    if (maybe_concatoffset_node->op() == "ConcatOffset") {
      auto axis_node = node_map_->GetNode(maybe_concatoffset_node->input(0));
      // Need to process if the channel is at dimension 3, which indicates the
      // NHWC format is being used. As mutiple Slice nodes may share the same
      // ConcatOffset node, the NHWC to NCHW conversion may have already
      // been performed when processing other Slice nodes.
      if (axis_node->attr().at("value").tensor().int_val(0) == 3) {
        for (int i = 1; i < maybe_concatoffset_node->input_size(); i++) {
          auto shape_node =
              node_map_->GetNode(maybe_concatoffset_node->input(i));
          AttrValue attr_tensor;
          Tensor tensor;
          CHECK(tensor.FromProto(shape_node->attr().at({"value"}).tensor()));
          int h = tensor.flat<int>()(1);
          int w = tensor.flat<int>()(2);
          int c = tensor.flat<int>()(3);
          tensor.flat<int>()(1) = c;
          tensor.flat<int>()(2) = h;
          tensor.flat<int>()(3) = w;
          tensor.AsProtoTensorContent(
              shape_node->mutable_attr()->at({"value"}).mutable_tensor());
        }
        // Set the channel dimension to 1, as we have converted the vector
        // element order from NHWC to NCHW.
        axis_node->mutable_attr()->at("value").mutable_tensor()->set_int_val(0,
                                                                             1);
      }
    }
  }
};

class SqueezeProcessor : public AgnosticNodeProcessor {
 public:
  SqueezeProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {}

 protected:
  bool ShouldProcess() const override {
    return IsDimsN(node_, 2) && HasOutputs() && IsNodeAfterNCHWToNHWC() &&
           IsInputConvertible() && IsAlongDimHW();
  }

  void AddLayoutTransposeToOutputs() override {}

  bool IsInputConvertible() const {
    auto input = node_map_->GetNode(node_->input(0));
    if (IsNodeNCHWToNHWC(input->name())) {
      input = node_map_->GetNode(input->input(0));
    }
    if (input->attr().find("_output_shapes") != input->attr().end()) {
      auto shape = input->attr().at("_output_shapes").list().shape(0);
      if (shape.dim_size() != 4) {
        return false;
      }
      if (shape.dim(1).size() == 1 && shape.dim(2).size() == 1) {
        return true;
      }
    }
    return false;
  }

  bool IsAlongDimHW() const {
    if (node_->attr().find("squeeze_dims") != node_->attr().end()) {
      auto list = node_->attr().at("squeeze_dims").list();
      if (list.i(0) == 1 && list.i(1) == 2) {
        return true;
      }
    }
    return false;
  }

  void CustomizedProcessing() override {
    auto list = node_->mutable_attr()->at("squeeze_dims").mutable_list();
    list->set_i(0, 2);
    list->set_i(1, 3);
  }
};

class SumProcessor : public AgnosticNodeProcessor {
 public:
  SumProcessor(GraphDef* graph, NodeDef* node, NodeMap* node_map)
      : AgnosticNodeProcessor(graph, node, node_map) {}

 protected:
  bool ShouldProcess() const override {
    auto input0 = node_map_->GetNode(node_->input(0));
    return HasOutputs() && IsNodeAfterNCHWToNHWC() &&
           (IsDimsFour(input0) || IsNodeNCHWToNHWC(input0->name())) &&
           IsAlongDimNHW();
  }

  void AddLayoutTransposeToOutputs() override {}

  void CustomizedProcessing() override {
    node_map_->AddOutput(kReductionConst, node_->name());
    *node_->mutable_input(1) = kReductionConst;
  }

 private:
  bool IsAlongDimNHW() const {
    NodeDef* node = node_map_->GetNode(node_->input(1));
    Tensor tensor;
    if (node->attr().find({"value"}) == node->attr().end()) {
      return false;
    }
    auto success = tensor.FromProto(node->attr().at({"value"}).tensor());
    if (!success) {
      LOG(ERROR) << "Failed to parse TensorProto.";
      return false;
    }
    if (tensor.flat<int>().size() != 3) {
      return false;
    }
    if (tensor.flat<int>()(0) == 0 && tensor.flat<int>()(1) == 1 &&
        tensor.flat<int>()(2) == 2) {
      return true;
    }
    return false;
  }
};

class DataLayoutOptimizer {
 public:
  explicit DataLayoutOptimizer(GraphDef* graph)
      : graph_(graph), node_map_(graph_) {
    LOG(INFO) << "Number of nodes for original graph: " << graph_->node_size();
    Expand();
    LOG(INFO) << "Number of nodes after Expand: " << graph_->node_size();
    Collapse();
    LOG(INFO) << "Number of nodes after Collapse: " << graph_->node_size();
  }

 private:
  void AddNodePermConst(const string& name,
                        const std::vector<int>& permutation) {
    NodeDef* node = graph_->add_node();
    node_map_.AddNode(name, node);
    node->set_name(name);
    node->set_op("Const");
    AttrValue attr_data_type;
    attr_data_type.set_type(DT_INT32);
    node->mutable_attr()->insert({"dtype", attr_data_type});
    AttrValue attr_tensor;
    Tensor tensor(DT_INT32, TensorShape({4}));
    for (int i = 0; i < permutation.size(); i++) {
      tensor.flat<int>()(i) = permutation[i];
    }
    tensor.AsProtoTensorContent(attr_tensor.mutable_tensor());
    node->mutable_attr()->insert({"value", attr_tensor});
  }

  void AddNodeConcatConst() {
    NodeDef* node = graph_->add_node();
    node_map_.AddNode(kConcatConst, node);
    node->set_name(kConcatConst);
    node->set_op("Const");
    AttrValue attr_data_type;
    attr_data_type.set_type(DT_INT32);
    node->mutable_attr()->insert({"dtype", attr_data_type});
    AttrValue attr_tensor;
    Tensor tensor(DT_INT32, TensorShape({}));
    tensor.scalar<int>()() = 1;
    tensor.AsProtoTensorContent(attr_tensor.mutable_tensor());
    node->mutable_attr()->insert({"value", attr_tensor});
  }

  void AddNodeReductionConst() {
    NodeDef* node = graph_->add_node();
    node_map_.AddNode(kReductionConst, node);
    node->set_name(kReductionConst);
    node->set_op("Const");
    AttrValue attr_data_type;
    attr_data_type.set_type(DT_INT32);
    node->mutable_attr()->insert({"dtype", attr_data_type});

    AttrValue attr_tensor;
    Tensor tensor(DT_INT32, TensorShape({3}));
    std::vector<int> axis = {0, 2, 3};
    for (int i = 0; i < axis.size(); i++) {
      tensor.flat<int>()(i) = axis[i];
    }
    tensor.AsProtoTensorContent(attr_tensor.mutable_tensor());
    node->mutable_attr()->insert({"value", attr_tensor});
  }

  // Expand all nodes which is in NHWC, but supports NCHW or is layout agnostic.
  void Expand() {
    int node_size_original = graph_->node_size();
    // This is the first pass where we expand the nodes which support NCHW.
    std::set<string> ops_format_supported = GetOpsFormatSupported();
    for (int i = 0; i < graph_->node_size(); i++) {
      if (ops_format_supported.find(graph_->node(i).op()) !=
          ops_format_supported.end()) {
        auto node = graph_->mutable_node(i);
        std::unique_ptr<NodeProcessor> node_processor;
        if (node->op().compare("AvgPoolGrad") == 0) {
          node_processor.reset(
              new AvgPoolGradProcessor(graph_, node, &node_map_));
        } else if (node->op().compare("BiasAddGrad") == 0) {
          node_processor.reset(
              new BiasAddGradProcessor(graph_, node, &node_map_));
        } else if (node->op().compare("Conv2DBackpropFilter") == 0) {
          node_processor.reset(
              new Conv2DBackpropFilterProcessor(graph_, node, &node_map_));
        } else if (node->op().compare("Conv2DBackpropInput") == 0) {
          node_processor.reset(
              new Conv2DBackpropInputProcessor(graph_, node, &node_map_));
        } else if (node->op().compare("FusedBatchNormGrad") == 0) {
          node_processor.reset(
              new FusedBatchNormGradProcessor(graph_, node, &node_map_));
        } else if (node->op().compare("MaxPoolGrad") == 0) {
          node_processor.reset(
              new MaxPoolGradProcessor(graph_, node, &node_map_));
        } else {
          node_processor.reset(new NodeProcessor(graph_, node, &node_map_));
        }
        node_processor->ConvertNode();
      }
    }

    // This is the second pass where we expand layout-agnostic nodes. This pass
    // only needs to be performed if at least one node in the previous pass is
    // expanded.
    if (graph_->node_size() > node_size_original) {
      AddNodePermConst(kPermNHWCToNCHW, {0, 3, 1, 2});
      AddNodePermConst(kPermNCHWToNHWC, {0, 2, 3, 1});
      AddNodeConcatConst();
      AddNodeReductionConst();
      std::set<string> ops_format_agnostic = GetOpsFormatAgnostic();
      for (int i = 0; i < graph_->node_size(); i++) {
        if (ops_format_agnostic.find(graph_->node(i).op()) !=
            ops_format_agnostic.end()) {
          auto node = graph_->mutable_node(i);
          std::unique_ptr<NodeProcessor> node_processor;
          if (node->op().compare("AddN") == 0) {
            node_processor.reset(new AddNProcessor(graph_, node, &node_map_));
          } else if (node->op().compare("Add") == 0 ||
                     node->op().compare("Mul") == 0 ||
                     node->op().compare("RealDiv") == 0 ||
                     node->op().compare("SquaredDifference") == 0 ||
                     node->op().compare("Sub") == 0) {
            node_processor.reset(
                new BinaryOpProcessor(graph_, node, &node_map_));
          } else if (node->op().compare("Concat") == 0 ||
                     node->op().compare("ConcatV2") == 0) {
            node_processor.reset(new ConcatProcessor(graph_, node, &node_map_));
          } else if (node->op().compare("ReluGrad") == 0) {
            node_processor.reset(
                new ReluGradProcessor(graph_, node, &node_map_));
          } else if (node->op().compare("Slice") == 0) {
            node_processor.reset(new SliceProcessor(graph_, node, &node_map_));
          } else if (node->op().compare("Squeeze") == 0) {
            node_processor.reset(
                new SqueezeProcessor(graph_, node, &node_map_));
          } else if (node->op().compare("Sum") == 0) {
            node_processor.reset(new SumProcessor(graph_, node, &node_map_));
          } else {
            node_processor.reset(
                new AgnosticNodeProcessor(graph_, node, &node_map_));
          }
          node_processor->ConvertNode();
        }
      }
    }
  }

  // Remove all node pairs, where a NCHW-to-NHWC node is followed by
  // a NHWC-to-NCHW node.
  void Collapse() {
    std::unordered_set<string> nodes_removable;
    for (int i = 0; i < graph_->node_size(); i++) {
      auto node = graph_->mutable_node(i);
      if (IsNodeNHWCToNCHW(node->name())) {
        if (IsNodeNCHWToNHWC(node->input(0))) {
          const string& trans_first = node->input(0);
          const string& trans_second = node->name();
          auto outputs = node_map_.GetOutputs(trans_second);
          CHECK(outputs.size() == 1)
              << "There is always only a single output for a Transpose node, "
              << "due to the way it is added by NodeProcessor.";
          NodeDef* output = *outputs.begin();
          string input = node_map_.GetNode(trans_first)->input(0);
          for (int i = 0; i < output->input_size(); i++) {
            if (output->input(i).compare(trans_second) == 0) {
              *output->mutable_input(i) = input;
              break;
            }
          }
          nodes_removable.insert(trans_first);
          nodes_removable.insert(trans_second);
        }
      }
    }
    graph_->mutable_node()->erase(
        std::remove_if(
            graph_->mutable_node()->begin(), graph_->mutable_node()->end(),
            [nodes_removable](const NodeDef& node) {
              return nodes_removable.find(node.name()) != nodes_removable.end();
            }),
        graph_->mutable_node()->end());
  }

  GraphDef* graph_;
  NodeMap node_map_;
};

Status LayoutOptimizer::Optimize(Cluster* cluster, const GrapplerItem& item,
                                 GraphDef* output) {
  *output = item.graph;
  DataLayoutOptimizer layout_optimizer(output);
  return Status::OK();
}

void LayoutOptimizer::Feedback(Cluster* cluster, const GrapplerItem& item,
                               const GraphDef& optimize_output, double result) {
  // Nothing to do for LayoutOptimizer.
}

}  // end namespace grappler
}  // end namespace tensorflow
