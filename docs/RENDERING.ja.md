[English](RENDERING.md) | **日本語**

# レンダリング

## 構造

Renderer が scene/GPU data、Render Graph が pass/resource 依存、Graphics が DX12 command と submission を扱います。CPU 準備と GPU 実行を区別します。標準 scene は SDR と保守的な設定です。

## Geometry と lighting

Bindless、format-aware vertex data、GPU culling、Hi-Z、indirect arguments、mesh/meshlet path と skinning/morph を組み合わせます。entity、draw、GPU work の数は同一ではありません。

Deferred G-buffer が材質／形状情報を保存し、clustered lighting が local light を整理します。directional cascade、point/spot、per-object/character の shadow path を持ちます。CPU/HLSL layout は生成 material offset とともに維持してください。

## Reflection と GI

SSR、ray-query fallback、reflection probes、DDGI と必要に応じた NRD denoising を提供します。device と frame plan により実行経路が変わります。

ReSTIR の実装は残していますが、RTXDI/SHaRC SDK tree は同梱せず、依存 backend は標準 CMake では無効です。

## Sky、AA、HDR

atmosphere、time-of-day、cloud、height/volumetric fog を実装しています。外部 HDR environment library は含みません。sun direction/intensity、exposure と environment を合わせて調整します。

TAA/FXAA と FidelityFX の経路を持ちます。Streamline は別途 SDK/runtime が必要です。temporal effect の history、motion vector、exposure、resolution と view lifetime に注意します。

SDR/HDR、bloom、DoF、auto-exposure、grading、sharpen、stylization が含まれます。capture の format/transfer function が違う画像をそのまま数値比較しないでください。

## NPR、world と effect

NPR は part-specific material、face/hair、overlay、outline を含みますが、対応する character/mask/texture は利用者が用意します。terrain、grass、water/ripple、billboard、trail、tracer、beam、particle、afterimage なども authored data と正しい frame/resource lifetime が必要です。

## Material と shader

内蔵 shader は DX12/shaders、project custom material は asset root に置きます。hook、parameter reflection、register space、GPU layout は importer/editor/renderer の共通 interface です。

graph が生成した HLSL を一次データとして手作業で書き換えないでください。compile 成功だけでは binding の正しさを保証しません。

## Preview と調査

preview は独自の view lifetime と資源を持ちます。一部は同梱しない autumn IBL を参照するため、環境反射が必要なら自分の環境を用意します。

実際の scene、draw、resource readiness、material binding、log/capture を確認し、barrier/order の問題には graph または GPU frame を調査します。CTest は全 shader permutation や全 GPU を検証していません。

現行の実装ファイル一覧は [英語版の Render-pass source index](RENDERING.md#render-pass-source-index) を参照してください。
