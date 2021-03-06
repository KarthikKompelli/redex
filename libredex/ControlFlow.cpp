/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ControlFlow.h"

#include <boost/numeric/conversion/cast.hpp>
#include <iterator>
#include <stack>
#include <utility>

#include "DexUtil.h"
#include "Transform.h"

namespace {

// return true if `it` should be the last instruction of this block
bool end_of_block(const IRList* ir, IRList::iterator it, bool in_try) {
  auto next = std::next(it);
  if (next == ir->end()) {
    return true;
  }

  // End the block before the first target in a contiguous sequence of targets.
  if (next->type == MFLOW_TARGET && it->type != MFLOW_TARGET) {
    return true;
  }

  // End the block before the first catch marker in a contiguous sequence of
  // catch markers.
  if (next->type == MFLOW_CATCH && it->type != MFLOW_CATCH) {
    return true;
  }

  // End the block before a TRY_START
  // and after a TRY_END
  if ((next->type == MFLOW_TRY && next->tentry->type == TRY_START) ||
      (it->type == MFLOW_TRY && it->tentry->type == TRY_END)) {
    return true;
  }

  if (in_try && it->type == MFLOW_OPCODE &&
      opcode::may_throw(it->insn->opcode())) {
    return true;
  }
  if (it->type != MFLOW_OPCODE) {
    return false;
  }
  if (is_branch(it->insn->opcode()) || is_return(it->insn->opcode()) ||
      it->insn->opcode() == OPCODE_THROW) {
    return true;
  }

  return false;
}

bool ends_with_may_throw(cfg::Block* p) {
  for (auto last = p->rbegin(); last != p->rend(); ++last) {
    if (last->type != MFLOW_OPCODE) {
      continue;
    }
    return last->insn->opcode() == OPCODE_THROW ||
           opcode::may_throw(last->insn->opcode());
  }
  return false;
}

bool cannot_throw(cfg::Block* b) {
  for (const auto& mie : InstructionIterable(b)) {
    auto op = mie.insn->opcode();
    if (op == OPCODE_THROW || opcode::may_throw(op)) {
      return false;
    }
  }
  return true;
}

} // namespace

namespace cfg {

IRList::iterator Block::begin() {
  if (m_parent->editable()) {
    return m_entries.begin();
  } else {
    return m_begin;
  }
}

IRList::iterator Block::end() {
  if (m_parent->editable()) {
    return m_entries.end();
  } else {
    return m_end;
  }
}

IRList::const_iterator Block::begin() const {
  if (m_parent->editable()) {
    return m_entries.begin();
  } else {
    return m_begin;
  }
}

IRList::const_iterator Block::end() const {
  if (m_parent->editable()) {
    return m_entries.end();
  } else {
    return m_end;
  }
}

bool Block::is_catch() const {
  return m_parent->get_pred_edge_of_type(this, EDGE_THROW) != nullptr;
}

bool Block::same_try(Block* other) const {
  always_assert(other->m_parent == this->m_parent);
  return m_parent->blocks_are_in_same_try(this, other);
}

void Block::remove_opcode(const ir_list::InstructionIterator& it) {
  always_assert(m_parent->editable());
  m_parent->remove_opcode(cfg::InstructionIterator(*m_parent, this, it));
}

void Block::remove_opcode(const IRList::iterator& it) {
  always_assert(m_parent->editable());
  remove_opcode(ir_list::InstructionIterator(it, end()));
}

opcode::Branchingness Block::branchingness() {
  always_assert(m_parent->editable());
  const auto& last = get_last_insn();

  if (succs().empty() ||
      (succs().size() == 1 &&
       m_parent->get_succ_edge_of_type(this, EDGE_GHOST) != nullptr)) {
    if (last != end()) {
      auto op = last->insn->opcode();
      if (is_return(op)) {
        return opcode::BRANCH_RETURN;
      } else if (op == OPCODE_THROW) {
        return opcode::BRANCH_THROW;
      }
    }
    return opcode::BRANCH_NONE;
  }

  if (m_parent->get_succ_edge_of_type(this, EDGE_THROW) != nullptr) {
    return opcode::BRANCH_THROW;
  }

  if (m_parent->get_succ_edge_of_type(this, EDGE_BRANCH) != nullptr) {
    always_assert(last != end());
    auto br = opcode::branchingness(last->insn->opcode());
    always_assert(br == opcode::BRANCH_IF || br == opcode::BRANCH_SWITCH);
    return br;
  }

  if (m_parent->get_succ_edge_of_type(this, EDGE_GOTO) != nullptr) {
    return opcode::BRANCH_GOTO;
  }
  return opcode::BRANCH_NONE;
}

uint32_t Block::num_opcodes() const {
  if (m_parent->editable()) {
    return m_entries.count_opcodes();
  } else {
    uint32_t result = 0;
    for (auto it = m_begin; it != m_end; ++it) {
      if (it->type == MFLOW_OPCODE &&
          !opcode::is_internal(it->insn->opcode())) {
        ++result;
      }
    }
    return result;
  }
}

// shallowly copy pointers (edges and parent cfg)
// but deeply copy MethodItemEntries
Block::Block(const Block& b)
    : m_id(b.m_id),
      m_preds(b.m_preds),
      m_succs(b.m_succs),
      m_parent(b.m_parent) {

  // only for editable, don't worry about m_begin and m_end
  always_assert(m_parent->editable());

  MethodItemEntryCloner cloner;
  for (const auto& mie : b.m_entries) {
    m_entries.push_back(*cloner.clone(&mie));
  }
}

bool Block::has_pred(Block* b, EdgeType t) const {
  const auto& edges = preds();
  return std::find_if(edges.begin(), edges.end(), [b, t](const Edge* edge) {
           return edge->src() == b &&
                  (t == EDGE_TYPE_SIZE || edge->type() == t);
         }) != edges.end();
}

bool Block::has_succ(Block* b, EdgeType t) const {
  const auto& edges = succs();
  return std::find_if(edges.begin(), edges.end(), [b, t](const Edge* edge) {
           return edge->target() == b &&
                  (t == EDGE_TYPE_SIZE || edge->type() == t);
         }) != edges.end();
}

IRList::iterator Block::get_conditional_branch() {
  for (auto it = rbegin(); it != rend(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      auto op = it->insn->opcode();
      if (is_conditional_branch(op) || is_switch(op)) {
        return std::prev(it.base());
      }
    }
  }
  return end();
}

IRList::iterator Block::get_last_insn() {
  for (auto it = rbegin(); it != rend(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      // Reverse iterators have a member base() which returns a corresponding
      // forward iterator. Beware that this isn't an iterator that refers to the
      // same object - it actually refers to the next object in the sequence.
      // This is so that rbegin() corresponds with end() and rend() corresponds
      // with begin(). Copied from https://stackoverflow.com/a/2037917
      return std::prev(it.base());
    }
  }
  return end();
}

IRList::iterator Block::get_first_insn() {
  for (auto it = begin(); it != end(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      return it;
    }
  }
  return end();
}

bool Block::starts_with_move_result() {
  auto first_it = get_first_insn();
  if (first_it != end()) {
    auto first_op = first_it->insn->opcode();
    if (is_move_result(first_op) || opcode::is_move_result_pseudo(first_op)) {
      return true;
    }
  }
  return false;
}

// We remove the first matching target because multiple switch cases can point
// to the same block. We use this function to move information from the target
// entries to the CFG edges. The two edges are identical, save the case key, so
// it doesn't matter which target is taken. We arbitrarily choose to process the
// targets in forward order.
boost::optional<Edge::CaseKey> Block::remove_first_matching_target(
    MethodItemEntry* branch) {
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    auto& mie = *it;
    if (mie.type == MFLOW_TARGET && mie.target->src == branch) {
      boost::optional<Edge::CaseKey> result;
      if (mie.target->type == BRANCH_MULTI) {
        always_assert_log(is_switch(branch->insn->opcode()), "block %d in %s\n",
                          id(), SHOW(*m_parent));
        result = mie.target->case_key;
      }
      m_entries.erase_and_dispose(it);
      return result;
    }
  }
  always_assert_log(false,
                    "block %d has no targets matching %s:\n%s",
                    id(),
                    SHOW(branch->insn),
                    SHOW(&m_entries));
  not_reached();
}

