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
  'provisioning',
  'softap-prototype.html',
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
  constructor(id) {
    this.id = id;
    this.classList = new ClassList();
    this.listeners = Object.create(null);
    this.children = [];
    this.value = '';
    this.checked = false;
    this.disabled = false;
    this.hidden = false;
    this.textContent = '';
    this.innerHTML = '';
    this.className = '';
  }

  addEventListener(type, listener) {
    if (!this.listeners[type]) this.listeners[type] = [];
    this.listeners[type].push(listener);
  }

  dispatch(event) {
    event.type = event.type || '';
    event.preventDefault = event.preventDefault || (() => {
      event.defaultPrevented = true;
    });

    for (const listener of this.listeners[event.type] || []) {
      listener.call(this, event);
    }
  }

  appendChild(child) {
    this.children.push(child);
    return child;
  }
}

function loadPage() {
  const ids = [
    'provisionForm',
    'testButton',
    'commitButton',
    'cancelButton',
    'openNetwork',
    'errorSummary',
    'errorList',
    'statusDot',
    'statusTitle',
    'statusText',
    'wifiSsid',
    'wifiPassword',
    'mqttHost',
    'mqttPort',
    'mqttTls',
    'mqttUsername',
    'mqttPassword',
    'gatewayId',
  ];

  const elements = {};
  ids.forEach((id) => {
    elements[id] = new Element(id);
  });

  ['commitButton', 'cancelButton', 'mqttTls'].forEach((id) => {
    elements[id].disabled = true;
  });


  const timers = new Map();
  let nextTimerId = 1;

  const sandbox = {
    console,
    TextEncoder,
    document: {
      getElementById(id) {
        if (!elements[id]) elements[id] = new Element(id);
        return elements[id];
      },
      createElement(tag) {
        return new Element(tag);
      },
    },
    setTimeout(callback) {
      const id = nextTimerId++;
      timers.set(id, callback);
      return id;
    },
    clearTimeout(id) {
      timers.delete(id);
    },
  };
  sandbox.window = sandbox;

  vm.createContext(sandbox);
  vm.runInContext(SCRIPT, sandbox, { filename: 'softap-prototype.html' });

  return {
    elements,
    timers,
    runNextTimer() {
      const entry = timers.entries().next().value;
      assert.ok(entry, 'expected a queued timer');
      timers.delete(entry[0]);
      entry[1]();
    },
  };
}

function fillValidForm(elements) {
  elements.wifiSsid.value = 'HomeWiFi';
  elements.wifiPassword.value = 'password123';
  elements.openNetwork.checked = false;
  elements.mqttHost.value = 'mqtt.example.com';
  elements.mqttPort.value = '1883';
  elements.mqttUsername.value = 'gateway-user';
  elements.mqttPassword.value = 'gateway-secret';
  elements.gatewayId.value = 'gw-001';
}

test('页面初始只能先测试，不能直接保存', () => {
  const { elements } = loadPage();

  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.cancelButton.disabled, true);
});

test('完整流程：测试通过后才允许保存，保存后进入已提交状态', () => {
  const { elements, timers, runNextTimer } = loadPage();
  fillValidForm(elements);

  elements.provisionForm.dispatch({ type: 'submit' });

  assert.equal(timers.size, 1);
  assert.equal(elements.testButton.disabled, true);
  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.cancelButton.disabled, false);
  assert.equal(elements.statusTitle.textContent, '正在测试连接');

  runNextTimer();

  assert.equal(elements.statusTitle.textContent, '连接测试通过');
  assert.equal(elements.testButton.disabled, false);
  assert.equal(elements.commitButton.disabled, false);
  assert.equal(elements.cancelButton.disabled, false);

  elements.commitButton.dispatch({ type: 'click' });
  assert.equal(elements.statusTitle.textContent, '正在保存');
  runNextTimer();

  assert.equal(elements.statusTitle.textContent, '配置已保存');
  assert.equal(elements.testButton.disabled, true);
  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.cancelButton.disabled, true);
});

