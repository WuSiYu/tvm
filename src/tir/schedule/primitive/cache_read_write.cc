/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "../utils.h"

namespace tvm {
namespace tir {

/******** Error Classes ********/

class NotSingleWriteBlock : public ScheduleError {
 public:
  explicit NotSingleWriteBlock(IRModule mod, Buffer buffer, Array<StmtSRef> write_blocks)
      : mod_(std::move(mod)), buffer_(std::move(buffer)) {
    ICHECK_GT(write_blocks.size(), 1);
    write_blocks_.reserve(write_blocks.size());
    for (const StmtSRef& block_sref : write_blocks) {
      const BlockNode* block = TVM_SREF_TO_BLOCK(block_sref);
      write_blocks_.push_back(GetRef<Block>(block));
    }
  }

  String FastErrorString() const final {
    return "ScheduleError: The buffer is allowed to be written by single block.";
  }

  String DetailRenderTemplate() const final {
    size_t k = write_blocks_.size();
    return "The buffer " + buffer_->name + " is expected to be written by single block, but got " +
           std::to_string(k) + " blocks who write it.";
  }

  IRModule mod() const final { return mod_; }
  Array<ObjectRef> LocationsOfInterest() const final {
    return {write_blocks_.begin(), write_blocks_.end()};
  }

