#pragma once
// PlayModeSession - the editor's Edit <-> Play edges: EnterPlay deep-copies the pristine editor
// world, ExitPlay restores it in place. The teardown order and its reasons are beside the code in
// PlayModeSession.cpp. The references are App's members. The editor's preview stages (Chain Physics
// Preview, Skeleton Editor, UI Editor) tear down through beforeSnapshot, which App installs on the
// editor host, so this file never needs WITH_EDITOR.

#include "ECS/ECS.h"
#include "Gameplay/GameState.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>

class Renderer;
class IGraphicsDevice;
class ScriptSystem;
namespace Audio { class AudioEngine; class AudioSystem; }
namespace DX12Physics { class PhysicsSystem; }
namespace Gameplay { class GameFlowStateMachine; }
namespace Diag { enum class DrainCause : uint8_t; }   // Diagnostics/OwnerAccounting.h

class PlayModeSession
{
public:
    PlayModeSession(World& world, Audio::AudioEngine& audioEngine, Audio::AudioSystem& audioSystem,
                    DX12Physics::PhysicsSystem& physicsSystem, ScriptSystem& scriptSystem,
                    Gameplay::GameFlowStateMachine& gameFlow, float& physicsAccumulator,
                    std::function<void(Diag::DrainCause)> drainFrameWork)
        : m_world(world), m_audioEngine(audioEngine), m_audioSystem(audioSystem),
          m_physicsSystem(physicsSystem), m_scriptSystem(scriptSystem), m_gameFlow(gameFlow),
          m_physicsAccumulator(physicsAccumulator), m_drainFrameWork(std::move(drainFrameWork)) {}

    // The Edit -> Playing edge (PlayModeController::onEnterPlay).
    void EnterPlay(Renderer& renderer);
    // The -> Edit edge (PlayModeController::onExitPlay).
    void ExitPlay(Renderer& renderer, IGraphicsDevice& backend);

    // Editor host: tear down what lives in the world but must not be snapshotted.
    std::function<void(Renderer&, World&)> beforeSnapshot;

private:
    World&                          m_world;
    Audio::AudioEngine&             m_audioEngine;
    Audio::AudioSystem&             m_audioSystem;
    DX12Physics::PhysicsSystem&     m_physicsSystem;
    ScriptSystem&                   m_scriptSystem;
    Gameplay::GameFlowStateMachine& m_gameFlow;
    float&                          m_physicsAccumulator;
    std::function<void(Diag::DrainCause)> m_drainFrameWork;
    // Pristine editor-world snapshot taken on EnterPlay; restored (in place,
    // preserving m_world's address so every system binding stays valid) on
    // ExitPlay. Null in Edit mode and in non-editor builds.
    std::unique_ptr<World> m_editorWorldSnapshot;
    // The gameplay store (flags, items, graveyard, playtime) as Play found it: it lives outside the
    // world, so the world snapshot cannot carry it. Put back by ExitPlay.
    std::optional<Gameplay::GameState::Snapshot> m_gameStateSnapshot;
};
