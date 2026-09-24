const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const HTML_PATH = path.resolve(
  __dirname,
  '..',
  '..',
  'docs',
  'console',
  'read-only-console-prototype.html',
);
const HTML = fs.readFileSync(HTML_PATH, 'utf8');
const SCRIPT = HTML.match(/<script>([\s\S]*?)<\/script>/)[1];

class ClassList {
  constructor() {
    this.values = new Set();
  }

  add(...names) {
    names.forEach((name) => this.values.add(name));
  }

  remove(...names) {
    names.forEach((name) => this.values.delete(name));
  }

  contains(name) {
    return this.values.has(name);
  }
}

class Element {
  constructor(tag) {
    this.tag = tag;
    this.classList = new ClassList();
    this.style = {};
    this.dataset = {};
    this.listeners = Object.create(null);
    this.children = [];
    this.value = '';
    this.disabled = false;
    this.textContent = '';
    this.colSpan = 1;
  }

  addEventListener(type, listener) {
    if (!this.listeners[type]) this.listeners[type] = [];
    this.listeners[type].push(listener);
  }

  appendChild(child) {
    this.children.push(child);
    return child;
  }

  replaceChildren(...children) {
    this.children = children;
  }

  setAttribute(name, value) {
    this[name] = value;
  }

  dispatch(event) {
    (this.listeners[event.type] || []).forEach((listener) => listener(event));
  }
}

function sampleData() {
  return {
    health: { status: 'healthy', mqtt_connected: true },
    devices: [
      {
        id: 'gw-001',
        name: '家庭网关',
        type: 'gateway',
        status: 'online',
        last_seen: '2026-09-24T21:00:00',
        capabilities: { sensors: [], switch: false },
      },
      {
        id: 'zb-82cb',
        name: '客厅节点',
        type: 'sensor_switch',
        status: 'online',
        last_seen: '2026-09-24T21:02:00',
        capabilities: {
          sensors: ['temperature', 'humidity', 'illuminance'],
          switch: true,
        },
      },
      {
        id: 'zb-custom',
        name: '<img src="x" onerror="alert(1)">',
        type: 'custom',
        status: 'offline',
        last_seen: null,
        capabilities: { sensors: ['future_metric'], switch: false },
      },
    ],
    metrics: [
      { device_id: 'zb-82cb', metric: 'temperature', value: 25.4, ts: '2026-09-24T21:01:00' },
      { device_id: 'zb-82cb', metric: 'humidity', value: 61, ts: '2026-09-24T21:01:05' },
      { device_id: 'zb-82cb', metric: 'illuminance', value: 320, ts: '2026-09-24T21:01:10' },
      { device_id: 'zb-82cb', metric: 'on_off', value: 1, ts: '2026-09-24T21:01:15' },
      { device_id: 'zb-custom', metric: 'future_metric', value: 7, ts: '2026-09-24T21:00:00' },
    ],
    commands: [
      { device_id: 'zb-82cb', command: 'rf_set_auto', status: 'acknowledged', created_at: '2026-09-24T20:00:00' },
      { device_id: 'zb-82cb', command: 'turn_on', status: 'sent', created_at: '2026-09-24T20:05:00' },
    ],
  };
}

