# powerpc-recomp-core

Shared PowerPC static-recompiler source package for GameCube/Wii low-level
recompilation projects. The initial consumers are gcnllr and wiillr.

The package contains instruction decoding, CPU semantics, code analysis and C
emission, with Gekko and Broadway profiles. Consumers own their machine runtime,
devices, boot policy and generated-code registration. Existing container readers
and the `dolrecomp` command-line tool remain available for compatibility.

This is an extraction of the adapted DolRecomp compiler used by gcnllr, with
its existing instruction and code-generation sources preserved. It is not a
complete console runtime or a claim of Wii firmware/game compatibility.
See [extraction details](EXTRACTION.md).

## Build and test

Requires CMake 3.16 or newer and a C11 compiler. Zlib is optional for compressed
RPX input.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Consumers can add this source directory with CMake and use the existing
`dr_cpu`, `dr_frontend`, `dr_analysis` and `dr_backend` library targets. Compiler
CPU-state helpers and the consumer runtime ABI are separate integration choices;
this package does not prescribe a console memory map or device implementation.

The test suite covers decoding, containers, dispatch, floating-point
semantics and emitted-code compilation/execution. Firmware, games, generated
guest code and native saves are not included.

An experimental, default-off `DOLRECOMP_FP_GUARD_SCOPE=1` definition for generated
C shares the FP-availability gate across audited FP arithmetic/move spans.
Interior dispatch entries retain their own exact fault gate; memory, MSR,
branch and unknown boundaries end the proof. Relocated emission keeps its
original gates. Regenerate through the normal compiler before selecting this
flag. The default path remains the reference; generated shape alone does not
establish a title performance improvement.

`DOLRECOMP_BLOCK_ENTRY=1` (or CMake target `dr_block_entry`) selects an
experimental block-entry body. Only proven basic-block leaders enter that hot
body; other PCs execute a separate complete native compatibility body, with
the same instruction semantics and cycle-prefix accounting. The default is
off. Relocatable emission retains its existing path.

Consumers that inject mid-block native hooks must define
`DOLRECOMP_BLOCK_ENTRY_COMPAT=1` before the generated ABI include on every
transformed owner page. This keeps the canonical `func_`/`label_`/`case 0x`
seams active for all instruction entries and excludes the extra resume body.
The latter uses a separate `resume_label_` namespace and parenthesized case
constants, so existing canonical entry rewrites stay unambiguous. Regenerate
with a header exposing `DOLRECOMP_BLOCK_ENTRY_ABI`; do not mix relative ABI
headers from different catalogs. This profile needs per-title qualification;
its emitted shape and component checks do not establish a performance win.

## License and upstream

Imported copyright notices and the [GPL license](LICENSE) are preserved.
The compiler derives from [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp)
through its gcnrecomp integration fork. See [provenance](UPSTREAM.md) and the
[original integration README](UPSTREAM_README.md) for attribution and history.
For the general-purpose upstream compiler, use the canonical DolRecomp project.
