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
const worker = spawn(process.env.PVT_REMOTE_PYTHON || 'python3', ['-m','pvt_remote.host','--directory', join(temporary,'host')], {stdio:['pipe','pipe','pipe']});
const lines = createInterface({input:worker.stdout});
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
try {
  const ready = await waitEvent(e=>e.event==='ready');
  if (useRelay) {
    relay = spawn(process.env.PVT_REMOTE_PYTHON || 'python3', ['-m', 'pvt_remote.relay', '--port', '49740']);
    await new Promise(resolve=>setTimeout(resolve,800));
  }
  context = await chromium.launchPersistentContext(join(temporary,'browser'), {channel:'chromium', headless:true,
    args:[`--disable-extensions-except=${rc},${rd}`, `--load-extension=${rc},${rd}`], viewport:{width:1360,height:950}});
  if (context.serviceWorkers().length < 2) await context.waitForEvent('serviceworker');
  const deadline = Date.now()+10000;
  while(context.serviceWorkers().length < 2 && Date.now()<deadline) await new Promise(resolve=>setTimeout(resolve,100));
  const workers = context.serviceWorkers(); assert.equal(workers.length,2);
  const pages=[];
  for (const service of workers) {
    const page=await context.newPage(); await page.goto(service.url().replace('background.js','index.html'));
    await page.locator('img.brand').waitFor();
    await page.waitForFunction(()=>document.querySelector('img.brand').naturalWidth === 128);
    await page.getByRole('button',{name:'Hosts & settings'}).click();
    await page.getByRole('button',{name:'Export .pvtremote'}).waitFor({state:'visible'});
    await page.waitForFunction(async()=>!!(await chrome.storage.local.get('identity')).identity);
    const identity=await page.evaluate(async()=>(await chrome.storage.local.get('identity')).identity.public);
    pages.push({page,identity});
  }
  const controller=pages.find(p=>p.identity.role==='control');
  const display=pages.find(p=>p.identity.role==='display');
  assert.ok(controller && display);
  send({op:'configure', enabled:true, config:{...ready.config,port:49739,remotes:pages.map(p=>p.identity),active_control:controller.identity.id, signaling_url:useRelay ? 'ws://127.0.0.1:49740' : ''}});
  const configured=await waitEvent(e=>e.event==='configured' && e.enabled);
  const hostFile=join(temporary,'host.pvthost'); await writeFile(hostFile, JSON.stringify({...configured.profile, endpoints:useRelay ? [] : configured.profile.endpoints}));
  if(useRelay) await new Promise(resolve=>setTimeout(resolve,800));
  const largeTargets = Array.from({length:700},(_,i)=>({path:`layer.fixture.${i}`,label:`Fixture parameter ${i}`,section:'Fixture layer',kind:2,minimum:0,maximum:100,value:i%100}));
  send({op:'state',state:{revision:'1',targets:[{path:'project.fps',label:'Playback FPS',section:'Project',kind:2,minimum:1,maximum:120,value:30},...largeTargets],background:false,live:false,playing:false,busy:false}});
  for (const {page} of pages) {
    await page.locator('input[type=file]').setInputFiles(hostFile);
    await page.getByRole('option',{name:'PVT host',exact:true}).waitFor({state:'attached'});
    await page.getByRole('button',{name:'Done',exact:true}).click();
    await page.getByRole('button',{name:'Connect',exact:true}).click();
    await page.getByRole('button',{name:'Disconnect',exact:true}).waitFor({timeout:30000});
    assert.equal(await page.getByRole('alert').count(),0);
  }
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
  send({op:'state',state:{revision:'2',targets:[{path:'project.fps',label:'Playback FPS',section:'Project',kind:2,minimum:1,maximum:120,value:45}],background:false,live:false,playing:false,busy:false}});
  // The media fixture uses the real encoder, DTLS/SRTP, and browser decoders.
  const fixture=spawn(process.env.PVT_REMOTE_PYTHON || 'python3',['-c',"from PIL import Image; import io,base64; b=io.BytesIO(); Image.new('RGB',(640,360),(30,150,120)).save(b,format='JPEG'); print(base64.b64encode(b.getvalue()).decode())"]);
  let jpeg=''; for await (const data of fixture.stdout) jpeg+=data;
  const interval=setInterval(()=>{
    send({op:'video',jpeg:jpeg.trim()}); send({op:'audio',pcm:Buffer.alloc(3840).toString('base64')});
  },33);
  try {
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
  console.log(JSON.stringify({ok:true,checks:[useRelay ? 'encrypted relay + fragmented data channel state' : 'authenticated loopback state','separate identities','public file import','mutual crypto across JS/Python','loopback authentication','real WebRTC audio/video','control request','responsive layout'],screenshots:temporary}));
} finally {
  await context?.close(); worker.stdin.end(); worker.kill(); relay?.kill();
}