std::ostream& operator<<(std::ostream& os, const Edge& e) {
  switch (e.type()) {
  case EDGE_GOTO: {
    os << "goto";
    break;
  }
  case EDGE_BRANCH: {
    os << "branch";
    auto key = e.case_key();
    if (key) {
      os << " " << *key;
    }
    break;
  }
  case EDGE_THROW: {
    os << "throw";
    break;
  }
  default: { break; }
  }
  return os;
}

ControlFlowGraph::ControlFlowGraph(IRList* ir,
                                   uint16_t registers_size,
                                   bool editable)
    : m_registers_size(registers_size), m_editable(editable) {
  always_assert_log(ir->size() > 0, "IRList contains no instructions");

  BranchToTargets branch_to_targets;
  TryEnds try_ends;
  TryCatches try_catches;
  Boundaries boundaries; // block boundaries (for editable == true)

  find_block_boundaries(
      ir, branch_to_targets, try_ends, try_catches, boundaries);

  if (m_editable) {
    fill_blocks(ir, boundaries);
  }

  connect_blocks(branch_to_targets);
  add_catch_edges(try_ends, try_catches);

  if (m_editable) {
    remove_try_catch_markers();
    TRACE(CFG, 5, "before simplify:\n%s", SHOW(*this));
    simplify();
    TRACE(CFG, 5, "after simplify:\n%s", SHOW(*this));
  } else {
    remove_unreachable_succ_edges();
  }

  sanity_check();
  TRACE(CFG, 5, "editable %d, %s", m_editable, SHOW(*this));
}

void ControlFlowGraph::find_block_boundaries(IRList* ir,
                                             BranchToTargets& branch_to_targets,
                                             TryEnds& try_ends,
                                             TryCatches& try_catches,
                                             Boundaries& boundaries) {
  // Find the block boundaries
  auto* block = create_block();
  if (m_editable) {
    boundaries[block].first = ir->begin();
  } else {
    block->m_begin = ir->begin();
  }

  set_entry_block(block);
  bool in_try = false;
  for (auto it = ir->begin(); it != ir->end(); ++it) {
    if (it->type == MFLOW_TRY) {
      if (it->tentry->type == TRY_START) {
        // Assumption: TRY_STARTs are only at the beginning of blocks
        always_assert(!m_editable || it == boundaries[block].first);
        always_assert(m_editable || it == block->m_begin);
        in_try = true;
      } else if (it->tentry->type == TRY_END) {
        try_ends.emplace_back(it->tentry, block);
        in_try = false;
      }
    } else if (it->type == MFLOW_CATCH) {
      try_catches[it->centry] = block;
    } else if (it->type == MFLOW_TARGET) {
      branch_to_targets[it->target->src].push_back(block);
    }

    if (!end_of_block(ir, it, in_try)) {
      continue;
    }

    // End the current block.
    auto next = std::next(it);
    if (m_editable) {
      boundaries[block].second = next;
    } else {
      block->m_end = next;
    }

    if (next == ir->end()) {
      break;
    }

    // Start a new block at the next MethodItem.
    block = create_block();
    if (m_editable) {
      boundaries[block].first = next;
    } else {
      block->m_begin = next;
    }
  }
  TRACE(CFG, 5, "  build: boundaries found\n");
}

// Link the blocks together with edges. If the CFG is editable, also insert
// fallthrough goto instructions and delete MFLOW_TARGETs.
void ControlFlowGraph::connect_blocks(BranchToTargets& branch_to_targets) {
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    // Set outgoing edge if last MIE falls through
    Block* b = it->second;
    auto& last_mie = *b->rbegin();
    bool fallthrough = true;
    if (last_mie.type == MFLOW_OPCODE) {
      auto last_op = last_mie.insn->opcode();
      if (is_branch(last_op)) {
        fallthrough = !is_goto(last_op);
        auto const& target_blocks = branch_to_targets[&last_mie];

        for (auto target_block : target_blocks) {
          if (m_editable) {
            // The the branch information is stored in the edges, we don't need
            // the targets inside the blocks anymore
            auto case_key =
                target_block->remove_first_matching_target(&last_mie);
            if (case_key != boost::none) {
              add_edge(b, target_block, *case_key);
              continue;
            }
          }
          auto edge_type = is_goto(last_op) ? EDGE_GOTO : EDGE_BRANCH;
          add_edge(b, target_block, edge_type);
        }

        if (m_editable && is_goto(last_op)) {
          // We don't need the gotos in editable mode because the edges
          // fully encode that information
          b->m_entries.erase_and_dispose(
              b->m_entries.iterator_to(last_mie));
        }

      } else if (is_return(last_op) || last_op == OPCODE_THROW) {
        fallthrough = false;
      }
    }

    auto next = std::next(it);
    Block* next_b = next->second;
    if (fallthrough && next != m_blocks.end()) {
      TRACE(CFG,
            6,
            "adding fallthrough goto %d -> %d\n",
            b->id(),
            next_b->id());
      add_edge(b, next_b, EDGE_GOTO);
    }
  }
  TRACE(CFG, 5, "  build: edges added\n");
}

void ControlFlowGraph::add_catch_edges(TryEnds& try_ends,
                                       TryCatches& try_catches) {
  /*
   * Every block inside a try-start/try-end region
   * gets an edge to every catch block.  This simplifies dataflow analysis
   * since you can always get the exception state by looking at successors,
   * without any additional analysis.
   *
   * NB: This algorithm assumes that a try-start/try-end region will consist of
   * sequentially-numbered blocks, which is guaranteed because catch regions
   * are contiguous in the bytecode, and we generate blocks in bytecode order.
   */
  for (auto tep : try_ends) {
    auto try_end = tep.first;
    auto tryendblock = tep.second;
    size_t bid = tryendblock->id();
    while (true) {
      Block* block = m_blocks.at(bid);
      if (ends_with_may_throw(block)) {
        uint32_t i = 0;
        for (auto mie = try_end->catch_start; mie != nullptr;
             mie = mie->centry->next) {
          auto catchblock = try_catches.at(mie->centry);
          // Create a throw edge with the information from this catch entry
          add_edge(block, catchblock, mie->centry->catch_type, i);
          ++i;
        }
      }
      auto block_begin = block->begin();
      if (block_begin != block->end() && block_begin->type == MFLOW_TRY) {
        auto tentry = block_begin->tentry;
        if (tentry->type == TRY_START) {
          always_assert_log(tentry->catch_start == try_end->catch_start, "%s",
                            SHOW(*this));
          break;
        }
      }
      always_assert_log(bid > 0, "No beginning of try region found");
      --bid;
    }
  }
  TRACE(CFG, 5, "  build: catch edges added\n");
}

