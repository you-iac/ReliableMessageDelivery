const state = {
  uid: null,
  token: '',
  sessionId: '',
  ws: null,
  connected: false,
  activePeerUid: null,
  conversations: new Map(),
  pendingByClientMsgId: new Map(),
};

const MESSAGE_PAGE_SIZE = 100;
const LOAD_OLDER_THRESHOLD_PX = 24;

const els = {
  authView: document.querySelector('#auth-view'),
  chatView: document.querySelector('#chat-view'),
  loginForm: document.querySelector('#login-form'),
  loginUserId: document.querySelector('#login-user-id'),
  loginPassword: document.querySelector('#login-password'),
  loginButton: document.querySelector('#login-button'),
  loginStatus: document.querySelector('#login-status'),
  registerForm: document.querySelector('#register-form'),
  registerUserId: document.querySelector('#register-user-id'),
  registerPassword: document.querySelector('#register-password'),
  registerConfirmPassword: document.querySelector('#register-confirm-password'),
  registerButton: document.querySelector('#register-button'),
  registerStatus: document.querySelector('#register-status'),
  showRegisterButton: document.querySelector('#show-register-button'),
  showLoginButton: document.querySelector('#show-login-button'),
  logoutButton: document.querySelector('#logout-button'),
  currentUserTitle: document.querySelector('#current-user-title'),
  chatLoginStatus: document.querySelector('#chat-login-status'),
  wsStatus: document.querySelector('#ws-status'),
  sessionId: document.querySelector('#session-id'),
  newChatForm: document.querySelector('#new-chat-form'),
  newPeerId: document.querySelector('#new-peer-id'),
  conversationList: document.querySelector('#conversation-list'),
  conversationTemplate: document.querySelector('#conversation-template'),
  conversationTitle: document.querySelector('#conversation-title'),
  conversationSubtitle: document.querySelector('#conversation-subtitle'),
  clearButton: document.querySelector('#clear-button'),
  emptyChat: document.querySelector('#empty-chat'),
  messageList: document.querySelector('#message-list'),
  messageTemplate: document.querySelector('#message-template'),
  sendForm: document.querySelector('#send-form'),
  messageInput: document.querySelector('#message-input'),
  sendButton: document.querySelector('#send-button'),
};

els.showRegisterButton.addEventListener('click', () => setAuthMode('register'));
els.showLoginButton.addEventListener('click', () => setAuthMode('login'));

els.loginForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  await login();
});

els.registerForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  await register();
});

els.logoutButton.addEventListener('click', () => logout());

els.newChatForm.addEventListener('submit', (event) => {
  event.preventDefault();
  const peerUid = parsePositiveUid(els.newPeerId.value);
  if (!peerUid) {
    addSystemMessageToActive('目标 UID 必须是正整数');
    return;
  }
  if (peerUid === state.uid) {
    addConversation(peerUid);
    addSystemMessageToConversation(peerUid, '这是你自己的 UID，第一版可以保留用于本地测试。');
  }
  selectConversation(peerUid);
  els.newPeerId.value = '';
});

els.sendForm.addEventListener('submit', (event) => {
  event.preventDefault();
  sendMessage();
});

els.clearButton.addEventListener('click', () => {
  const conversation = getActiveConversation();
  if (!conversation) {
    return;
  }
  conversation.messages = [];
  conversation.unread = 0;
  conversation.visibleMessageCount = 0;
  renderMessages();
  renderConversationList();
});

els.messageInput.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    sendMessage();
  }
});

els.messageList.addEventListener('scroll', () => {
  if (els.messageList.scrollTop <= LOAD_OLDER_THRESHOLD_PX) {
    loadOlderMessages();
  }
});

async function login() {
  const userId = parsePositiveUid(els.loginUserId.value);
  const password = els.loginPassword.value;

  if (!userId) {
    setLoginStatus('用户 ID 必须是正整数', 'bad');
    return;
  }

  setLoginStatus('登录中', 'pending');
  els.loginButton.disabled = true;

  try {
    const response = await fetch('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ user_id: userId, password }),
    });

    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok !== true || !data.token) {
      throw new Error(data.reason || `登录失败: HTTP ${response.status}`);
    }

    resetChatState();
    state.uid = Number(data.uid || userId);
    state.token = data.token;
    setLoginStatus(`已登录 uid=${state.uid}`, 'good');
    showChatView();
    connectWebSocket();
  } catch (error) {
    closeWebSocket();
    setLoginStatus(error.message || '登录失败', 'bad');
  } finally {
    els.loginButton.disabled = false;
  }
}

