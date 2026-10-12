# APU DSP memory windows

Checks every aligned word in the GP X, MIXBUF, Y and P windows and the EP X,
Y and P windows against the DSP engine's memory API. Also checks register
fallbacks at the exclusive window ends and the GP/EP reset registers.
This guards the portable bounds checks that replace GNU switch-case ranges
for MSVC, without executing guest DSP programs or requiring game files.
The same executable verifies that supplied physical mappers receive the
original addresses and complete transfer extents, and that the guest audio
guard is recursive and excludes competing threads.

From the toolkit root on Windows:

```powershell
cmake -S tests\apu_dsp_memory -B build-xr\apu-dsp-memory
cmake --build build-xr\apu-dsp-memory --config Release --target apu_dsp_memory_test
ctest --test-dir build-xr\apu-dsp-memory -C Release --output-on-failure
```
