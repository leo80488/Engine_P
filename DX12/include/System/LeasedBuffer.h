#pragma once
#include <memory>
#include <vector>

namespace Sys {
// Exclusive owner acquires storage before dispatch. Existing leases may be used
// by workers; never reuse their bytes until every consumer releases its lease.
// Storage keeps its high-water size. Consumers must overwrite requested bytes,
// use their own logical size, and never resize the vector after dispatch.
template<class T> class LeasedBuffer {
public:
    LeasedBuffer() = default;
    LeasedBuffer(const LeasedBuffer&) = delete;
    LeasedBuffer& operator=(const LeasedBuffer&) = delete;
    std::shared_ptr<std::vector<T>> Acquire(size_t count, bool* allocated = nullptr) {
        bool changed = false;
        if (!storage || storage.use_count() != 1) {
            storage = std::make_shared<std::vector<T>>(count);
            changed = count != 0;
        } else if (storage->size() < count) {
            changed = storage->capacity() < count;
            storage->resize(count);
        }
        if (allocated) *allocated = changed;
        return storage;
    }
private:
    std::shared_ptr<std::vector<T>> storage;
};
}