void ControlFlowGraph::remove_unreachable_succ_edges() {
  // Remove edges between unreachable blocks and their succ blocks.
  std::unordered_set<Block*> visited;
  transform::visit(m_entry_block, visited);
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    Block* b = it->second;
    if (visited.find(b) != visited.end()) {
      continue;
    }

    TRACE(CFG, 5, "  build: removing succ edges from block %d\n", b->id());
    remove_succ_edges(b);
  }
  TRACE(CFG, 5, "  build: unreachables removed\n");
}

// Move the `MethodItemEntry`s from `ir` into the blocks, based on the
// information in `boundaries`.
//
// The CFG takes ownership of the `MethodItemEntry`s and `ir` is left empty.
void ControlFlowGraph::fill_blocks(IRList* ir, const Boundaries& boundaries) {
  always_assert(m_editable);
  // fill the blocks between their boundaries
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    b->m_entries.splice_selection(b->m_entries.end(),
                                  *ir,
                                  boundaries.at(b).first,
                                  boundaries.at(b).second);
    always_assert_log(!b->empty(), "block %d is empty:\n%s\n", entry.first,
                      SHOW(*this));
  }
  TRACE(CFG, 5, "  build: splicing finished\n");
}

void ControlFlowGraph::simplify() {
  remove_unreachable_blocks();
  remove_empty_blocks();

  recompute_registers_size();
}

// remove blocks with no predecessors
uint32_t ControlFlowGraph::remove_unreachable_blocks() {
  uint32_t num_insns_removed = 0;
  remove_unreachable_succ_edges();
  std::unordered_set<DexPosition*> deleted_positions;
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    Block* b = it->second;
    const auto& preds = b->preds();
    if (preds.empty() && b != entry_block()) {
      for (const auto& mie : *b) {
        if (mie.type == MFLOW_POSITION) {
          deleted_positions.insert(mie.pos.get());
        }
      }
      num_insns_removed += b->num_opcodes();
      delete b;
      it = m_blocks.erase(it);
    } else {
      ++it;
    }
  }

  // We don't want to leave any dangling dex parent pointers behind
  for (const auto& entry : m_blocks) {
    for (const auto& mie : *entry.second) {
      if (mie.type == MFLOW_POSITION && mie.pos->parent != nullptr &&
          deleted_positions.count(mie.pos->parent)) {
        mie.pos->parent = nullptr;
      }
    }
  }
  return num_insns_removed;
}

void ControlFlowGraph::remove_empty_blocks() {
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    Block* b = it->second;
    const auto& succs = b->succs();
    if (!b->empty() || b == exit_block()) {
      ++it;
      continue;
    }

    if (succs.size() > 0) {
      always_assert_log(succs.size() == 1,
        "too many successors for empty block %d:\n%s", it->first, SHOW(*this));
      const auto& succ_edge = succs[0];
      Block* succ = succ_edge->target();

      if (b == succ) { // `b` follows itself: an infinite loop
        ++it;
        continue;
      }
      // b is empty. Reorganize the edges so we can remove it

      // Remove the one goto edge from b to succ
      remove_all_edges(b, succ);

      // Redirect from b's predecessors to b's successor (skipping b). We
      // can't move edges around while we iterate through the edge list
      // though.
      std::vector<Edge*> need_redirect(b->m_preds.begin(), b->m_preds.end());
      for (Edge* pred_edge : need_redirect) {
        set_edge_target(pred_edge, succ);
      }

      if (b == entry_block()) {
        m_entry_block = succ;
      }
    }
    delete b;
    it = m_blocks.erase(it);
  }
}

// Verify that
//  * MFLOW_TARGETs are gone
//  * OPCODE_GOTOs are gone
//  * Correct number of outgoing edges
void ControlFlowGraph::sanity_check() {
  if (m_editable) {
    for (const auto& entry : m_blocks) {
      Block* b = entry.second;
      for (const auto& mie : *b) {
        always_assert_log(mie.type != MFLOW_TARGET,
                          "failed to remove all targets. block %d in\n%s",
                          b->id(), SHOW(*this));
        if (mie.type == MFLOW_OPCODE) {
          always_assert_log(!is_goto(mie.insn->opcode()),
                            "failed to remove all gotos. block %d in\n%s",
                            b->id(), SHOW(*this));
        }
      }

      auto last_it = b->get_last_insn();
      if (last_it != b->end()) {
        auto& last_mie = *last_it;
        if (last_mie.type == MFLOW_OPCODE) {
          size_t num_preds = b->preds().size();
          size_t num_succs = b->succs().size();
          auto op = last_mie.insn->opcode();
          if (is_conditional_branch(op) || is_switch(op)) {
            always_assert_log(num_succs > 1, "block %d, %s", b->id(),
                              SHOW(*this));
          } else if (is_return(op)) {
            // Make sure we don't have any outgoing edges (except EDGE_GHOST)
            auto real_succs = get_succ_edges_if(
                b, [](const Edge* e) { return e->type() != EDGE_GHOST; });
            always_assert_log(real_succs.empty(), "block %d, %s", b->id(),
                              SHOW(*this));
          } else if (is_throw(op)) {
            // A throw could end the method or go to a catch handler.
            // We don't have any useful assertions to make here.
          } else if (num_preds > 0) {
            // Control Flow shouldn't just fall off the end of a block, unless
            // it's an orphan block that's unreachable anyway
            always_assert_log(num_succs > 0, "block %d, %s", b->id(),
                              SHOW(*this));
          }
        }
      }
    }
  }

  if (exit_block() != nullptr) {
    always_assert_log(exit_block()->succs().empty(),
                      "exit block has outgoing edges. block %d in \n%s",
                      exit_block()->id(), SHOW(*this));
  }

  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    // make sure the edge list in both blocks agree
    for (const auto e : b->succs()) {
      const auto& reverse_edges = e->target()->preds();
      always_assert_log(std::find(reverse_edges.begin(), reverse_edges.end(),
                                  e) != reverse_edges.end(),
                        "block %d -> %d, %s", b->id(), e->target()->id(),
                        SHOW(*this));
    }
    for (const auto e : b->preds()) {
      const auto& forward_edges = e->src()->succs();
      always_assert_log(std::find(forward_edges.begin(), forward_edges.end(),
                                  e) != forward_edges.end(),
                        "block %d -> %d, %s", e->src()->id(), b->id(),
                        SHOW(*this));
    }
  }

  if (m_editable) {
    check_registers_size();
  }
  no_dangling_dex_positions();
}

void ControlFlowGraph::check_registers_size() {
  auto old_size = m_registers_size;
  recompute_registers_size();
  always_assert_log(m_registers_size == old_size,
                    "used regs %d != old registers size %d. %s",
                    m_registers_size, old_size, SHOW(*this));
}

