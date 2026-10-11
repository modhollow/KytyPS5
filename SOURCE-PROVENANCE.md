# Source provenance and notices

This fork builds on [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5) and [Kyty](https://github.com/InoriRus/Kyty).

Chen's [Demon's Souls integration](https://github.com/chenxiao07/KytyPS5) supplies the complete emulator integration, standalone preparation tooling, tests and build dependencies represented by public revision [75aa4b9e5efa72d20b825afd93718ccfc1cc4fe9](https://github.com/chenxiao07/KytyPS5/commit/75aa4b9e5efa72d20b825afd93718ccfc1cc4fe9), plus the subsequent eight-commit update through [e019e72d051e856031fc5bd0472838116bd2afe1](https://github.com/chenxiao07/KytyPS5/commit/e019e72d051e856031fc5bd0472838116bd2afe1). Local corrections and documentation accompany those sources. Historical donor measurements do not establish the behavior or performance of this fork's builds.

The root [LICENSE](LICENSE) is GPL-2.0. Earlier Kyty notices are preserved in [LICENSES/Kyty-MIT.txt](LICENSES/Kyty-MIT.txt); ZArchive and zstd notices are also preserved in [LICENSES](LICENSES). Third-party component notices remain in their source trees, including cpuinfo/clog, stb and winpthread. Submodule revisions are recorded by Git and public repository URLs by `.gitmodules`; FetchContent revisions are recorded in `3rdparty/CMakeLists.txt`. Preserve component notices and corresponding-source obligations when redistributing.

See the [public source credits](https://github.com/modhollow/KytyPS5/wiki/Sources-and-Credits) for the repository references and licenses. Release source packages must include the dependency sources and notices corresponding to the actual binaries, rather than relying solely on an automatically generated archive that omits submodules.