 private:
  IRModule mod_;
  Buffer buffer_;
  Array<Block> write_blocks_;
};

/******** Helper Functions/Classes ********/

/*! \brief The auxiliary info used for the insertion point and content of the cache stage. */
struct CacheStageInfo {
  /*! \brief The buffer to be read. */
  Buffer read_buffer;
  /*! \brief The buffer to be written. */
  Buffer write_buffer;
  /*! \brief The buffer allocation to be inserted into the block signature. */
  Buffer alloc;
  /*! \brief The AST node whose body is where the cache stage should be inserted. */
  StmtSRef loc_sref;
  /*! \brief The index to insert the cache_read/cache_write stage. */
  size_t loc_pos;
  /*! \brief The cache_read/cache_write stage to be inserted. */
  Stmt cache_stage;
  /*! \brief The map used for ScheduleStateNode::Replace. */
  Map<Block, Block> block_reuse;
  /*! \brief A list of blocks that will consume the new cache. */
  Array<StmtSRef> consumer_blocks;
};

/*! \brief Return the buffer region realted with the buffer */
Optional<BufferRegion> GetBufferRegionFromBuffer(const Array<BufferRegion>& buffer_regions,
                                                 const Buffer& buffer) {
  Optional<BufferRegion> res = NullOpt;
  for (const auto& region : buffer_regions) {
    if (region->buffer.same_as(buffer)) {
      ICHECK(!res.defined());
      res = region;
    }
  }
  return res;
}

/*!
 * \brief Create a loop nest that represents cache copy (cache_read / cache_write) from read buffer
 *        to write buffer.
 * \note This function will store the stmt with loop nesting to the CacheStageInfo, but only return
 *        the inside block.
 * \param cache_region The cached copy region.
 * \param info The cache stage information, which will be updated in the function.
 * \param storage_scope The storage scope of the cached buffer (only used in naming here)
 * \returns A block indicating the body of the loop nesting.
 */
Block MakeCacheStage(const BufferRegion& cache_region, CacheStageInfo* info,
                     const String& storage_scope) {
  // loop variables
  std::vector<Var> loop_vars;
  // bindings in block realize
  std::vector<PrimExpr> iter_values;
  // Create loop vars and block vars' binding_value
  for (const Range& axis_range : cache_region->region) {
    Var loop_var("ax" + std::to_string(loop_vars.size()), axis_range->extent.dtype());
    loop_vars.push_back(loop_var);
    iter_values.push_back(axis_range->min + loop_var);
  }
  // block variables
  Array<IterVar> block_vars;
  // block access region for read/write buffers
  Region access_region;
  // indices used in block body
  Array<PrimExpr> access_indices;
  // Create block vars, block's accessed region and accessing indices
  for (const PrimExpr& dim : cache_region->buffer->shape) {
    Var var("v" + std::to_string(access_indices.size()), dim.dtype());
    block_vars.push_back(IterVar(/*dom=*/Range::FromMinExtent(make_zero(dim->dtype), dim),
                                 /*var=*/var,
                                 /*IterVarType=*/kDataPar));
    access_indices.push_back(var);
    access_region.push_back(Range::FromMinExtent(var, make_const(var.dtype(), 1)));
  }

  // Create the body block:
  //   reads = [read_buffer[access_region]]
  //   writes = [write_buffer[access_region]]
  //     write_buffer[access_indices] = read_buffer[access_indices]
  Block block(
      /*iter_vars=*/std::move(block_vars),
      /*reads=*/{BufferRegion(info->read_buffer, access_region)},
      /*writes=*/{BufferRegion(info->write_buffer, access_region)},
      /*name_hint=*/cache_region->buffer->name + "_" + storage_scope,
      /*body=*/
      BufferStore(info->write_buffer, BufferLoad(info->read_buffer, access_indices),
                  access_indices),
      /*init=*/NullOpt,
      /*alloc_buffers=*/{},
      /*match_buffers=*/{},
      /*annotations=*/{});
  // Create the block realize node
  Stmt body = BlockRealize(/*values=*/iter_values,
                           /*predicate=*/const_true(),
                           /*block=*/block);
  // Create surrounding loops
  for (size_t i = loop_vars.size(); i >= 1; --i) {
    body = For(/*loop_var=*/loop_vars[i - 1],
               /*min=*/0,
               /*extent=*/cache_region->region[i - 1]->extent,
               /*kind=*/ForKind::kSerial,
               /*body=*/body);
  }
  info->cache_stage = std::move(body);
  return block;
}

/*!
 * \brief Create the reindex block and generate the corresponding outer loops.
 * \details The reindex block is a data copy block between the reindex buffer (the intermediate
 * buffer), and the target buffer.
    If buffer_index_type == kWrite, copy from the reindex buffer to the target buffer.
    If buffer_index_type == kRead, copy from the target buffer to the reindex buffer.
    The reindex block has the same block iters and the surrounding loops as the input block.
 However, if a block iter is not used in the indices of the target buffer being reindexed, the
 domain of the block iter, and the corresponding outer loop, will become constant value one, making
 it a trivial iter.
 * \param block The block to be reindexed
 * \param info The cache info
 * \param covered The set of block iter vars covered in the buffer access indices
 * \param original_indices The original buffer access indices
 * \param buffer_index The index of the target buffer
 * \param buffer_index_type The type of buffer index
 * \return The reindex block.
 */
Block MakeReIndexStage(const Block& block, CacheStageInfo* info,
                       const std::unordered_set<Var, ObjectPtrHash, ObjectPtrEqual>& covered,
                       const Array<PrimExpr>& original_indices, int buffer_index,
                       BufferIndexType buffer_index_type) {
  // iters of the reindex block
  Array<IterVar> new_block_iters;
  // the substition map from the original block iter to the iters of the reindex block
  std::unordered_map<Var, PrimExpr, ObjectPtrHash, ObjectEqual> block_var_replace_map;
  // indices to access the reindex buffer and the target buffer
  Array<PrimExpr> reindex_indices, target_indices;

  // Step 1: Create block iters, access regions of the reindex block, and accessing indices to the
  // reindex buffer.
  std::unordered_set<int> skipped_block_iters;
  for (int i = 0, n = block->iter_vars.size(); i < n; ++i) {
    const IterVar& iter = block->iter_vars[i];
    Var var("v" + std::to_string(new_block_iters.size()), iter->var->dtype);
    bool used = covered.count(iter->var);
    if (used) {
      new_block_iters.push_back(IterVar(/*dom=*/iter->dom,
                                        /*var=*/var,
                                        /*IterVarType=*/kDataPar));
    } else {
      skipped_block_iters.insert(i);
    }
    if (used) {
      reindex_indices.push_back(var);
    }
    block_var_replace_map[iter->var] = var;
  }

  // Step 2: Replace the original block iters with the new block iters
  for (const PrimExpr& index : original_indices) {
    target_indices.push_back(Substitute(index, block_var_replace_map));
  }

  // Step 3: Create the reindex block

  // The src and the dst region and indices of the data copy
  Region src_region{nullptr};
  Region dst_region{nullptr};
  Array<PrimExpr> src_indices{nullptr};
  Array<PrimExpr> dst_indices{nullptr};

  if (buffer_index_type == BufferIndexType::kWrite) {
    src_indices = reindex_indices;
    dst_indices = target_indices;
  } else {
    src_indices = target_indices;
    dst_indices = reindex_indices;
  }

  // Create the body block
  Block new_block(
      /*iter_vars=*/new_block_iters,
      /*reads=*/{BufferRegion::FromPoint(info->read_buffer, src_indices)},
      /*writes=*/{BufferRegion::FromPoint(info->write_buffer, dst_indices)},
      /*name_hint=*/info->write_buffer->name + "_reindex",
      /*body=*/
      BufferStore(info->write_buffer, BufferLoad(info->read_buffer, src_indices), dst_indices));

  // Step 4: Create surrounding loops

  // Create loop vars and bindings for block iters
  std::vector<Var> loop_vars;         // loop variables
  std::vector<PrimExpr> iter_values;  // bindings in block realize
  for (int i = 0; i < static_cast<int>(block->iter_vars.size()); ++i) {
    if (skipped_block_iters.count(i)) {
      continue;
    }
    Var loop_var("ax" + std::to_string(loop_vars.size()), block->iter_vars[i]->var->dtype);
    loop_vars.push_back(loop_var);
    iter_values.push_back(loop_var);
  }

  // Create the block realize node
  Stmt body = BlockRealize(/*values=*/iter_values,
                           /*predicate=*/const_true(),
                           /*block=*/new_block);

  // Create the chain of loops
  for (int i = static_cast<int>(new_block_iters.size()) - 1; i >= 0; --i) {
    body = For(/*loop_var=*/loop_vars[i],
               /*min=*/new_block_iters[i]->dom->min,
               /*extent=*/new_block_iters[i]->dom->extent,
               /*kind=*/ForKind::kSerial,
               /*body=*/std::move(body));
  }
  // Update cache info, which will be used in the later rewriting.
  info->cache_stage = std::move(body);
  return new_block;
}

/*!
 * \brief Recalculate the `affine_binding` flag of a specifc block
 * \param block_sref The sref to the specific block
 */
bool CalculateAffineFlag(const ScheduleState& self, const StmtSRef& block_sref) {
  if (block_sref->parent == nullptr) {
    return true;
  }
  arith::Analyzer analyzer;
  StmtSRef parent_sref = GetRef<StmtSRef>(block_sref->parent);
  return IsAffineBinding(/*realize=*/GetBlockRealize(self, block_sref),
                         /*loop_var_ranges=*/LoopDomainOfSRefTreePath(parent_sref),
                         /*analyzer=*/&analyzer);
}

/*!
 * \brief Insert the cache_read/cache_write stage into the specific position
 * \param stmt A sequence of statements or a single statement that the new stage is inserted in
 * \param pos The position where the cache stage is inserted
 * \param stage The stage to be inserted
 * \return A SeqStmt, the result after insertion
 */
SeqStmt InsertCacheStage(const Stmt& stmt, int pos, const Stmt& stage) {
  if (const auto* seq_stmt = stmt.as<SeqStmtNode>()) {
    ObjectPtr<SeqStmtNode> result = make_object<SeqStmtNode>(*seq_stmt);
    result->seq.insert(result->seq.begin() + pos, stage);
    return SeqStmt(result);
  }
  if (pos == 0) {
    return SeqStmt({stage, stmt});
  }
  ICHECK_EQ(pos, 1);
  return SeqStmt({stmt, stage});
}

/*!
 * \brief Get the only writer block of the input buffer in a given scope block.
 * \param self The state of the schedule
 * \param scope_sref The scope block where the write is considered
 * \param buffer The queried buffer
 * \return The sref of the only writer of the input buffer in the given scope,
 *         or `NullOpt` if no block writes it in the scope.
 * \throw NotSingleWriteBlock if there are more than one intrested block.
 */
Optional<StmtSRef> GetOnlyWriteBlock(ScheduleState self, const StmtSRef& scope_sref,
                                     const Buffer& buffer) {
  BlockScope scope = self->GetBlockScope(scope_sref);
  auto it = scope->buffer_writers.find(buffer);
  if (it == scope->buffer_writers.end()) {
    return NullOpt;
  } else {
    const Array<StmtSRef>& block_srefs = it->second;
    ICHECK(!block_srefs.empty());
    if (block_srefs.size() > 1) {
      throw NotSingleWriteBlock(self->mod, buffer, block_srefs);
    }
    return block_srefs[0];
  }
}

/*!
 * \brief Get the buffer region under the sref tree path [dom_low_inclusive, dom_high_exclusive)
 * \param self The state of the schedule.
 * \param buffer_region The buffer region to be analyzed.
 * \param block_sref The sref of the block related to the region.
 * \param dom_low_inclusive The lowest node in the sref tree path.
 * \param dom_high_exclusive The highest node in the sref tree path.
 * \return The relaxed buffer region.
 */
BufferRegion RelaxBufferRegion(ScheduleState self, const BufferRegion& buffer_region,
                               const StmtSRef& block_sref, const StmtSRef& dom_low_inclusive,
                               const StmtSRef& dom_high_exclusive) {
  BlockRealize realize = GetBlockRealize(self, block_sref);
  Map<Var, PrimExpr> binding = GetBindings(realize);
  const Buffer& buffer = buffer_region->buffer;
  arith::Analyzer analyzer;
  BufferRegion subst_region = BufferRegion(buffer, Substitute(buffer_region->region, binding));
  Array<arith::IntSet> int_sets = AnalyzeRegionUpperBound(
      /*region=*/subst_region,
      /*predicate=*/realize->predicate,
      /*dom_low_inclusive=*/dom_low_inclusive,
      /*dom_high_exclusive=*/dom_high_exclusive,
      /*analyzer=*/&analyzer);
  ICHECK_EQ(buffer_region->region.size(), int_sets.size());

  Region region;
  region.reserve(int_sets.size());
  for (size_t i = 0; i < int_sets.size(); ++i) {
    region.push_back(int_sets[i].CoverRange(Range::FromMinExtent(0, buffer->shape[i])));
  }
  return BufferRegion(buffer, region);
}

/*! \brief Detect the insertion position of the new cache stage */
class CacheLocDetector : public StmtVisitor {
 public:
  /*!
   * \brief Detect the insertion position of the cache stage, and write the position into the
   * CacheStageInfo \param self The state of the schedule \param block_sref The sref of the unique
   * writer block of the buffer being applied cache_read or cache_write \param scope_sref The sref
   * of the scope block of the cached block \param info The cache stage info.
   */
  static void Detect(const ScheduleState& self, const StmtSRef& block_sref,
                     const StmtSRef& scope_sref, CacheStageInfo* info) {
    std::vector<StmtSRef> related_blocks;
    // If consumer is specified, skip detecting the others
    if (info->consumer_blocks.size() > 0) {
      for (StmtSRef consumer : info->consumer_blocks) {
        related_blocks.emplace_back(consumer);
      }
    } else {
      for (const Dependency& def : self->GetBlockScope(scope_sref)->GetDepsBySrc(block_sref)) {
        if (def->kind == DepKind::kRAW) {
          related_blocks.push_back(def->dst);
        }
      }
    }
    if (!related_blocks.empty()) {
      CacheLocDetector detector(self, block_sref, scope_sref, related_blocks);
      detector(GetRef<Stmt>(scope_sref->stmt));
      info->loc_sref = detector.loc_sref_;
      info->loc_pos = detector.loc_pos_;
    } else {
      info->loc_sref = scope_sref;
      const auto* body = scope_sref->StmtAs<BlockNode>()->body.as<SeqStmtNode>();
      info->loc_pos = body == nullptr ? 1 : body->size();
    }
  }

