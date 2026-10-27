#pragma once
// Harness/PakOrder.h - -pak_order[=<file>]: the asset keys this run read, each in the order it was first
// read (AssetFS::RecordReadOrder), one per line - the layout tools/package_game.py --pak-order gives a
// pak, so a first launch reads it front to back. The file defaults to <artifacts>/pak_order.txt; a run
// without -artifacts must name one. Logs `[PAK_ORDER] keys= file=`.

namespace Harness
{
    // RunLedger::Begin, before the content roots are mounted: the boot's own reads count.
    void EnablePakOrderIfRequested();
    // RunLedger::Finalize, after the teardown.
    void WritePakOrderIfRequested();
}
