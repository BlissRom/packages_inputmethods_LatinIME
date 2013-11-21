/*
 * Copyright (C) 2013, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "suggest/policyimpl/dictionary/structure/v3/dynamic_patricia_trie_reading_helper.h"

#include "suggest/policyimpl/dictionary/utils/buffer_with_extendable_buffer.h"
#include "suggest/policyimpl/dictionary/structure/v2/patricia_trie_reading_utils.h"
#include "suggest/policyimpl/dictionary/structure/v3/dynamic_patricia_trie_reading_utils.h"
#include "utils/char_utils.h"

namespace latinime {

// To avoid infinite loop caused by invalid or malicious forward links.
const int DynamicPatriciaTrieReadingHelper::MAX_CHILD_COUNT_TO_AVOID_INFINITE_LOOP = 100000;
const int DynamicPatriciaTrieReadingHelper::MAX_PT_NODE_ARRAY_COUNT_TO_AVOID_INFINITE_LOOP = 100000;
const size_t DynamicPatriciaTrieReadingHelper::MAX_READING_STATE_STACK_SIZE = MAX_WORD_LENGTH;

// Visits all PtNodes in post-order depth first manner.
// For example, visits c -> b -> y -> x -> a for the following dictionary:
// a _ b _ c
//   \ x _ y
bool DynamicPatriciaTrieReadingHelper::traverseAllPtNodesInPostorderDepthFirstManner(
        TraversingEventListener *const listener) {
    bool alreadyVisitedChildren = false;
    // Descend from the root to the root PtNode array.
    if (!listener->onDescend(getPosOfLastPtNodeArrayHead())) {
        return false;
    }
    while (!isEnd()) {
        const PtNodeParams ptNodeParams(getPtNodeParams());
        if (!ptNodeParams.isValid()) {
            break;
        }
        if (!alreadyVisitedChildren) {
            if (ptNodeParams.hasChildren()) {
                // Move to the first child.
                if (!listener->onDescend(ptNodeParams.getChildrenPos())) {
                    return false;
                }
                pushReadingStateToStack();
                readChildNode(ptNodeParams);
            } else {
                alreadyVisitedChildren = true;
            }
        } else {
            if (!listener->onVisitingPtNode(&ptNodeParams)) {
                return false;
            }
            readNextSiblingNode(ptNodeParams);
            if (isEnd()) {
                // All PtNodes in current linked PtNode arrays have been visited.
                // Return to the parent.
                if (!listener->onReadingPtNodeArrayTail()) {
                    return false;
                }
                if (mReadingStateStack.size() <= 0) {
                    break;
                }
                if (!listener->onAscend()) {
                    return false;
                }
                popReadingStateFromStack();
                alreadyVisitedChildren = true;
            } else {
                // Process sibling PtNode.
                alreadyVisitedChildren = false;
            }
        }
    }
    // Ascend from the root PtNode array to the root.
    if (!listener->onAscend()) {
        return false;
    }
    return !isError();
}

// Visits all PtNodes in PtNode array level pre-order depth first manner, which is the same order
// that PtNodes are written in the dictionary buffer.
// For example, visits a -> b -> x -> c -> y for the following dictionary:
// a _ b _ c
//   \ x _ y
bool DynamicPatriciaTrieReadingHelper::traverseAllPtNodesInPtNodeArrayLevelPreorderDepthFirstManner(
        TraversingEventListener *const listener) {
    bool alreadyVisitedAllPtNodesInArray = false;
    bool alreadyVisitedChildren = false;
    // Descend from the root to the root PtNode array.
    if (!listener->onDescend(getPosOfLastPtNodeArrayHead())) {
        return false;
    }
    if (isEnd()) {
        // Empty dictionary. Needs to notify the listener of the tail of empty PtNode array.
        if (!listener->onReadingPtNodeArrayTail()) {
            return false;
        }
    }
    pushReadingStateToStack();
    while (!isEnd()) {
        const PtNodeParams ptNodeParams(getPtNodeParams());
        if (!ptNodeParams.isValid()) {
            break;
        }
        if (alreadyVisitedAllPtNodesInArray) {
            if (alreadyVisitedChildren) {
                // Move to next sibling PtNode's children.
                readNextSiblingNode(ptNodeParams);
                if (isEnd()) {
                    // Return to the parent PTNode.
                    if (!listener->onAscend()) {
                        return false;
                    }
                    if (mReadingStateStack.size() <= 0) {
                        break;
                    }
                    popReadingStateFromStack();
                    alreadyVisitedChildren = true;
                    alreadyVisitedAllPtNodesInArray = true;
                } else {
                    alreadyVisitedChildren = false;
                }
            } else {
                if (ptNodeParams.hasChildren()) {
                    // Move to the first child.
                    if (!listener->onDescend(ptNodeParams.getChildrenPos())) {
                        return false;
                    }
                    pushReadingStateToStack();
                    readChildNode(ptNodeParams);
                    // Push state to return the head of PtNode array.
                    pushReadingStateToStack();
                    alreadyVisitedAllPtNodesInArray = false;
                    alreadyVisitedChildren = false;
                } else {
                    alreadyVisitedChildren = true;
                }
            }
        } else {
            if (!listener->onVisitingPtNode(&ptNodeParams)) {
                return false;
            }
            readNextSiblingNode(ptNodeParams);
            if (isEnd()) {
                if (!listener->onReadingPtNodeArrayTail()) {
                    return false;
                }
                // Return to the head of current PtNode array.
                popReadingStateFromStack();
                alreadyVisitedAllPtNodesInArray = true;
            }
        }
    }
    popReadingStateFromStack();
    // Ascend from the root PtNode array to the root.
    if (!listener->onAscend()) {
        return false;
    }
    return !isError();
}

int DynamicPatriciaTrieReadingHelper::getCodePointsAndProbabilityAndReturnCodePointCount(
        const int maxCodePointCount, int *const outCodePoints, int *const outUnigramProbability) {
    // This method traverses parent nodes from the terminal by following parent pointers; thus,
    // node code points are stored in the buffer in the reverse order.
    int reverseCodePoints[maxCodePointCount];
    const PtNodeParams terminalPtNodeParams(getPtNodeParams());
    // First, read the terminal node and get its probability.
    if (!isValidTerminalNode(terminalPtNodeParams)) {
        // Node at the ptNodePos is not a valid terminal node.
        *outUnigramProbability = NOT_A_PROBABILITY;
        return 0;
    }
    // Store terminal node probability.
    *outUnigramProbability = terminalPtNodeParams.getProbability();
    // Then, following parent node link to the dictionary root and fetch node code points.
    int totalCodePointCount = 0;
    while (!isEnd()) {
        const PtNodeParams ptNodeParams(getPtNodeParams());
        totalCodePointCount = getTotalCodePointCount(ptNodeParams);
        if (!ptNodeParams.isValid() || totalCodePointCount > maxCodePointCount) {
            // The ptNodePos is not a valid terminal node position in the dictionary.
            *outUnigramProbability = NOT_A_PROBABILITY;
            return 0;
        }
        // Store node code points to buffer in the reverse order.
        fetchMergedNodeCodePointsInReverseOrder(ptNodeParams, getPrevTotalCodePointCount(),
                reverseCodePoints);
        // Follow parent node toward the root node.
        readParentNode(ptNodeParams);
    }
    if (isError()) {
        // The node position or the dictionary is invalid.
        *outUnigramProbability = NOT_A_PROBABILITY;
        return 0;
    }
    // Reverse the stored code points to output them.
    for (int i = 0; i < totalCodePointCount; ++i) {
        outCodePoints[i] = reverseCodePoints[totalCodePointCount - i - 1];
    }
    return totalCodePointCount;
}

int DynamicPatriciaTrieReadingHelper::getTerminalPtNodePositionOfWord(const int *const inWord,
        const int length, const bool forceLowerCaseSearch) {
    int searchCodePoints[length];
    for (int i = 0; i < length; ++i) {
        searchCodePoints[i] = forceLowerCaseSearch ? CharUtils::toLowerCase(inWord[i]) : inWord[i];
    }
    while (!isEnd()) {
        const PtNodeParams ptNodeParams(getPtNodeParams());
        const int matchedCodePointCount = getPrevTotalCodePointCount();
        if (getTotalCodePointCount(ptNodeParams) > length
                || !isMatchedCodePoint(ptNodeParams, 0 /* index */,
                        searchCodePoints[matchedCodePointCount])) {
            // Current node has too many code points or its first code point is different from
            // target code point. Skip this node and read the next sibling node.
            readNextSiblingNode(ptNodeParams);
            continue;
        }
        // Check following merged node code points.
        const int nodeCodePointCount = ptNodeParams.getCodePointCount();
        for (int j = 1; j < nodeCodePointCount; ++j) {
            if (!isMatchedCodePoint(ptNodeParams, j, searchCodePoints[matchedCodePointCount + j])) {
                // Different code point is found. The given word is not included in the dictionary.
                return NOT_A_DICT_POS;
            }
        }
        // All characters are matched.
        if (length == getTotalCodePointCount(ptNodeParams)) {
            if (!ptNodeParams.isTerminal()) {
                return NOT_A_DICT_POS;
            }
            // Terminal position is found.
            return ptNodeParams.getHeadPos();
        }
        if (!ptNodeParams.hasChildren()) {
            return NOT_A_DICT_POS;
        }
        // Advance to the children nodes.
        readChildNode(ptNodeParams);
    }
    // If we already traversed the tree further than the word is long, there means
    // there was no match (or we would have found it).
    return NOT_A_DICT_POS;
}

