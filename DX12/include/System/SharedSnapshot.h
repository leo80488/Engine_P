#pragma once
#include <memory>

namespace Sys {
// One exclusive publisher; readers hold immutable leases. Acquire/Assign must
// not run concurrently on this wrapper. Previously acquired leases may be read
// concurrently with Assign. Unique storage preserves nested container capacity.
template<class T> class SharedSnapshot {
public:
    explicit SharedSnapshot(const T& source) : value(std::make_shared<T>(source)) {}
    const T* Read() const { return value.get(); }
    std::shared_ptr<const T> Acquire() const { return value; }
    void Assign(const T& source) {
        if (value.use_count() == 1) *value = source;
        else value = std::make_shared<T>(source);
    }
    // The live object when no lease shares it, else null: for an in-place
    // PARTIAL refresh. Same single-publisher rule as Assign; a leased object
    // must go through Assign's copy-on-write instead.
    T* MutableIfUnique() { return value.use_count() == 1 ? value.get() : nullptr; }
private:
    std::shared_ptr<T> value;
};
}
