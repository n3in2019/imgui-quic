// Real insecure-origin HTTP + WS + native WebRTC + production WebGL acceptance.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile,mkdir,writeFile,rm} from 'node:fs/promises';
import {resolve} from 'node:path';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const {chromium}=require(resolve('build/browser/node_modules/playwright'));
const output=resolve('build/webrtc-acceptance'); await mkdir(output,{recursive:true});
const report={status:'FAIL',scope:'HTTP/WS WebRTC functional acceptance; no hardware performance claim'};
const env=Object.fromEntries(Object.entries(process.env).filter(([k])=>!k.startsWith('IMGUI_QUIC_')));
let child,browser,native='',exit;
const delay=ms=>new Promise(r=>setTimeout(r,ms));
try {
    await rm('build/webrtc-e2e-credentials',{recursive:true,force:true});
    child=spawn('build/examples/imgui_quic_dev',['--transport','webrtc','--example','minimal',
        '--executable','build/tests/webrtc_acceptance_host',
        '--port','18888','--signal-port','18889','--credentials','build/webrtc-e2e-credentials'],{env,stdio:['ignore','pipe','pipe']});
    child.stdout.on('data',b=>{native+=b;});child.stderr.on('data',b=>{native+=b;});
    child.on('exit',(code,signal)=>{exit={code,signal};});
    child.on('error',e=>{report.native_error=e.message;});
    let url;
    for(let i=0;i<100;i++) {
        if(exit || report.native_error) throw Error('native launcher failed');
        try {url=(await readFile('build/webrtc-e2e-credentials/url.txt','utf8')).trim();
            const ready=await fetch(url.split('#')[0]);if(ready.ok) break;url=null;
        }catch{}
        await delay(50);
    }
    assert.ok(url,'HTTP server did not start');
    const noSandbox=process.env.IMGUI_WEBRTC_TEST_NO_SANDBOX==='1';
    browser=await chromium.launch({headless:true,executablePath:process.env.IMGUI_WEBRTC_TEST_CHROMIUM,
        chromiumSandbox:!noSandbox,args:[...(noSandbox?['--no-sandbox']:[]),'--host-resolver-rules=MAP imgui-http.test 127.0.0.1','--no-proxy-server']});
    report.browser=browser.version();report.sandbox=!noSandbox;
    const page=await browser.newPage({viewport:{width:1280,height:720}});
    const errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.addInitScript(()=>{
        window.rtcFrames=[];window.rtcErrors=[];window.rtcDraws=0;
        const original=WebGLRenderingContext.prototype.drawElements;
        WebGLRenderingContext.prototype.drawElements=function(...args){window.rtcDraws++;return original.apply(this,args);};
        window.ImGuiBenchmark={begin(decoded,evt){window.rtcFrames.push({id:decoded.id,path:evt.path});},end(){},error(e){window.rtcErrors.push(e);}};
    });
    const link=new URL(url);link.hostname='imgui-http.test';
    await page.goto(link.href);
    await page.waitForFunction(()=>window.rtcFrames.length>=30,{},{timeout:30000});
    report.initial=await page.evaluate(()=>({secure:isSecureContext,protocol:location.protocol,
        status:document.querySelector('#status').textContent,frames:rtcFrames,draws:rtcDraws,errors:rtcErrors}));
    assert.equal(report.initial.secure,false);assert.equal(report.initial.protocol,'http:');
    assert.equal(report.initial.status,'connected (WebRTC)');assert.ok(report.initial.draws>0);
    assert.deepEqual(report.initial.errors,[]);assert.deepEqual(errors,[]);
    assert.ok(report.initial.frames.some(r=>r.path==='datagram'),'unreliable draw path not exercised');
    for(let i=1;i<report.initial.frames.length;i++)assert.ok(report.initial.frames[i].id>report.initial.frames[i-1].id);
    await page.mouse.click(80,100);
    for(let i=0;i<100&&!native.includes('clicks=1');i++)await delay(20);
    assert.match(native,/clicks=1/,'real pointer/button input did not reach native ImGui');
    // Explicit recovery must advance epoch and resume complete render/ACK cycles.
    const before=await page.evaluate(()=>({count:rtcFrames.length,epoch:connection.receiver.epoch}));
    await page.evaluate(()=>sendDrawAck(0));
    await page.waitForFunction(old=>connection.receiver.epoch>old.epoch && rtcFrames.length>old.count+5,before,{timeout:15000});
    // Forged future ACK must not promote an unsent baseline.
    await page.evaluate(()=>{const b=new Uint8Array(9),v=new DataView(b.buffer);b[0]=7;v.setUint32(1,connection.receiver.epoch,true);v.setUint32(5,0x7fffffff,true);connection.write(b);});
    const count=await page.evaluate(()=>rtcFrames.length);
    await page.waitForFunction(n=>rtcFrames.length>n+5,count,{timeout:10000});
    // Bad bearer token closes the signaling session without admitting a core client.
    const invalid=await page.evaluate(async endpoint=>new Promise(resolve=>{
        const ws=new WebSocket(endpoint),timer=setTimeout(()=>{ws.close();resolve(false);},8000);
        ws.onopen=()=>ws.send(JSON.stringify({type:'offer',token:'invalid',sdp:''}));
        ws.onclose=()=>{clearTimeout(timer);resolve(true);};
    }),new URLSearchParams(link.hash.slice(1)).get('rtc-url'));
    assert.equal(invalid,true);
    // Same browser reload must acquire a new native session and resume drawing.
    await page.reload();await page.waitForFunction(()=>rtcFrames.length>=15,{},{timeout:30000});
    assert.deepEqual(await page.evaluate(()=>rtcErrors),[]);assert.deepEqual(errors,[]);
    report.reconnect=true;report.recovery=true;report.forged_ack_ignored=true;report.input=true;report.bad_token_rejected=true;
    await page.screenshot({path:resolve(output,'http-webrtc.png')});
    report.status='PASS';
} catch(e){report.failure=String(e.message).replace(/#rtc-[^\s]*/g,'#[redacted]');}
finally {
    await browser?.close();
    if(child && !exit){child.kill('SIGTERM');for(let i=0;i<60&&!exit;i++)await delay(50);if(!exit)child.kill('SIGKILL');}
    report.native_exit=exit??null;
    await writeFile(resolve(output,'native.log'),native);
    await writeFile(resolve(output,'results.json'),JSON.stringify(report,null,2)+'\n');
}
console.log(`${report.status}: ${output}/results.json`);process.exitCode=report.status==='PASS'?0:1;