 private:
  /*!
   * \brief Constructor
   * \param self The state of the schedule
   * \param block_sref The sref of the unique writer block of the buffer being applied cache_read or
   * cache_write \param scope_sref The sref of the scope block of the cached block \param
   * related_blocks Producer blocks for cache_write, or consumer blocks for cache_read
   */
  CacheLocDetector(const ScheduleState self, const StmtSRef& block_sref, const StmtSRef& scope_sref,
                   const std::vector<StmtSRef>& related_blocks)
      : self_(self),
        block_sref_(block_sref),
        scope_sref_(scope_sref),
        related_blocks_(related_blocks) {}

  void VisitStmt_(const SeqStmtNode* seq_stmt) final {
    bool previous_visited_block = visited_block_;
    visited_block_ = false;

    for (size_t i = 0; i < seq_stmt->size(); ++i) {
      if (loc_pos_ != -1) {
        break;
      }
      VisitStmt(seq_stmt->seq[i]);
      // `pos` can be assigned only once when we visited `block_sref`
      if (visited_block_ && visited_related_ && loc_pos_ == -1) {
        // The offset of insert position from the block
        loc_pos_ = i;
        return;
      } else if (visited_related_) {
        // If meet the target consumer, stop searching
        visited_block_ = visited_block_ || previous_visited_block;
        return;
      }
    }
  }

  void VisitStmt_(const BlockNode* block) final {
    // Only visit the current scope under buffer writer's parent block
    if (block == scope_sref_->stmt) {
      // The block vistied is the current parent scope
      StmtVisitor::VisitStmt_(block);
      // Handling cases when insert outside any loop or cache_read for input buffer
      if (visited_related_ && !loc_sref_.defined()) {
        loc_sref_ = self_->stmt2ref.at(block);
        // Handling cache_read for input buffer
        if (visited_block_ == false && loc_pos_ == -1) {
          loc_pos_ = 0;
        }
      }
      return;
    }
    // Update `visited_block`
    if (block_sref_->stmt == block) {
      visited_block_ = true;
      return;
    }
    // Update `visited_related`
    for (const StmtSRef& related_block : related_blocks_) {
      if (related_block->stmt == block) {
        visited_related_ = true;
        return;
      }
    }
  }

  void VisitStmt_(const ForNode* loop) final {
    StmtVisitor::VisitStmt_(loop);
    if (visited_block_ && visited_related_ && !loc_sref_.defined() && loc_pos_ != -1) {
      loc_sref_ = self_->stmt2ref.at(loop);
    }
  }

 private:
  /*! \brief The schedule class */
  const ScheduleState self_;
  /*! \brief The dominate block which write the buffer */
  const StmtSRef& block_sref_;
  /*! \brief The parent scope of the dominate block */
  const StmtSRef& scope_sref_;
  /*! \brief Producer blocks for cache_write and consumer blocks for cache_read */
  const std::vector<StmtSRef>& related_blocks_;
  /*! \brief The flag whether we have visited the dominate block */
  bool visited_block_{false};
  /*! \brief The flag whether we have visited at least one related blocks */
  bool visited_related_{false};
  /*! \brief The AST node whose body is where the cache stage should be inserted */
  StmtSRef loc_sref_{nullptr};
  /*! \brief The index to insert the cache_read/cache_write stage */
  int loc_pos_{-1};
};

/*! \brief Mutator for CacheRead. */
class CacheReadRewriter : public StmtExprMutator {
 public:
  /*!
   * \brief Rewrite the AST and add a cache_read stage with the information provided
   * \param scope_sref The parent scope of this mutation
   * \param info The cache stage information
   * \return The new AST rooting at the original parent scope
   */
  static Stmt Rewrite(const StmtSRef& scope_sref, CacheStageInfo* info) {
    CacheReadRewriter rewriter(scope_sref, info);
    return rewriter(GetRef<Stmt>(scope_sref->stmt));
  }

 private:
  explicit CacheReadRewriter(const StmtSRef& scope_sref, CacheStageInfo* info)
      : scope_sref_(scope_sref), info_(info) {}

