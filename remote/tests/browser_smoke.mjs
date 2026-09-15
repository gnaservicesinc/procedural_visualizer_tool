// Run with PVT_PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs
// and PVT_REMOTE_PYTHON=/path/to/venv/bin/python after building both extensions.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile, readFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {createInterface} from 'node:readline';
const {chromium} = await import(process.env.PVT_PLAYWRIGHT_MODULE || 'playwright');
const root = resolve(import.meta.dirname, '../../..');
const rc = join(root, 'PVT-RC/dist/chrome'), rd = join(root, 'PVT-RD/dist/chrome');
const temporary = await mkdtemp(join(tmpdir(), 'pvt-browser-smoke-'));
const worker = spawn(process.env.PVT_REMOTE_WORKER || process.env.PVT_REMOTE_PYTHON || 'python3', [...(process.env.PVT_REMOTE_WORKER ? [] : ['-m','pvt_remote.host']),'--directory', join(temporary,'host')], {stdio:['pipe','pipe','pipe']});
const lines = createInterface({input:worker.stdout});
const browserErrors = [];
const expectedNetworkErrors = [];
let networkInterruption = false;
const waiters = [];
const events = [];
let stderr = '';
worker.stderr.on('data', data => {stderr += data;});
lines.on('line', line => {
  const message = JSON.parse(line); events.push(message);
  for (const waiter of [...waiters]) if (waiter.predicate(message)) { waiters.splice(waiters.indexOf(waiter), 1); waiter.resolve(message); }
});
function waitEvent(predicate) {
  const previous = events.find(predicate); if (previous) return Promise.resolve(previous);
  return new Promise((resolve,reject) => {const timer=setTimeout(()=>reject(Error(`Worker timeout: ${stderr}`)),15000); waiters.push({predicate,resolve:message=>{clearTimeout(timer);resolve(message);}});});
}
const send = message => worker.stdin.write(JSON.stringify(message)+'\n');
let context;
let relay;
const useRelay = process.argv.includes("--relay");
const useLan = process.argv.includes("--lan");
try {
  const ready = await waitEvent(e=>e.event==='ready');
  if (useRelay) {
    relay = spawn(process.env.PVT_REMOTE_PYTHON || 'python3', ['-m', 'pvt_remote.relay', '--port', '49740']);
    await new Promise(resolve=>setTimeout(resolve,800));
  }
  context = await chromium.launchPersistentContext(join(temporary,'browser'), {channel:'chromium', headless:true,
    args:[`--disable-extensions-except=${rc},${rd}`, `--load-extension=${rc},${rd}`], viewport:{width:1360,height:950}});
  if (useRelay) await context.addInitScript(() => {
    const Original = WebSocket;
    window.WebSocket = class extends Original {
      constructor(url, protocols) {
        if (url !== 'ws://127.0.0.1:49740') throw Error('Direct paths disabled by relay test');
        super(url, protocols);
      }
    };
  });
  if (context.serviceWorkers().length < 2) await context.waitForEvent('serviceworker');
  const deadline = Date.now()+10000;
  while(context.serviceWorkers().length < 2 && Date.now()<deadline) await new Promise(resolve=>setTimeout(resolve,100));
  const workers = context.serviceWorkers(); assert.equal(workers.length,2);
  const pages=[];
  for (const service of workers) {
    const page=await context.newPage();
    page.on('console', message => { if (message.type() === 'error') {
      if (networkInterruption && /WebSocket connection.*ERR_CONNECTION_REFUSED/.test(message.text())) expectedNetworkErrors.push(message.text());
      else browserErrors.push(message.text());
    } });
    page.on('pageerror', error => browserErrors.push(error.message));
    await page.addInitScript(() => {
      window.openedConnections = [];
      window.peerConnections = [];
      const Peer = RTCPeerConnection;
      window.RTCPeerConnection = class extends Peer {
        constructor(config) { super(config); window.peerConnections.push(this); }
      };
      const Original = WebSocket;
      window.WebSocket = class extends Original {
        constructor(url, protocols) { super(url, protocols); this.addEventListener('open', () => window.openedConnections.push(url)); }
      };
    });
    await page.goto(service.url().replace('background.js','index.html'));
    await page.locator('img.brand').waitFor();
    await page.waitForFunction(()=>document.querySelector('img.brand').naturalWidth === 128);
    // First-run pairing is open automatically.
    await page.getByRole('button',{name:'Export Remote file (.pvtremote)'}).waitFor({state:'visible'});
    const companion = page.getByRole('link', {name:/on the Chrome Web Store/});
    const isDisplay = (await page.title()).includes('Display');
    assert.equal(await companion.getAttribute('href'), isDisplay
      ? 'https://chromewebstore.google.com/detail/pvt-remote-control/paachfdeekmbojpfifnaadedhogpgcde'
      : 'https://chromewebstore.google.com/detail/pvt-remote-display/ebehogflkicknbgeimbmhfeaagjfgfda');
    assert.equal(await companion.getAttribute('rel'), 'noopener noreferrer');
    await page.waitForFunction(async()=>!!(await chrome.storage.local.get('identity')).identity);
    const identity=await page.evaluate(async()=>(await chrome.storage.local.get('identity')).identity.public);
    pages.push({page,identity});
  }
  const controller=pages.find(p=>p.identity.role==='control');
  const display=pages.find(p=>p.identity.role==='display');
  assert.ok(controller && display);
  send({op:'configure', enabled:true, config:{...ready.config,port:49739,remotes:pages.map(p=>p.identity),active_control:controller.identity.id, address_scope:useRelay ? 'any' : 'subnet', signaling_url:useRelay ? 'ws://127.0.0.1:49740' : ''}});
  const configured=await waitEvent(e=>e.event==='configured' && e.enabled);
  const hostFile=join(temporary,'host.pvthost'); await writeFile(hostFile, JSON.stringify({...configured.profile, endpoints:useRelay ? [] : useLan ? configured.profile.endpoints.filter(url => url.includes(".local:")) : configured.profile.endpoints}));
  if(useRelay) await new Promise(resolve=>setTimeout(resolve,800));
  const layerNames = ['Aurora', 'Prism', 'Mirage', 'Ripple', 'Bloom', 'Drift', 'Haze', 'Echo', 'Afterglow'];
  const largeTargets = layerNames.flatMap((name, layer) => [
    ...['Layer visible', 'Layer opacity', 'Blend mode', 'Alpha order'].map((label, i) => ({path:`layer/fixture-${layer}/mix${i}`, label, section:`${name} — Mix`, kind:i === 0 ? 0 : i === 1 ? 2 : 3, minimum:0, maximum:i === 2 ? 13 : 1, value:i === 0 ? 1 : i === 1 ? 0.5 : 0})),
    ...Array.from({length:40}, (_, wave) => ['Enabled', 'Use master clock', 'Audio response', 'Amplitude', 'Spatial frequency', 'Cycles per loop', 'Phase', 'Direction', 'Center X', 'Center Y'].map((label, i) => ({path:`layer/fixture-${layer}/wave/${wave + 1}/value${i}`, label, section:`${name} — Wave — Wave ${wave + 1}`, kind:i < 2 ? 0 : 2, minimum:0, maximum:100, value:i < 2 ? 1 : 20}))).flat(),
  ]);
  const initialTargets = [{path:'project.fps',label:'Playback FPS',section:'Project',kind:2,minimum:1,maximum:120,value:30},...largeTargets];
  send({op:'state',state:{revision:'1',targets:initialTargets,background:false,live:false,playing:false,busy:false}});
  for (const {page} of pages) {
    await page.locator('input[type=file]').setInputFiles(hostFile);
    await page.getByLabel('Host name', {exact:true}).waitFor();
    await page.getByRole('button',{name:'Save changes',exact:true}).click();
    await page.getByRole('option',{name:configured.profile.label,exact:true}).waitFor({state:'attached'});
    await page.getByRole('button',{name:'Done',exact:true}).click();
    await page.getByRole('status').filter({hasText:/^Connected$/}).waitFor({timeout:60000});
    assert.equal(await page.getByRole('alert').count(),0);
    if (useLan) assert.ok((await page.evaluate(() => window.openedConnections[0])).includes('.local:'));
  }
  // Settings drafts are protected on every close path in BOTH extensions.
  for (const {page} of pages) {
    await page.getByRole('button', {name:'Hosts & settings', exact:true}).click();
    const hostName = page.getByLabel('Host name', {exact:true});
    await hostName.fill('Unsaved desktop');
    await page.getByRole('button', {name:'Done', exact:true}).click();
    await page.getByRole('dialog', {name:'Save your changes?', exact:true}).waitFor();
    await page.getByRole('button', {name:'Keep editing', exact:true}).click();
    assert.equal(await hostName.inputValue(), 'Unsaved desktop');
    // Browser reload must warn and a dismissed warning must keep the draft.
    const beforeUnload = page.waitForEvent('dialog');
    await page.evaluate(() => { setTimeout(() => location.reload(), 0); });
    const warning = await beforeUnload;
    assert.equal(warning.type(), 'beforeunload'); await warning.dismiss();
    assert.equal(await hostName.inputValue(), 'Unsaved desktop');
    await page.keyboard.press('Escape');
    await page.getByRole('button', {name:'Discard changes', exact:true}).click();
    await page.getByRole('button', {name:'Hosts & settings', exact:true}).click();
    assert.equal(await hostName.inputValue(), configured.profile.label);
    await hostName.fill('Saved desktop');
    await page.getByRole('button', {name:'Done', exact:true}).click();
    await page.getByRole('button', {name:'Save and close', exact:true}).click();
    await page.getByRole('dialog', {name:'Hosts & settings', exact:true}).waitFor({state:'detached'});
    assert.equal(await page.evaluate(async () => (await chrome.storage.local.get('hosts')).hosts[0].label), 'Saved desktop');
    await page.getByRole('button', {name:'Hosts & settings', exact:true}).click();
    assert.equal(await hostName.inputValue(), 'Saved desktop');
    // Invalid names stay visible and are not committed.
    await hostName.fill('   ');
    await page.getByRole('button', {name:'Save changes', exact:true}).click();
    await page.getByRole('alert').filter({hasText:'Invalid identity or name'}).waitFor();
    assert.equal(await hostName.inputValue(), '   ');
    await hostName.fill('Retry desktop');
    await page.evaluate(() => { window.originalStorageSet = chrome.storage.local.set; chrome.storage.local.set = () => { throw Error('Storage temporarily unavailable'); }; });
    await page.getByRole('button', {name:'Save changes', exact:true}).click();
    await page.getByRole('alert').filter({hasText:'Storage temporarily unavailable'}).waitFor();
    assert.equal(await hostName.inputValue(), 'Retry desktop');
    await page.evaluate(() => { chrome.storage.local.set = window.originalStorageSet; });
    await hostName.fill(configured.profile.label);
    await page.getByRole('button', {name:'Save changes', exact:true}).click();
    await page.getByText('Changes saved', {exact:true}).waitFor();
    await page.screenshot({path:join(temporary, page===controller.page ? 'control-settings.png' : 'display-settings.png')});
    await page.getByRole('button', {name:'Done', exact:true}).click();
    await page.getByRole('status').filter({hasText:/^Connected$/}).waitFor();
    assert.ok((await page.title()).includes('PVT'));
    assert.ok(page.url().startsWith('chrome-extension://'));
    assert.equal(await page.locator('vite-error-overlay').count(), 0);
  }
  const cp = controller.page;
  await cp.getByRole('spinbutton', {name:'Playback FPS'}).waitFor();
  assert.equal(await cp.locator('.parameter').count(), 1); // Never dump the entire registry.
  await cp.getByRole('navigation', {name:'Layers and groups'}).getByRole('button', {name:'Aurora'}).click();
  assert.equal(await cp.locator('.parameter').count(), 4);
  await cp.screenshot({path:join(temporary,'control-layer.png'),fullPage:true});
  await cp.getByRole('navigation', {name:'Control sections'}).getByRole('button', {name:'Waves'}).click();
  assert.equal(await cp.locator('.item-grid button').count(), 8);
  await cp.getByRole('button', {name:'Next items page', exact:true}).click();
  await cp.getByRole('button', {name:'Wave 9 #9'}).click();
  assert.equal(await cp.locator('.parameter').count(), 10);
  await cp.screenshot({path:join(temporary,'control-waves.png'),fullPage:true});
  const search = cp.getByRole('searchbox', {name:'Find a control', exact:true});
  await search.fill('Spatial frequency');
  assert.equal(await cp.locator('.parameter').count(), 24);
  await cp.getByRole('button', {name:'Next controls page', exact:true}).click();
  assert.equal(await cp.locator('.parameter').count(), 24);
  await search.fill('layer/fixture-8/wave/40/value6');
  assert.equal(await cp.locator('.parameter').count(), 1);
  await cp.getByRole('button', {name:'Afterglow — Wave — Wave 40 ↗', exact:true}).click();
  assert.equal(await cp.locator('.parameter').count(), 10);
  assert.equal(await search.inputValue(), '');
  // A polling refresh must preserve the selected item and a numeric draft.
  const phase = cp.getByRole('spinbutton', {name:'Phase', exact:true});
  await phase.fill('37');
  await cp.waitForTimeout(1200);
  assert.equal(await phase.inputValue(), '37');
  await phase.press('Escape'); assert.equal(await phase.inputValue(), '20');
  await cp.setViewportSize({width:390,height:844});
  assert.ok(await cp.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
  await cp.screenshot({path:join(temporary,'control-browse-mobile.png'),fullPage:true});
  await cp.getByRole('button', {name:'Project & layers Afterglow', exact:true}).click();
  await cp.getByRole('navigation', {name:'Layers and groups'}).getByRole('button', {name:'Aurora'}).click();
  assert.equal(await cp.locator('.parameter').count(), 4);
  await cp.setViewportSize({width:1360,height:950});
  await cp.getByRole('navigation', {name:'Project navigation'}).getByRole('button', {name:'Project'}).click();
  const input=controller.page.getByRole('spinbutton',{name:'Playback FPS'});
  await input.fill('35');
  await input.fill('40');
  await input.fill('45');
  await new Promise(resolve=>setTimeout(resolve,100));
  assert.equal(events.filter(e=>e.event==='command' && e.command.action==='set').length,0);
  const commandReceived=waitEvent(e=>e.event==='command' && e.command.action==='set');
  await controller.page.locator('form').filter({has:input}).getByRole('button',{name:'Set',exact:true}).click();
  const command=await commandReceived;
  assert.equal(events.filter(e=>e.event==='command' && e.command.action==='set').length,1);
  assert.equal(command.command.value,45); assert.equal(command.remote,controller.identity.id);
  send({op:'reply',token:command.token,result:{ok:true,revision:'2'}});
  send({op:'state',state:{revision:'2',targets:initialTargets.map(t => t.path === 'project.fps' ? {...t, value:45} : t),background:false,live:false,playing:false,busy:false}});
  // The media fixture uses the real encoder, DTLS/SRTP, and browser decoders.
  const fixture=spawn(process.env.PVT_REMOTE_PYTHON || 'python3',['-c',"from PIL import Image; import io,base64; b=io.BytesIO(); Image.new('RGB',(640,360),(30,150,120)).save(b,format='JPEG'); print(base64.b64encode(b.getvalue()).decode())"]);
  let jpeg=''; for await (const data of fixture.stdout) jpeg+=data;
  const interval=setInterval(()=>{
    send({op:'video',jpeg:jpeg.trim()}); send({op:'audio',pcm:Buffer.alloc(3840).toString('base64')});
  },33);
  try {
    await display.page.waitForFunction(()=>document.querySelector('video').videoWidth===640,{},{timeout:15000});
    if (process.platform === 'darwin') {
      const codecs = await display.page.evaluate(async()=>{
        const stats = await window.peerConnections.at(-1).getStats();
        return [...stats.values()].filter(s=>s.type==='inbound-rtp' && s.kind==='video')
          .map(s=>({mime:stats.get(s.codecId).mimeType,frames:s.framesDecoded}));
      });
      assert.ok(codecs.some(codec=>codec.mime==='video/H264' && codec.frames>0));
    }
    await display.page.getByRole('button', {name:'Hosts & settings', exact:true}).click();
    assert.equal(await display.page.locator('video').evaluate(v => v.videoWidth), 640);
    await display.page.getByRole('button', {name:'Done', exact:true}).click();
    await display.page.getByRole('button', {name:'Enable audio', exact:true}).click();
    assert.equal(await display.page.locator('video').evaluate(v => v.muted), false);
    await display.page.getByRole('button', {name:'Mute audio', exact:true}).click();
    await display.page.getByRole('button', {name:'Full screen', exact:true}).click();
    await display.page.waitForFunction(() => document.fullscreenElement?.tagName === 'VIDEO');
    await display.page.evaluate(() => document.exitFullscreen());
    // Reload must restore the selected pairing and resume without Connect.
    await display.page.reload();
    await display.page.getByRole('status').filter({hasText:/^Connected$/}).waitFor({timeout:45000});
    await display.page.waitForFunction(()=>document.querySelector('video').videoWidth===640,{},{timeout:15000});
    // Simulate a desktop/network interruption while retaining both identities.
    const since = events.length;
    networkInterruption = true;
    send({op:'enable',enabled:false});
    await display.page.getByRole('status').filter({hasText:/reconnecting|Connecting/}).waitFor({timeout:20000});
    send({op:'enable',enabled:true});
    await display.page.getByRole('status').filter({hasText:/^Connected$/}).waitFor({timeout:60000});
    await display.page.waitForFunction(()=>document.querySelector('video').videoWidth===640,{},{timeout:15000});
    assert.ok(events.slice(since).some(e => e.event === 'configured' && e.enabled));
    networkInterruption = false;
    // Migrate old browser pauses, then exercise two tabs with the same identity.
    await display.page.evaluate(() => chrome.storage.local.set({paused: true}));
    await display.page.reload();
    await display.page.getByRole('status').filter({hasText:/^Connected$/}).waitFor({timeout:45000});
    assert.equal(await display.page.evaluate(async () => (await chrome.storage.local.get('paused')).paused), undefined);
    const second = await context.newPage();
    await second.addInitScript(() => {
      window.peerConnections = [];
      const Peer = RTCPeerConnection;
      window.RTCPeerConnection = class extends Peer {
        constructor(config) { super(config); window.peerConnections.push(this); }
      };
    });
    await second.goto(display.page.url());
    await second.getByRole('status').filter({hasText:/^Connected$/}).waitFor({timeout:45000});
    await second.waitForFunction(()=>document.querySelector('video').videoWidth===640,{},{timeout:15000});
    const stableCounts = await Promise.all([display.page, second].map(page => page.evaluate(() => window.peerConnections.length)));
    // Longer than two reported 14-second churn cycles.
    await new Promise(resolve => setTimeout(resolve, 32000));
    for (const [index, page] of [display.page, second].entries()) {
      assert.equal(await page.getByRole('status').filter({hasText:/^Connected$/}).count(), 1);
      assert.equal(await page.evaluate(() => window.peerConnections.length), stableCounts[index], 'Parallel tabs must not replace one another');
    }
    await second.close();
    await display.page.waitForFunction(()=>document.querySelector('video').videoWidth===640,{},{timeout:15000});
    const tracks=await display.page.locator('video').evaluate(video=>video.srcObject.getTracks().map(t=>t.kind));
    assert.deepEqual(tracks.sort(),['audio','video']);
    await controller.page.screenshot({path:join(temporary,'control.png'),fullPage:true});
    await display.page.screenshot({path:join(temporary,'display.png'),fullPage:true});
    for(const {page} of pages){
      await page.setViewportSize({width:390,height:844});
      const overflow = await page.evaluate(()=>({width:innerWidth,scroll:document.documentElement.scrollWidth,elements:[...document.querySelectorAll('*')].filter(e=>e.getBoundingClientRect().right>innerWidth+1).map(e=>[e.tagName,e.className,e.getBoundingClientRect().width])}));
      if(overflow.scroll>overflow.width) console.log(JSON.stringify(overflow));
      await page.screenshot({path:join(temporary,(page===controller.page?'control':'display')+'-mobile.png'),fullPage:true});
      assert.ok(overflow.scroll<=overflow.width);
      await page.screenshot({path:join(temporary,(page===controller.page?'control':'display')+'-mobile.png'),fullPage:true});
    }
  } finally {clearInterval(interval);}
  assert.deepEqual(browserErrors, []);
  console.log(JSON.stringify({ok:true,checks:[useRelay ? 'encrypted relay + fragmented data channel state' : useLan ? 'stable mDNS name + encrypted LAN control' : 'authenticated loopback state','separate identities','public file import','mutual crypto across JS/Python','loopback authentication','real WebRTC audio/video','automatic pairing connection','reload reconnect','interruption recovery','legacy pause migration', 'sustained same-profile multi-tab media','control request','responsive layout','3637-control hierarchy and bounded pagination','global search and locate','settings save/discard/reload guards in both roles','invalid save retains draft','audio and fullscreen','zero unexpected browser errors'],expectedNetworkErrors:expectedNetworkErrors.length,screenshots:temporary}));
} catch (error) {
  console.error('Worker diagnostics:', stderr);
  for (const page of context?.pages() || []) {
    if (page.url().includes('index.html')) {
      console.error(await page.locator('body').innerText());
      console.error(await page.evaluate(async()=>Promise.all(window.peerConnections.map(async pc=>({

        stats:[...(await pc.getStats()).values()].filter(s=>['inbound-rtp','codec'].includes(s.type))
      })))));
    }
  }
  throw error;
} finally {
  await context?.close(); worker.stdin.end(); worker.kill(); relay?.kill();
}
