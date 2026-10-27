# zstd 1.5.7 - the decoder

The decompression half of [zstd](https://github.com/facebook/zstd) 1.5.7 (BSD; `LICENSE`), from the
release tarball `zstd-1.5.7.tar.gz` (sha256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`):
`lib/zstd.h`, `lib/zstd_errors.h`, the `lib/common/` files the decoder needs and `lib/decompress/` without
its GCC-only assembly. EngineCore compiles the `.c` files (`EngineCore.vcxproj`) for `.ipak` version 2's
zstd entries (`Resource::AssetFS`); the writer is Python 3.14's `compression.zstd` (`tools/pack_assets.py`).
