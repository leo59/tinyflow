// Copyright (c) 2016 by Contributors
#include <tinyflow/base.h>
#include <nnvm/pass_functions.h>

#include <memory>
#include "./torch_util.h"

namespace tinyflow {

using dmlc::any;
using nnvm::Graph;
using nnvm::IndexedGraph;
using nnvm::ShapeVector;
using nnvm::DTypeVector;
using nnvm::StorageVector;

class TorchExecutor;

/*! \brief shared variable */
struct VarState {
  /*! \brief The internal internal tensor */
  LuaRef tensor;
  /*! \brief The corresponding tblob */
  TBlob blob;

  /*! \return Whether the tensor is initialized already */
  inline bool initialized() const {
    return !tensor.is_nil();
  }
  // reset the space.
  inline void ResetSpace(TShape shape, int dev_mask = kCPU, int dtype = 0) {
    if (tensor.is_nil() ||
        shape != blob.shape ||
        dev_mask != blob.dev_mask ||
        dtype != blob.dtype) {
      TorchState* th = TorchState::ThreadLocalState();
      if (tensor.is_nil()) {
        tensor = th->NewTensorEmpty(dev_mask, dtype);
      }
      th->ResetStorage(
          tensor, th->NewStorage(shape.Size(), dev_mask, dtype), shape);
      this->blob = th->GetTBlob(tensor);
    }
  }
};

// shared variable map structure
using VarStateMap = std::unordered_map<std::string, std::shared_ptr<VarState> >;

// torch session.
class TorchSession : public Session {
 public:
  const std::vector<TBlob>&
  Run(nnvm::Symbol* sym,
      const std::unordered_map<std::string, TBlob>& inputs) override;

 private:
  // entry to store cached executor
  struct ExecEntry {
    std::shared_ptr<TorchExecutor> exec;
    size_t use_count{0};
  };
  // local cached variable states.
  VarStateMap states_;
  // cached executor
  std::unordered_map<Symbol*, ExecEntry> cached_execs_;
};


class TorchExecutor {
 public:
  // initialize the executor
  // possibly update the states.
  void Init(nnvm::Symbol symbol, VarStateMap* states);
  /// run the executor, return the outputs.
  const std::vector<TBlob>& Run(const std::unordered_map<std::string, TBlob>& inputs);
  // return corresponding internal symbol
  inline const nnvm::Symbol& symbol() const {
    return symbol_;
  }