  Stmt VisitStmt_(const ForNode* loop) final {
    Stmt stmt = StmtMutator::VisitStmt_(loop);
    // Check the insertion point
    if (loop == info_->loc_sref->stmt) {
      // Insert cache stage into the loop if it is the right place
      ObjectPtr<ForNode> n = make_object<ForNode>(*stmt.as<ForNode>());
      n->body = InsertCacheStage(n->body, info_->loc_pos, info_->cache_stage);
      stmt = Stmt(n);
    }
    return stmt;
  }

  Stmt VisitStmt_(const BlockNode* block) final {
    Block old_stmt = GetRef<Block>(block);
    // Check if this block is one of the specified consumers.
    // If no consumer blocks are specified, all blocks should be considered consumers.
    bool is_consumer = info_->consumer_blocks.empty();
    // Otherwise check if this is one of the specified blocks.
    for (StmtSRef consumer_sref : info_->consumer_blocks) {
      const BlockNode* consumer_node = TVM_SREF_TO_BLOCK(consumer_sref);
      Block consumer_block = GetRef<Block>(consumer_node);
      if (old_stmt.same_as(consumer_block)) {
        is_consumer = true;
      }
    }
    // Keep track of this blocks status. We'll use this when rewriting loads.
    current_block_consumes = is_consumer;
    // We don't mutate the block which generates info->read_buffer.
    if (block != scope_sref_->stmt &&
        GetBufferRegionFromBuffer(block->writes, info_->read_buffer).defined()) {
      return std::move(old_stmt);
    }
    // Mutate the body
    Block stmt = Downcast<Block>(StmtMutator::VisitStmt_(block));
    // Check the insertion point
    if (block == info_->loc_sref->stmt) {
      // Insert cache stage into the block if it is the right place
      ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
      n->body = InsertCacheStage(n->body, info_->loc_pos, info_->cache_stage);
      stmt = Block(n);
    }
    // Check if it is the block corresponding to the parent scope
    if (block == scope_sref_->stmt) {
      // If so, put buffer allocation on the parent scope
      ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
      n->alloc_buffers.push_back(info_->alloc);
      stmt = Block(n);
    } else {
      // Otherwise, update read regions and match_buffers
      // Only make this change if the block is one of the specified consumers.
      if (is_consumer) {
        Array<BufferRegion> reads =
            ReplaceBuffer(block->reads, info_->read_buffer, info_->write_buffer);
        Array<MatchBufferRegion> match_buffers =
            ReplaceBuffer(block->match_buffers, info_->read_buffer, info_->write_buffer);
        if (!reads.same_as(block->reads) || !match_buffers.same_as(block->match_buffers)) {
          ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
          n->reads = std::move(reads);
          n->match_buffers = std::move(match_buffers);
          stmt = Block(n);
        }
      }
    }
    info_->block_reuse.Set(old_stmt, stmt);
    return std::move(stmt);
  }

  PrimExpr VisitExpr_(const BufferLoadNode* load) final {
    if (load->buffer.same_as(info_->read_buffer) && current_block_consumes) {
      ObjectPtr<BufferLoadNode> n = make_object<BufferLoadNode>(*load);
      n->buffer = info_->write_buffer;
      return PrimExpr(n);
    }
    return ExprMutator::VisitExpr_(load);
  }

  PrimExpr VisitExpr_(const LoadNode* op) final {
    LOG(FATAL) << "Unexpected use of deprecated LoadNode.  Please use BufferLoadNode instead.";
    return PrimExpr();
  }

  PrimExpr VisitExpr_(const VarNode* op) final {
    if (op == info_->read_buffer->data.get()) {
      return info_->write_buffer->data;
    }
    return GetRef<PrimExpr>(op);
  }

 private:
  /*! \brief The parent scope of the insertion */
  const StmtSRef& scope_sref_;
  /*! \brief The info for inserting cache stage */
  CacheStageInfo* info_;
  /*! \brief Whether the most recently visited block is a specified consumer. */
  bool current_block_consumes;
};

/*! \brief Mutator for CacheWrite */
class CacheWriteRewriter : public StmtExprMutator {
 public:
  /*!
   * \brief Rewrite the AST and add a cache_write stage with the information provided.
   * \param scope_sref The parent scope of this mutation.
   * \param writer_block_sref The only writer block in the scope.
   * \param info The cache stage information.
   * \return The new AST rooting at the original parent scope.
   */
  static Stmt Rewrite(const StmtSRef& scope_sref, const StmtSRef& writer_block_sref,
                      CacheStageInfo* info) {
    CacheWriteRewriter rewriter(scope_sref, writer_block_sref, info);
    return rewriter(GetRef<Stmt>(scope_sref->stmt));
  }

 private:
  explicit CacheWriteRewriter(const StmtSRef& scope_sref, const StmtSRef& writer_block_sref,
                              CacheStageInfo* info)
      : scope_sref_(scope_sref), writer_block_sref_(writer_block_sref), info_(info) {}

  Stmt VisitStmt_(const ForNode* loop) final {
    Stmt stmt = StmtMutator::VisitStmt_(loop);
    // Check the insertion point
    if (loop == info_->loc_sref->stmt) {
      // Insert cache stage into the loop if it is the right place
      ObjectPtr<ForNode> n = make_object<ForNode>(*stmt.as<ForNode>());
      n->body = InsertCacheStage(n->body, info_->loc_pos, info_->cache_stage);
      stmt = Stmt(n);
    }
    return stmt;
  }

  Stmt VisitStmt_(const BlockNode* block) final {
    Block old_stmt = GetRef<Block>(block);
    // We only mutate the block which generates info->write_buffer
    if (block != writer_block_sref_->stmt && block != scope_sref_->stmt && !under_writer_block_) {
      return std::move(old_stmt);
    }

    // Mutate the body
    bool under_scope = under_writer_block_ || block == writer_block_sref_->stmt;
    std::swap(under_scope, under_writer_block_);
    Block stmt = Downcast<Block>(StmtMutator::VisitStmt_(block));
    std::swap(under_scope, under_writer_block_);

    // Find the insertion point
    if (block == info_->loc_sref->stmt) {
      ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
      n->body = InsertCacheStage(n->body, info_->loc_pos, info_->cache_stage);
      stmt = Block(n);
    }
    // Put buffer allocation on the parent scope
    if (block == scope_sref_->stmt) {
      ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
      n->alloc_buffers.push_back(info_->alloc);
      stmt = Block(n);
    } else {
      // Since cache_write changes the block, we need to update the buffer it writes
      auto writes = ReplaceBuffer(block->writes, info_->write_buffer, info_->read_buffer);
      auto reads = ReplaceBuffer(block->reads, info_->write_buffer, info_->read_buffer);
      auto match_buffers =
          ReplaceBuffer(block->match_buffers, info_->write_buffer, info_->read_buffer);
      if (!writes.same_as(block->writes) || !reads.same_as(block->reads) ||
          !match_buffers.same_as(block->match_buffers)) {
        ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
        n->writes = std::move(writes);
        n->reads = std::move(reads);
        n->match_buffers = std::move(match_buffers);
        stmt = Block(n);
      }
    }
    info_->block_reuse.Set(old_stmt, stmt);
    return std::move(stmt);
  }

