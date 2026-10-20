# Testing and release flow

The long-lived branches are `develop` and `main`. Create a release candidate
from `develop`, stabilize it without adding features, and merge it into both
`main` and `develop` after all required checks pass.

```bash
git switch develop
git pull --ff-only origin develop
git switch -c release/0.1.0
```

## Automated checks

Gateway unit tests:

```bash
cd web_gateway
npm ci
npm test
```

C++ protocol tests are run by CTest during the server image build:

```bash
docker compose build server gateway
```

Start the release candidate and run the HTTP/WebSocket end-to-end smoke test:

```bash
docker compose up -d --no-build --wait --wait-timeout 60
cd web_gateway
npm run test:e2e
```

The smoke test verifies the home page, registration, login, token-authenticated
WebSocket sessions, sender ACK, receiver push, and receiver message ACK.

## Manual release checks

- Redis is healthy and the Server and Gateway remain running without restarts.
- Two browser users can exchange messages in both directions.
- A message sent to an offline user is delivered after the user logs in.
- A delivered and acknowledged message is not shown twice.
- Server and Redis restarts preserve stored messages and restore service.
- Gateway restart behavior is documented: authentication is currently in
  memory and is a known limitation of the demo release.
- Logs contain no crashes, repeated connection failures, or invalid frames.

After the checks pass, merge `release/0.1.0` into `main`, tag the merge commit
as `v0.1.0`, and merge the release fixes back into `develop`.
