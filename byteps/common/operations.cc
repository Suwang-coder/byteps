// Copyright 2019 ByteDance Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include <cstring>
#include <memory>
#include <thread>

#include "logging.h"
#include "operations.h"
#include "core_loops.h"
#include "global.h"

namespace byteps {
namespace common {

extern "C" {

void byteps_init() {
    BytePSGlobal::Init();

    // The order of func does not matter
    std::vector<LoopFunction> func;

    // Push & Pull in distirbuted mode
    if (BytePSGlobal::IsDistributed()) {
        if (BytePSGlobal::IsRootDevice()) {
            func.push_back(PushLoop);
            func.push_back(PullLoop);
        }
        else {
            func.push_back(CoordinatePushLoop);
        }
    }

    // Cross-PCIe-switch reduce
    if (BytePSGlobal::IsCrossPcieSwitch()) {
        func.push_back(PcieReduceLoop);
    }

    // Copy between GPU and CPU
    if (BytePSGlobal::IsCrossPcieSwitch() || BytePSGlobal::IsDistributed()) {
        func.push_back(CopyDevice2HostLoop);
        if (BytePSGlobal::IsRootDevice()) {
            func.push_back(RootCopyHost2DeviceLoop);
        }
        else {
            func.push_back(NonRootCopyHost2DeviceLoop);
            func.push_back(NonRootCopyListenLoop);
        }
    }

    // Per-PCIe-switch NCCL calls
    func.push_back(SyncNcclLoop);
    if (BytePSGlobal::GetNccl()->IsSignalRoot()) {
        func.push_back(RootNcclLoop);
    }
    else {
        func.push_back(CoordinateReduceLoop);
        func.push_back(CoordinateBroadcastLoop);
        func.push_back(NonRootNcclLoop);
    }
    
    BytePSGlobal::Start(func);
    return;
}

void byteps_shutdown() {
    BytePSGlobal::Shutdown();
    BPS_LOG(TRACE) << "BytePS is shutdown.";
    return;
}

int byteps_rank() {
    return BytePSGlobal::GetRank();
}

int byteps_local_rank() {
    return BytePSGlobal::GetLocalRank();
}

int byteps_size() {
    return BytePSGlobal::GetSize();
}

int byteps_local_size() {
    return BytePSGlobal::GetLocalSize();
}

} // extern "C"

Status CheckInitialized() {
    return BytePSGlobal::CheckInit();
}

void PartitionTensor(std::shared_ptr<TensorTableEntry> entry,
                    std::vector<std::shared_ptr<TensorTableEntry> > &partitions) {
    BPS_CHECK(entry->counter_ptr) << entry->tensor_name << " counter pointer is null";
    auto size = entry->tensor ? entry->tensor->size() : entry->output->size();
    auto bound = BytePSGlobal::GetPartitionBound();
    auto accumulated = 0;
    int i = 0;

    while (accumulated < size) {
        std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
        // will assign the key later, so don't do it now
        // e->key = entry->key;
        e->tensor_name = entry->tensor_name + std::string("_") + std::to_string(i);
        e->context = entry->context;
        e->ready_event = entry->ready_event;
        e->device = entry->device;
        e->priority = entry->priority;
        e->version = entry->version;
        e->callback = entry->callback;
        e->cpubuff = entry->cpubuff;
        e->pcie_cpubuff = entry->pcie_cpubuff;
        e->queue_list = entry->queue_list;
        e->tensor = entry->tensor;
        e->output = entry->output;
        e->offset = accumulated;
        e->len = ((size - accumulated) > bound) ? bound : (size - accumulated);
        e->counter_ptr = entry->counter_ptr;
        e->total_partnum = entry->total_partnum;

        accumulated += e->len;
        ++i;

        partitions.push_back(e);
    }
}

Status EnqueueTensor(BPSContext &context,
                     std::shared_ptr<Tensor> input,
                     std::shared_ptr<Tensor> output,
                     std::shared_ptr<ReadyEvent> ready_event,
                     const std::string &name,
                     const int device, const int priority, const int version,
                     StatusCallback callback,
                     std::shared_ptr<std::vector<QueueType>> queue_list) {
    if (input && output) {
        BPS_CHECK_EQ(input->size(), output->size()) << name << " output tensor size does not match";
    }

    std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
    e->tensor_name = name;
    e->context = &context;
    e->tensor = input;
    e->output = output;
    e->ready_event = ready_event;
    e->device = device;
    e->priority = priority;
    e->version = version;
    e->callback = callback;
    e->cpubuff = context.cpubuff;
    e->pcie_cpubuff = context.pcie_cpubuff;
    e->queue_list = *queue_list;
    e->counter_ptr = std::make_shared<std::atomic_int>(0);
    e->total_partnum = context.key_list.size();

    std::vector<std::shared_ptr<TensorTableEntry> > partitions;
    PartitionTensor(e, partitions);
    BPS_CHECK_EQ(context.key_list.size(), partitions.size()) << name
            << ": " << context.key_list.size()
            << ", " << partitions.size();

    if (e->queue_list.size() == 0) {
        BPS_LOG(DEBUG) << e->tensor_name << ", device=" << e->device
                       << " has no queue_list assigned, skipped";
        e->callback(Status::OK());
        return Status::OK();
    }

    unsigned int accumulated = 0;
    for (size_t i = 0; i < partitions.size(); ++i) {
        auto task = partitions[i];
        task->key = context.key_list[i]; // assign the key now
        BPS_LOG(TRACE) << "EnqueueTensor: " << task->tensor_name
                       << ", key=" << task->key
                       << ", offset=" << task->offset
                       << ", len=" << task->len
                       << ", device=" << task->device
                       << " rank=" << BytePSGlobal::GetLocalRank();

        BytePSGlobal::GetScheduledQueue(e->queue_list[0])->addTask(task);
        accumulated += task->len;
    }
    BPS_CHECK_EQ(accumulated, (e->tensor ? e->tensor->size(): e->output->size()))
        << "accumulated partition size not equal to original tensor size";

    BPS_LOG(TRACE) << "EnqueueTensor finished: " << name << ", rank=" << BytePSGlobal::GetLocalRank();
    return Status::OK();
}

void InitTensor(BPSContext &context, const std::string &name, int dtype, void *cpubuff) {

    // Get metadata
    auto key_list = context.key_list;
    size_t size = context.buff_len;
    auto bound = BytePSGlobal::GetPartitionBound();

    BPS_CHECK_GT(key_list.size(), 0) << name;
    BPS_CHECK_EQ(key_list.size(), (unsigned int) (size+bound-1)/bound) // round up
                    << key_list.size()
                    << ", size=" << size
                    << ", bound=" << bound;

    BPS_LOG(TRACE) << "Begin init " << name
                   << ", size=" << size
                   << ", parts=" << key_list.size();

    if (cpubuff) {
        BPS_LOG(TRACE) << name << " is already on cpu, len=" << size;
        context.cpubuff = cpubuff;
        context.reuse_buff = true;
    }
    else {
        // use the first key in key_list as the index
        auto shm_obj = BytePSGlobal::GetSharedMemoryObj(); 
        if (BytePSGlobal::IsCrossPcieSwitch()) {
            context.pcie_cpubuff = shm_obj->openPcieSharedMemory(key_list[0], size);
            context.cpubuff = context.pcie_cpubuff.back();
        }
        else {
            context.cpubuff = shm_obj->openSharedMemory(key_list[0], size);
        }
        context.reuse_buff = false;
        BPS_LOG(TRACE) << name << ": open shared memory size " << size;
    }

    char* data = const_cast<char*> (static_cast<const char*> (context.cpubuff));

    size_t accumulated = 0;
    size_t i = 0;
    while (accumulated < size) {
        auto key = key_list[i];
        int len = ((size - accumulated) > bound) ? bound : (size - accumulated);

        if (BytePSGlobal::IsDistributed() && BytePSGlobal::IsRootDevice()) {
            if (BytePSGlobal::GetWorkerID() == 0) { // only worker0 pushes init data
                // encode the key for pskv scattering
                auto& pskv = BytePSGlobal::EncodeDefaultKey(key, len);
                // false means not to delete data when SArray is deleted
                ps::SArray<char> vals(data + accumulated, len, false);
                // cmd type
                int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
                // blocking push
                BytePSGlobal::GetPS()->Wait(BytePSGlobal::GetPS()->ZPush(
                    pskv.keys, vals, pskv.lens, cmd));
            }
            // sync all workers
            ps::Postoffice::Get()->Barrier(0, ps::kWorkerGroup);
        }

        accumulated += len;
        ++i;
    }

    BPS_CHECK_EQ(accumulated, size);
    BPS_CHECK_EQ(i, key_list.size());

    context.initialized = true;

    BPS_LOG(TRACE) << "Finish Init " << name
                   << ", size=" << size
                   << ", parts=" << key_list.size();
}

Status EnqueueTensorInit(BPSContext &context, const std::string &name, int dtype, void *cpubuff,
                         StatusCallback callback) {
    InitTensor(context, name, dtype, cpubuff);
    callback(Status::OK());
    return Status::OK();
}

BPSContext& GetContextFromName(const std::string &name) {
    return BytePSGlobal::GetContextFromName(name);
}

bool IsTensorInitialized(const std::string &name, size_t size) {
    return BytePSGlobal::IsTensorInitialized(name, size);
}

std::shared_ptr<std::vector<QueueType>> GetPushQueueList(int device) {
    auto queue_list = std::make_shared<std::vector<QueueType>>();
    if (device != CPU_DEVICE_ID) {

        // Per-PCIe-switch NCCL reduce
        if (BytePSGlobal::GetNccl()->IsSignalRoot()) {
            queue_list->push_back(REDUCE);
        }
        else {
            queue_list->push_back(COORDINATE_REDUCE);
            queue_list->push_back(REDUCE);
        }

        // Copy from GPU to CPU
        if (BytePSGlobal::IsDistributed() || BytePSGlobal::IsCrossPcieSwitch()) {
            queue_list->push_back(COPYD2H);
        }

        // Cross-PCIe-switch reduce
        if (BytePSGlobal::IsCrossPcieSwitch()) {
            queue_list->push_back(PCIE_REDUCE);
        }

        // Push in distirbuted mode
        if (BytePSGlobal::IsDistributed()) {
            if (BytePSGlobal::IsRootDevice()) {
                queue_list->push_back(PUSH);
            }
            else {
                queue_list->push_back(COORDINATE_PUSH);
            }
        }
    }
    return queue_list;
}

std::shared_ptr<std::vector<QueueType>> GetPullQueueList(int device) {
    auto queue_list = std::make_shared<std::vector<QueueType>>();
    if (device != CPU_DEVICE_ID) {

        // Pull in distirbuted mode
        if (BytePSGlobal::IsDistributed()) {
            if (BytePSGlobal::IsRootDevice()) {
                queue_list->push_back(PULL);
            }
        }

        // Copy from CPU to GPU
        if (BytePSGlobal::IsDistributed() || BytePSGlobal::IsCrossPcieSwitch()) {
            queue_list->push_back(COPYH2D);
        }

        // Per-PCIe-switch NCCL broadcast
        if (BytePSGlobal::GetNccl()->IsSignalRoot()) {
            queue_list->push_back(BROADCAST);
        }
        else {
            queue_list->push_back(COORDINATE_BROADCAST);
            queue_list->push_back(BROADCAST);
        }
    }
    return queue_list;
}

} // namespace common
} // namespace byteps
