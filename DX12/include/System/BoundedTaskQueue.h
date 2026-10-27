#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

// Sequence-ring adaptation of Dmitry Vyukov's bounded MPMC queue.
// https://sites.google.com/site/1024cores/home/lock-free-algorithms/queues/bounded-mpmc-queue
// Copyright (c) 2010-2011 Dmitry Vyukov. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// THIS SOFTWARE IS PROVIDED BY DMITRY VYUKOV "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL DMITRY VYUKOV OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
// ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// The views and conclusions contained in the software and documentation are
// those of the authors and should not be interpreted as representing official
// policies, either expressed or implied, of Dmitry Vyukov.

namespace Sys {
// No mutex or allocation on Push/Pop; NOT formally lock-free: a preempted
// producer can temporarily hide the head. The scheduler uses another shard or
// sleeps until publication. A failed Push leaves its argument untouched.
template<class T, size_t Capacity>
class BoundedTaskQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_nothrow_move_assignable_v<T>);
    struct alignas(64) Slot { std::atomic<size_t> sequence{0}; T value{}; };
    std::array<Slot, Capacity> slots;
    alignas(64) std::atomic<size_t> write{0};
    alignas(64) std::atomic<size_t> read{0};
public:
    BoundedTaskQueue() {
        for (size_t i=0; i<Capacity; ++i) slots[i].sequence.store(i, std::memory_order_relaxed);
    }
    bool Push(T&& value) noexcept {
        auto position=write.load(std::memory_order_relaxed);
        Slot* slot;
        for (;;) {
            slot=&slots[position & (Capacity-1)];
            const auto delta=static_cast<intptr_t>(slot->sequence.load(std::memory_order_acquire)-position);
            if (delta == 0) {
                if (write.compare_exchange_weak(position,position+1,std::memory_order_relaxed)) break;
            } else if (delta < 0) return false;
            else position=write.load(std::memory_order_relaxed);
        }
        slot->value=std::move(value);
        slot->sequence.store(position+1,std::memory_order_release);
        return true;
    }
    bool Pop(T& value) noexcept {
        auto position=read.load(std::memory_order_relaxed);
        Slot* slot;
        for (;;) {
            slot=&slots[position & (Capacity-1)];
            const auto delta=static_cast<intptr_t>(slot->sequence.load(std::memory_order_acquire)-(position+1));
            if (delta == 0) {
                if (read.compare_exchange_weak(position,position+1,std::memory_order_relaxed)) break;
            } else if (delta < 0) return false;
            else position=read.load(std::memory_order_relaxed);
        }
        value=std::move(slot->value);
        slot->sequence.store(position+Capacity,std::memory_order_release);
        return true;
    }
};
}
