# Issues

Open problems found from logs or testing. Each entry has the evidence, the cause and suggested fixes.

## 1. Sudden quality drop in 4K: the bitrate falls to 2 Mbps

**Found:** 2026-10-10, in `%LOCALAPPDATA%\MyCam\mycam.log` (4K session starting 02:21:42, 3840×2160 at 39 Mbps).
**Status:** open.

The USB link isn't the problem. The PC can't decode 4K fast enough, and the phone mistakes that for a slow
link, so it keeps lowering the bitrate until it reaches the 2 Mbps floor.

### What happened

1. **The PC was too slow from the start.** At 4K, decode + copy averaged **35–40 ms** per frame, but at
   30 fps each frame only has 33 ms. The PC received only **~23–25 fps** (70–78 frames per 3 s), and the
   phone's "capture to encoded" latency grew to about **1.5 s** as frames piled up on the phone. At 1080p
   the same steps take about 7 ms.
2. **Decode slowed further at 02:22:37** (averages of 50–60 ms). Frames are decoded inside the USB read
   callback (`pc/companion/phone_link.cpp:277`), so the 16 queued reads filled up, the PC stopped accepting
   data, and the phone's frame writes stalled for 67–145 ms.
3. **The phone treated each stall as a lack of bandwidth** (`WebcamService.kt` `adaptToLink`). Each time
   it cut the bitrate by a quarter and skipped frames until the next key frame: 39 → 29 → 22 → 16 → 12 →
   9 Mbps. It recovered to 19 Mbps, then a second burst of stalls at 02:23:46 took it down to **2 Mbps**.
   4K at 2 Mbps is very blocky, and 348 frames had been dropped by then.
4. **Lowering the bitrate didn't help.** Decode time barely depends on bitrate, so stalls continued even at
   2 Mbps (~230 KB/s, a tiny fraction of what USB 2.0 can carry). The bitrate stayed at the floor until the
   PC's load eased around 02:24:20.
5. **Recovery is slow.** The bitrate rises 15% every 5 s, so going from 2 back to 19 Mbps takes about
   80 s. The log ends at 9.3 Mbps, still climbing.

Key log lines:

```
02:22:35.204 stats: 4746 KB/s, 78 packets, 76 video frames in, 76 decoded, ... decode+copy avg 36.0 ms
02:22:37.070 phone says: link behind (frame write took 111 ms): bitrate now 29859 kbps, skipping to next key frame
02:23:46.286 phone says: link behind (frame write took 80 ms): bitrate now 14252 kbps, skipping to next key frame
02:23:57.664 phone says: link behind (frame write took 77 ms): bitrate now 2000 kbps, skipping to next key frame
02:24:09.098 stats: 154 KB/s, 55 packets, 50 video frames in, 50 decoded, ... decode+copy avg 60.5 ms
```

### Root causes

- **4K decoding uses the CPU.** `pc/companion/decoder.cpp:28` asks for a synchronous MFT and not
  specifically a hardware one, and doesn't connect it to the GPU (Direct3D). Decoding plus a 12 MB NV12
  copy per 4K frame leaves no headroom.
- **Decoding runs on the USB event thread**, so any decode hiccup holds back the phone's writes.
- **The phone's bitrate control only measures how long a write takes.** It can't tell a full link from a
  busy PC, so it lowers the bitrate even when that can't help.

### Fixed before commit: the `linkCeiling` trap

The `linkCeiling` cap in `WebcamService.kt` (90% of the rate where a stall happened) could fall below the
2 Mbps floor, pinning the stream at the worst quality for the rest of the session. Fixed in 1.3.2 before it
was committed: the cap is never below 4 Mbps (twice the floor) and expires 30 s after the last stall.

Partly helped in 1.3.2: the PC now queues 16 reads (256 KB) instead of 4, so a slow decode stalls the phone
less often. The fixes below are still open.

### Suggested fixes (in order of impact)

1. **Decode on a separate thread on the PC.** The USB callback would only collect complete frames. If the
   decoder falls behind, the PC skips ahead to the next key frame itself, so the USB link never stalls and
   the phone keeps its bitrate.
2. **Use the GPU decoder for 4K:** a hardware MFT connected to Direct3D 11. This should bring decoding well
   under 33 ms.
3. **Make the phone's bitrate control smarter:**
   - Don't lower the bitrate when the data rate is already far below what the link has carried before;
     the bottleneck is elsewhere.
   - Make the `linkCeiling` cap expire after a while, and never let it go below the floor.
   - Recover faster after a stall burst ends.
