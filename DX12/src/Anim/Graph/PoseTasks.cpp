#include "Anim/Graph/PoseTasks.h"
#include "Resource/SkeletonAsset.h"
#include <algorithm>
#include "Anim/IK/Kinematics.h"

namespace Anim::Graph {
std::shared_ptr<const BoundGraph> BoundGraph::Bind(const Definition& def, const SkeletonAsset& sk) {
    auto bound = std::make_shared<BoundGraph>();
    bound->rest.resize(sk.boneCount);
    bound->parents.assign(sk.parentIndex,sk.parentIndex+sk.boneCount);
    for(uint32_t b=0;b<sk.boneCount;++b)bound->boneNames.emplace_back(sk.boneNames[b]);
    PoseOps::RefPose(sk, bound->rest.data());
    for (const auto& mask : def.masks) {
        std::vector<float> weights;
        PoseOps::BuildBoneMask(sk, mask.mode, mask.root, weights);
        if (mask.featherDepth > 0) {
            std::vector<int> depth(sk.boneCount, -1);
            for (uint32_t b = 0; b < sk.boneCount; ++b) if (weights[b] > 0) {
                const int p = sk.parentIndex[b];
                depth[b] = p >= 0 && depth[p] >= 0 ? depth[p] + 1 : 0;
                weights[b] *= std::min(1.f, static_cast<float>(depth[b] + 1) / mask.featherDepth);
            }
        }
        bound->masks.push_back(std::move(weights));
    }
    return bound;
}
bool PoseExecutor::Execute(const std::vector<PoseTask>& tasks, int root,
                           const BoundGraph& bound, const ClipLibrary& clips, LocalPose* out) {
    if (root < 0 || static_cast<size_t>(root) >= tasks.size()) return false;
    struct WorkerScratch{std::vector<std::vector<LocalPose>> pool;std::vector<int> refs,buffers,free;std::vector<LocalPose> reference;std::vector<DirectX::XMFLOAT4> globalA,globalB,globalRef;};
    thread_local WorkerScratch t_scratch;
    auto& m_pool=t_scratch.pool;auto& m_refs=t_scratch.refs;auto& m_buffers=t_scratch.buffers;auto& m_free=t_scratch.free;auto& m_reference=t_scratch.reference;auto& m_globalA=t_scratch.globalA;auto& m_globalB=t_scratch.globalB;auto& m_globalRef=t_scratch.globalRef;
    m_refs.assign(tasks.size(), 0);
    m_buffers.assign(tasks.size(), -1);
    m_free.clear();
    for (size_t i = 0; i < m_pool.size(); ++i) m_free.push_back(static_cast<int>(i));
    for (size_t i = 0; i < tasks.size(); ++i) {
        for (int src : {tasks[i].a, tasks[i].b, tasks[i].reference}) {
            if (src >= 0) {
                if (static_cast<size_t>(src) >= i) return false;
                ++m_refs[src];
            }
        }
    }
    ++m_refs[root];
    const auto bones = static_cast<uint32_t>(bound.rest.size());
    size_t live = 0;
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        const bool reuse=task.a>=0 && m_refs[task.a]==1;
        int buffer;
        if(reuse)buffer=m_buffers[task.a];
        else {
            if (m_free.empty()) { buffer = static_cast<int>(m_pool.size()); m_pool.emplace_back(); }
            else { buffer = m_free.back(); m_free.pop_back(); }
            m_pool[buffer].resize(bones);
            ++live; m_highWater = std::max(m_highWater, live);
        }
        m_buffers[i] = buffer;
        auto* dst = m_pool[buffer].data();
        const bool fullSample=task.op==TaskOp::Sample && task.clip<clips.Count() && clips.Get(task.clip).boneCount>=bones
            && clips.Get(task.clip).frameCount>0 && clips.Get(task.clip).frameRate>0;
        const bool fullSnapshot=task.op==TaskOp::SnapshotLoad && task.snapshot && task.snapshot->size()==bones;
        if(task.a>=0){if(!reuse)std::copy_n(m_pool[m_buffers[task.a]].data(),bones,dst);}
        else if(!fullSample && !fullSnapshot)std::copy(bound.rest.begin(),bound.rest.end(),dst);
        if (task.op == TaskOp::SnapshotLoad && task.snapshot && task.snapshot->size() == bones)
            std::copy_n(task.snapshot->data(), bones, dst);
        if (task.op == TaskOp::Sample && task.clip < clips.Count())
            PoseOps::SampleClip(clips.Get(task.clip), task.time, bones, dst, task.looping);
        const float* mask = task.mask >= 0 && static_cast<size_t>(task.mask) < bound.masks.size()
            ? bound.masks[task.mask].data() : nullptr;
        if (task.b >= 0) {
            const auto* other = m_pool[m_buffers[task.b]].data();
            if (task.op == TaskOp::Additive) {
                const LocalPose* reference = bound.rest.data();
                if (task.reference >= 0) reference = m_pool[m_buffers[task.reference]].data();
                else if (tasks[task.b].op == TaskOp::Sample && tasks[task.b].clip < clips.Count()) {
                    m_reference = bound.rest;
                    PoseOps::SampleClip(clips.Get(tasks[task.b].clip), 0, bones, m_reference.data());
                    reference = m_reference.data();
                }
                if(task.meshSpace)Anim::IK::MeshAdditive(dst,other,reference,bound.parents,task.alpha,mask,m_globalA,m_globalB,m_globalRef);
                else PoseOps::Additive(dst, other, reference, bones, task.alpha, mask);
            }
            else PoseOps::Blend(dst, other, bones, task.alpha, mask);
        }
        if(task.op==TaskOp::Inertialize && task.inertializer)
            task.inertializer->Process(dst,bones,task.dt,task.duration,task.inertialRequest,task.deadBlend);
        for (int src : {task.a, task.b, task.reference}) if (src >= 0 && --m_refs[src] == 0) {
            if(!(reuse && src==task.a)){m_free.push_back(m_buffers[src]); --live;}
        }
        if (static_cast<int>(i) == root) std::copy_n(dst, bones, out);
        if (task.op != TaskOp::SnapshotLoad && task.snapshot) task.snapshot->assign(dst, dst + bones);
        if (m_refs[i] == 0) { m_free.push_back(buffer); --live; }
    }
    return true;
}
}
