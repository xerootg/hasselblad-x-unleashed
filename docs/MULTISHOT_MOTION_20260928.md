# Stock multishot displacement validation (Auto4 / Auto6 / Auto16)

## Scope and conclusions

On 2026-9-28, with authorization, the shutter was triggered remotely in normal single-shot mode to complete one full sequence for each of the three stock multishot modes (persistent Pixel Shift injection was stopped; all captures used the same boot session and camera-service process). This record establishes the **native command sequence and measured image displacement**; it does not certify the persistent GUI / gateway, composite output, actuator precision, or effective pixel count.

| Stock mode | Position index | Exposure ID | Original files (RAW + HEIF) |
| --- | --- | --- | --- |
| Auto4 = 1 | 0–3 | 279707655–279707658 | B0001063–B0001066 |
| Auto6 = 2 | 0–5 | 279707659–279707664 | B0001067–B0001072 |
| Auto16 = 3 | 0–15 | 279707665–279707680 | B0001073–B0001088 |

Each capture position corresponds to a unique ExposureId, one accepted position call, two axis requests (both with successful responses), and one displacement readback. The reset call at the end of the session is classified separately and is not counted as an exposure. File association is based on exclusive sequential shooting, consecutive file numbering, and consistent file counts and EXIF timestamps; this record does not directly map each stored filename to a specific ExposureId. Hashes of all 26 downloaded RAW files match those on the camera.

## Confirmed native command sequence

Coordinates are stock displacement-command parameters, in sensor-pixel steps, matching the position table in the firmware used.

```text
Auto4:
0 (0,0)   1 (0,1)   2 (1,0)   3 (1,1)

Auto6:
0 (0,0)   1 (0,1)   2 (1,0)   3 (1,1)
4 (0.5,0.5)        5 (0.5,1.5)

Auto16:
 0 (0,0)       1 (0,0.5)     2 (0,1)       3 (0,1.5)
 4 (0.5,1.5)   5 (0.5,1)    6 (0.5,0.5)   7 (0.5,0)
 8 (1,0)       9 (1,0.5)    10 (1,1)      11 (1,1.5)
12 (1.5,1.5)  13 (1.5,1)   14 (1.5,0.5)  15 (1.5,0)
```

Auto4 does move the sensor: it traverses integer offsets to obtain measured R, G1, G2, and B samples at the same scene location, but does not include the half-pixel sampling of Auto6 / Auto16. Auto16 follows a serpentine path in half-pixel steps; it is not four repetitions of Auto4.

**Do not copy these motor-axis signs directly as composite sampling offsets.** Under the RAW registration convention used here, observed image displacement is approximately `(X, -Y)`; the inverse resampling transform uses different signs again. The Bayer origin of the original on-disk RAW must also be distinguished from the active crop in memory.

## Image evidence and limitations

The 26 original 3FR files decode to 11904 × 8842 Bayer arrays. Measurement method: a fixed 3 × 3 spatial grid, four independent Bayer planes per region, blind fitting initialized at zero, and no hints from expected positions. Four of the nine regions passed the texture / convergence / correlation criteria in all measured frames (16 Bayer-plane samples); the other regions remain in the data rather than being hidden. The first frame of each group is the reference, not an independent zero-displacement measurement.

| Mode | Median image-to-target distance | Maximum image-to-target distance | Maximum readback-to-target distance |
| --- | ---: | ---: | ---: |
| Auto4 | 0.154 px | 0.205 px | 0.184 px |
| Auto6 | 0.289 px | 0.361 px | 0.352 px |
| Auto16 | 0.180 px | 0.388 px | 0.400 px |

Units are original sensor pixels. Image differences confirm displacement consistent with the commanded traversal, but do not establish that nominal positions were reached exactly. The scene was an indoor chair: ISO 6400, 1/100 s, f/4.8, 100 mm. Noise, local texture, scene motion, optical effects, and registration uncertainty have not been separated from actuator error. Readback is an asynchronous sample taken near ExposureId allocation, not a position measurement throughout the exposure.

