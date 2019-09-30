/* Copyright (c) 2019 The Khronos Group Inc.
 * Copyright (c) 2019 Valve Corporation
 * Copyright (c) 2019 LunarG, Inc.
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
 *
 * Author: John Zulauf <jzulauf@lunarg.com>
 */

#pragma once

#include <map>
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.h>

#include "synchronization_validation_types.h"
#include "state_tracker.h"

enum SyncHazard { NONE = 0, READ_AFTER_WRITE, WRITE_AFTER_READ, WRITE_AFTER_WRITE };

// Useful Utilites for manipulating StageAccess parameters, suitable as base class to save typing
struct SyncStageAccess {
    static SyncStageAccessFlagBits FlagBit(SyncStageAccessIndex stage_access) {
        return syncStageAccessInfoByStageAccessIndex[stage_access].stage_access_bit;
    }

    static bool IsRead(SyncStageAccessFlagBits stage_access_bit) { return stage_access_bit & syncStageAccessReadMask; }
    static bool IsRead(SyncStageAccessIndex stage_access_index) { return IsRead(FlagBit(stage_access_index)); }

    static bool IsWrite(SyncStageAccessFlagBits stage_access_bit) { return stage_access_bit & syncStageAccessWriteMask; }
    static bool IsWrite(SyncStageAccessIndex stage_access_index) { return IsWrite(FlagBit(stage_access_index)); }
    static VkPipelineStageFlagBits PipelineStageBit(SyncStageAccessIndex stage_access_index) {
        return syncStageAccessInfoByStageAccessIndex[stage_access_index].stage_mask;
    }
    static SyncStageAccessFlags AccessScopeByStage(VkPipelineStageFlags stages);
    static SyncStageAccessFlags AccessScopeByAccess(VkAccessFlags access);
    static SyncStageAccessFlags AccessScope(VkPipelineStageFlags stages, VkAccessFlags access);
    static SyncStageAccessFlags AccessScope(SyncStageAccessFlags stage_scope, VkAccessFlags accesses) {
        return stage_scope & AccessScopeByAccess(accesses);
    }
};

using ResourceUsageTag = uint64_t;  // TODO -- identify a better DWORD or QWORD size UID/Tag for usages causing hazards
struct HazardResult {
    SyncHazard hazard = NONE;
    ResourceUsageTag tag = ResourceUsageTag();
    void Set(SyncHazard hazard_, const ResourceUsageTag &tag_) {
        hazard = hazard_;
        tag = tag_;
    }
};

class ResourceAccessState : public SyncStageAccess {
  protected:
    // Mutliple read operations can be simlutaneously (and independently) synchronized,
    // given the only the second execution scope creates a dependency chain, we have to track each,
    // but only up to one per pipeline stage (as another read from the *same* stage become more recent,
    // and applicable one for hazard detection
    struct ReadState {
        VkPipelineStageFlagBits stage;  // The stage of this read
        VkPipelineStageFlags barriers;  // all applicable barriered stages
        ResourceUsageTag tag;
    };

  public:
    HazardResult DetectHazard(SyncStageAccessIndex usage_index) const;
    void Update(SyncStageAccessIndex usage_index, const ResourceUsageTag &tag);
    void ApplyExecutionBarrier(VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask);
    void ApplyMemoryBarrier(SyncStageAccessFlags src_scope, SyncStageAccessFlags dst_scope);

  private:
    bool IsWriteHazard(SyncStageAccessFlagBits usage) const { return usage & ~write_barriers; }
    bool IsReadHazard(VkPipelineStageFlagBits stage, const ReadState &read_access) const { return stage & ~read_access.barriers; }
    // With reads, each must be "safe" relative to it's prior write, so we need only
    // save the most recent write operation (as anything *transitively* unsafe would arleady
    // be included
    SyncStageAccessFlagBits last_write;  // only the most recent write
    ResourceUsageTag write_tag;
    SyncStageAccessFlags write_barriers;  // union of applicable barrier masks since last write

    std::array<ReadState, 8 * sizeof(VkPipelineStageFlags)> last_reads;
    uint32_t last_read_count;
    VkPipelineStageFlags last_read_stages;
};

// This is a place holder as we test the barrier update logic, we're only going to be tracking whole buffers at this point
// For the first pass we're only tracking ResourceAccess and barriers within a single command buffer, at record time.e
class ResourceAccessTracker : public SyncStageAccess {
  public:
    using Map = std::map<VkBuffer, ResourceAccessState>;