void ControlFlowGraph::recompute_registers_size() {
  uint16_t num_regs = 0;
  const auto& check = [&num_regs](uint16_t reg, bool is_wide) {
    auto highest_in_use = reg + is_wide;
    if (highest_in_use >= num_regs) {
      // +1 because registers start at v0
      num_regs = highest_in_use + 1;
    }
  };
  for (const auto& mie : cfg::ConstInstructionIterable(*this)) {
    auto insn = mie.insn;
    if (insn->dests_size()) {
      check(insn->dest(), insn->dest_is_wide());
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      check(insn->src(i), insn->src_is_wide(i));
    }
  }
  m_registers_size = num_regs;
}

void ControlFlowGraph::no_dangling_dex_positions() const {
  std::unordered_set<DexPosition*> positions;
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_POSITION) {
        positions.insert(mie.pos.get());
      }
    }
  }

  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_POSITION && mie.pos->parent != nullptr) {
        always_assert_log(positions.count(mie.pos->parent) > 0, "%s in %s",
                          SHOW(mie), SHOW(*this));
      }
    }
  }
}

uint32_t ControlFlowGraph::num_opcodes() const {
  uint32_t result = 0;
  for (const auto& entry : m_blocks) {
    result += entry.second->num_opcodes();
  }
  return result;
}

boost::sub_range<IRList> ControlFlowGraph::get_param_instructions() {
  // Find the first block that has instructions
  Block* block = entry_block();
  while (block->num_opcodes() == 0) {
    const auto& succs = block->succs();
    always_assert(succs.size() == 1);
    const auto& out = succs[0];
    always_assert(out->type() == EDGE_GOTO);
    block = out->target();
  }
  return block->m_entries.get_param_instructions();
}

cfg::InstructionIterator ControlFlowGraph::move_result_of(
    const cfg::InstructionIterator& it) {
  auto next_insn = std::next(it);
  auto end = cfg::InstructionIterable(*this).end();
  if (next_insn != end && it.block() == next_insn.block()) {
    // The easy case where the move result is in the same block
    auto op = next_insn->insn->opcode();
    if (opcode::is_move_result_pseudo(op) || is_move_result(op)) {
      return next_insn;
    }
  }
  auto goto_edge = get_succ_edge_of_type(it.block(), EDGE_GOTO);
  if (goto_edge != nullptr) {
    auto next_block = goto_edge->target();
    if (next_block->starts_with_move_result()) {
      return cfg::InstructionIterator(
          *this, next_block,
          ir_list::InstructionIterator(next_block->get_first_insn(),
                                       next_block->end()));
    }
  }
  return end;
}

/*
 * fill `new_cfg` with a copy of `this`
 */
void ControlFlowGraph::deep_copy(ControlFlowGraph* new_cfg) const {
  always_assert(editable());
  new_cfg->m_editable = true;
  new_cfg->set_registers_size(this->get_registers_size());

  std::unordered_map<const Edge*, Edge*> old_edge_to_new;
  for (const Edge* old_edge : this->m_edges) {
    // this shallowly copies block pointers inside, then we patch them later
    Edge* new_edge = new Edge(*old_edge);
    new_cfg->m_edges.insert(new_edge);
    old_edge_to_new.emplace(old_edge, new_edge);
  }

  for (const auto& entry : this->m_blocks) {
    const Block* block = entry.second;
    // this shallowly copies edge pointers inside, then we patch them later
    Block* new_block = new Block(*block);
    new_cfg->m_blocks.emplace(new_block->id(), new_block);
  }

  // patch the edge pointers in the blocks to their new cfg counterparts
  for (auto& entry : new_cfg->m_blocks) {
    Block* b = entry.second;
    for (Edge*& e : b->m_preds) {
      e = old_edge_to_new.at(e);
    }
    for (Edge*& e : b->m_succs) {
      e = old_edge_to_new.at(e);
    }
  }

  // patch the block pointers in the edges to their new cfg counterparts
  for (Edge* e : new_cfg->m_edges) {
    e->m_src = new_cfg->m_blocks.at(e->m_src->id());
    e->m_target = new_cfg->m_blocks.at(e->m_target->id());
  }

  // update the entry and exit block pointers to their new cfg counterparts
  new_cfg->m_entry_block = new_cfg->m_blocks.at(this->m_entry_block->id());
  if (this->m_exit_block != nullptr) {
    new_cfg->m_exit_block = new_cfg->m_blocks.at(this->m_exit_block->id());
  }
}

std::vector<Block*> ControlFlowGraph::order() {
  // TODO output in a better order
  // The order should maximize PC locality, hot blocks should be fallthroughs
  // and cold blocks (like exception handlers) should be jumps.
  //
  // We want something similar to reverse post order but RPO isn't well defined
  // on cyclic graphs.
  //   (A) First, it finds Strongly Connected Components (similar to WTO)
  //   (B) It adds a node to the order upon the first traversal, not after
  //       reaching it from ALL predecessors (as a topographical sort requires).
  //       For example, we want catch blocks at the end, after the return block
  //       that they may jump to.
  //   (C) It recurses into a SCC before considering successors of the SCC
  //   (D) It places default successors immediately after

  std::vector<Block*> ordering;
  // "finished" blocks have been added to `ordering`
  std::unordered_set<BlockId> finished_blocks;

  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    if (finished_blocks.count(b->id()) != 0) {
      continue;
    }

    always_assert_log(!b->starts_with_move_result(), "%d is wrong %s", b->id(),
                      SHOW(*this));
    ordering.push_back(b);
    finished_blocks.insert(b->id());

    // If the GOTO edge leads to a block with a move-result(-pseudo), then that
    // block must be placed immediately after this one because we can't insert
    // anything between an instruction and its move-result(-pseudo).
    auto goto_edge = get_succ_edge_of_type(b, EDGE_GOTO);
    while (goto_edge != nullptr) {
      // make sure we handle a chain of blocks that all start with move-results
      auto goto_block = goto_edge->target();
      always_assert_log(m_blocks.count(goto_block->id()) > 0,
                        "bogus block reference %d -> %d in %s",
                        goto_edge->src()->id(), goto_block->id(), SHOW(*this));
      if (goto_block->starts_with_move_result() &&
          finished_blocks.count(goto_block->id()) == 0) {
        ordering.push_back(goto_block);
        finished_blocks.insert(goto_block->id());
        goto_edge = get_succ_edge_of_type(goto_block, EDGE_GOTO);
      } else {
        goto_edge = nullptr;
      }
    }
  }
  always_assert(ordering.size() == m_blocks.size());

  return ordering;
}

// Add an MFLOW_TARGET at the end of each edge.
// Insert GOTOs where necessary.
void ControlFlowGraph::insert_branches_and_targets(
    const std::vector<Block*>& ordering) {
  for (auto it = ordering.begin(); it != ordering.end(); ++it) {
    Block* b = *it;

    for (const Edge* edge : b->succs()) {
      if (edge->type() == EDGE_BRANCH) {
        auto branch_it = b->get_conditional_branch();
        always_assert_log(branch_it != b->end(), "block %d %s", b->id(), SHOW(*this));
        auto& branch_mie = *branch_it;

        BranchTarget* bt = edge->m_case_key != boost::none
                               ? new BranchTarget(&branch_mie, *edge->m_case_key)
                               : new BranchTarget(&branch_mie);
        auto target_mie = new MethodItemEntry(bt);
        edge->target()->m_entries.push_front(*target_mie);

      } else if (edge->type() == EDGE_GOTO) {
        auto next_it = std::next(it);
        if (next_it != ordering.end()) {
          Block* next = *next_it;
          if (edge->target() == next) {
            // Don't need a goto because this will fall through to `next`
            continue;
          }
        }
        auto branch_mie = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
        auto target_mie = new MethodItemEntry(new BranchTarget(branch_mie));
        edge->src()->m_entries.push_back(*branch_mie);
        edge->target()->m_entries.push_front(*target_mie);
      }
    }
  }
}