// Read node array size and process empty node arrays. Nodes and arrays are counted up in this
// method to avoid an infinite loop.
void DynamicPatriciaTrieReadingHelper::nextPtNodeArray() {
    if (mReadingState.mPos < 0 || mReadingState.mPos >= mBuffer->getTailPosition()) {
        // Reading invalid position because of a bug or a broken dictionary.
        AKLOGE("Reading PtNode array info from invalid dictionary position: %d, dict size: %d",
                mReadingState.mPos, mBuffer->getTailPosition());
        ASSERT(false);
        mIsError = true;
        mReadingState.mPos = NOT_A_DICT_POS;
        return;
    }
    mReadingState.mPosOfThisPtNodeArrayHead = mReadingState.mPos;
    const bool usesAdditionalBuffer = mBuffer->isInAdditionalBuffer(mReadingState.mPos);
    const uint8_t *const dictBuf = mBuffer->getBuffer(usesAdditionalBuffer);
    if (usesAdditionalBuffer) {
        mReadingState.mPos -= mBuffer->getOriginalBufferSize();
    }
    mReadingState.mRemainingPtNodeCountInThisArray =
            PatriciaTrieReadingUtils::getPtNodeArraySizeAndAdvancePosition(dictBuf,
                    &mReadingState.mPos);
    if (usesAdditionalBuffer) {
        mReadingState.mPos += mBuffer->getOriginalBufferSize();
    }
    // Count up nodes and node arrays to avoid infinite loop.
    mReadingState.mTotalPtNodeIndexInThisArrayChain +=
            mReadingState.mRemainingPtNodeCountInThisArray;
    mReadingState.mPtNodeArrayIndexInThisArrayChain++;
    if (mReadingState.mRemainingPtNodeCountInThisArray < 0
            || mReadingState.mTotalPtNodeIndexInThisArrayChain
                    > MAX_CHILD_COUNT_TO_AVOID_INFINITE_LOOP
            || mReadingState.mPtNodeArrayIndexInThisArrayChain
                    > MAX_PT_NODE_ARRAY_COUNT_TO_AVOID_INFINITE_LOOP) {
        // Invalid dictionary.
        AKLOGI("Invalid dictionary. nodeCount: %d, totalNodeCount: %d, MAX_CHILD_COUNT: %d"
                "nodeArrayCount: %d, MAX_NODE_ARRAY_COUNT: %d",
                mReadingState.mRemainingPtNodeCountInThisArray,
                mReadingState.mTotalPtNodeIndexInThisArrayChain,
                MAX_CHILD_COUNT_TO_AVOID_INFINITE_LOOP,
                mReadingState.mPtNodeArrayIndexInThisArrayChain,
                MAX_PT_NODE_ARRAY_COUNT_TO_AVOID_INFINITE_LOOP);
        ASSERT(false);
        mIsError = true;
        mReadingState.mPos = NOT_A_DICT_POS;
        return;
    }
    if (mReadingState.mRemainingPtNodeCountInThisArray == 0) {
        // Empty node array. Try following forward link.
        followForwardLink();
    }
}