  Stmt VisitStmt_(const BufferStoreNode* store) final {
    BufferStore stmt = Downcast<BufferStore>(StmtMutator::VisitStmt_(store));
    if (stmt->buffer.same_as(info_->write_buffer)) {
      auto n = CopyOnWrite(stmt.get());
      n->buffer = info_->read_buffer;
      return Stmt(n);
    } else {
      return std::move(stmt);
    }
  }

  PrimExpr VisitExpr_(const BufferLoadNode* load) final {
    if (load->buffer.same_as(info_->write_buffer)) {
      ObjectPtr<BufferLoadNode> n = make_object<BufferLoadNode>(*load);
      n->buffer = info_->read_buffer;
      return PrimExpr(n);
    }
    return ExprMutator::VisitExpr_(load);
  }

  PrimExpr VisitExpr_(const LoadNode* op) final {
    LOG(FATAL) << "Unexpected use of deprecated LoadNode.  Please use BufferLoadNode instead.";
    return PrimExpr();
  }

  Stmt VisitStmt_(const StoreNode* op) final {
    LOG(FATAL) << "Unexpected use of deprecated StoreNode.  Please use BufferStoreNode instead.";
    return Stmt();
  }

  PrimExpr VisitExpr_(const VarNode* op) final {
    if (op == info_->write_buffer->data.get()) {
      return info_->read_buffer->data;
    }
    return GetRef<PrimExpr>(op);
  }

 private:
  /*! \brief The parent scope of the insertion. */
  const StmtSRef& scope_sref_;
  /*! \brief The parent scope of the insertion. */
  const StmtSRef& writer_block_sref_;
  /*! \brief The info for inserting cache stage. */
  CacheStageInfo* info_;
  /*! \brief Whether the current node is under the given block. */
  bool under_writer_block_{false};
};

/*!
 * \brief Create a new buffer by change the shape with block iters to be used as the reindex buffer
 * \param buffer The given buffer.
 * \param block_iters The block iters.
 * \param covered Set of block iter vars covered by the buffer access indices
 * \return The new buffer with target shape.
 */
Buffer CreateReindexBuffer(const Buffer& buffer, const Array<IterVar>& block_iters,
                           const std::unordered_set<Var, ObjectPtrHash, ObjectPtrEqual>& covered) {
  ObjectPtr<BufferNode> new_buffer = make_object<BufferNode>(*buffer.get());
  ObjectPtr<VarNode> new_var = make_object<VarNode>(*buffer->data.get());
  std::vector<PrimExpr> new_shape;
  std::vector<PrimExpr> new_strides;
  for (const auto& iter : block_iters) {
    if (covered.count(iter->var)) {
      new_shape.push_back(iter->dom->min + iter->dom->extent);
    }
  }
  new_strides.clear();
  new_buffer->shape = new_shape;
  new_buffer->strides = new_strides;
  new_buffer->data = buffer->data.copy_with_suffix("_reindex");
  new_buffer->name = buffer->name + "_reindex";
  return Buffer(new_buffer);
}

/*! \brief The schedule error that the target is not a leaf block. */
class NotLeafBlockError : public ScheduleError {
 public:
  NotLeafBlockError(IRModule mod, Block block) : mod_(std::move(mod)), block_(std::move(block)) {}
  String FastErrorString() const final {
    return "ScheduleError: The target block is not a leaf block.";
  }

  String DetailRenderTemplate() const final { return "The target block {0} is not a leaf block."; }

  IRModule mod() const final { return mod_; }
  Array<ObjectRef> LocationsOfInterest() const final { return {block_}; }
  IRModule mod_;
  Block block_;
};

/*! \brief The schedule error that the buffer access is invalid for reindex. */
class InvalidBufferAccessError : public ScheduleError {
 public:
  enum class ErrorKind {
    kNoAccess,         // buffer access not found
    kNonUniqueAccess,  // multiple buffer accesses with different indices
    kOpaqueAccess,     // opaque access to the buffer
  };

  InvalidBufferAccessError(IRModule mod, Buffer buffer, Block block, ErrorKind kind)
      : mod_(std::move(mod)), buffer_(std::move(buffer)), block_(std::move(block)), kind_(kind) {}
  String FastErrorString() const final {
    return "ScheduleError: The target buffer should be accessed via BufferLoad or BufferStore. The "
           "indices should be the same if there are multiple accesses to the target buffer.";
  }

  String DetailRenderTemplate() const final {
    std::ostringstream os;
    os << "The target buffer " << buffer_->name
       << " should be accessed in the leaf block {0} via BufferLoad or BufferStore. The indices "
          "should be the same if there are multiple accesses to the target buffer. ";
    if (kind_ == ErrorKind::kNoAccess) {
      os << "No buffer accesses found.";
    } else if (kind_ == ErrorKind::kNonUniqueAccess) {
      os << "Multiple buffer accesses have non-unique indices.";
    } else if (kind_ == ErrorKind::kOpaqueAccess) {
      os << "Opaque buffer accesses found.";
    }
    return os.str();
  }
  IRModule mod() const final { return mod_; }
  Array<ObjectRef> LocationsOfInterest() const final { return {block_}; }

 private:
  IRModule mod_;
  Buffer buffer_;
  Block block_;
  ErrorKind kind_;
};

/*! \brief Collect the related Load/Store to reindex */
class ReIndexCollector : public StmtExprVisitor {
 public:
  static Array<PrimExpr> Collect(const IRModule& mod, const Buffer& buffer, const Block& block) {
    ReIndexCollector collector(mod, buffer, block);
    collector(block->body);
    if (!collector.buffer_access_indices_.defined()) {
      throw InvalidBufferAccessError(mod, buffer, block,
                                     InvalidBufferAccessError::ErrorKind::kNoAccess);
    }
    return collector.buffer_access_indices_.value();
  }

 private:
  explicit ReIndexCollector(const IRModule& mod, const Buffer& buffer, const Block& block)
      : mod_(mod), buffer_(buffer), block_(block) {}

  void VisitExpr_(const BufferLoadNode* load) final {
    StmtExprVisitor::VisitExpr_(load);
    if (load->buffer.same_as(buffer_)) {
      CheckAndUpdateBufferAccessIndices(load->indices);
    }
  }

  void VisitStmt_(const BlockNode* block) final {
    // no sub-blocks under this block
    throw NotLeafBlockError(mod_, block_);
  }

  void VisitStmt_(const BufferStoreNode* store) final {
    StmtExprVisitor::VisitStmt_(store);
    if (store->buffer.same_as(buffer_)) {
      CheckAndUpdateBufferAccessIndices(store->indices);
    }
  }