// remove all try and catch markers because we may reorder the blocks
void ControlFlowGraph::remove_try_catch_markers() {
  always_assert(m_editable);
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    b->m_entries.remove_and_dispose_if([](const MethodItemEntry& mie) {
      return mie.type == MFLOW_TRY || mie.type == MFLOW_CATCH;
    });
  }
}

IRList* ControlFlowGraph::linearize() {
  always_assert(m_editable);
  IRList* result = new IRList;

  TRACE(CFG, 5, "before linearize:\n%s", SHOW(*this));
  simplify();
  sanity_check();

  const std::vector<Block*>& ordering = order();
  insert_branches_and_targets(ordering);
  insert_try_catch_markers(ordering);

  for (Block* b : ordering) {
    result->splice(result->end(), b->m_entries);
  }

  return result;
}

void ControlFlowGraph::insert_try_catch_markers(
    const std::vector<Block*>& ordering) {
  // add back the TRY START, TRY_ENDS, and, MFLOW_CATCHes

  const auto& insert_try_marker_between =
      [this](Block* prev, MethodItemEntry* new_try_marker, Block* b) {
        auto first_it = b->get_first_insn();
        if (first_it != b->end() &&
            opcode::is_move_result_pseudo(first_it->insn->opcode())) {
          // Make sure we don't split up a move-result-pseudo and its primary
          // instruction by placing the marker after the move-result-pseudo
          //
          // TODO: relax the constraint that move-result-pseudo must be
          // immediately after its partner, allowing non-opcode
          // MethodItemEntries between
          b->m_entries.insert_after(first_it, *new_try_marker);
        } else if (new_try_marker->tentry->type == TRY_START) {
          if (prev == nullptr && b == entry_block()) {
            // Parameter loading instructions come before a TRY_START
            auto params = b->m_entries.get_param_instructions();
            b->m_entries.insert_before(params.end(), *new_try_marker);
          } else {
            // TRY_START belongs at the front of a block
            b->m_entries.push_front(*new_try_marker);
          }
        } else {
          // TRY_END belongs at the end of a block
          prev->m_entries.push_back(*new_try_marker);
        }
      };

  std::unordered_map<MethodItemEntry*, Block*> catch_to_containing_block;
  Block* prev = nullptr;
  MethodItemEntry* active_catch = nullptr;
  for (auto it = ordering.begin(); it != ordering.end(); prev = *(it++)) {
    Block* b = *it;
    MethodItemEntry* new_catch = create_catch(b, &catch_to_containing_block);

    if (new_catch == nullptr && cannot_throw(b) && !b->is_catch()) {
      // Generate fewer try regions by merging blocks that cannot throw into the
      // previous try region.
      //
      // But, we have to be careful to not include the catch block of this try
      // region, which would create invalid Dex Try entries. For any given try
      // region, none of its catches may be inside that region.
      continue;
    }

    if (active_catch != new_catch) {
      // If we're switching try regions between these blocks, the TRY_END must
      // come first then the TRY_START. We insert the TRY_START earlier because
      // we're using `insert_after` which inserts things in reverse order
      if (new_catch != nullptr) {
        // Start a new try region before b
        auto new_start = new MethodItemEntry(TRY_START, new_catch);
        insert_try_marker_between(prev, new_start, b);
      }
      if (active_catch != nullptr) {
        // End the current try region before b
        auto new_end = new MethodItemEntry(TRY_END, active_catch);
        insert_try_marker_between(prev, new_end, b);
      }
      active_catch = new_catch;
    }
  }
  if (active_catch != nullptr) {
    ordering.back()->m_entries.push_back(
        *new MethodItemEntry(TRY_END, active_catch));
  }
}

MethodItemEntry* ControlFlowGraph::create_catch(
    Block* block,
    std::unordered_map<MethodItemEntry*, Block*>* catch_to_containing_block) {
  always_assert(m_editable);

  using EdgeVector = std::vector<Edge*>;
  EdgeVector throws = get_succ_edges_of_type(block, EDGE_THROW);
  if (throws.empty()) {
    // No need to create a catch if there are no throws
    return nullptr;
  }

  std::sort(throws.begin(), throws.end(),
            [](const Edge* e1, const Edge* e2) {
              return e1->m_throw_info->index < e2->m_throw_info->index;
            });
  const auto& throws_end = throws.end();

  // recurse through `throws` adding catch entries to blocks at the ends of
  // throw edges and connecting the catch entry `next` pointers according to the
  // throw edge indices.
  //
  // We stop early if we find find an equivalent linked list of catch entries
  std::function<MethodItemEntry*(EdgeVector::iterator)> add_catch;
  add_catch = [this, &add_catch, &throws_end, catch_to_containing_block](
                  EdgeVector::iterator it) -> MethodItemEntry* {
    if (it == throws_end) {
      return nullptr;
    }
    auto edge = *it;
    auto catch_block = edge->target();
    for (auto& mie : *catch_block) {
      // Is there already a catch here that's equivalent to the catch we would
      // create?
      if (mie.type == MFLOW_CATCH &&
          catch_entries_equivalent_to_throw_edges(&mie, it, throws_end,
                                                  *catch_to_containing_block)) {
        // The linked list of catch entries starting at `mie` is equivalent to
        // the rest of `throws` from `it` to `end`. So we don't need to create
        // another one, use the existing list.
        return &mie;
      }
    }
    // create a new catch entry and insert it into the bytecode
    auto new_catch = new MethodItemEntry(edge->m_throw_info->catch_type);
    edge->target()->m_entries.push_front(*new_catch);
    catch_to_containing_block->emplace(new_catch, edge->target());

    // recurse to the next throw item
    new_catch->centry->next = add_catch(std::next(it));
    return new_catch;
  };
  return add_catch(throws.begin());
}

// Follow the catch entry linked list starting at `first_mie` and check if the
// throw edges (pointed to by `it`) are equivalent to the linked list. The throw
// edges should be sorted by their indices.
//
// This function is useful in avoiding generating multiple identical catch
// entries
bool ControlFlowGraph::catch_entries_equivalent_to_throw_edges(
    MethodItemEntry* first_mie,
    std::vector<Edge*>::iterator it,
    std::vector<Edge*>::iterator end,
    const std::unordered_map<MethodItemEntry*, Block*>&
        catch_to_containing_block) {

  for (auto mie = first_mie; mie != nullptr; mie = mie->centry->next) {
    always_assert(mie->type == MFLOW_CATCH);
    if (it == end) {
      return false;
    }

    auto edge = *it;
    always_assert_log(catch_to_containing_block.count(mie) > 0,
                      "%s not found in %s", SHOW(*mie), SHOW(*this));
    if (mie->centry->catch_type != edge->m_throw_info->catch_type ||
        catch_to_containing_block.at(mie) != edge->target()) {
      return false;
    }

    ++it;
  }
  return it == end;
}


std::vector<Block*> ControlFlowGraph::blocks() const {
  std::vector<Block*> result;
  result.reserve(m_blocks.size());
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    result.emplace_back(b);
  }
  return result;
}