  private:
    ResourceAccessState default_state;

  public:
    // TODO -- hide the details of the implementation..
    Map map;
    ResourceAccessState *GetImpl(VkBuffer buffer, bool do_insert) {
        auto find_it = map.find(buffer);
        if (find_it == map.end()) {
            if (!do_insert) return nullptr;
            auto insert_pair = map.insert(std::make_pair(buffer, default_state));
            find_it = insert_pair.first;
        }
        return &find_it->second;
    }
    ResourceAccessState *Get(VkBuffer buffer) { return GetImpl(buffer, true); }

    ResourceAccessState *GetNoInsert(VkBuffer buffer) { return GetImpl(buffer, false); }

    const ResourceAccessState *Get(VkBuffer buffer) const {
        auto find_it = map.find(buffer);
        if (find_it == map.cend()) {
            return nullptr;
        }
        return &find_it->second;
    }

    void Reset() { map.clear(); }
    ResourceAccessTracker() : default_state(), map() {}
};

class SyncValidator : public ValidationStateTracker, public SyncStageAccess {
  public:
    using StateTracker = ValidationStateTracker;

    using StateTracker::AccessorTraitsTypes;
    ResourceUsageTag tag = 0;  // Find a better tagging scheme...
    std::map<VkCommandBuffer, std::unique_ptr<ResourceAccessTracker>> cb_access_state;
    ResourceAccessTracker *GetAccessTrackerImpl(VkCommandBuffer command_buffer, bool do_insert) {
        auto found_it = cb_access_state.find(command_buffer);
        if (found_it == cb_access_state.end()) {
            if (!do_insert) return nullptr;
            // If we don't have one, make it.
            std::unique_ptr<ResourceAccessTracker> tracker(new ResourceAccessTracker);
            auto insert_pair = cb_access_state.insert(std::make_pair(command_buffer, std::move(tracker)));
            found_it = insert_pair.first;
        }
        return found_it->second.get();
    }
    ResourceAccessTracker *GetAccessTracker(VkCommandBuffer command_buffer) {
        return GetAccessTrackerImpl(command_buffer, true);  // true -> do_insert on not found
    }
    ResourceAccessTracker *GetAccessTrackerNoInsert(VkCommandBuffer command_buffer) {
        return GetAccessTrackerImpl(command_buffer, false);  // false -> don't do_insert on not found
    }
    const ResourceAccessTracker *GetAccessTracker(VkCommandBuffer command_buffer) const {
        const auto found_it = cb_access_state.find(command_buffer);
        if (found_it == cb_access_state.end()) {
            return nullptr;
        }
        return found_it->second.get();
    }

    void ApplyGlobalBarriers(ResourceAccessTracker *tracker, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
                             SyncStageAccessFlags src_stage_scope, SyncStageAccessFlags dst_stage_scope,
                             uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers);
    void ApplyBufferBarriers(ResourceAccessTracker *tracker, SyncStageAccessFlags src_stage_scope,
                             SyncStageAccessFlags dst_stage_scope, uint32_t barrier_count, const VkBufferMemoryBarrier *barriers);
    void ApplyImageBarriers(ResourceAccessTracker *tracker, SyncStageAccessFlags src_stage_scope,
                            SyncStageAccessFlags dst_stage_scope, uint32_t imageMemoryBarrierCount,
                            const VkImageMemoryBarrier *pImageMemoryBarriers);

    void UpdateAccessState(ResourceAccessTracker *tracker, SyncStageAccessIndex current_usage, VkBuffer buffer,
                           const VkBufferCopy &region);
    HazardResult DetectHazard(const ResourceAccessTracker *tracker, SyncStageAccessIndex current_usage, VkBuffer buffer,
                              const VkBufferCopy &region) const;

    void ResetCommandBuffer(VkCommandBuffer command_buffer);

    void PostCallRecordCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice, VkResult result);

    bool PreCallValidateCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount,
                                      const VkBufferCopy *pRegions);

    void PreCallRecordCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount,
                                    const VkBufferCopy *pRegions);

    bool PreCallValidateCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                                           VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                                           uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                                           uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                                           uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers);

    void PreCallRecordCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                                         VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                                         uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                                         uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                                         uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers);
};