  void CheckAndUpdateBufferAccessIndices(const Array<PrimExpr> indices) {
    if (!buffer_access_indices_.defined()) {
      buffer_access_indices_ = indices;
      return;
    } else if (!std::equal(buffer_access_indices_.value().begin(),
                           buffer_access_indices_.value().end(), indices.begin(), indices.end(),
                           ExprDeepEqual())) {
      throw InvalidBufferAccessError(mod_, buffer_, block_,
                                     InvalidBufferAccessError::ErrorKind::kNonUniqueAccess);
    }
  }

  void VisitExpr_(const VarNode* var) final {
    if (var == buffer_->data.get()) {
      throw InvalidBufferAccessError(mod_, buffer_, block_,
                                     InvalidBufferAccessError::ErrorKind::kOpaqueAccess);
    }
  }
  /*! \brief The IR module */
  IRModule mod_;
  /*! \brief The buffer to rewrite */
  Buffer buffer_;
  /*! \brief The block to visit */
  Block block_;
  /*! \brief The indices of buffer acess to rewrite */
  Optional<Array<PrimExpr>> buffer_access_indices_;
};

/*! \brief Mutator of ReIndex */
class ReIndexRewriter : public StmtExprMutator {
 public:
  static Stmt Rewrite(const StmtSRef& scope_sref, const StmtSRef& block_sref, CacheStageInfo* info,
                      const std::unordered_set<Var, ObjectPtrHash, ObjectPtrEqual>& covered) {
    ReIndexRewriter rewriter(block_sref, info, covered);
    return rewriter(GetRef<Stmt>(scope_sref->stmt));
  }

 private:
  explicit ReIndexRewriter(const StmtSRef& block_sref, CacheStageInfo* info,
                           const std::unordered_set<Var, ObjectPtrHash, ObjectPtrEqual>& covered)
      : block_sref_(block_sref), info_(info), covered_(covered) {
    new_buffer_ = info->alloc;
    old_buffer_ = info->read_buffer.same_as(new_buffer_) ? info->write_buffer : info->read_buffer;
  }

  Stmt VisitStmt_(const BlockNode* block) final {
    Block old_stmt = GetRef<Block>(block);
    if (is_scope_) {
      is_scope_ = false;
      Block stmt = Downcast<Block>(StmtExprMutator::VisitStmt_(block));
      // Insert cache stage into the loop
      ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
      n->body = InsertCacheStage(n->body, info_->loc_pos, info_->cache_stage);
      n->alloc_buffers.push_back(info_->alloc);
      stmt = Block(n);
      info_->block_reuse.Set(old_stmt, stmt);
      return std::move(stmt);
    }

    // Visiting the blokc being reindexed
    if (block == block_sref_->stmt) {
      // Collect the updated indices and regions
      for (const IterVar& iter : block->iter_vars) {
        if (covered_.count(iter->var)) {
          indices_.push_back(iter->var);
          region_.push_back(Range::FromMinExtent(iter->var, IntImm(iter->var->dtype, 1)));
        }
      }
      Block stmt = Downcast<Block>(StmtExprMutator::VisitStmt_(block));
      // Update block reads/writes to use the intermediate reindex buffer
      auto writes =
          ReplaceBufferRegion(block->writes, old_buffer_, BufferRegion{new_buffer_, region_});
      auto reads =
          ReplaceBufferRegion(block->reads, old_buffer_, BufferRegion{new_buffer_, region_});
      auto match_buffers = ReplaceBufferRegion(block->match_buffers, old_buffer_,
                                               BufferRegion{new_buffer_, region_});
      if (!writes.same_as(block->writes) || !reads.same_as(block->reads) ||
          !match_buffers.same_as(block->match_buffers)) {
        ObjectPtr<BlockNode> n = make_object<BlockNode>(*stmt.as<BlockNode>());
        n->writes = std::move(writes);
        n->reads = std::move(reads);
        n->match_buffers = std::move(match_buffers);
        stmt = Block(n);
      }
      info_->block_reuse.Set(old_stmt, stmt);
      return std::move(stmt);
    }
    return std::move(old_stmt);
  }

  template <typename Node>
  Node VisitBufferAccess(Node node) {
    if (node->buffer.same_as(old_buffer_)) {
      auto* n = node.CopyOnWrite();
      n->buffer = new_buffer_;
      n->indices = indices_;
    }
    return node;
  }
  Stmt VisitStmt_(const BufferStoreNode* op) final {
    BufferStore buffer_store = Downcast<BufferStore>(StmtExprMutator::VisitStmt_(op));
    return VisitBufferAccess(std::move(buffer_store));
  }

  PrimExpr VisitExpr_(const BufferLoadNode* op) final {
    BufferLoad buffer_load = Downcast<BufferLoad>(StmtExprMutator::VisitExpr_(op));
    return VisitBufferAccess(std::move(buffer_load));
  }