// Follow the forward link and read the next node array if exists.
void DynamicPatriciaTrieReadingHelper::followForwardLink() {
    if (mReadingState.mPos < 0 || mReadingState.mPos >= mBuffer->getTailPosition()) {
        // Reading invalid position because of bug or broken dictionary.
        AKLOGE("Reading forward link from invalid dictionary position: %d, dict size: %d",
                mReadingState.mPos, mBuffer->getTailPosition());
        ASSERT(false);
        mIsError = true;
        mReadingState.mPos = NOT_A_DICT_POS;
        return;
    }
    const bool usesAdditionalBuffer = mBuffer->isInAdditionalBuffer(mReadingState.mPos);
    const uint8_t *const dictBuf = mBuffer->getBuffer(usesAdditionalBuffer);
    if (usesAdditionalBuffer) {
        mReadingState.mPos -= mBuffer->getOriginalBufferSize();
    }
    const int forwardLinkPosition =
            DynamicPatriciaTrieReadingUtils::getForwardLinkPosition(dictBuf, mReadingState.mPos);
    if (usesAdditionalBuffer) {
        mReadingState.mPos += mBuffer->getOriginalBufferSize();
    }
    mReadingState.mPosOfLastForwardLinkField = mReadingState.mPos;
    if (DynamicPatriciaTrieReadingUtils::isValidForwardLinkPosition(forwardLinkPosition)) {
        // Follow the forward link.
        mReadingState.mPos += forwardLinkPosition;
        nextPtNodeArray();
    } else {
        // All node arrays have been read.
        mReadingState.mPos = NOT_A_DICT_POS;
    }
}

} // namespace latinime