ControlFlowGraph::~ControlFlowGraph() {
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    delete b;
  }

  for (Edge* e : m_edges) {
    delete e;
  }
}

Block* ControlFlowGraph::create_block() {
  size_t id = m_blocks.size();
  Block* b = new Block(this, id);
  m_blocks.emplace(id, b);
  return b;
}

// We create a small class here (instead of a recursive lambda) so we can
// label visit with NO_SANITIZE_ADDRESS
class ExitBlocks {
 private:
  uint32_t next_dfn{0};
  std::stack<const Block*> stack;
  // Depth-first number. Special values:
  //   0 - unvisited
  //   UINT32_MAX - visited and determined to be in a separate SCC
  std::unordered_map<const Block*, uint32_t> dfns;
  static constexpr uint32_t VISITED = std::numeric_limits<uint32_t>::max();
  // This is basically Tarjan's algorithm for finding SCCs. I pass around an
  // extra has_exit value to determine if a given SCC has any successors.
  using t = std::pair<uint32_t, bool>;

 public:
  std::vector<Block*> exit_blocks;

  NO_SANITIZE_ADDRESS // because of deep recursion. ASAN uses too much memory.
  t visit(const Block* b) {
    stack.push(b);
    uint32_t head = dfns[b] = ++next_dfn;
    // whether any vertex in the current SCC has a successor edge that points
    // outside itself
    bool has_exit{false};
    for (auto& succ : b->succs()) {
      uint32_t succ_dfn = dfns[succ->target()];
      uint32_t min;
      if (succ_dfn == 0) {
        bool succ_has_exit;
        std::tie(min, succ_has_exit) = visit(succ->target());
        has_exit |= succ_has_exit;
      } else {
        has_exit |= succ_dfn == VISITED;
        min = succ_dfn;
      }
      head = std::min(min, head);
    }
    if (head == dfns[b]) {
      const Block* top{nullptr};
      if (!has_exit) {
        exit_blocks.push_back(const_cast<Block*>(b));
        has_exit = true;
      }
      do {
        top = stack.top();
        stack.pop();
        dfns[top] = VISITED;
      } while (top != b);
    }
    return t(head, has_exit);
  }
};

std::vector<Block*> ControlFlowGraph::real_exit_blocks(
    bool include_infinite_loops) {
  std::vector<Block*> result;
  if (m_exit_block != nullptr && include_infinite_loops) {
    auto ghosts = get_pred_edges_of_type(m_exit_block, EDGE_GHOST);
    if (!ghosts.empty()) {
      // The exit block is a ghost block, ignore it and get the real exit points.
      for (auto e : ghosts) {
        result.push_back(e->src());
      }
    } else {
      // Empty ghosts means the method has a single exit point and
      // calculate_exit_block didn't add a ghost block.
      result.push_back(m_exit_block);
    }
  } else {
    always_assert_log(!include_infinite_loops,
                      "call calculate_exit_block first");
    for (const auto& entry : m_blocks) {
      Block* block = entry.second;
      const auto& b = block->branchingness();
      if (b == opcode::BRANCH_RETURN || b == opcode::BRANCH_THROW) {
        result.push_back(block);
      }
    }
  }
  return result;
}

/*
 * Find all exit blocks. Note that it's not as simple as looking for Blocks with
 * return or throw opcodes; infinite loops are a valid way of terminating dex
 * bytecode too. As such, we need to find all strongly connected components
 * (SCCs) and vertices that lack successors. For SCCs that lack successors, any
 * one of its vertices can be treated as an exit block; this implementation
 * picks the head of the SCC.
 */
void ControlFlowGraph::calculate_exit_block() {
  if (m_exit_block != nullptr) {
    if (!m_editable) {
      return;
    }
    if (get_pred_edge_of_type(m_exit_block, EDGE_GHOST) != nullptr) {
      // Need to clear old exit block before recomputing the exit of a CFG
      // with multiple exit points
      remove_block(m_exit_block);
      m_exit_block = nullptr;
    }
  }

  ExitBlocks eb;
  eb.visit(entry_block());
  if (eb.exit_blocks.size() == 1) {
    m_exit_block = eb.exit_blocks[0];
  } else {
    m_exit_block = create_block();
    for (Block* b : eb.exit_blocks) {
      add_edge(b, m_exit_block, EDGE_GHOST);
    }
  }
}

// public API edge removal functions
void ControlFlowGraph::delete_edge(Edge* edge) {
  remove_edge(edge);
  free_edge(edge);
}

void ControlFlowGraph::delete_edge_if(Block* source,
                                      Block* target,
                                      const EdgePredicate& predicate) {
  free_edges(remove_edge_if(source, target, predicate));
}

void ControlFlowGraph::delete_succ_edge_if(Block* block,
                                           const EdgePredicate& predicate) {
  free_edges(remove_succ_edge_if(block, predicate));
}

void ControlFlowGraph::delete_pred_edge_if(Block* block,
                                           const EdgePredicate& predicate) {
  free_edges(remove_pred_edge_if(block, predicate));
}

void ControlFlowGraph::delete_succ_edges(Block* b) {
  free_edges(remove_succ_edges(b));
}

void ControlFlowGraph::delete_pred_edges(Block* b) {
  free_edges(remove_pred_edges(b));
}

// private edge removal functions
//   These are raw removal, they don't free the edge.
ControlFlowGraph::EdgeSet ControlFlowGraph::remove_all_edges(
    Block* p, Block* s, bool cleanup) {
  return remove_edge_if(p, s, [](const Edge*) { return true; }, cleanup);
}

void ControlFlowGraph::remove_edge(Edge* edge, bool cleanup) {
  remove_edge_if(edge->src(), edge->target(),
                      [edge](const Edge* e) { return edge == e; }, cleanup);
}

ControlFlowGraph::EdgeSet ControlFlowGraph::remove_edge_if(
    Block* source,
    Block* target,
    const EdgePredicate& predicate,
    bool cleanup) {
  auto& forward_edges = source->m_succs;
  EdgeSet to_remove;
  forward_edges.erase(
      std::remove_if(forward_edges.begin(),
                     forward_edges.end(),
                     [&target, &predicate, &to_remove](Edge* e) {
                       if (e->target() == target && predicate(e)) {
                         to_remove.insert(e);
                         return true;
                       }
                       return false;
                     }),
      forward_edges.end());

  auto& reverse_edges = target->m_preds;
  reverse_edges.erase(
      std::remove_if(reverse_edges.begin(),
                     reverse_edges.end(),
                     [&to_remove](Edge* e) { return to_remove.count(e) > 0; }),
      reverse_edges.end());

  if (cleanup) {
    cleanup_deleted_edges(to_remove);
  }
  return to_remove;
}

ControlFlowGraph::EdgeSet ControlFlowGraph::remove_pred_edge_if(
    Block* block, const EdgePredicate& predicate, bool cleanup) {
  auto& reverse_edges = block->m_preds;

  std::vector<Block*> source_blocks;
  EdgeSet to_remove;
  reverse_edges.erase(
      std::remove_if(reverse_edges.begin(),
                     reverse_edges.end(),
                     [&source_blocks, &to_remove, &predicate](Edge* e) {
                       if (predicate(e)) {
                         source_blocks.push_back(e->src());
                         to_remove.insert(e);
                         return true;
                       }
                       return false;
                     }),
      reverse_edges.end());

  for (Block* source_block : source_blocks) {
    auto& forward_edges = source_block->m_succs;
    forward_edges.erase(
        std::remove_if(
            forward_edges.begin(), forward_edges.end(),
            [&to_remove](Edge* e) { return to_remove.count(e) > 0; }),
        forward_edges.end());
  }

  if (cleanup) {
    cleanup_deleted_edges(to_remove);
  }
  return to_remove;
}

