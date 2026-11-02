# Native ZPASS queries

Exercises the real D3D11 draw path with a depth-writing wall, a hidden quad,
a visible quad with colour/depth writes disabled, cumulative reports, enable
changes, reset while enabled, and a 2x render target. Counts are expressed in
guest pixels rather than scaled host samples. No disc data or window is used.

```powershell
cmake -S tests/nv2a_zpass -B build/nv2a_zpass -A x64
cmake --build build/nv2a_zpass --config Release
ctest --test-dir build/nv2a_zpass -C Release --output-on-failure
```

`tests/nv2a_clear` also verifies the actual NV097 report method, small RAMHT
handle, report DMA bounds, unsupported report type, completion and following
memory. An empty counter must report zero, not whole-surface visibility.