async function register() {
  const userId = parsePositiveUid(els.registerUserId.value);
  const password = els.registerPassword.value;
  const confirmPassword = els.registerConfirmPassword.value;

  if (!userId) {
    setRegisterStatus('用户 ID 必须是正整数', 'bad');
    return;
  }
  if (!password) {
    setRegisterStatus('请输入密码', 'bad');
    return;
  }
  if (password !== confirmPassword) {
    setRegisterStatus('两次输入的密码不一致', 'bad');
    return;
  }

  setRegisterStatus('注册中', 'pending');
  els.registerButton.disabled = true;

  try {
    const response = await fetch('/api/register', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ user_id: userId, password }),
    });

    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok !== true) {
      throw new Error(data.reason || `注册失败: HTTP ${response.status}`);
    }

    setRegisterStatus('注册成功，请登录', 'good');
    els.loginUserId.value = String(userId);
    els.loginPassword.value = password;
    setAuthMode('login');
    setLoginStatus('注册成功，请登录', 'good');
  } catch (error) {
    setRegisterStatus(error.message || '注册失败', 'bad');
  } finally {
    els.registerButton.disabled = false;
  }
}

function connectWebSocket() {
  closeWebSocket();

  const wsUrl = buildWebSocketUrl(state.token);
  const ws = new WebSocket(wsUrl);
  state.ws = ws;
  state.connected = false;
  setWsStatus('连接中', 'pending');
  setComposerEnabled(false);

  ws.addEventListener('open', () => {
    setWsStatus('等待 IMServer 上线确认', 'pending');
  });

  ws.addEventListener('message', (event) => {
    handleServerMessage(event.data);
  });

  ws.addEventListener('close', () => {
    if (state.ws === ws) {
      state.connected = false;
      setWsStatus('已断开', 'bad');
      setComposerEnabled(false);
      updateHeader();
    }
  });

  ws.addEventListener('error', () => {
    if (state.ws === ws) {
      setWsStatus('连接错误', 'bad');
    }
  });
}

function closeWebSocket() {
  if (state.ws) {
    state.ws.close();
    state.ws = null;
  }
  state.connected = false;
  state.sessionId = '';
  setComposerEnabled(false);
}

function handleServerMessage(raw) {
  let msg;
  try {
    msg = JSON.parse(raw);
  } catch (error) {
    addSystemMessageToActive('收到无法解析的消息');
    return;
  }

  if (msg.type === 'online') {
    state.connected = true;
    state.uid = Number(msg.uid || state.uid);
    state.sessionId = msg.session_id || '';
    setWsStatus('在线', 'good');
    updateHeader();
    renderConversationList();
    renderMessages();
    return;
  }

  if (msg.type === 'send_ack') {
    applySendAck(msg);
    return;
  }

  if (msg.type === 'message') {
    appendIncomingMessage(msg);
    sendMessageAck(msg.msg_id);
    return;
  }

  if (msg.type === 'error') {
    addSystemMessageToActive(msg.reason || '服务端返回错误');
    return;
  }

  if (msg.type === 'pong') {
    return;
  }

  addSystemMessageToActive(`未知消息类型: ${msg.type || 'unknown'}`);
}

function sendMessage() {
  const conversation = getActiveConversation();
  if (!conversation) {
    addSystemMessageToActive('请先选择聊天对象');
    return;
  }
  if (!state.connected || !state.ws || state.ws.readyState !== WebSocket.OPEN) {
    addSystemMessageToConversation(conversation.peerUid, 'WebSocket 未在线，无法发送');
    return;
  }

  const content = els.messageInput.value.trim();
  if (!content) {
    return;
  }

  const clientMsgId = makeClientMsgId();
  const localMessage = {
    direction: 'outgoing',
    client_msg_id: clientMsgId,
    msg_id: null,
    from_uid: state.uid,
    to_uid: conversation.peerUid,
    content,
    status: 'sending',
    created_at_ms: Date.now(),
  };

  conversation.messages.push(localMessage);
  conversation.lastMessage = content;
  conversation.lastAtMs = localMessage.created_at_ms;
  resetVisibleMessageWindow(conversation);
  state.pendingByClientMsgId.set(clientMsgId, { peerUid: conversation.peerUid, message: localMessage });
  renderConversationList();
  renderMessages();

  state.ws.send(JSON.stringify({
    type: 'send_message',
    to_uid: conversation.peerUid,
    content,
    client_msg_id: clientMsgId,
  }));

  els.messageInput.value = '';
  els.messageInput.focus();
}

