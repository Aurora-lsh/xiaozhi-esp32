const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const deviceSource = fs.readFileSync(path.join(root, 'app_httpd.cpp'), 'utf8');
const firmwareSource = fs.readFileSync(path.join(root, 'CameraWebServer.ino'), 'utf8');
const portalHtml = deviceSource.split('static const char kProvisioningHtml[] = R"HTML(')[1].split(')HTML";')[0];
const portalScript = portalHtml.split('<script>')[1].split('</script>')[0];
new vm.Script(portalScript);

async function exercisePortal() {
  const fields = new Map();
  const element = identifier => {
    if (!fields.has(identifier)) fields.set(identifier, { value: '', checked: false, style: {}, textContent: '', className: '' });
    return fields.get(identifier);
  };
  const requests = [];
  let responseStatus = 200;
  const context = {
    document: { getElementById: element },
    fetch: async (url, options = {}) => {
      requests.push({ url, options });
      return { ok: responseStatus < 400, json: async () => options.method
        ? { success: responseStatus < 400, error: 'save failure' }
        : { ssid: 'Initial AP', mcp_ip: '192.168.1.100' } };
    }
  };
  vm.createContext(context);
  vm.runInContext(portalScript, context);
  const settle = () => new Promise(resolve => setImmediate(resolve));
  await settle();
  assert.equal(element('ssid').value, 'Initial AP');
  assert.equal(element('mcpIp').value, '192.168.1.100');
  assert.match(portalHtml, /ESP32-CAM 网络配置/);
  assert.match(portalHtml, /ESP32-CAM-7/);
  assert.ok(firmwareSource.includes('WiFi.softAP(WIFI_CONFIG_AP_SSID, NULL'));
  assert.ok(deviceSource.includes('.uri = "/config"'));
  assert.ok(deviceSource.includes('.handler = provisioning_page_handler'));
  element('ssid').value = ' Mixed AP ';
  element('mcpIp').value = '192.168.1.42';
  vm.runInContext('saveConfig()', context); await settle();
  let payload = JSON.parse(requests.at(-1).options.body);
  assert.deepEqual(payload, { ssid: ' Mixed AP ', mcp_ip: '192.168.1.42' });
  assert.equal(requests.at(-1).options.headers['Content-Type'], 'application/json');
  element('pwd').value = 'Case=Secret+"';
  vm.runInContext('saveConfig()', context); await settle();
  payload = JSON.parse(requests.at(-1).options.body);
  assert.equal(payload.pwd, 'Case=Secret+"');
  assert.equal(element('pwd').value, '');
  element('clearPwd').checked = true;
  vm.runInContext('saveConfig()', context); await settle();
  assert.equal(JSON.parse(requests.at(-1).options.body).pwd, '');
  responseStatus = 503;
  element('pwd').value = 'Unsaved';
  vm.runInContext('saveConfig()', context); await settle();
  assert.equal(element('pwd').value, 'Unsaved');
  assert.equal(element('hint').textContent, 'save failure');
  responseStatus = 200;
  vm.runInContext('rebootDevice()', context); await settle();
  assert.equal(requests.at(-1).url, '/device/reboot');
  assert.equal(requests.at(-1).options.method, 'POST');
  console.log('PASS: ESP32 hotspot page configuration loading, password preservation, explicit clearing, errors and reboot');
}

exercisePortal().catch(error => { console.error(error); process.exitCode = 1; });
