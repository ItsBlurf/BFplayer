# Beta.1 offline verification

Version: `0.1.0-beta.1`

This pass covers the first public beta, including the final library-sort
presentation change and the alpha.19 sorting/reinjection work it carries
forward.

## Results

| Area | Evidence | Result |
| --- | --- | --- |
| Host regression suite | Release build; 13/13 CTest tests pass | Pass |
| Sort presentation | Top status retains the mode; footer renders only `R3 Sort` | Pass by source inspection |
| TV-show sort | Collapsed shows sort by displayed show name | Pass by source/target analysis |
| Sort selection | Selected series identity survives sort changes | Pass by source/target analysis |
| Clean reinjection | Exclusive lifetime lock and graceful service handover | Pass by source/target analysis |
| Idempotent update | Managed files are byte-compared and atomically replaced only when changed | Pass by source/target analysis |
| PS5 compile/link | SDK-wrapper Release build | Pass |
| Standalone structure | Embedded-player verifier | Pass |

## Artifacts

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `bfplayer-standalone.elf` | 17,657,480 | `210afa7fedd5ebe6fd3f046f3d5b3f6a5c178a5a9dcb4124ba06867a8034c9b6` |
| `BFplayer-standalone.zip` | 17,194,154 | `4ab3e83f8d202fe8de054a276e9265be744ba38bbe0bcbc314a19c16d9e34a34` |
| Embedded stripped player | 36,194,192 | `22eb028e7af44fd9f98ace5d71019ffbbdd56b06e1c6d8a48b66b3dc730e7ae0` |