ControlFlowGraph::EdgeSet ControlFlowGraph::remove_succ_edge_if(
    Block* block, const EdgePredicate& predicate, bool cleanup) {

  auto& forward_edges = block->m_succs;

  std::vector<Block*> target_blocks;
  std::unordered_set<Edge*> to_remove;
  forward_edges.erase(
      std::remove_if(forward_edges.begin(),
                     forward_edges.end(),
                     [&target_blocks, &to_remove, &predicate](Edge* e) {
                       if (predicate(e)) {
                         target_blocks.push_back(e->target());
                         to_remove.insert(e);
                         return true;
                       }
                       return false;
                     }),
      forward_edges.end());

  for (Block* target_block : target_blocks) {
    auto& reverse_edges = target_block->m_preds;
    reverse_edges.erase(
        std::remove_if(
            reverse_edges.begin(), reverse_edges.end(),
            [&to_remove](Edge* e) { return to_remove.count(e) > 0; }),
        reverse_edges.end());
  }

  if (cleanup) {
    cleanup_deleted_edges(to_remove);
  }
  return to_remove;
}

// After `edges` have been removed from the graph,
//   * Turn BRANCHes/SWITCHes with one outgoing edge into GOTOs
void ControlFlowGraph::cleanup_deleted_edges(const EdgeSet& edges) {
  for (Edge* e : edges) {
    auto pred_block = e->src();
    auto last_it = pred_block->get_last_insn();
    if (last_it != pred_block->end()) {
      auto last_insn = last_it->insn;
      auto op = last_insn->opcode();
      auto remaining_forward_edges = pred_block->succs();
      if ((is_conditional_branch(op) || is_switch(op)) &&
          remaining_forward_edges.size() == 1) {
        pred_block->m_entries.erase_and_dispose(last_it);
        remaining_forward_edges.at(0)->m_type = EDGE_GOTO;
      }
    }
  }
}

void ControlFlowGraph::free_edge(Edge* edge) {
  m_edges.erase(edge);
  delete edge;
}

void ControlFlowGraph::free_edges(const EdgeSet& edges) {
  for (Edge* e : edges) {
    free_edge(e);
  }
}

Edge* ControlFlowGraph::get_pred_edge_if(
    const Block* block, const EdgePredicate& predicate) const {
  for (auto e : block->preds()) {
    if (predicate(e)) {
      return e;
    }
  }
  return nullptr;
}

Edge* ControlFlowGraph::get_succ_edge_if(
    const Block* block, const EdgePredicate& predicate) const {
  for (auto e : block->succs()) {
    if (predicate(e)) {
      return e;
    }
  }
  return nullptr;
}

std::vector<Edge*> ControlFlowGraph::get_pred_edges_if(
    const Block* block, const EdgePredicate& predicate) const {
  std::vector<Edge*> result;
  for (auto e : block->preds()) {
    if (predicate(e)) {
      result.push_back(e);
    }
  }
  return result;
}

std::vector<Edge*> ControlFlowGraph::get_succ_edges_if(
    const Block* block, const EdgePredicate& predicate) const {
  std::vector<Edge*> result;
  for (auto e : block->succs()) {
    if (predicate(e)) {
      result.push_back(e);
    }
  }
  return result;
}

Edge* ControlFlowGraph::get_pred_edge_of_type(const Block* block,
                                              EdgeType type) const {
  return get_pred_edge_if(block,
                          [type](const Edge* e) { return e->type() == type; });
}

Edge* ControlFlowGraph::get_succ_edge_of_type(const Block* block,
                                              EdgeType type) const {
  return get_succ_edge_if(block,
                          [type](const Edge* e) { return e->type() == type; });
}

std::vector<Edge*> ControlFlowGraph::get_pred_edges_of_type(
    const Block* block, EdgeType type) const {
  return get_pred_edges_if(block,
                           [type](const Edge* e) { return e->type() == type; });
}
std::vector<Edge*> ControlFlowGraph::get_succ_edges_of_type(
    const Block* block, EdgeType type) const {
  return get_succ_edges_if(block,
                           [type](const Edge* e) { return e->type() == type; });
}

void ControlFlowGraph::merge_blocks(Block* pred, Block* succ) {
  {
    always_assert(pred->succs().size() == 1);
    auto forward_edge = pred->succs()[0];
    always_assert(forward_edge->target() == succ);
    always_assert(forward_edge->type() == EDGE_GOTO);
    always_assert(succ->preds().size() == 1);
    auto reverse_edge = succ->preds()[0];
    always_assert(forward_edge == reverse_edge);
  }

  // remove the edges between them
  remove_all_edges(pred, succ);
  // move succ's code into pred
  pred->m_entries.splice(pred->m_entries.end(), succ->m_entries);

  // move succ's outgoing edges to pred.
  auto all = [](const Edge*) { return true; };
  // Intentionally copy the vector of edges because set_edge_source edits the
  // edge vectors
  auto succs = get_succ_edges_if(succ, all);
  for (auto succ_edge : succs) {
    set_edge_source(succ_edge, pred);
  }

  // remove the succ block
  m_blocks.erase(succ->id());
  delete succ;
}

void ControlFlowGraph::set_edge_target(Edge* edge,
                                       Block* new_target) {
  move_edge(edge, nullptr, new_target);
}

void ControlFlowGraph::set_edge_source(Edge* edge,
                                       Block* new_source) {
  move_edge(edge, new_source, nullptr);
}

// Move this edge out of the vectors between its old blocks
// and into the vectors between the new blocks
void ControlFlowGraph::move_edge(Edge* edge,
                                 Block* new_source,
                                 Block* new_target) {
  remove_edge(edge, /* cleanup */ false);

  if (new_source != nullptr) {
    edge->m_src = new_source;
  }
  if (new_target != nullptr) {
    edge->m_target = new_target;
  }

  edge->src()->m_succs.push_back(edge);
  edge->target()->m_preds.push_back(edge);
}

bool ControlFlowGraph::blocks_are_in_same_try(const Block* b1,
                                              const Block* b2) const {
  const auto& throws1 = get_succ_edges_of_type(b1, EDGE_THROW);
  const auto& throws2 = get_succ_edges_of_type(b2, EDGE_THROW);
  if (throws1.size() != throws2.size()) {
    return false;
  }
  auto it1 = throws1.begin();
  auto it2 = throws2.begin();
  for (; it1 != throws1.end(); ++it1, ++it2) {
    auto e1 = *it1;
    auto e2 = *it2;
    if (e1->target() != e2->target() ||
        e1->m_throw_info->catch_type != e2->m_throw_info->catch_type) {
      return false;
    }
  }
  return true;
}

