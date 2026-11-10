# Shared PowerPC compiler package

This source package was extracted without instruction or code-generation changes
from `mstan/gcnllr` commit `f07bfb9515082a964c36a4bbafb095dbb2d584b7`,
directory `third_party/lle/recompiler`. It preserves the DolRecomp provenance,
copyright notices and GPL license documented in UPSTREAM.md and LICENSE.

The initial extraction retains the existing CMake targets and `dolrecomp` CLI
so consumers can adopt a pinned package without changing generated-runtime
contracts. `dr_cpu`, `dr_frontend`, `dr_analysis` and `dr_backend` are reusable
library targets. Gekko and Broadway code-generation profiles already exist;
their presence does not establish Wii firmware compatibility.

Consumers own machine memory maps, device models, scheduling, title metadata,
generated dispatch registration and boot policy. The compatibility CLI and its
container readers remain here initially; they are not a shared console runtime.
No firmware, game content or generated guest code is included.

Build and test with CMake/CTest. Run the full existing suite, including emitted
code execution, before changing instruction semantics or generated contracts.
