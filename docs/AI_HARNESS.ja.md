# AI Harness

[English](AI_HARNESS.md) | [日本語](AI_HARNESS.ja.md) | [Documentation](../README.ja.md#documentation)

## 目的と対象範囲

AI Harness は、外部の coding agent や CI が実際の engine を起動し、Lua scenario を実行し、構造化された evidence を確認するための engine 側の実行・観測機能です。この repository に LLM、認証情報、自律 agent service は同梱していません。behavior tree や navigation などの gameplay AI は [Engine Systems](ENGINE.ja.md) を参照してください。

公開版には bounded run、Lua assertion、capture、ledger、任意で有効化する local control channel が含まれます。CTest startup wrapper は同梱していますが、元 project の Python agent SDK、`engine_ctl.py`、scenario runner、capability catalog generator、内部 scenario corpus は含みません。

## Build と runtime の有効化条件

| CMake option | Default | 効果 |
| --- | --- | --- |
| `ENGINE_ENABLE_HARNESS` | ON | `WITH_HARNESS` を定義し、scenario と evidence 機能をコンパイル |
| `ENGINE_ENABLE_CONTROL_CHANNEL` | ON | Harness も有効な場合に `WITH_CONTROL` を定義 |
| `BUILD_TESTING` | ON | Harness が有効な host の startup test を登録 |

[Build](BUILD.ja.md) の手順で構築し、option を変更したら再 build してください。CMake の Debug/Release は元 project の Retail policy を再現しません。これらの機能を除いた build にするには、両方の harness option を明示的に無効化してください。

runtime では `-artifacts` で evidence の保存先を指定します。`-exec` には保存先の指定が必要で、`-control` は local channel の起動を選択します。channel support のコンパイルだけでは listener は起動しません。hidden run も実際の Windows window と D3D12 device を作成するため、CI には対応する Windows/GPU session が必要です。

## CTest による startup check

build 後、repository root から実行します。

```powershell
ctest --preset vs-x64-release
```

`Editor.startup` と `Game.startup` は [cmake_smoke.py](../DX12/tools/cmake_smoke.py) を通じて各 90 frame を要求します。wrapper は `DX12/tests/_artifacts/cmake/` に timestamp 付き directory を作成し、engine watchdog を 180 秒、process timeout を 210 秒に設定します。test は直列に実行し、Game は終了前の screenshot と state dump も要求します。

wrapper は process/ledger の成功、frame budget、error/access counter、unread flag、空でない frame evidence を確認します。Python assertion を使用するため、`-O` や `PYTHONOPTIMIZE` を指定せず実行してください。確認範囲は starter scene の startup であり、画像の正しさ、gameplay 全体、optional SDK の動作までは保証しません。

## Frame 数を制限した手動実行

repository root から開始し、次の二つの例は同じ PowerShell session で実行してください。

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

resource/settings を解決できるよう `DX12/` から起動してください。installed build を調べる場合は installed root を使用します。`BaseScene` は組み込みの starter scene です。ledger が存在しない、または読み取れない場合は失敗・未完了であり、成功として扱えません。上の短い判定は実行完了を確認するものです。動作の合否には後述の acceptance 条件も適用してください。

| Flag | 意味 |
| --- | --- |
| `-unattended` | 対話型 error dialog による停止を避ける |
| `-hidden` | 作業用 window を前面表示せず起動する。rendering は実行する |
| `-frames=N` | present した frame 数で実行を制限 |
| `-maxseconds=S` | watchdog の期限。外部 process timeout も併用する |
| `-maxerrors=N` | 記録された engine error が N を超えると失敗 |
| `-require_draws=N` | N frame の猶予後に draw がない状態を検出。N は最低 draw 数ではない |
| `-accessguard_strict` | 宣言された ECS access contract を強制 |
| `-artifacts=directory` | この実行専用の evidence 保存先 |
| `-user_dir=directory` | Game の settings と save を隔離 |
| `-at_frame=N` | 要求した screenshot/state capture の frame |
| `-exec=file.lua` | Lua System driver を読み込む。`-artifacts` が必要 |
| `-expect_min=N` | N 件の成功した expectation を要求 |
| `-play` | Editor の scenario 更新用に Play に入る。Game は gameplay を実行済み |

## Lua scenario の作成

driver は table を返す必要があります。callback の第 1 引数は instance なので colon syntax を使用します。`set` は設定しないでください。GameFlow が入っていない system set は更新されません。同名の System は置き換えられるため、filename stem は一意にしてください。

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

この例で確認するのは driver の更新と assertion の記録だけです。component の状態や時間経過に伴う移動など、対象 feature の結果を確認する expectation に置き換え、必要な scene/content を用意してください。API を使う前に bindings を確認します。Editor では `Editor.exe` を選んで `-play` を追加してください。Edit mode で driver を読み込んでも更新は始まりません。

`Test.Expect(condition, message)` は condition を記録して返します。失敗しても後続の Lua statement は停止しません。`Test.Fail(message)` と `Test.Pass(message)` は結果を明示的に記録し、`Test.Passed()` / `Test.Failed()` で件数を取得できます。`Test.Done()` は正常な終了処理を要求しますが、記録済みの失敗は最終 verdict に反映されます。frame budget や capture より前に終了する可能性があるため、この例は `-frames` で process を止めます。

assert する driver には `-expect_min` を指定してください。assertion に到達しなかった実行を成功扱いにしないためです。Lua error により後続の更新が無効になる場合があるため、passed/failed 件数と `expectations.scriptFaults` の両方を確認します。file 不在、syntax error、返される table の欠如は load failure です。

## Evidence と acceptance

| Evidence | 確認内容 |
| --- | --- |
| `run.json` | verdict、exit code/reason、build identity、scene、frame 数、counter、expectation、unread flag |
| `frames.jsonl` | frame の進行、draw の有無、関連する計測値 |
| `DX12Log.txt` | この実行の message。ledger の `logPath` を使用 |
| 要求した screenshot | file の存在、意図した scene/画像、capture timing |
| 要求した state dump | 意図した world state と feature 固有の値 |

process と ledger の両方の成功、意図した workload/frame budget、許容範囲内の error/access counter、script fault のない expectation 完了を要求してください。`unreadFlags` は typo や未実行の code path を示す場合があるため、理由を説明できるまで合格にしません。build、scene、invocation が意図した test と一致することも確認します。

最初の frame に draw がない場合もあるため、関連する複数 frame を確認してください。`-framelog_every` は sampling を変更し、JSONL の行数と `framesRun` が一致するとは限りません。shader/PSO counter は累積値の場合があり、frame 単位の処理量として合計してはいけません。`null` は未計測を示し、ゼロではありません。screenshot だけでは正常な frame 進行を証明できません。

毎回新しい output directory を使ってください。途中で `Test.Done()` を呼ぶ場合は、期待する frame 数も整合させます。startup smoke test には golden-image comparison や feature 固有の state assertion はありません。

## Exit code と障害調査

| Code | 意味 / 次に確認する内容 |
| --- | --- |
| 0 | 正常終了。workload と evidence の確認は必要 |
| 1 | run loop から exception が伝播。log/crash evidence を確認 |
| 3 | termination または pure virtual failure |
| 10 | assertion 失敗、script fault、expectation 最低件数の未達 |
| 11 | 記録された error が budget を超過 |
| 12 | scene、driver、evidence setup の前提条件違反 |
| 13 | watchdog timeout。最後の進行状況と diagnostics を確認 |
| 14 | 猶予期間後にも draw がない |
| 15 | device loss。device/driver diagnostics を確認 |

Windows 固有の crash status も発生します。crash や外部 timeout では evidence が途中までしか残らない、または ledger が作成されないことがあります。そのまま保存し、失敗・未完了として報告してください。error budget を増やしても原因は解消しません。

## Local control channel

両方の harness option が有効な build では、frame 数などで制限した invocation に `-control` を追加すると Windows named-pipe channel を起動できます。resident session 中の query と Lua 実行をサポートします。信頼できる local tool 向けの機能であり、engine state を変更できます。sandbox ではありません。

Python client/agent SDK は同梱していません。独自 client は [ControlChannel.cpp](../DX12/src/Harness/ControlChannel.cpp) の framing、response、job completion に従ってください。queued job は完了済みではありません。main-thread action の終了を待ってから、依存する read/save を行います。timeout 時点ですでに変更が始まっている場合があるため、再実行前に state を確認してください。listener の応答だけでは frame loop が進行していると判断できません。

最初の integration には、新しい bounded process と `-exec` を使用してください。上の例に独自 pipe client は不要です。

## 再現性と CI

evidence とともに build identity、scene、settings、GPU/driver、invocation を保存してください。`-user_dir` で save data を隔離します。simulation の再現性には source の `-fixeddt`、`-seed`、`-workers` を調べてください。これらも異なる GPU/driver 間で同一 pixel を保証しません。cold shader/cache startup と warm measurement を分けます。

CI は configure/build、選択した CTest または自作 scenario の外部 timeout 付き実行、evidence の検証を行い、失敗時も run directory を保存してください。実行した fixture と assertion の範囲を明記します。build output は `DX12/build/`、log は `DX12/log/`、run evidence は `DX12/tests/_artifacts/` に配置します。これらの local output は Git の対象外です。

## Source map

| Source | 責務 |
| --- | --- |
| [cmake_smoke.py](../DX12/tools/cmake_smoke.py) | CTest wrapper と acceptance check |
| [ExecScenario.h](../DX12/include/Harness/ExecScenario.h), [ExecScenario.cpp](../DX12/src/Harness/ExecScenario.cpp) | driver contract、lifecycle、artifact gate |
| [RunLedger.cpp](../DX12/src/Harness/RunLedger.cpp) | verdict の決定と JSON evidence |
| [ExitCodes.h](../DX12/include/Harness/ExitCodes.h) | exit code 定義 |
| [ScriptSystem_Bindings.cpp](../DX12/src/Scripting/ScriptSystem_Bindings.cpp) | assertion と synthetic input bindings |
| [ControlChannel.cpp](../DX12/src/Harness/ControlChannel.cpp) | local protocol と request 実行 |
