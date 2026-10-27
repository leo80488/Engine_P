#pragma once

// Pending GPU upload work: prepared on a background thread, executed later on the
// main thread by ResourceManager::ProcessPendingGPUUploads (time-budgeted; leftovers
// wait for the next call). The renderer provides the implementations (e.g. create an
// ID3D12Resource and copy D3D12_SUBRESOURCE_DATA).
namespace Resource
{
    class IPendingGPUUpload
    {
    public:
        virtual ~IPendingGPUUpload() = default;
        // Performs the GPU upload (create the resource, copy the data); main thread only.
        virtual void Execute() = 0;
    };
}