function applySendAck(msg) {
  const pending = state.pendingByClientMsgId.get(msg.client_msg_id);
  if (!pending) {
    addSystemMessageToActive(`收到未匹配的 send_ack: ${msg.client_msg_id || '-'}`);
    return;
  }

  const shouldScrollToBottom = pending.peerUid === state.activePeerUid && isMessageListNearBottom();
  pending.message.msg_id = msg.msg_id || null;
  pending.message.status = msg.ok ? 'sent' : 'failed';
  pending.message.reason = msg.reason || '';
  state.pendingByClientMsgId.delete(msg.client_msg_id);
  renderConversationList();
  renderMessages({ scrollMode: shouldScrollToBottom ? 'bottom' : 'preserve' });
}

function appendIncomingMessage(msg) {
  const fromUid = Number(msg.from_uid);
  const toUid = Number(msg.to_uid || state.uid);
  const peerUid = fromUid === state.uid ? toUid : fromUid;
  const conversation = addConversation(peerUid);
  const isActiveConversation = state.activePeerUid === peerUid;
  const shouldScrollToBottom = isActiveConversation && isMessageListNearBottom();

  conversation.messages.push({
    direction: fromUid === state.uid ? 'outgoing' : 'incoming',
    msg_id: msg.msg_id,
    from_uid: fromUid,
    to_uid: toUid,
    content: msg.content || '',
    status: 'received',
    created_at_ms: Number(msg.server_timestamp_ms || Date.now()),
  });
  conversation.lastMessage = msg.content || '';
  conversation.lastAtMs = Number(msg.server_timestamp_ms || Date.now());
  updateVisibleWindowAfterAppend(conversation, isActiveConversation && !shouldScrollToBottom);

  if (state.activePeerUid !== peerUid) {
    conversation.unread += 1;
  }

  renderConversationList();
  renderMessages({ scrollMode: shouldScrollToBottom ? 'bottom' : 'preserve' });
}

function sendMessageAck(msgId) {
  if (!msgId || !state.ws || state.ws.readyState !== WebSocket.OPEN) {
    return;
  }

  state.ws.send(JSON.stringify({
    type: 'message_ack',
    msg_id: msgId,
  }));
}

function addConversation(peerUid) {
  let conversation = state.conversations.get(peerUid);
  if (!conversation) {
    conversation = {
      peerUid,
      messages: [],
      unread: 0,
      lastMessage: '新会话',
      lastAtMs: Date.now(),
      visibleMessageCount: 0,
    };
    state.conversations.set(peerUid, conversation);
  }
  return conversation;
}

function selectConversation(peerUid) {
  const conversation = addConversation(peerUid);
  state.activePeerUid = peerUid;
  conversation.unread = 0;
  resetVisibleMessageWindow(conversation);
  renderConversationList();
  renderMessages();
  updateHeader();
  setComposerEnabled(state.connected);
  els.messageInput.focus();
}

function getActiveConversation() {
  if (state.activePeerUid === null) {
    return null;
  }
  return state.conversations.get(state.activePeerUid) || null;
}

function addSystemMessageToActive(content) {
  const conversation = getActiveConversation();
  if (!conversation || conversation.peerUid === 0) {
    els.emptyChat.hidden = false;
    els.emptyChat.querySelector('h3').textContent = '系统提示';
    els.emptyChat.querySelector('p').textContent = content;
    return;
  }
  addSystemMessageToConversation(conversation.peerUid, content);
}

function addSystemMessageToConversation(peerUid, content) {
  const conversation = addConversation(peerUid);
  const isActiveConversation = state.activePeerUid === peerUid;
  const shouldScrollToBottom = isActiveConversation && isMessageListNearBottom();

  conversation.messages.push({
    direction: 'system',
    content,
    status: 'notice',
    created_at_ms: Date.now(),
  });
  conversation.lastMessage = content;
  conversation.lastAtMs = Date.now();
  updateVisibleWindowAfterAppend(conversation, isActiveConversation && !shouldScrollToBottom);
  renderConversationList();
  renderMessages({ scrollMode: shouldScrollToBottom ? 'bottom' : 'preserve' });
}