 private:
  /*! \brief The parent scope of the insertion. */
  const StmtSRef& block_sref_;
  /*! \brief The info for inserting reindex stage. */
  CacheStageInfo* info_;
  /*! \brief Whether old block var is covered in the indices */
  const std::unordered_set<Var, ObjectPtrHash, ObjectPtrEqual>& covered_;
  /*! \brief Whether the current block is scope block */
  bool is_scope_{true};
  /*! \brief The  buffer to be replaced */
  Buffer old_buffer_;
  /*! \brief The reindex buffer */
  Buffer new_buffer_;
  /*! \brief The new indices */
  Array<PrimExpr> indices_;
  /*! \brief The new region */
  Region region_;
};

void CheckRegionCover(const ScheduleState& self, StmtSRef scope_root) {
  class NotRegionCoverError : public ScheduleError {
   public:
    explicit NotRegionCoverError(IRModule mod, Block block) : mod_(mod), block_(block) {}
    IRModule mod() const final { return mod_; }
    String FastErrorString() const final {
      return "ScheduleError: The scope root's region cover is not complete.";
    }
    String DetailRenderTemplate() const final {
      return R"(The scope {0} 's region cover is not complete.
The region cover property require to hold for every of its child blocks
)";
    }
    Array<ObjectRef> LocationsOfInterest() const final { return {block_}; }
    IRModule mod_;
    Block block_;
  };
  BlockScope scope = self->GetBlockScope(scope_root);
  for (const auto& kv : scope->dst2deps) {
    const StmtSRef& consumer_block_sref = kv.first;
    if (!self->block_info.at(consumer_block_sref).region_cover) {
      const BlockNode* block = TVM_SREF_TO_BLOCK(scope_root);
      throw NotRegionCoverError(self->mod, GetRef<Block>(block));
    }
  }
}

/******** Implementation ********/

StmtSRef CacheRead(ScheduleState self, const StmtSRef& block_sref, int read_buffer_index,
                   const String& storage_scope, const Array<StmtSRef> consumer_blocks) {
  /*!
   * Check:
   *   - The index is in the array of block reading region
   *   - There is at most one block who write the buffer in the scope
   *
   * Mutate:
   *   - Allocate new cache buffer under the current scope.
   *   - Find the lowest ancestor of the block and ANY ONE of the consumers blocks.
   *   - Copy the buffer with the consumed region.
   */

  // Step 0. Check the input storage scope.
  CheckStorageScope(self, storage_scope);

  // Step 1. Check index, getting the target buffer and the parent scope
  const BlockNode* block = TVM_SREF_TO_BLOCK(block_sref);
  Buffer read_buffer =
      GetNthAccessBuffer(self, GetRef<Block>(block), read_buffer_index, BufferIndexType::kRead);
  StmtSRef scope_sref = GetScopeRoot(self, block_sref, /*require_stage_pipeline=*/false);
  // Check required region cover for cache_read
  CheckRegionCover(self, scope_sref);
  const BlockNode* scope_block = TVM_SREF_TO_BLOCK(scope_sref);

  // Step 2. Create CacheStageInfo
  CacheStageInfo info;
  info.read_buffer = read_buffer;
  // Create the corresponding buffer to be written, i.e. result of cache_read
  info.write_buffer = WithScope(read_buffer, storage_scope);
  // Create the corresponding buffer allocation
  info.alloc = info.write_buffer;
  // Indicate which buffers should consume the cache.
  info.consumer_blocks = consumer_blocks;

  // Step 3. Update cache stage info.
  BufferRegion cache_region{nullptr};
  if (Optional<StmtSRef> _write_block_sref = GetOnlyWriteBlock(self, scope_sref, read_buffer)) {
    // Case 1. The buffer is written inside the block.
    StmtSRef write_block_sref = _write_block_sref.value();
    const BlockNode* write_block = TVM_SREF_TO_BLOCK(write_block_sref);
    // Find the producing region
    BufferRegion region = GetBufferRegionFromBuffer(write_block->writes, read_buffer).value();
    StmtSRef parent_sref = GetRef<StmtSRef>(write_block_sref->parent);

    // Detect insert position
    CacheLocDetector::Detect(self, write_block_sref, scope_sref, &info);
    cache_region = RelaxBufferRegion(self, region, write_block_sref, parent_sref, info.loc_sref);
  } else {
    // Case 2. The buffer is the input block for the scope.
    info.loc_sref = scope_sref;
    info.loc_pos = 0;
    if (Optional<BufferRegion> region =
            GetBufferRegionFromBuffer(scope_block->reads, read_buffer)) {
      cache_region = region.value();
    } else {
      cache_region = BufferRegion::FullRegion(read_buffer);
    }
  }

  // Step 4. Making new cache stage block and rewrite readers.
  Block cache_read_stage = MakeCacheStage(/*cache_region=*/cache_region, /*info=*/&info,
                                          /*storage_scope=*/storage_scope);
  Stmt new_scope = CacheReadRewriter::Rewrite(/*scope_sref=*/scope_sref, /*info=*/&info);

  // Step 5. Replacing and updating flags.
  self->Replace(scope_sref, new_scope, info.block_reuse);
  StmtSRef result_block_sref = self->stmt2ref.at(cache_read_stage.get());
  BlockInfo& block_info = self->block_info[result_block_sref];
  block_info.affine_binding = CalculateAffineFlag(self, result_block_sref);
  block_info.region_cover = true;
  block_info.scope->stage_pipeline = true;
  return result_block_sref;
}

StmtSRef CacheWrite(ScheduleState self, const StmtSRef& block_sref, int write_buffer_index,
                    const String& storage_scope) {
  /*!
   * Check:
   *   - The index is in the array of block reading region
   *   - There is only one block who write the buffer in the scope
   *
   * Mutate:
   *   - Allocate new cache buffer under the current scope.
   *   - Find the lowest ancestor of the block and ANY ONE of the producer blocks.
   *   - Copy the buffer with the consumed region.
   */

  // Step 0. Check the input storage scope.
  CheckStorageScope(self, storage_scope);

  // Step 1. Checking index, getting the target buffer and the parent scope
  const BlockNode* block = TVM_SREF_TO_BLOCK(block_sref);
  Buffer write_buffer =
      GetNthAccessBuffer(self, GetRef<Block>(block), write_buffer_index, BufferIndexType::kWrite);
  StmtSRef scope_sref = GetScopeRoot(self, block_sref, /*require_stage_pipeline=*/false);

  // Step 2. Creating CacheStageInfo
  CacheStageInfo info;
  info.read_buffer = WithScope(write_buffer, storage_scope);
  // Create the corresponding buffer to be written, i.e. result of cache_write
  info.write_buffer = write_buffer;
  // Create the corresponding buffer allocation
  info.alloc = info.read_buffer;

  // Step 3. Check the only writer block.
  ICHECK_EQ(block_sref.get(), GetOnlyWriteBlock(self, scope_sref, write_buffer).get());

  // Step 4. Find the producing region and insert position
  BufferRegion region = GetBufferRegionFromBuffer(block->writes, write_buffer).value();
  StmtSRef parent_sref = GetRef<StmtSRef>(block_sref->parent);
  // Detect insert position
  CacheLocDetector::Detect(self, block_sref, scope_sref, &info);
  BufferRegion cache_region =
      RelaxBufferRegion(self, region, block_sref, parent_sref, info.loc_sref);

  // Step 5. Making new cache stage block and rewrite readers.
  Block cache_write_stage = MakeCacheStage(/*cache_region=*/cache_region, /*info=*/&info,
                                           /*storage_scope=*/storage_scope);
  Stmt new_scope = CacheWriteRewriter::Rewrite(/*scope_sref=*/scope_sref,
                                               /*writer_block_sref=*/block_sref, /*info=*/&info);

  // Step 6. Replacing and updating flags.
  self->Replace(scope_sref, new_scope, info.block_reuse);
  StmtSRef result_block_sref = self->stmt2ref.at(cache_write_stage.get());
  BlockInfo& block_info = self->block_info[result_block_sref];
  block_info.affine_binding = CalculateAffineFlag(self, result_block_sref);
  block_info.region_cover = true;
  block_info.scope->stage_pipeline = true;
  return result_block_sref;
}

StmtSRef ReIndex(ScheduleState self, const StmtSRef& block_sref, int buffer_index,
                 BufferIndexType buffer_index_type) {
  const BlockNode* block_ptr = TVM_SREF_TO_BLOCK(block_sref);
  Block block = GetRef<Block>(block_ptr);
  Buffer buffer = GetNthAccessBuffer(self, block, buffer_index, buffer_index_type);
  StmtSRef scope_sref = GetScopeRoot(self, block_sref, /*require_stage_pipeline=*/true);
  arith::Analyzer analyzer;

  // Step 1. Collect the original indices and check there's only single pattern of related
  // Load/Store and the buffer is not accessed opaquely
  Array<PrimExpr> original_indices = ReIndexCollector::Collect(self->mod, buffer, block);
  // Simplify the indices if possible
  for (const IterVar& iter : block->iter_vars) {
    analyzer.Bind(iter->var, iter->dom);
  }
  original_indices.MutateByApply(
      [&analyzer](const PrimExpr& expr) { return SimplifyNonTrivialExpr(expr, &analyzer); });

  // Collect block iters appearing in the original_indices
  std::unordered_set<Var, ObjectPtrHash, ObjectPtrEqual> covered;
  for (const PrimExpr& index : original_indices) {
    PreOrderVisit(index, [&](const ObjectRef& obj) -> bool {
      if (const VarNode* var = obj.as<VarNode>()) {
        covered.insert(GetRef<Var>(var));
      }
      return true;
    });
  }

  // Step 2. Creating CacheStageInfo
  CacheStageInfo info;
  // Create the corresponding buffer to be read(write), i.e. the result of reindex read(write)
  if (buffer_index_type == BufferIndexType::kWrite) {
    info.read_buffer = CreateReindexBuffer(buffer, block->iter_vars, covered);
    info.write_buffer = buffer;
    info.alloc = info.read_buffer;
  } else {
    info.read_buffer = buffer;
    info.write_buffer = CreateReindexBuffer(buffer, block->iter_vars, covered);
    info.alloc = info.write_buffer;
  }

  // Step 3. Check the block belongs to a chain loop nesting under the scope,
  //         and get the insert location
  const StmtSRefNode* loop;
  for (loop = block_sref->parent; loop->parent != scope_sref.get();) {
    const ForNode* outer = loop->parent->StmtAs<ForNode>();
    const ForNode* inner = loop->StmtAs<ForNode>();
    ICHECK(outer != nullptr && inner != nullptr);
    ICHECK(outer->body.get() == inner);
    loop = loop->parent;
  }

  info.loc_pos = loop->seq_index == -1 ? 0 : loop->seq_index;
  if (buffer_index_type == BufferIndexType::kWrite) {
    info.loc_pos++;
  }

  // Step 4. Making new reindex stage block and rewrite
  Block reindex_stage =
      MakeReIndexStage(block, &info, covered, original_indices, buffer_index, buffer_index_type);
  Stmt new_scope = ReIndexRewriter::Rewrite(scope_sref, block_sref, &info, covered);

  // Step 5. Replacing and updating flags
  self->Replace(scope_sref, new_scope, info.block_reuse);
  StmtSRef result_block_sref = self->stmt2ref.at(reindex_stage.get());
  BlockInfo& block_info = self->block_info[result_block_sref];
  block_info.affine_binding = CalculateAffineFlag(self, result_block_sref);
  block_info.region_cover = true;
  block_info.scope->stage_pipeline = true;
  return result_block_sref;
}

/******** Instruction Registration ********/

struct CacheReadTraits : public UnpackedInstTraits<CacheReadTraits> {
  static constexpr const char* kName = "CacheRead";
  static constexpr bool kIsPure = false;