function loadPage(options = {}) {
  const data = sampleData();
  const elements = {};
  const requests = [];
  const timers = new Map();
  const pending = [];
  let nextTimerId = 1;
  let callCount = 0;
  const pendingCallIndexes = new Set(options.pendingCallIndexes || []);
  const manualResolveCallIndexes = new Set(options.manualResolveCallIndexes || []);

  const responder = options.responder || ((url) => {
    const health = options.health || data.health;
    if (url.includes('/api/health')) return health;
    if (url.includes('/api/devices/') && url.includes('/rf-policy')) {
      const match = url.match(/\/api\/devices\/([^/]+)\/rf-policy/);
      const id = decodeURIComponent(match[1]);
      if (id === 'zb-82cb') {
        return { device_id: id, mode: 'auto', power_dbm: -10, status: 'confirmed' };
      }
      return { device_id: id, mode: null, power_dbm: null, status: 'unknown' };
    }
    if (url.includes('/api/devices')) return data.devices;
    if (url.includes('/api/metrics/latest')) return data.metrics;
    if (url.includes('/api/commands')) return data.commands;
    throw new Error('unmocked url: ' + url);
  });

  const sandbox = {
    console,
    URL,
    AbortController,
    Promise,
    Object,
    Array,
    String,
    Number,
    Error,
    document: {
      getElementById(id) {
        if (!elements[id]) elements[id] = new Element('div');
        return elements[id];
      },
      createElement(tag) {
        return new Element(tag);
      },
    },
    fetch(url, init) {
      const callIndex = ++callCount;
      const entry = { url, init, signal: init.signal, callIndex };
      requests.push(entry);

      return new Promise((resolve, reject) => {
        let settled = false;
        const responseFor = (body) => {
          const status = body.__status || 200;
          return {
            ok: status >= 200 && status < 300,
            status,
            json: async () => body,
          };
        };

        const pendingEntry = {
          entry,
          settleResponse(body) {
            settled = true;
            resolve(responseFor(body));
          },
        };

        // Some tests simulate a response completing after AbortController
        // cancellation, which can happen when the network reply was already
        // in flight. Those calls are manually resolved and do not auto-reject.
        if (!manualResolveCallIndexes.has(callIndex)) {
          init.signal.addEventListener('abort', () => {
            if (settled) return;
            settled = true;
            const err = new Error('aborted');
            err.name = 'AbortError';
            reject(err);
          });
        }

        if (pendingCallIndexes.has(callIndex)) {
          pending.push(pendingEntry);
          return;
        }

        queueMicrotask(() => {
          if (settled) return;
          try {
            const body = responder(url);
            settled = true;
            resolve(responseFor(body));
          } catch (err) {
            settled = true;
            reject(err);
          }
        });
      });
    },
    setTimeout(callback, delayMs) {
      const id = nextTimerId++;
      timers.set(id, { callback, delay: delayMs || 0 });
      return id;
    },
    clearTimeout(id) {
      timers.delete(id);
    },
  };
  sandbox.window = sandbox;

  elements.healthTitle = new Element('strong');
  elements.healthTitle.textContent = '尚未连接';
  elements.healthText = new Element('span');
  elements.healthText.textContent = '请填写 API 地址后手动刷新。';
  elements.deviceList = new Element('div');
  elements.deviceList.appendChild(new Element('p'));

  vm.createContext(sandbox);
  vm.runInContext(SCRIPT, sandbox, { filename: 'read-only-console-prototype.html' });

  return {
    elements,
    requests,
    timers,
    pending,
    clickRefresh() {
      elements.refreshButton.dispatch({ type: 'click' });
    },
    triggerTimeout() {
      const entries = [...timers.values()];
      assert.equal(entries.length, 1, 'expected one request timeout');
      entries[0].callback();
    },
    resolvePending(predicate) {
      pending.forEach((pendingEntry) => {
        if (!predicate || predicate(pendingEntry)) {
          pendingEntry.settleResponse(responder(pendingEntry.entry.url));
        }
      });
    },
  };
}

async function settle(rounds = 20) {
  for (let i = 0; i < rounds; i += 1) {
    await new Promise((resolve) => globalThis.setTimeout(resolve, 0));
  }
}

function collectText(element) {
  const own = element.textContent || '';
  return [own].concat(element.children.map(collectText)).join('\n');
}

