# GPU Execution

## Use It When

Search terms: CUDA, OpenCL, GPU registration, device, stream, CPU/GPU task, fallback, device memory, kernel failure.

Treat GPU support as an optional expert path. Begin with ordinary CPU behavior and make fallback policy explicit; registration, device availability, capacity, and kernel execution are separate facts.

## Public Boundary

- `include/executor/gpu/`: device query, scheduler, launch and transfer optimizers.
- `include/executor/executor.hpp`: registration, status, and dual-path submission facade.
- `include/executor/config.hpp` and `types.hpp`: GPU configuration and status.

## Implementation Trail

Read `src/executor/gpu/` for CUDA/OpenCL executor, loader lease, memory manager, and scheduler ownership. Advanced raw executor access is non-owning and must not outlive or race shutdown.

## Observable Contract

- CPU fallback occurs only under an explicit policy; a GPU preference is not a success guarantee.
- Registration success does not prove a kernel completed, and a selected device does not prove a performance win.
- Record backend/device error, queue/capacity state, and task exception through status/future/event surfaces.

## Change Safeguards

Protect backend unload, stream/resource lifetime, queue/wait synchronization, and fallback explanation. Run the affected CUDA/OpenCL tests; preserve CPU-only behavior when an optional backend is absent.

## Related Material

`website/en/gpu/`, `docs/design/gpu_executor.md`, and `docs/BUILD.md`.
