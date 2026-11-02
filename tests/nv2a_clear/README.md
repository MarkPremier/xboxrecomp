# Partial framebuffer clears

This test sends NV097 methods through the real pushbuffer executor, then checks
the resulting pixel bytes. It needs no game assets or visible window. Windows
runs both the software path and the D3D11 path, including GPU readback.

It covers inclusive rectangle edges, preservation of pixels outside a slider
rectangle and pitch padding, packed RGB565 and ARGB colours, an off-surface
rectangle, a later full clear, Z16 depth, and separate Z24/stencil clears.
Serious Sam's Options screen exposed the bug: its three slider fills cleared
the whole menu, and the packed RGB565 gold `0xCCC0` became cyan.

```powershell
cmake -S tests/nv2a_clear -B build/nv2a_clear -A x64
cmake --build build/nv2a_clear --config Release --target nv2a_clear_test
ctest --test-dir build/nv2a_clear -C Release --output-on-failure
```