test('页面初始不自动请求、不自动定时，默认等待手动刷新', () => {
  const page = loadPage();

  assert.equal(page.requests.length, 0);
  assert.equal(page.timers.size, 0);
  assert.equal(page.elements.refreshButton.disabled, false);
  assert.equal(page.elements.apiBase.value, 'http://localhost:8000');
  assert.equal(page.elements.healthTitle.textContent, '尚未连接');
  assert.doesNotMatch(SCRIPT, /setInterval\s*\(/);
  assert.doesNotMatch(SCRIPT, /\.innerHTML/);
});

test('手动刷新成功后展示健康状态、设备、遥测、RF 和命令', async () => {
  const page = loadPage();
  page.clickRefresh();
  await settle();

  assert.equal(page.requests.length, 7);
  assert.equal(page.elements.refreshButton.disabled, false);
  assert.equal(page.elements.healthTitle.textContent, '连接正常');
  assert.equal(page.elements.healthText.textContent, 'MQTT 已连接');
  assert.equal(page.elements.deviceCount.textContent, '(3)');
  assert.equal(page.elements.metricCount.textContent, '(5)');
  assert.equal(page.elements.commandCount.textContent, '(2)');

  const deviceText = collectText(page.elements.deviceList);
  assert.match(deviceText, /客厅节点/);
  assert.match(deviceText, /RF：auto \/ -10 dBm/);
  assert.match(deviceText, /温度/);
  assert.match(deviceText, /开关/);
  assert.match(deviceText, /future_metric/);
  assert.match(deviceText, /<img src="x" onerror="alert\(1\)">/);

  const metricText = collectText(page.elements.metricsBody);
  assert.match(metricText, /25\.4 °C/);
  assert.match(metricText, /61 %/);
  assert.match(metricText, /320 lx/);
  assert.match(metricText, /开/);

  const commandText = collectText(page.elements.commandsBody);
  assert.match(commandText, /rf_set_auto/);
  assert.match(commandText, /acknowledged/);
});

test('所有请求均为 GET，核心路径和命令数量符合资源保护约束', () => {
  const page = loadPage();
  page.clickRefresh();

  assert.ok(page.requests.length >= 4);
  page.requests.forEach((request) => {
    assert.equal(request.init.method, 'GET');
    assert.equal(request.init.headers.Accept, 'application/json');
    assert.equal(request.init.cache, 'no-store');
  });
  assert.match(page.requests[3].url, /limit=20/);
});

test('RF 缓存接口普通失败时，不影响其他数据展示', async () => {
  const page = loadPage({
    responder(url) {
      if (url.includes('/rf-policy')) {
        const err = new Error('rf unavailable');
        err.__status = 500;
        return err;
      }
      if (url.includes('/api/health')) return sampleData().health;
      if (url.includes('/api/devices')) return sampleData().devices;
      if (url.includes('/api/metrics/latest')) return sampleData().metrics;
      if (url.includes('/api/commands')) return sampleData().commands;
      throw new Error('unmocked url');
    },
  });

  page.clickRefresh();
  await settle();

  assert.equal(page.elements.healthTitle.textContent, '连接正常');
  assert.equal(page.elements.deviceCount.textContent, '(3)');
  const deviceText = collectText(page.elements.deviceList);
  assert.doesNotMatch(deviceText, /RF：/);
});

test('核心接口失败时显示错误，不伪装成连接正常', async () => {
  const page = loadPage({
    responder(url) {
      if (url.includes('/api/devices') && !url.includes('/rf-policy')) {
        const err = new Error('HTTP 500');
        err.__status = 500;
        return err;
      }
      return sampleData().health;
    },
  });

  page.clickRefresh();
  await settle();

  assert.equal(page.elements.errorBanner.classList.contains('visible'), true);
  assert.equal(page.elements.healthTitle.textContent, '读取失败');
  assert.equal(page.elements.refreshButton.disabled, false);
});

test('API 地址非法时不会发起核心请求，并恢复刷新按钮', () => {
  const page = loadPage();
  page.elements.apiBase.value = 'ftp://example.com';
  page.clickRefresh();

  assert.equal(page.requests.length, 0);
  assert.equal(page.elements.errorBanner.classList.contains('visible'), true);
  assert.equal(page.elements.healthTitle.textContent, '地址无效');
  assert.equal(page.elements.refreshButton.disabled, false);
  assert.equal(page.timers.size, 0);
});

test('核心请求阶段再次刷新会中止旧请求，旧失败信息不会覆盖新结果', async () => {
  const page = loadPage({ pendingCallIndexes: [1, 2, 3, 4] });

  page.clickRefresh();
  assert.equal(page.pending.length, 4);
  const oldSignals = page.requests.slice(0, 4).map((request) => request.signal);

  page.clickRefresh();
  await settle();

  oldSignals.forEach((signal) => assert.equal(signal.aborted, true));
  assert.equal(page.timers.size, 0);
  assert.equal(page.elements.healthTitle.textContent, '连接正常');
  assert.equal(page.elements.refreshButton.disabled, false);
  assert.equal(page.elements.errorBanner.classList.contains('visible'), false);
});

test('RF 请求阶段达到 10 秒超时时明确失败，不停留在正在读取', async () => {
  const page = loadPage({ pendingCallIndexes: [5, 6, 7] });

  page.clickRefresh();
  await settle();

  assert.equal(page.requests.length, 7);
  assert.equal(page.elements.healthTitle.textContent, '正在读取');
  const timer = [...page.timers.values()][0];
  assert.equal(timer.delay, 10000);

  page.triggerTimeout();
  await settle();

  assert.equal(page.elements.errorBanner.classList.contains('visible'), true);
  assert.equal(page.elements.healthTitle.textContent, '读取失败');
  assert.equal(page.elements.healthText.textContent, '请求未完成。');
  assert.equal(page.elements.refreshButton.disabled, false);
  assert.equal(page.timers.size, 0);
  assert.equal(page.requests.length, 7);
});

test('RF 阶段被新请求取代后，旧 RF 响应不能覆盖新页面', async () => {
  const heldIndexes = [5, 6, 7];
  const page = loadPage({
    pendingCallIndexes: heldIndexes,
    manualResolveCallIndexes: heldIndexes,
  });

  page.clickRefresh();
  await settle();
  assert.equal(page.elements.healthTitle.textContent, '正在读取');

  page.clickRefresh();
  await settle();

  assert.equal(page.requests.length, 14);
  assert.equal(page.elements.healthTitle.textContent, '连接正常');
  assert.equal(page.elements.errorBanner.classList.contains('visible'), false);

  page.resolvePending((entry) => entry.entry.url.includes('/rf-policy'));
  await settle();

  assert.equal(page.requests.length, 14);
  assert.equal(page.elements.healthTitle.textContent, '连接正常');
  assert.equal(page.elements.errorBanner.classList.contains('visible'), false);
});

test('MQTT 断开时显示服务降级，不显示绿色连接正常', async () => {
  const page = loadPage({
    health: { status: 'healthy', mqtt_connected: false },
  });

  page.clickRefresh();
  await settle();

  assert.equal(page.elements.errorBanner.classList.contains('visible'), false);
  assert.equal(page.elements.healthDot.classList.contains('warn'), true);
  assert.equal(page.elements.healthDot.classList.contains('ok'), false);
  assert.equal(page.elements.healthTitle.textContent, 'MQTT 未连接');
  assert.equal(page.elements.healthText.textContent, 'API 可访问，但消息链路异常');
});

