# Alpha.15 Hardware Import Probe

On 2026-07-26, a small diagnostic payload linked the exact alpha.15
`bulk_import.cpp`, `library_scanner.cpp`, and `media_sources.cpp` production
objects and ran `discover_bulk_media_sources("/mnt/usb0")` on the target PS5.

The probe completed successfully:

| Result | Value |
| --- | ---: |
| Fatal error | 0 |
| Canceled | No |
| Direct and recursive entries checked | 6 |
| Movie sources found | 1 |
| TV-folder sources found | 1 |
| Unreadable entries | 0 |
| Skipped symlinks | 0 |
| Skipped devices | 0 |
| Progress callbacks | 8 |

The returned sources were the loose MKV at the USB root and its immediate
child TV folder. This proves that the production discovery implementation can
classify the real removable-storage layout that previously produced no
sources.

This probe does not prove the SDL controller path, background-thread
presentation, cancellation gesture, database commit, or post-import library
render. Those remain the focused alpha.15 controller acceptance test.
