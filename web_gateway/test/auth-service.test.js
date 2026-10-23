const assert = require('node:assert/strict');
const test = require('node:test');

const { AuthService } = require('../dist/AuthService');

test('register and login issue a verifiable token', () => {
  const auth = new AuthService(60_000);

  assert.deepEqual(auth.register(1001, '1001'), { ok: true, uid: 1001 });
  const login = auth.login(1001, '1001');

  assert.equal(login.ok, true);
  assert.equal(login.uid, 1001);
  assert.match(login.token, /^[0-9a-f]{48}$/);
  assert.deepEqual(auth.verifyToken(login.token), {
    ok: true,
    uid: 1001,
    token: login.token,
  });
});

test('register rejects invalid and duplicate credentials', () => {
  const auth = new AuthService(60_000);

  assert.equal(auth.register(0, '0').ok, false);
  assert.equal(auth.register(1002, '').ok, false);
  assert.equal(auth.register(1002, 'different').ok, false);
  assert.equal(auth.register(1002, '1002').ok, true);
  assert.equal(auth.register(1002, '1002').ok, false);
});

test('login rejects an incorrect password', () => {
  const auth = new AuthService(60_000);
  auth.register(1003, '1003');

  assert.equal(auth.login(1003, 'wrong').ok, false);
});

test('expired tokens are rejected', () => {
  const auth = new AuthService(-1);
  const login = auth.login(1004, '1004');

  assert.equal(login.ok, true);
  assert.equal(auth.verifyToken(login.token).ok, false);
});
