# AI Harness

[English](AI_HARNESS.md) | [日本語](AI_HARNESS.ja.md) | [Documentation](../README.md#documentation)

## Purpose and scope

AI Harness gives external coding agents and CI an engine-side execution and observation layer: launch the real engine, drive Lua scenarios and inspect structured evidence. This repository does not bundle an LLM, credentials or an autonomous agent service. Gameplay AI, including behavior trees and navigation, is covered in [Engine Systems](ENGINE.md).

The public edition includes bounded runs, Lua assertions, captures, ledgers and an optional local control channel. It includes a CTest startup wrapper, but excludes the original project's Python agent SDK, `engine_ctl.py`, scenario runner, capability catalog generators and internal scenario corpus.

## Build and runtime gates

| CMake option | Default | Effect |
| --- | --- | --- |
| `ENGINE_ENABLE_HARNESS` | ON | Compiles `WITH_HARNESS`: scenarios and evidence |
| `ENGINE_ENABLE_CONTROL_CHANNEL` | ON | Compiles `WITH_CONTROL` when Harness is also enabled |
| `BUILD_TESTING` | ON | Registers startup tests for enabled hosts with Harness |

Follow [Build](BUILD.md), and rebuild after changing options. CMake Debug/Release does not reproduce the upstream Retail policy; disable both harness options explicitly for a build without these capabilities.

At runtime, `-artifacts` selects an evidence destination, `-exec` requires that destination, and `-control` opts into the local channel. Compiling channel support does not start a listener. Hidden runs still create a real Windows window and D3D12 device: CI needs a compatible Windows/GPU session.

## CTest startup checks

From the repository root after building:

```powershell
ctest --preset vs-x64-release
```

`Editor.startup` and `Game.startup` each request 90 frames through [cmake_smoke.py](../DX12/tools/cmake_smoke.py). The wrapper creates a timestamped directory under `DX12/tests/_artifacts/cmake/`, sets a 180-second engine watchdog and a 210-second process timeout, and runs tests serially. Game also requests a screenshot and state dump near the end.

The wrapper checks process/ledger success, frame budget, error/access counters, unread flags and nonempty frame evidence. It uses Python assertions: run without `-O` or `PYTHONOPTIMIZE`. This verifies starter-scene startup, not visual correctness, full gameplay or optional SDKs.

## A bounded manual run

From the repository root, use this PowerShell session for both examples:

```powershell
Set-Location DX12
$runDir = Join-Path (Get-Location) ("tests/_artifacts/adhoc/" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$engine = ".\build\ci\cmake\vs-x64\bin\Release\Game.exe"
& $engine -unattended -hidden -scene=BaseScene -frames=90 `
    -maxseconds=180 -maxerrors=0 -require_draws=20 -accessguard_strict `
    -settings=settings.default.json "-artifacts=$runDir" "-user_dir=$runDir/user" `
    -at_frame=80 -screenshot=shot.png -dumpstate=state.json
$processExit = $LASTEXITCODE
$ledger = Get-Content -Raw (Join-Path $runDir "run.json") | ConvertFrom-Json
$ledger | Select-Object verdict, exitCode, reason, framesRun, unreadFlags
if ($processExit -ne 0 -or $ledger.exitCode -ne 0 -or $ledger.verdict -ne "pass") {
    throw "Harness run failed; inspect $runDir"
}
```

Launch from `DX12/` for resource/settings resolution; use the installed root for an installed build. `BaseScene` is the built-in starter scene. A missing/unreadable ledger is failed or incomplete evidence, never a pass. The small check above checks completion; apply the acceptance guidance below to validate behavior.

| Flag | Meaning |
| --- | --- |
| `-unattended` | Avoid blocking interactive error dialogs |
| `-hidden` | Start without activating a visible working window; rendering still occurs |
| `-frames=N` | Bound the run by presented frames |
| `-maxseconds=S` | Watchdog deadline; also use an external process timeout |
| `-maxerrors=N` | Fail when logged engine errors exceed N |
| `-require_draws=N` | Detect zero draws after N grace frames; N is not a minimum draw count |
| `-accessguard_strict` | Enforce declared ECS access contracts |
| `-artifacts=directory` | Dedicated evidence destination |
| `-user_dir=directory` | Isolate Game settings and saves |
| `-at_frame=N` | Frame for requested screenshot/state capture |
| `-exec=file.lua` | Load a Lua System driver; requires `-artifacts` |
| `-expect_min=N` | Require N successful expectations |
| `-play` | Enter Play for Editor scenario ticks; Game already runs gameplay |

## Writing a Lua scenario

A driver must return a table. Callbacks receive the instance first: use colon syntax. Do not assign a `set`: a system set GameFlow has not entered will not tick. Choose a unique filename stem because a System with the same name is replaced.

```powershell
$driver = Join-Path $runDir "HarnessPublicExample.lua"
@'
local scenario = { frames = 0 }

function scenario:OnUpdate(dt)
    self.frames = self.frames + 1
    if self.frames == 30 then
        Test.Expect(type(dt) == "number", "scenario receives a numeric timestep")
        Test.Expect(dt >= 0, "scenario timestep is nonnegative")
    end
end

return scenario
'@ | Set-Content -Encoding ascii $driver

& $engine -unattended -hidden -scene=BaseScene -frames=90 `
    -maxseconds=180 -maxerrors=0 -require_draws=20 -accessguard_strict `
    -settings=settings.default.json "-artifacts=$runDir/scenario" `
    "-user_dir=$runDir/scenario/user" "-exec=$driver" -expect_min=2
$scenarioExit = $LASTEXITCODE
$scenarioLedger = Get-Content -Raw "$runDir/scenario/run.json" | ConvertFrom-Json
$scenarioLedger | Select-Object verdict, exitCode, framesRun, expectations, unreadFlags
if ($scenarioExit -ne 0 -or $scenarioLedger.exitCode -ne 0 -or $scenarioLedger.verdict -ne "pass") {
    throw "Scenario failed; inspect $runDir/scenario"
}
```

This checks driver ticking and assertion recording only. Replace the expectations with feature outcomes, such as component state or movement over time, and supply the necessary scene/content. Inspect the bindings before using an API. With Editor, select `Editor.exe` and add `-play`; loading a driver in Edit mode does not make it tick.

`Test.Expect(condition, message)` records and returns the condition; a failure does not stop later Lua statements. `Test.Fail(message)` and `Test.Pass(message)` record explicit results. `Test.Passed()` / `Test.Failed()` expose counts. `Test.Done()` requests orderly completion; recorded failures still determine the verdict. It can end before a frame budget/capture, so this example lets `-frames` stop the process.

Set `-expect_min` whenever a driver should assert. An assertion path never reached must fail. Lua errors can disable subsequent updates; inspect `expectations.scriptFaults` as well as passed/failed counts. Missing files, syntax errors and failure to return a table are load failures.

## Evidence and acceptance

| Evidence | Inspect |
| --- | --- |
| `run.json` | Verdict, exit code/reason, build identity, scene, frames, counters, expectations and unread flags |
| `frames.jsonl` | Frame progression, draw activity and relevant measurements |
| `DX12Log.txt` | This run's messages; use the ledger's `logPath` |
| Requested screenshot | File exists, intended scene/image and capture timing |
| Requested state dump | Intended world state and feature-specific values |

Require both process and ledger success, the intended workload/frame budget, acceptable error/access counters and completed expectations without script faults. Explain any `unreadFlags` before accepting a run; they can indicate a typo or a code path never reached. Confirm the build, scene and invocation match your intended test.

Inspect several relevant frames; the first can have no draws. `-framelog_every` changes sampling, so JSONL line count need not equal `framesRun`. Shader/PSO counters can be cumulative: do not sum them as per-frame work. A `null` metric is unmeasured, not zero. A screenshot alone does not prove healthy progression.

Use a fresh output directory every time. Keep frame expectations consistent with early `Test.Done()` calls. Startup smoke tests do not perform golden-image comparison or feature-specific state assertions.

## Exit codes and triage

| Code | Meaning / next check |
| --- | --- |
| 0 | Orderly success; still inspect workload and evidence |
| 1 | Exception escaped the run loop; inspect logs/crash evidence |
| 3 | Termination or pure virtual failure |
| 10 | Failed assertion, script fault or unmet expectation minimum |
| 11 | Logged errors exceeded the budget |
| 12 | Precondition failure: scene, driver or evidence setup |
| 13 | Watchdog timeout; inspect last progress and diagnostics |
| 14 | No draws after the grace period |
| 15 | Device loss; inspect device/driver diagnostics |

Other Windows crash statuses are possible. A crash or external timeout may leave partial evidence or no ledger: preserve it and report failed/incomplete execution. Raising the error budget does not resolve the underlying error.

## Local control channel

With both harness options enabled, add `-control` to a bounded invocation to opt into the Windows named-pipe channel. It supports queries and Lua execution during a resident session. It is for trusted local tools and can mutate engine state; it is not a sandbox.

No Python client/agent SDK is included. Custom clients must follow [ControlChannel.cpp](../DX12/src/Harness/ControlChannel.cpp), including framing, responses and job completion. A queued job is not complete. Wait for main-thread actions before dependent reads/saves. A timeout may occur after a mutation began: inspect state before repeating it. Listener responsiveness alone does not prove the frame loop is advancing.

For a first integration, use a fresh bounded process and `-exec`; the examples need no custom pipe client.

## Reproducibility and CI

Retain build identity, scene, settings, GPU/driver and invocation with evidence. Isolate save data using `-user_dir`. Investigate `-fixeddt`, `-seed` and `-workers` in source for repeatable simulation; these do not promise identical pixels across GPUs/drivers. Separate cold shader/cache startup from warm measurements.

CI should configure/build, run selected CTest checks or authored scenarios with an outer timeout, validate evidence, and archive run directories even on failure. Describe exactly which fixtures and assertions ran. Build outputs belong under `DX12/build/`, logs under `DX12/log/`, and run evidence under `DX12/tests/_artifacts/`; these local outputs are ignored by Git.

## Source map

| Source | Responsibility |
| --- | --- |
| [cmake_smoke.py](../DX12/tools/cmake_smoke.py) | CTest wrapper and acceptance checks |
| [ExecScenario.h](../DX12/include/Harness/ExecScenario.h), [ExecScenario.cpp](../DX12/src/Harness/ExecScenario.cpp) | Driver contract, lifecycle and artifact gate |
| [RunLedger.cpp](../DX12/src/Harness/RunLedger.cpp) | Verdict resolution and JSON evidence |
| [ExitCodes.h](../DX12/include/Harness/ExitCodes.h) | Exit-code definitions |
| [ScriptSystem_Bindings.cpp](../DX12/src/Scripting/ScriptSystem_Bindings.cpp) | Assertions and synthetic input bindings |
| [ControlChannel.cpp](../DX12/src/Harness/ControlChannel.cpp) | Local protocol and request execution |
