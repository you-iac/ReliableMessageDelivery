const assert = require('node:assert/strict');
const { WebSocket } = require('ws');

const baseUrl = process.env.RMD_BASE_URL || 'http://127.0.0.1:6780';
const timeoutMs = Number(process.env.RMD_TEST_TIMEOUT_MS || 10_000);

async function post(path, body) {
  const response = await fetch(new URL(path, baseUrl), {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const data = await response.json();
  assert.equal(response.ok, true, `${path} failed: ${JSON.stringify(data)}`);
  assert.equal(data.ok, true, `${path} returned an unsuccessful result`);
  return data;
}

function waitForMessage(ws, predicate, description) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      ws.off('message', onMessage);
      reject(new Error(`timed out waiting for ${description}`));
    }, timeoutMs);

    function onMessage(raw) {
      let message;
      try {
        message = JSON.parse(raw.toString());
      } catch {
        return;
      }
      if (!predicate(message)) {
        return;
      }
      clearTimeout(timer);
      ws.off('message', onMessage);
      resolve(message);
    }

    ws.on('message', onMessage);
  });
}

async function openSession(token, uid) {
  const wsUrl = new URL('/ws', baseUrl);
  wsUrl.protocol = wsUrl.protocol === 'https:' ? 'wss:' : 'ws:';
  wsUrl.searchParams.set('token', token);

  const ws = new WebSocket(wsUrl);
  const online = await waitForMessage(
    ws,
    (message) => message.type === 'online',
    `uid ${uid} to become online`,
  );
  assert.equal(online.uid, uid);
  return ws;
}

async function main() {
  const response = await fetch(baseUrl);
  assert.equal(response.status, 200, 'gateway root should return HTTP 200');

  const seed = (Date.now() % 800_000_000) + 100_000_000;
  const uidA = seed;
  const uidB = seed + 1;
  const passwordA = String(uidA);
  const passwordB = String(uidB);

  await post('/api/register', { user_id: uidA, password: passwordA });
  await post('/api/register', { user_id: uidB, password: passwordB });
  const loginA = await post('/api/login', { user_id: uidA, password: passwordA });
  const loginB = await post('/api/login', { user_id: uidB, password: passwordB });

  const wsA = await openSession(loginA.token, uidA);
  const wsB = await openSession(loginB.token, uidB);

  try {
    const clientMsgId = `smoke-${Date.now()}`;
    const content = `release smoke ${clientMsgId}`;
    const ackPromise = waitForMessage(
      wsA,
      (message) => message.type === 'send_ack' && message.client_msg_id === clientMsgId,
      'sender acknowledgement',
    );
    const pushPromise = waitForMessage(
      wsB,
      (message) => message.type === 'message' && message.content === content,
      'receiver message push',
    );

    wsA.send(JSON.stringify({
      type: 'send_message',
      to_uid: uidB,
      content,
      client_msg_id: clientMsgId,
    }));

    const [ack, push] = await Promise.all([ackPromise, pushPromise]);
    assert.equal(ack.ok, true);
    assert.ok(Number(ack.msg_id) > 0);
    assert.equal(push.msg_id, ack.msg_id);
    assert.equal(push.from_uid, uidA);
    assert.equal(push.to_uid, uidB);

    wsB.send(JSON.stringify({ type: 'message_ack', msg_id: push.msg_id }));
    console.log(`E2E smoke passed: msg_id=${push.msg_id}, ${uidA}->${uidB}`);
  } finally {
    wsA.close();
    wsB.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
