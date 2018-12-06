// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/editing/text_offset_mapping.h"

#include <ostream>
#include "third_party/blink/renderer/core/dom/node.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/editing/iterators/character_iterator.h"
#include "third_party/blink/renderer/core/editing/iterators/text_iterator.h"
#include "third_party/blink/renderer/core/editing/position.h"
#include "third_party/blink/renderer/core/layout/layout_block_flow.h"

namespace blink {

namespace {

// TODO(editing-dev): We may not need to do full-subtree traversal, but we're
// not sure, e.g. ::first-line. See |enum PseudoId| for list of pseudo elements
// used in Blink.
bool HasNonPsuedoNode(const LayoutObject& parent) {
  if (parent.NonPseudoNode())
    return true;
  for (const LayoutObject* runner = &parent; runner;
       runner = runner->NextInPreOrder(&parent)) {
    if (runner->NonPseudoNode())
      return true;
  }
  // Following HTML reach here:
  //  [1] <div style="columns: 5 31px">...</div>; http://crbug.com/832055
  //  [2] <select></select>; http://crbug.com/834623
  return false;
}

bool CanBeInlineContentsContainer(const LayoutObject& layout_object) {
  if (!layout_object.IsLayoutBlockFlow())
    return false;
  const LayoutBlockFlow& block_flow = ToLayoutBlockFlow(layout_object);
  if (!block_flow.ChildrenInline() || block_flow.IsAtomicInlineLevel())
    return false;
  if (block_flow.NonPseudoNode()) {
    // It is OK as long as |block_flow| is associated to non-pseudo |Node| even
    // if it is empty block or containing only anonymous objects.
    // See LinkSelectionClickEventsTest.SingleAndDoubleClickWillBeHandled
    return true;
  }
  // Since we can't create |EphemeralRange|, we exclude a |LayoutBlockFlow| if
  // its entire subtree is anonymous, e.g. |LayoutMultiColumnSet|,
  // and with anonymous layout objects.
  return HasNonPsuedoNode(block_flow);
}

// Returns outer most nested inline formatting context.
const LayoutBlockFlow& RootInlineContentsContainerOf(
    const LayoutBlockFlow& block_flow) {
  DCHECK(block_flow.ChildrenInline()) << block_flow;
  const LayoutBlockFlow* root_block_flow = &block_flow;
  for (const LayoutBlock* runner = block_flow.ContainingBlock(); runner;
       runner = runner->ContainingBlock()) {
    if (!runner->IsLayoutBlockFlow() || !runner->ChildrenInline())
      break;
    root_block_flow = ToLayoutBlockFlow(runner);
  }
  DCHECK(!root_block_flow->IsAtomicInlineLevel())
      << block_flow << ' ' << root_block_flow;
  return *root_block_flow;
}

// TODO(editing-dev): We should have |ComputeInlineContents()| computing first
// and last layout objects representing a run of inline layout objects in
// |LayoutBlockFlow| instead of using |ComputeInlineContentsAsBlockFlow()|.
//
// For example "<p>a<b>CD<p>EF</p>G</b>h</p>", where b has display:inline-block.
// We should have three ranges:
//  1. aCD
//  2. EF
//  3. Gh
// See RangeWithNestedInlineBlock* tests.

// Note: Since "inline-block" and "float" are not considered as text segment
// boundary, we should not consider them as block for scanning.
// Example in selection text:
//  <div>|ab<b style="display:inline-block">CD</b>ef</div>
//  selection.modify('extent', 'forward', 'word')
//  <div>^ab<b style="display:inline-block">CD</b>ef|</div>
// See also test cases for "inline-block" and "float" in |TextIterator|
//
// This is a helper function to compute inline layout object run from
// |LayoutBlockFlow|.
const LayoutBlockFlow* ComputeInlineContentsAsBlockFlow(
    const LayoutObject& layout_object) {
  const LayoutBlock* const block = layout_object.IsLayoutBlock()
                                       ? &ToLayoutBlock(layout_object)
                                       : layout_object.ContainingBlock();
  DCHECK(block) << layout_object;
  if (!block->IsLayoutBlockFlow())
    return nullptr;
  const LayoutBlockFlow& block_flow = ToLayoutBlockFlow(*block);
  if (!block_flow.ChildrenInline())
    return nullptr;
  if (block_flow.IsAtomicInlineLevel() ||
      block_flow.IsFloatingOrOutOfFlowPositioned()) {
    const LayoutBlockFlow& root_block_flow =
        RootInlineContentsContainerOf(block_flow);
    DCHECK(CanBeInlineContentsContainer(root_block_flow))
        << layout_object << " block_flow=" << block_flow
        << " root_block_flow=" << root_block_flow;
    return &root_block_flow;
  }
  if (!CanBeInlineContentsContainer(block_flow))
    return nullptr;
  return &block_flow;
}

TextOffsetMapping::InlineContents CreateInlineContentsFromBlockFlow(
    const LayoutBlockFlow& block_flow) {
  const LayoutObject* first = nullptr;
  for (const LayoutObject* runner = block_flow.FirstChild(); runner;
       runner = runner->NextInPreOrder(&block_flow)) {
    if (runner->NonPseudoNode()) {
      first = runner;
      break;
    }
  }
  if (!first) {
    DCHECK(block_flow.NonPseudoNode()) << block_flow;
    return TextOffsetMapping::InlineContents(block_flow);
  }
  const LayoutObject* last = nullptr;
  for (const LayoutObject* runner = block_flow.LastLeafChild(); runner;
       runner = runner->PreviousInPreOrder(&block_flow)) {
    if (runner->NonPseudoNode()) {
      last = runner;
      break;
    }
  }
  DCHECK(last);
  return TextOffsetMapping::InlineContents(block_flow, *first, *last);
}

TextOffsetMapping::InlineContents ComputeInlineContentsFromNode(
    const Node& node) {
  const LayoutObject* const layout_object = node.GetLayoutObject();
  if (!layout_object)
    return TextOffsetMapping::InlineContents();
  const LayoutBlockFlow* const block_flow =
      ComputeInlineContentsAsBlockFlow(*layout_object);
  if (!block_flow)
    return TextOffsetMapping::InlineContents();
  return CreateInlineContentsFromBlockFlow(*block_flow);
}

String Ensure16Bit(const String& text) {
  String text16(text);
  text16.Ensure16Bit();
  return text16;
}

}  // namespace

TextOffsetMapping::TextOffsetMapping(const InlineContents& inline_contents,
                                     const TextIteratorBehavior& behavior)
    : behavior_(behavior),
      range_(inline_contents.GetRange()),
      text16_(Ensure16Bit(PlainText(range_, behavior_))) {}

TextOffsetMapping::TextOffsetMapping(const InlineContents& inline_contents)
    : TextOffsetMapping(inline_contents,
                        TextIteratorBehavior::Builder()
                            .SetEmitsCharactersBetweenAllVisiblePositions(true)
                            .SetEmitsSmallXForTextSecurity(true)
                            .Build()) {}

int TextOffsetMapping::ComputeTextOffset(
    const PositionInFlatTree& position) const {
  if (position <= range_.StartPosition())
    return 0;
  if (position >= range_.EndPosition())
    return text16_.length();
  return TextIteratorInFlatTree::RangeLength(range_.StartPosition(), position,
                                             behavior_);
}

PositionInFlatTree TextOffsetMapping::GetPositionBefore(unsigned offset) const {
  DCHECK_LE(offset, text16_.length());
  CharacterIteratorInFlatTree iterator(range_, behavior_);
  if (offset >= 1 && offset == text16_.length()) {
    iterator.Advance(offset - 1);
    return iterator.GetPositionAfter();
  }
  iterator.Advance(offset);
  return iterator.GetPositionBefore();
}

PositionInFlatTree TextOffsetMapping::GetPositionAfter(unsigned offset) const {
  DCHECK_LE(offset, text16_.length());
  CharacterIteratorInFlatTree iterator(range_, behavior_);
  iterator.Advance(offset);
  return iterator.GetPositionAfter();
}

EphemeralRangeInFlatTree TextOffsetMapping::ComputeRange(unsigned start,
                                                         unsigned end) const {
  DCHECK_LE(end, text16_.length());
  DCHECK_LE(start, end);
  if (start == end)
    return EphemeralRangeInFlatTree();
  return EphemeralRangeInFlatTree(GetPositionBefore(start),
                                  GetPositionAfter(end));
}

unsigned TextOffsetMapping::FindNonWhitespaceCharacterFrom(
    unsigned offset) const {
  for (unsigned runner = offset; runner < text16_.length(); ++runner) {
    if (!IsWhitespace(text16_[runner]))
      return runner;
  }
  return text16_.length();
}

// static
TextOffsetMapping::BackwardRange TextOffsetMapping::BackwardRangeOf(
    const PositionInFlatTree& position) {
  return BackwardRange(FindBackwardInlineContents(position));
}

// static
TextOffsetMapping::ForwardRange TextOffsetMapping::ForwardRangeOf(
    const PositionInFlatTree& position) {
  return ForwardRange(FindForwardInlineContents(position));
}

// static
TextOffsetMapping::InlineContents TextOffsetMapping::FindBackwardInlineContents(
    const PositionInFlatTree& position) {
  for (const Node* node = position.NodeAsRangeLastNode(); node;
       node = FlatTreeTraversal::Previous(*node)) {
    const InlineContents inline_contents = ComputeInlineContentsFromNode(*node);
    if (inline_contents.IsNotNull())
      return inline_contents;
  }
  return InlineContents();
}

// static
// Note: "doubleclick-whitespace-img-crash.html" call |NextWordPosition())
// with AfterNode(IMG) for <body><img></body>
TextOffsetMapping::InlineContents TextOffsetMapping::FindForwardInlineContents(
    const PositionInFlatTree& position) {
  for (const Node* node = position.NodeAsRangeFirstNode(); node;
       node = FlatTreeTraversal::Next(*node)) {
    const InlineContents inline_contents = ComputeInlineContentsFromNode(*node);
    if (inline_contents.IsNotNull())
      return inline_contents;
  }
  return InlineContents();
}

// ----

TextOffsetMapping::InlineContents::InlineContents(
    const LayoutBlockFlow& block_flow)
    : block_flow_(&block_flow) {
  DCHECK(block_flow_->NonPseudoNode());
  DCHECK(CanBeInlineContentsContainer(*block_flow_)) << block_flow_;
}

// |first| and |last| should not be anonymous object.
// Note: "extend_selection_10_ltr_backward_word.html" has a block starts with
// collapsible whitespace with anonymous object.
TextOffsetMapping::InlineContents::InlineContents(
    const LayoutBlockFlow& block_flow,
    const LayoutObject& first,
    const LayoutObject& last)
    : block_flow_(&block_flow), first_(&first), last_(&last) {
  DCHECK(first_->NonPseudoNode()) << first_;
  DCHECK(last_->NonPseudoNode()) << last_;
  DCHECK(CanBeInlineContentsContainer(*block_flow_)) << block_flow_;
  DCHECK(first_->IsDescendantOf(block_flow_));
  DCHECK(last_->IsDescendantOf(block_flow_));
}

bool TextOffsetMapping::InlineContents::operator==(
    const InlineContents& other) const {
  return block_flow_ == other.block_flow_;
}

const LayoutBlockFlow* TextOffsetMapping::InlineContents::GetEmptyBlock()
    const {
  DCHECK(block_flow_ && !first_ && !last_);
  return block_flow_;
}

const LayoutObject& TextOffsetMapping::InlineContents::FirstLayoutObject()
    const {
  DCHECK(first_);
  return *first_;
}

const LayoutObject& TextOffsetMapping::InlineContents::LastLayoutObject()
    const {
  DCHECK(last_);
  return *last_;
}

EphemeralRangeInFlatTree TextOffsetMapping::InlineContents::GetRange() const {
  DCHECK(block_flow_);
  if (!first_) {
    const Node& node = *block_flow_->NonPseudoNode();
    return EphemeralRangeInFlatTree(
        PositionInFlatTree::FirstPositionInNode(node),
        PositionInFlatTree::LastPositionInNode(node));
  }
  const Node& first_node = *first_->NonPseudoNode();
  const Node& last_node = *last_->NonPseudoNode();
  return EphemeralRangeInFlatTree(
      first_node.IsTextNode() ? PositionInFlatTree(first_node, 0)
                              : PositionInFlatTree::BeforeNode(first_node),
      last_node.IsTextNode()
          ? PositionInFlatTree(last_node, ToText(last_node).length())
          : PositionInFlatTree::AfterNode(last_node));
}

// static
TextOffsetMapping::InlineContents TextOffsetMapping::InlineContents::NextOf(
    const InlineContents& inline_contents) {
  for (LayoutObject* runner =
           inline_contents.block_flow_->NextInPreOrderAfterChildren();
       runner; runner = runner->NextInPreOrder()) {
    if (!CanBeInlineContentsContainer(*runner))
      continue;
    const LayoutBlockFlow& block_flow = ToLayoutBlockFlow(*runner);
    if (block_flow.IsFloatingOrOutOfFlowPositioned())
      continue;
    DCHECK(!block_flow.IsAtomicInlineLevel()) << block_flow;
    return CreateInlineContentsFromBlockFlow(block_flow);
  }
  return InlineContents();
}

// static
TextOffsetMapping::InlineContents TextOffsetMapping::InlineContents::PreviousOf(
    const InlineContents& inline_contents) {
  for (LayoutObject* runner = inline_contents.block_flow_->PreviousInPreOrder();
       runner; runner = runner->PreviousInPreOrder()) {
    const LayoutBlockFlow* const block_flow =
        ComputeInlineContentsAsBlockFlow(*runner);
    if (!block_flow || block_flow->IsFloatingOrOutOfFlowPositioned())
      continue;
    DCHECK(!block_flow->IsDescendantOf(inline_contents.block_flow_))
        << block_flow;
    DCHECK(!block_flow->IsAtomicInlineLevel()) << block_flow;
    return CreateInlineContentsFromBlockFlow(*block_flow);
  }
  return InlineContents();
}

std::ostream& operator<<(
    std::ostream& ostream,
    const TextOffsetMapping::InlineContents& inline_contents) {
  return ostream << '[' << inline_contents.FirstLayoutObject() << ", "
                 << inline_contents.LastLayoutObject() << ']';
}

// ----

TextOffsetMapping::InlineContents TextOffsetMapping::BackwardRange::Iterator::
operator*() const {
  DCHECK(current_.IsNotNull());
  return current_;
}

void TextOffsetMapping::BackwardRange::Iterator::operator++() {
  DCHECK(current_.IsNotNull());
  current_ = TextOffsetMapping::InlineContents::PreviousOf(current_);
}

// ----

TextOffsetMapping::InlineContents TextOffsetMapping::ForwardRange::Iterator::
operator*() const {
  DCHECK(current_.IsNotNull());
  return current_;
}

void TextOffsetMapping::ForwardRange::Iterator::operator++() {
  DCHECK(current_.IsNotNull());
  current_ = TextOffsetMapping::InlineContents::NextOf(current_);
}

}  // namespace blink