function renderConversationList() {
  els.conversationList.replaceChildren();

  const conversations = Array.from(state.conversations.values())
    .filter((conversation) => conversation.peerUid !== 0)
    .sort((a, b) => b.lastAtMs - a.lastAtMs);

  if (conversations.length === 0) {
    const empty = document.createElement('li');
    empty.className = 'conversation-empty';
    empty.textContent = '暂无会话';
    els.conversationList.appendChild(empty);
    return;
  }

  for (const conversation of conversations) {
    const node = els.conversationTemplate.content.firstElementChild.cloneNode(true);
    const button = node.querySelector('.conversation-item');
    const avatar = node.querySelector('.avatar');
    const name = node.querySelector('.conversation-name');
    const preview = node.querySelector('.conversation-preview');
    const badge = node.querySelector('.conversation-badge');

    button.dataset.peerUid = String(conversation.peerUid);
    button.classList.toggle('active', conversation.peerUid === state.activePeerUid);
    avatar.textContent = String(conversation.peerUid).slice(-2).padStart(2, '0');
    name.textContent = `UID ${conversation.peerUid}`;
    preview.textContent = conversation.lastMessage || '新会话';
    badge.textContent = conversation.unread > 0 ? String(conversation.unread) : '';
    badge.hidden = conversation.unread === 0;

    button.addEventListener('click', () => selectConversation(conversation.peerUid));
    els.conversationList.appendChild(node);
  }
}

function loadOlderMessages() {
  const conversation = getActiveConversation();
  if (!conversation || conversation.peerUid === 0 || els.messageList.hidden) {
    return;
  }

  const visibleCount = getVisibleMessageCount(conversation);
  if (visibleCount >= conversation.messages.length) {
    return;
  }

  const previousScrollHeight = els.messageList.scrollHeight;
  const previousScrollTop = els.messageList.scrollTop;
  conversation.visibleMessageCount = Math.min(conversation.messages.length, visibleCount + MESSAGE_PAGE_SIZE);
  renderMessages({
    scrollMode: 'preserve-prepend',
    previousScrollHeight,
    previousScrollTop,
  });
}

function resetVisibleMessageWindow(conversation) {
  conversation.visibleMessageCount = Math.min(conversation.messages.length, MESSAGE_PAGE_SIZE);
}

function updateVisibleWindowAfterAppend(conversation, keepTopMessageVisible) {
  const visibleCount = getVisibleMessageCount(conversation);
  if (keepTopMessageVisible) {
    conversation.visibleMessageCount = Math.min(conversation.messages.length, visibleCount + 1);
    return;
  }
  if (visibleCount === 0) {
    resetVisibleMessageWindow(conversation);
  }
}

function getVisibleMessageCount(conversation) {
  if (Number.isInteger(conversation.visibleMessageCount)) {
    return Math.min(conversation.messages.length, Math.max(0, conversation.visibleMessageCount));
  }
  return Math.min(conversation.messages.length, MESSAGE_PAGE_SIZE);
}

function getVisibleMessages(conversation) {
  const visibleCount = getVisibleMessageCount(conversation);
  const startIndex = Math.max(0, conversation.messages.length - visibleCount);
  return conversation.messages.slice(startIndex);
}

function isMessageListNearBottom() {
  if (els.messageList.hidden) {
    return true;
  }
  return els.messageList.scrollHeight - els.messageList.scrollTop - els.messageList.clientHeight <= 32;
}

function renderMessages(options = {}) {
  const conversation = getActiveConversation();
  const previousScrollHeight = options.previousScrollHeight ?? els.messageList.scrollHeight;
  const previousScrollTop = options.previousScrollTop ?? els.messageList.scrollTop;
  els.messageList.replaceChildren();

  if (!conversation || conversation.peerUid === 0) {
    els.emptyChat.hidden = false;
    els.emptyChat.querySelector('h3').textContent = '还没有选择会话';
    els.emptyChat.querySelector('p').textContent = '在左侧输入目标 UID 开始聊天，或等待别人给你发消息。';
    els.messageList.hidden = true;
    setComposerEnabled(false);
    updateHeader();
    return;
  }

  els.emptyChat.hidden = conversation.messages.length > 0;
  els.messageList.hidden = false;

  for (const message of getVisibleMessages(conversation)) {
    const node = els.messageTemplate.content.firstElementChild.cloneNode(true);
    node.classList.add(message.direction);

    const meta = node.querySelector('.message-meta');
    const content = node.querySelector('.message-content');
    const status = node.querySelector('.message-status');

    meta.textContent = buildMessageMeta(message);
    content.textContent = message.content;
    status.textContent = buildMessageStatus(message);

    els.messageList.appendChild(node);
  }

  setComposerEnabled(state.connected);
  updateHeader();

  if (options.scrollMode === 'preserve-prepend') {
    els.messageList.scrollTop = els.messageList.scrollHeight - previousScrollHeight + previousScrollTop;
  } else if (options.scrollMode === 'preserve') {
    els.messageList.scrollTop = previousScrollTop;
  } else {
    els.messageList.scrollTop = els.messageList.scrollHeight;
  }
}