 private:
  static constexpr size_t kNumInputs = 2;
  static constexpr size_t kNumAttrs = 2;
  static constexpr size_t kNumDecisions = 0;

  static BlockRV UnpackedApplyToSchedule(Schedule sch, BlockRV block,
                                         Array<BlockRV> consumer_blocks, Integer read_buffer_index,
                                         String storage_scope) {
    return sch->CacheRead(block, read_buffer_index->value, storage_scope, consumer_blocks);
  }

  static String UnpackedAsPython(Array<String> outputs, String block, Array<String> consumer_blocks,
                                 Integer read_buffer_index, String storage_scope) {
    PythonAPICall py("cache_read");
    py.Input("block", block);
    py.Input("read_buffer_index", read_buffer_index->value);
    py.Input("storage_scope", storage_scope);
    // Only write out consumer blocks if provided.
    if (!consumer_blocks.empty()) {
      py.Input("consumer_blocks", consumer_blocks);
    }
    py.SingleOutput(outputs);
    return py.Str();
  }

  template <typename>
  friend struct ::tvm::tir::UnpackedInstTraits;
};

struct CacheWriteTraits : public UnpackedInstTraits<CacheWriteTraits> {
  static constexpr const char* kName = "CacheWrite";
  static constexpr bool kIsPure = false;

 private:
  static constexpr size_t kNumInputs = 1;
  static constexpr size_t kNumAttrs = 2;
  static constexpr size_t kNumDecisions = 0;

  static BlockRV UnpackedApplyToSchedule(Schedule sch, BlockRV block, Integer write_buffer_index,
                                         String storage_scope) {
    return sch->CacheWrite(block, write_buffer_index->value, storage_scope);
  }

  static String UnpackedAsPython(Array<String> outputs, String block, Integer write_buffer_index,
                                 String storage_scope) {
    PythonAPICall py("cache_write");
    py.Input("block", block);
    py.Input("write_buffer_index", write_buffer_index->value);
    py.Input("storage_scope", storage_scope);
    py.SingleOutput(outputs);
    return py.Str();
  }

  template <typename>
  friend struct ::tvm::tir::UnpackedInstTraits;
};

struct ReIndexTraits : public UnpackedInstTraits<ReIndexTraits> {
  static constexpr const char* kName = "ReIndex";
  static constexpr bool kIsPure = false;

 private:
  static constexpr size_t kNumInputs = 1;
  static constexpr size_t kNumAttrs = 2;
  static constexpr size_t kNumDecisions = 0;

  static BlockRV UnpackedApplyToSchedule(Schedule sch, BlockRV block, Integer buffer_index,
                                         Integer buffer_index_type) {
    return sch->ReIndex(block, buffer_index.IntValue(),
                        static_cast<BufferIndexType>(buffer_index_type->value));
  }

  static String UnpackedAsPython(Array<String> outputs, String block, Integer buffer_index,
                                 Integer buffer_index_type) {
    PythonAPICall py("reindex");
    py.Input("block", block);
    std::ostringstream os;
    os << "(\"" << BufferIndexType2Str(static_cast<BufferIndexType>(buffer_index_type->value))
       << "\", " << buffer_index << ")";
    py.Input("buffer", os.str());
    py.SingleOutput(outputs);
    return py.Str();
  }

  template <typename>
  friend struct ::tvm::tir::UnpackedInstTraits;
};

TVM_REGISTER_INST_KIND_TRAITS(CacheReadTraits);
TVM_REGISTER_INST_KIND_TRAITS(CacheWriteTraits);
TVM_REGISTER_INST_KIND_TRAITS(ReIndexTraits);
}  // namespace tir
}  // namespace tvm
