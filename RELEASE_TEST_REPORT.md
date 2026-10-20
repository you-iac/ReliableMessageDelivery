# Release candidate test report

- Date: 2026-10-21
- Branch: `release/0.1.0`
- Candidate commit: `7fcb7b9`
- Base commit from `develop`: `805834a`
- Environment: Docker Compose on Linux

## Automated results

| Check | Result | Evidence |
| --- | --- | --- |
| Compose configuration | Passed | `docker compose config --quiet` |
| Patch formatting | Passed | `git diff --check` |
| C++ build | Passed | Server and client Release targets built in Docker |
| C++ protocol test | Passed | CTest `codec_test`; single, partial, combined, trailing partial, and invalid frames |
| Gateway TypeScript build | Passed | `tsc -p tsconfig.json` during `npm test` |
| Gateway authentication tests | Passed | 4/4 Node tests |
| Initial HTTP/WebSocket smoke | Passed | Registration, login, token-authenticated sessions, push, sender ACK, receiver ACK; `msg_id=1` |
| Redis message state | Passed | `rmd:msg:1` reached `Acked`; `retry_count=0` |
| IMServer restart recovery | Passed | End-to-end smoke repeated successfully; `msg_id=2` |
| Redis restart recovery | Passed | End-to-end smoke repeated successfully; `msg_id=3` |

## Known limitations and follow-up

- Authentication users and tokens remain in Gateway process memory. Restarting the
  Gateway loses them; persistent authentication is outside this demo release.
- `npm audit --omit=dev` reports one moderate direct dependency finding for
  `protobufjs 8.6.4` (GHSA-jfj6-75fj-8934 and GHSA-j3f2-48v5-ccww). The current
  application loads a controlled local `.proto` file rather than user-supplied
  schema text, but the dependency should be upgraded and the suite rerun before
  a production release.
- Browser-based offline delivery and duplicate-render behavior still require the
  manual checks listed in `TESTING.md` before tagging `v0.1.0`.
- Redis reports that host `vm.overcommit_memory` is disabled. It did not block
  this test run, but the production host should set it to `1` and persist the
  setting before release.

## Release decision

The implemented automated checks pass. This commit is suitable as an initial
release candidate, not yet as the final `v0.1.0`, until the remaining manual
checks and dependency decision are completed.
