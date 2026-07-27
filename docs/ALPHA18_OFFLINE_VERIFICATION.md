# Alpha.18 offline verification

Version: `0.1.0-alpha.18`

This pass verifies the launch-handoff, injection-notification, and native-exit
changes without claiming the remaining PS5 browser/SystemService behavior was
observed on hardware.

## Results

| Area | Evidence | Result |
| --- | --- | --- |
| Host regression suite | Release build; 13/13 CTest tests pass | Pass |
| Browser handoff | Close/self-close/history retries and Circle fallback required by structural test | Pass |
| Injection notification | Native notification request occurs only after tile/listener setup succeeds | Pass |
| Native exit | Player cleanup is followed by active-BigApp lookup, `PSMC00001` title verification, and SystemService termination | Pass by source/target analysis |
| PS5 static analysis | Changed launcher and player units analyzed through SDK wrappers | Pass |
| PS5 compile/link | Notification and SystemService imports link in the Release payload | Pass |
| Standalone structure | FreeBSD x86-64 DYN ELF; standalone verifier passes | Pass |
| Local hygiene | Diff check and active-source secret/IP/unsafe-string scan | Pass |

The target symbols `sceKernelSendNotificationRequest`,
`sceSystemServiceGetAppIdOfRunningBigApp`,
`sceSystemServiceGetAppTitleId`, and
`sceSystemServiceKillApp` are present as expected imports in the built PS5
artifacts.

## Artifacts

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer-standalone.elf` | 17,640,792 | `c5b3193a80b601648fd81c414508b23aabf7c866a110d937ff68e53ae68dd183` |
| `BFplayer-standalone.zip` | 17,189,107 | `101ea501bb2bedc317a029b983e1736d68ac268e3e0f590e0155b46cc866bd38` |
| Embedded stripped player | 36,177,808 | `829a599e654be3bdedc6fb660c9d8947003288d74dfe6d564c622a1bef90db68` |

The hardware gate is to confirm the toast appears after injection, the launch
page closes automatically or dismisses with Circle, and Exit Media Center
returns to PlayStation Home instead of leaving a black surface.