function buildMessageMeta(message) {
  const time = new Date(message.created_at_ms || Date.now()).toLocaleTimeString();
  if (message.direction === 'outgoing') {
    return `我 -> uid ${message.to_uid} · ${time}`;
  }
  if (message.direction === 'incoming') {
    return `uid ${message.from_uid} -> 我 · ${time}`;
  }
  return `系统 · ${time}`;
}

function buildMessageStatus(message) {
  if (message.direction === 'system') {
    return '';
  }
  if (message.status === 'sending') {
    return '发送中';
  }
  if (message.status === 'sent') {
    return message.msg_id ? `已被服务器接收 #${message.msg_id}` : '已被服务器接收';
  }
  if (message.status === 'failed') {
    return message.reason ? `发送失败: ${message.reason}` : '发送失败';
  }
  if (message.status === 'received') {
    return message.msg_id ? `已 ACK #${message.msg_id}` : '已 ACK';
  }
  return '';
}

function showChatView() {
  els.authView.classList.add('hidden');
  els.chatView.classList.remove('hidden');
  setChatLoginStatus(`已登录 uid=${state.uid}`, 'good');
  updateHeader();
  renderConversationList();
  renderMessages();
}

function logout() {
  closeWebSocket();
  resetChatState();
  els.chatView.classList.add('hidden');
  els.authView.classList.remove('hidden');
  setAuthMode('login');
  setLoginStatus('已退出登录', 'idle');
  setWsStatus('未连接', 'idle');
}

function resetChatState() {
  state.uid = null;
  state.token = '';
  state.sessionId = '';
  state.connected = false;
  state.activePeerUid = null;
  state.conversations = new Map();
  state.pendingByClientMsgId = new Map();
}

function updateHeader() {
  els.currentUserTitle.textContent = state.uid ? `UID ${state.uid}` : '未登录';
  els.sessionId.textContent = state.sessionId || '-';

  const conversation = getActiveConversation();
  if (conversation && conversation.peerUid !== 0) {
    els.conversationTitle.textContent = `UID ${conversation.peerUid}`;
    els.conversationSubtitle.textContent = state.connected ? '在线，可发送消息' : '等待 WebSocket 上线确认';
  } else {
    els.conversationTitle.textContent = '选择一个聊天对象';
    els.conversationSubtitle.textContent = '左侧新建或选择会话后开始发送消息';
  }
}

function setAuthMode(mode) {
  els.authView.classList.toggle('mode-register', mode === 'register');
  els.authView.classList.toggle('mode-login', mode !== 'register');
}

function buildWebSocketUrl(token) {
  const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const url = new URL(`${scheme}//${window.location.host}/ws`);
  url.searchParams.set('token', token);
  return url.toString();
}

function parsePositiveUid(value) {
  const uid = Number(value);
  return Number.isInteger(uid) && uid > 0 ? uid : null;
}

function setComposerEnabled(enabled) {
  const hasConversation = Boolean(getActiveConversation() && state.activePeerUid !== 0);
  els.messageInput.disabled = !(enabled && hasConversation);
  els.sendButton.disabled = !(enabled && hasConversation);
}

function setLoginStatus(text, tone) {
  setStatus(els.loginStatus, text, tone);
}

function setRegisterStatus(text, tone) {
  setStatus(els.registerStatus, text, tone);
}

function setChatLoginStatus(text, tone) {
  setStatus(els.chatLoginStatus, text, tone);
}

function setWsStatus(text, tone) {
  setStatus(els.wsStatus, text, tone);
}

function setStatus(element, text, tone) {
  element.textContent = text;
  element.dataset.tone = tone || 'idle';
}

function makeClientMsgId() {
  const random = Math.random().toString(36).slice(2, 10);
  return `browser-${Date.now()}-${random}`;
}

setAuthMode('login');
setLoginStatus('请输入账号信息', 'idle');
setRegisterStatus('默认密码: rmd-demo', 'idle');
setChatLoginStatus('未登录', 'idle');
setWsStatus('未连接', 'idle');
renderConversationList();
renderMessages();