test('测试通过后修改任意字段，旧测试结果立即失效', () => {
  const { elements, runNextTimer } = loadPage();
  fillValidForm(elements);

  elements.provisionForm.dispatch({ type: 'submit' });
  runNextTimer();
  assert.equal(elements.commitButton.disabled, false);

  elements.wifiSsid.value = 'ChangedWiFi';
  elements.provisionForm.dispatch({ type: 'change' });

  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.statusTitle.textContent, '测试结果已失效');
});

test('非法网关 ID 不能进入测试或保存流程', () => {
  for (const badGatewayId of ['bad/topic', 'bad+id', ' ', '\t']) {
    const { elements, timers } = loadPage();
    fillValidForm(elements);
    elements.gatewayId.value = badGatewayId;

    elements.provisionForm.dispatch({ type: 'submit' });

    assert.equal(timers.size, 0, `bad gateway ID was tested: ${badGatewayId}`);
    assert.equal(elements.commitButton.disabled, true);
    assert.equal(elements.errorSummary.hidden, false);
  }
});

test('连接测试失败时不会开放保存按钮', () => {
  const { elements, runNextTimer } = loadPage();
  fillValidForm(elements);
  elements.wifiSsid.value = '失败网络';

  elements.provisionForm.dispatch({ type: 'submit' });
  runNextTimer();

  assert.equal(elements.statusTitle.textContent, '连接测试失败');
  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.testButton.disabled, false);
});

test('测试过程中点击取消会清除待执行定时器', () => {
  const { elements, timers } = loadPage();
  fillValidForm(elements);

  elements.provisionForm.dispatch({ type: 'submit' });
  elements.cancelButton.dispatch({ type: 'click' });

  assert.equal(timers.size, 0);
  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.statusText.textContent, '测试已取消，未写入任何配置。');
});

test('测试过程中表单变化会取消旧测试', () => {
  const { elements, timers } = loadPage();
  fillValidForm(elements);

  elements.provisionForm.dispatch({ type: 'submit' });
  elements.wifiSsid.value = 'ChangedWhileTesting';
  elements.provisionForm.dispatch({ type: 'change' });

  assert.equal(timers.size, 0);
  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.statusTitle.textContent, '测试已取消');
});

test('清空重填会取消测试并恢复初始操作状态', () => {
  const { elements, timers, runNextTimer } = loadPage();
  fillValidForm(elements);

  elements.provisionForm.dispatch({ type: 'submit' });
  assert.equal(timers.size, 1);

  elements.provisionForm.dispatch({ type: 'reset' });
  assert.equal(timers.size, 1);
  runNextTimer();

  assert.equal(elements.commitButton.disabled, true);
  assert.equal(elements.cancelButton.disabled, true);
  assert.equal(elements.statusTitle.textContent, '等待填写');
});

test('开放网络允许空密码，但不允许同时填写密码', () => {
  const { elements: openElements, timers: openTimers, runNextTimer: runOpenTimer } = loadPage();
  fillValidForm(openElements);
  openElements.openNetwork.checked = true;
  openElements.wifiPassword.value = '';

  openElements.provisionForm.dispatch({ type: 'submit' });
  assert.equal(openTimers.size, 1);
  runOpenTimer();
  assert.equal(openElements.statusTitle.textContent, '连接测试通过');

  const { elements: mixedElements, timers: mixedTimers } = loadPage();
  fillValidForm(mixedElements);
  mixedElements.openNetwork.checked = true;

  mixedElements.provisionForm.dispatch({ type: 'submit' });
  assert.equal(mixedTimers.size, 0);
  assert.equal(mixedElements.errorSummary.hidden, false);
});

test('源码结构约束：保存阶段引用测试时捕获的快照', () => {
  assert.match(SCRIPT, /const snapshot = activeTest\.payload;/);
  assert.match(SCRIPT, /acceptedTest = \{\s*id: testId,\s*payload: snapshot/);
  assert.match(SCRIPT, /const context = acceptedTest;/);
});