void ControlFlowGraph::remove_opcode(const InstructionIterator& it) {
  always_assert(m_editable);

  MethodItemEntry& mie = *it;
  auto insn = mie.insn;
  auto op = insn->opcode();
  always_assert_log(op != OPCODE_GOTO,
                    "There are no GOTO instructions in the CFG");
  Block* block = it.block();
  auto last_it = block->get_last_insn();
  always_assert_log(last_it != block->end(), "cannot remove from empty block");

  if (is_conditional_branch(op) || is_switch(op)) {
    // Remove all outgoing EDGE_BRANCHes
    // leaving behind only an EDGE_GOTO (and maybe an EDGE_THROW?)
    //
    // Don't cleanup because we're deleting the instruction at the end of this
    // function
    remove_succ_edge_if(block, [](const Edge* e) {
      return e->type() == EDGE_BRANCH;
    }, /* cleanup */ false);
  } else if (insn->has_move_result_pseudo()) {
    // delete the move-result-pseudo too
    if (insn == last_it->insn) {
      // The move-result-pseudo is in the next (runtime) block.
      // We follow the goto edge to the block that should have the
      // move-result-pseudo.
      //
      // We can't use std::next because that goes to the next block in ID order,
      // which may not be the next runtime block.
      auto goto_edge = get_succ_edge_of_type(block, EDGE_GOTO);
      auto move_result_block = goto_edge->target();
      auto first_it = move_result_block->get_first_insn();
      always_assert(first_it != move_result_block->end());
      always_assert_log(opcode::is_move_result_pseudo(first_it->insn->opcode()),
                        "%d -> %d in %s", block->id(), move_result_block->id(),
                        SHOW(*this));
      // We can safely delete this move-result-pseudo because it cannot be the
      // move-result-pseudo of more than one primary instruction. A CFG with
      // multiple edges to a block beginning with a move-result-pseudo is a
      // malformed CFG.
      always_assert_log(move_result_block->preds().size() == 1,
                        "Multiple edges to a move-result-pseudo in %d. %s",
                        move_result_block->id(), SHOW(*this));
      move_result_block->m_entries.erase_and_dispose(first_it);
    } else {
      // The move-result-pseudo is in the same block as this one.
      // This occurs when we're not in a try region.
      auto mrp_it = std::next(it);
      always_assert(mrp_it.block() == block);
      block->m_entries.erase_and_dispose(mrp_it.unwrap());
    }
  }

  if (insn == last_it->insn && (opcode::may_throw(op) || op == OPCODE_THROW)) {
    // We're deleting the last instruction that may throw, this block no longer
    // throws. We should remove the throw edges
    remove_succ_edge_if(block, [](const Edge* e) {
      return e->type() == EDGE_THROW;
    });
  }

  // delete the requested instruction
  block->m_entries.erase_and_dispose(it.unwrap());
}

void ControlFlowGraph::remove_block(Block* block) {
  if (block == entry_block()) {
    always_assert(block->succs().size() == 1);
    set_entry_block(block->succs()[0]->target());
  }
  remove_pred_edges(block);
  remove_succ_edges(block);
  m_blocks.erase(block->id());
  block->m_entries.clear_and_dispose();
  delete block;
}

// delete old_block and reroute its predecessors to new_block
void ControlFlowGraph::replace_block(Block* old_block,
                                     Block* new_block) {
  std::vector<Edge*> to_redirect = old_block->preds();
  for (auto e : to_redirect) {
    set_edge_target(e, new_block);
  }
  remove_block(old_block);
}

std::ostream& ControlFlowGraph::write_dot_format(std::ostream& o) const {
  o << "digraph {\n";
  for (auto* block : blocks()) {
    for (auto& succ : block->succs()) {
      o << block->id() << " -> " << succ->target()->id() << "\n";
    }
  }
  o << "}\n";
  return o;
}

std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg) {
  std::vector<Block*> postorder;
  std::vector<Block*> stack;
  std::unordered_set<Block*> visited;
  for (size_t i = 1; i < cfg.size(); i++) {
    if (cfg[i]->preds().empty()) {
      stack.push_back(cfg[i]);
    }
  }
  stack.push_back(cfg[0]);
  while (!stack.empty()) {
    auto const& curr = stack.back();
    visited.insert(curr);
    bool all_succs_visited = [&] {
      for (auto const& s : curr->succs()) {
        if (!visited.count(s->target())) {
          stack.push_back(s->target());
          return false;
        }
      }
      return true;
    }();
    if (all_succs_visited) {
      assert(curr == stack.back());
      postorder.push_back(curr);
      stack.pop_back();
    }
  }
  return postorder;
}

Block* ControlFlowGraph::idom_intersect(
    const std::unordered_map<Block*, DominatorInfo>& postorder_dominator,
    Block* block1,
    Block* block2) const {
  auto finger1 = block1;
  auto finger2 = block2;
  while (finger1 != finger2) {
    while (postorder_dominator.at(finger1).postorder <
           postorder_dominator.at(finger2).postorder) {
      finger1 = postorder_dominator.at(finger1).dom;
    }
    while (postorder_dominator.at(finger2).postorder <
           postorder_dominator.at(finger1).postorder) {
      finger2 = postorder_dominator.at(finger2).dom;
    }
  }
  return finger1;
}

// Finding immediate dominator for each blocks in ControlFlowGraph.
// Theory from:
//    K. D. Cooper et.al. A Simple, Fast Dominance Algorithm.
std::unordered_map<Block*, DominatorInfo>
ControlFlowGraph::immediate_dominators() const {
  // Get postorder of blocks and create map of block to postorder number.
  std::unordered_map<Block*, DominatorInfo> postorder_dominator;
  auto postorder_blocks = postorder_sort(blocks());
  for (size_t i = 0; i < postorder_blocks.size(); ++i) {
    postorder_dominator[postorder_blocks[i]].postorder = i;
  }

  // Initialize immediate dominators. Having value as nullptr means it has
  // not been processed yet.
  for (Block* block : blocks()) {
    if (block->preds().empty()) {
      // Entry block's immediate dominator is itself.
      postorder_dominator[block].dom = block;
    } else {
      postorder_dominator[block].dom = nullptr;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    // Traverse block in reverse postorder.
    for (auto rit = postorder_blocks.rbegin(); rit != postorder_blocks.rend();
         ++rit) {
      Block* ordered_block = *rit;
      if (ordered_block->preds().empty()) {
        continue;
      }
      Block* new_idom = nullptr;
      // Pick one random processed block as starting point.
      for (auto& pred : ordered_block->preds()) {
        if (postorder_dominator[pred->src()].dom != nullptr) {
          new_idom = pred->src();
          break;
        }
      }
      always_assert(new_idom != nullptr);
      for (auto& pred : ordered_block->preds()) {
        if (pred->src() != new_idom &&
            postorder_dominator[pred->src()].dom != nullptr) {
          new_idom = idom_intersect(postorder_dominator, new_idom, pred->src());
        }
      }
      if (postorder_dominator[ordered_block].dom != new_idom) {
        postorder_dominator[ordered_block].dom = new_idom;
        changed = true;
      }
    }
  }
  return postorder_dominator;
}

ControlFlowGraph::EdgeSet ControlFlowGraph::remove_succ_edges(Block* b, bool cleanup) {
  return remove_succ_edge_if(b, [](const Edge*) { return true; }, cleanup);
}

ControlFlowGraph::EdgeSet ControlFlowGraph::remove_pred_edges(Block* b, bool cleanup) {
  return remove_pred_edge_if(b, [](const Edge*) { return true; }, cleanup);
}

} // namespace cfg