 private:
  // setup the executor space.
  void Setup(const std::unordered_map<std::string, TBlob>& inputs);
  void SetupShapeDType(const std::unordered_map<std::string, TBlob>& inputs, bool* need_redo_infer);
  void SetupStorage();
  void SetupOpExecs();
  // internal symbol and graph
  nnvm::Symbol symbol_;
  nnvm::Graph graph_;
  // shape vector in graph attribute
  const ShapeVector* node_shape_{nullptr};
  // type vector in graph attribute
  const DTypeVector* node_dtype_{nullptr};
  // ----------------------------
  // node auxiliary data structures
  // The device of this executor
  int dev_mask_{kCPU};
  // node id of place holder ops
  std::vector<uint32_t> placeholder_nids_;
  // node id of variable that is assigned in this executor
  std::vector<uint32_t> assign_var_nids_;
  // node id of variable that is readed by this executor
  // can overlap with assign_var_nids_
  std::vector<uint32_t> read_var_nids_;
  // vector maps nid->state, nullptr for non variables.
  std::vector<VarState*> node_states_;
  // ----------------------------
  // execution information
  // data of each outputs
  std::vector<LuaRef> data_entry_;
  // whether data entry is variable.
  std::vector<bool> data_entry_is_var_;
  // internal storage space.
  std::vector<LuaRef> storage_pool_;
  // operator executor closures
  std::vector<LuaRef> op_execs_;
  // The storage space to hold outputs.
  std::vector<LuaRef> outputs_;
  std::vector<TBlob> output_blobs_;
};

Session* Session::Create(const std::string& type) {
  return new TorchSession();
}

const std::vector<TBlob>& TorchSession::Run(
    nnvm::Symbol* sym,
    const std::unordered_map<std::string, TBlob>& inputs) {
  if (cached_execs_.count(sym) != 0) {
    auto& entry = cached_execs_.at(sym);
    const nnvm::Symbol& s = entry.exec->symbol();
    bool stale_exec = (s.outputs.size() != sym->outputs.size());
    if (!stale_exec) {
      for (size_t i = 0; i < s.outputs.size(); ++i) {
        if (s.outputs[i].node.get() != sym->outputs[i].node.get() ||
            s.outputs[i].index != sym->outputs[i].index ||
            s.outputs[i].version != sym->outputs[i].version) {
          stale_exec = true; break;
        }
      }
    }
    if (!stale_exec) {
      ++entry.use_count;
      return entry.exec->Run(inputs);
    } else {
      cached_execs_.erase(sym);
    }
  }
  // dump technique, remove all previous executors
  // better strategy, LRU?
  cached_execs_.clear();
  ExecEntry e;
  e.exec = std::make_shared<TorchExecutor>();
  e.exec->Init(*sym, &states_);
  cached_execs_[sym] = e;
  return e.exec->Run(inputs);
}

void TorchExecutor::Init(nnvm::Symbol symbol, VarStateMap* states) {
  graph_.outputs = symbol.outputs;
  symbol_ = std::move(symbol);
  // initialize all node auxiliary data structures.
  const Op* assign_op = Op::Get("assign");
  const Op* placeholder_op = Op::Get("placeholder");
  const auto& idx = graph_.indexed_graph();
  node_states_.resize(idx.num_nodes(), nullptr);

  std::vector<int> read_count(idx.num_nodes(), 0);
  std::vector<int> assign_count(idx.num_nodes(), 0);

  for (uint32_t i = idx.num_nodes(); i != 0; --i) {
    uint32_t nid = i - 1;
    auto& inode = idx[nid];
    if (inode.source->is_variable()) {
      const std::string& key = inode.source->attrs.name;
      if (states->count(key) == 0) {
        (*states)[key] = std::make_shared<VarState>();
      }
      node_states_[nid] = states->at(key).get();
      if (read_count[nid] != 0) {
        read_var_nids_.push_back(nid);
      }
      if (assign_count[nid] != 0) {
        assign_var_nids_.push_back(nid);
      }
    } else {
      if (inode.source->op() == placeholder_op) {
        placeholder_nids_.push_back(nid);
      } else if (inode.source->op() == assign_op) {
        CHECK_EQ(inode.inputs.size(), 2);
        ++read_count[inode.inputs[0].node_id];
        ++assign_count[inode.inputs[0].node_id];
      } else {
        for (auto e : inode.inputs) {
          ++read_count[e.node_id];
        }
      }
    }
  }
}

const std::vector<TBlob>&
TorchExecutor::Run(const std::unordered_map<std::string, TBlob>& inputs) {
  Setup(inputs);
  for (size_t i = 0; i < op_execs_.size(); ++i) {
    if (!op_execs_[i].is_nil()) op_execs_[i]();
  }
  {
    // copy outputs
    output_blobs_.clear();
    auto* th = TorchState::ThreadLocalState();
    const auto& idx = graph_.indexed_graph();
    for (size_t i = 0; i < outputs_.size(); ++i) {
      uint32_t eid = idx.entry_id(idx.outputs()[i]);
      th->CopyFromTo(data_entry_[eid], outputs_[i]);
      output_blobs_.push_back(th->GetTBlob(outputs_[i]));
    }
  }
  return output_blobs_;
}

void TorchExecutor::Setup(const std::unordered_map<std::string, TBlob>& inputs) {
  bool need_redo_infer;
  SetupShapeDType(inputs, &need_redo_infer);
  if (need_redo_infer) SetupStorage();
  if (op_execs_.size() == 0) SetupOpExecs();
  {
    // copy inputs
    auto* th = TorchState::ThreadLocalState();
    const auto& idx = graph_.indexed_graph();
    for (uint32_t nid : placeholder_nids_) {
      const std::string& key = idx[nid].source->attrs.name;
      const TBlob& value = inputs.at(key);
      th->CopyFromTo(th->NewTensorShared(value),
                     data_entry_[idx.entry_id(nid, 0)]);
    }
  }
}

void TorchExecutor::SetupShapeDType(
    const std::unordered_map<std::string, TBlob>& inputs,
    bool* p_need_redo_infer) {
  const auto& idx = graph_.indexed_graph();
  bool& need_redo_infer = *p_need_redo_infer;
  need_redo_infer = (node_shape_ == nullptr);

  // check the variable states
  if (!need_redo_infer) {
    CHECK(node_dtype_ != nullptr);
    for (uint32_t nid : read_var_nids_) {
      VarState* state = node_states_[nid];
      CHECK(state != nullptr);
      CHECK(state->initialized())
          << "Attempt to execute a graph un-initialized Variable";
      if (node_shape_->at(nid) != state->blob.shape) {
        need_redo_infer = true; break;
      }
      if (node_dtype_->at(nid) != state->blob.dtype) {
        need_redo_infer = true; break;
      }
    }
  }
  // check placeholder shapes.
  if (!need_redo_infer) {
    for (uint32_t nid : placeholder_nids_) {
      const std::string& key = idx[nid].source->attrs.name;
      CHECK(inputs.count(key))
          << "Not enought placeholder argument to feed_dict";
      const TBlob& value = inputs.at(key);
      if (node_shape_->at(idx.entry_id(nid, 0)) != value.shape) {
        need_redo_infer = true; break;
      }
      if (node_dtype_->at(idx.entry_id(nid, 0)) != value.dtype) {
        need_redo_infer = true; break;
      }
    }
  }

  if (!need_redo_infer) return;
  // run shape inference.
  ShapeVector new_shape(idx.num_node_entries(), TShape());
  DTypeVector new_dtype(idx.num_node_entries(), -1);

  for (uint32_t nid : read_var_nids_) {
    new_shape[idx.entry_id(nid, 0)] = node_states_[nid]->blob.shape;
    new_dtype[idx.entry_id(nid, 0)] = node_states_[nid]->blob.dtype;
  }
  for (uint32_t nid : placeholder_nids_) {
    const std::string& key = idx[nid].source->attrs.name;
    const TBlob& value = inputs.at(key);
    new_shape[idx.entry_id(nid, 0)] = value.shape;
    new_dtype[idx.entry_id(nid, 0)] = value.dtype;
  }
  graph_.attrs["shape"] = std::make_shared<any>(std::move(new_shape));
  graph_.attrs["dtype"] = std::make_shared<any>(std::move(new_dtype));
  graph_ = ApplyPasses(std::move(graph_), {"InferShape", "InferType"});
  CHECK_EQ(graph_.GetAttr<size_t>("shape_num_unknown_nodes"), 0)
      << "Shape information in the graph is in-complete";
  CHECK_EQ(graph_.GetAttr<size_t>("dtype_num_unknown_nodes"), 0)
      << "Type information in the graph is in-complete";
  node_shape_ = &(graph_.GetAttr<ShapeVector>("shape"));
  node_dtype_ = &(graph_.GetAttr<DTypeVector>("dtype"));
  // setup out Variable space.
  for (uint32_t nid : assign_var_nids_) {
    node_states_[nid]->ResetSpace(
        node_shape_->at(idx.entry_id(nid, 0)),
        dev_mask_,
        node_dtype_->at(idx.entry_id(nid, 0)));
  }
}

void TorchExecutor::SetupStorage() {
  const auto& idx = graph_.indexed_graph();
  if (storage_pool_.size() == 0) {
    graph_ = nnvm::ApplyPass(std::move(graph_), "PlanMemory");
  }
  const auto& vstorage = graph_.GetAttr<StorageVector>("storage_id");
  const auto& vshape = graph_.GetAttr<ShapeVector>("shape");
  auto* th = TorchState::ThreadLocalState();

  if (data_entry_.size() == 0) {
    data_entry_.resize(idx.num_node_entries());
    data_entry_is_var_.resize(idx.num_node_entries(), false);
    for (size_t i = 0; i < data_entry_.size(); ++i) {
      data_entry_[i] = th->NewTensorEmpty(dev_mask_);
    }
    for (uint32_t nid : idx.input_nodes()) {
      CHECK(node_states_[nid] != nullptr);
      data_entry_[idx.entry_id(nid, 0)] = node_states_[nid]->tensor;
      data_entry_is_var_[idx.entry_id(nid, 0)] = true;
    }
  }

  // size of each storage pool entry
  std::vector<size_t> pool_entry_size;
  for (size_t i = 0; i < vshape.size(); ++i) {
    if (data_entry_is_var_[i]) continue;
    int storage_id = vstorage[i];
    size_t size = vshape[i].Size();
    CHECK_GE(storage_id, 0) << "Do not support runtime shape op yet";
    size_t sid = static_cast<size_t>(storage_id);
    if (sid >= pool_entry_size.size()) {
      pool_entry_size.resize(sid + 1, 0);
    }
    pool_entry_size[sid] = std::max(pool_entry_size[sid], size);
  }
  storage_pool_.clear();
  for (size_t i = 0; i < pool_entry_size.size(); ++i) {
    storage_pool_.push_back(
        th->NewStorage(pool_entry_size[i], dev_mask_));
  }
  // assign pooled data to entry
  for (size_t i = 0; i < data_entry_.size(); ++i) {
    if (data_entry_is_var_[i]) continue;
    int storage_id = vstorage[i];
    th->ResetStorage(data_entry_[i], storage_pool_.at(storage_id), vshape[i]);
  }

  outputs_.resize(idx.outputs().size());
  for (size_t i = 0; i < outputs_.size(); ++i) {
    uint32_t eid = idx.entry_id(idx.outputs()[i]);
    LuaRef t = th->NewTensorEmpty(kCPU);
    th->ResetStorage(t, th->NewStorage(vshape[eid].Size(), kCPU), vshape[eid]);
    outputs_[i] = t;
  }
}

void TorchExecutor::SetupOpExecs() {
  const auto& lua_compute_code =
      nnvm::Op::GetAttr<FLuaComputeCode>("FLuaComputeCode");
  auto* lua = LuaState::ThreadLocalState();
  LuaRef fcreate_exec_closure = lua->Eval(R"(
    return
    function(fcompute, ins, outs)
      return function() fcompute(ins, outs) end
    end
  )");
  // get the graph
  const auto& idx = graph_.indexed_graph();
  op_execs_.resize(idx.num_nodes());
  // setup the array and requirements.
  for (uint32_t nid = 0; nid < idx.num_nodes(); ++nid) {
    const auto& inode = idx[nid];
    if (inode.source->is_variable()) continue;
    std::vector<LuaRef> in_array, out_array;
    for (const auto& e : inode.inputs) {
      in_array.push_back(data_entry_[idx.entry_id(e)]);
    }
    for (uint32_t index = 0; index < inode.source->num_outputs(); ++index) {
      uint32_t eid = idx.entry_id(nid, index);
      out_array.push_back(data_entry_[eid]);
    }
    LuaRef fcompute;
    if (lua_compute_code.count(inode.source->op())) {
      std::string lua_str = "return " + lua_compute_code[inode.source->op()];
      try {
        fcompute = lua->Eval(lua_str);
      } catch(dmlc::Error e) {
        LOG(FATAL) << "Error during setuo FLuaComputeCode for "
                   << inode.source->op()->name << "\nlua code\n----\n"
                   << lua_str
                   << "-----\n"
                   << e.what();
      }
    } else {
      LOG(FATAL) << "Function FLuaCompute is not registered on "
                 << inode.source->op()->name;
    }
    op_execs_[nid] = fcreate_exec_closure(
        fcompute, in_array, out_array);
  }
}

}  // namespace tinyflow