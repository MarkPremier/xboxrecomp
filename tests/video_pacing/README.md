# Video presentation pacing

Checks packed-YUV video classification, full-screen and letterbox coverage,
global/video cap selection, and deadline changes on entering/leaving video.
Late frames restart the schedule without accumulating catch-up presentations.

```powershell
cmake -S tests/video_pacing -B build/video_pacing -A x64
cmake --build build/video_pacing --config Release
ctest --test-dir build/video_pacing -C Release --output-on-failure
```

The runtime defaults to no video limit. Serious Sam's launcher supplies
`RECOMP_VIDEO_FPS_CAP=200`; `0` disables it. This limits presentation, leaving
the title's decoder/audio timing intact. A lower numeric `RECOMP_FPS_CAP`
still wins, and the explicit `vblank` policy is preserved.
