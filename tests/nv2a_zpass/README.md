# Native ZPASS queries

Exercises the real D3D11 draw path with a depth-writing wall, a hidden quad,
a visible quad with colour/depth writes disabled, cumulative reports, enable
changes, reset while enabled, and a 2x render target. Counts are expressed in
guest pixels rather than scaled host samples. No disc data or window is used.

The async regression queues reports, resets/reuses the counter before results
arrive, and verifies the original snapshots and FIFO completion. Colour-masked
queries and ordinary depth prepasses must retain the scene's scaled depth owner
without layout syncs; a separate real shadow target remains unscaled.

```powershell
cmake -S tests/nv2a_zpass -B build/nv2a_zpass -A x64
cmake --build build/nv2a_zpass --config Release
ctest --test-dir build/nv2a_zpass -C Release --output-on-failure
```

`tests/nv2a_clear` also verifies the actual NV097 report method, small RAMHT
handle, report DMA bounds, unsupported report type, completion and following
memory. An empty counter must report zero, not whole-surface visibility.
The native dispatch case also checks that a nonempty report stays pending until
polled, and completes with 48 pixels even after submission stops.

`nv2a-shader-cache-restart` runs the graphics fixture twice in separate processes
against a fresh private cache directory. It also draws a partial clear, scales
targets, and publishes multisampled depth to exercise utility shaders. The cold
run must invoke the source compiler; the warm run must load disk bytecode with
zero source compiler calls. Normal query tests disable the disk cache, and the
restart test never clears or writes the user's shared shader cache.
