# B0001053 / B0001054 Same-Size Detail Comparison

2026-10-16 · The 3FR and HEIC files for both photos were downloaded · SHA-256 hashes for all four files match the camera (see `camera-sha256.txt` / `local-sha256.txt`) · Original files were not modified · Original 3FR / HEIC files and intermediate data are not distributed with the repository; only compressed comparison images are retained.

> **Conclusion: 1053 has about four times as many output pixels as 1054, but this sample does not show a clear gain in real detail.**
> In the same regions, 1053 has finer grain and smoother edges, with some fabric boundaries appearing softer; no consistently new, discernible fine fibers are visible. Applying the same processing to the RAW green channel also shows no clear advantage. Therefore, this result cannot be called “verified 400-megapixel resolving power,” nor can two finished images alone establish that the sensor did not shift.

## Original files

| Item | 1053: Pixel Shift merge | 1054: Normal single shot |
| --- | --- | --- |
| HEIC dimensions | 23310 × 17482 (407.51 MP) | 11656 × 8742 (101.90 MP) |
| RAW storage | 23310×17482, 16-bit single-channel CFA | 11904×8842, 16-bit single-channel CFA (including sensor border) |
| ISO / focal length | 6400 / 100 mm | 6400 / 100 mm |
| Recorded exposure time / aperture | 0.00929 s / f4.74 | 0.01000 s / f4.8 |
| Capture time | 20:13:07 | 20:17:27 |
| 3FR | 819,400,704 bytes | 213,864,448 bytes |
| HEIC | 132,702,208 bytes | 56,125,440 bytes |

## 400% / 800% comparison

**Left: 1054 normal single shot; right: 1053 Pixel Shift.** Both images were first registered to the same coordinates and frame area in 1053, then enlarged together. 1054 was resampled once using bicubic interpolation; the 400% / 800% views both use nearest-neighbor interpolation, with no extra sharpening. Percentages here are based on the normalized 400-megapixel grid; the 800% image contains no additional information compared with the 400% image.

Example: back center at 400% (single shot on the left, pixel shift on the right):

![Back center at 400%](back_center_400pct.png)

Locations of the four analysis regions:

![Region overview](regions_overview.jpg)

All comparison images (both finished HEIC images and RAW green channel):

| Region | 400% | 800% | 100% context |
| --- | --- | --- | --- |
| Back center | [HEIC](back_center_400pct.png) · [RAW](back_center_raw_400pct.png) | [HEIC](back_center_800pct.png) · [RAW](back_center_raw_800pct.png) | [HEIC](back_center_context.png) · [RAW](back_center_raw_context.png) |
| Back left | [HEIC](back_left_400pct.png) · [RAW](back_left_raw_400pct.png) | [HEIC](back_left_800pct.png) · [RAW](back_left_raw_800pct.png) | [HEIC](back_left_context.png) · [RAW](back_left_raw_context.png) |
| Seat center | [HEIC](seat_center_400pct.png) · [RAW](seat_center_raw_400pct.png) | [HEIC](seat_center_800pct.png) · [RAW](seat_center_raw_800pct.png) | [HEIC](seat_center_context.png) · [RAW](seat_center_raw_context.png) |
| Back edge | [HEIC](back_border_400pct.png) · [RAW](back_border_raw_400pct.png) | [HEIC](back_border_800pct.png) · [RAW](back_border_raw_800pct.png) | [HEIC](back_border_context.png) · [RAW](back_border_raw_context.png) |

When opening PNGs directly, set the viewer to 100% to avoid additional scaling.

## Registration and observations

- HEIC: 4,330 RANSAC inliers, overall scale about 1.999933, median residual about 0.448 pixels on the 400-megapixel image grid; each of the four regions then received a translational correction of less than 1 pixel. No local stretching, sharpening, or generated detail was applied.
- HEIC grayscale correlation across the four regions is about 0.9935–0.9949. High correlation only indicates that the same textures are aligned; it is not a measure of resolving power or shift success.
- RAW: independently registered with 3,658 inliers and a median residual of about 0.363 pixels on the 400-megapixel image grid. Only the green channel is compared: green samples are read according to each CFA phase, the black level identified by LibRaw is subtracted, missing positions are reconstructed using the same bilinear green interpolation, and the linear range and γ=1/2.2 are normalized. No white balance, color matrix, sharpening, or denoising is applied. This is a detail diagnostic, not a complete RAW development.
- The HEIC comparison uses the same library to decode 10-bit data for display as 8-bit RGB; RAW is computed from 16-bit samples and then displayed as an 8-bit PNG using the same curve. This is not a pixel-value precision test.
- In the selected regions, the average HEIC brightness of 1053 is about 5–6% lower than 1054. The display preserves this difference and does not stretch contrast per image. Exposure times and development parameters differ, so brightness differences should not be treated as detail gains.

## What this comparison can establish

It can establish the output file dimensions, the appearance of the same scene, and the RAW green-channel detail after removing differences in post-processing sharpening and white balance. It cannot determine the sensor's step size, whether the six frames landed at the intended phases, the lens's actual resolving limit, the effective pixel count, MTF, or a resolving-power improvement factor. ISO 6400, variation in fabric depth, focus or small movements between the two captures, and different preprocessing can also affect the result.

For further diagnosis, inspect the actual relative shifts of these six frames, their sequence and CFA positions, and edges before and after merging. The two final files alone are insufficient to attribute the issue uniquely to the motor, merging, or development.

## Interactive viewer

[`REPORT.html`](REPORT.html) in this directory is a local interactive version of the same content, with dropdowns for region / input / zoom and a fit-to-page toggle. GitHub does not render HTML files; download the entire directory and open it in a browser.
