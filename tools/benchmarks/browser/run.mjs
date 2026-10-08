#!/usr/bin/env node
import {createRequire} from 'node:module';
import {readFile, writeFile, mkdir, mkdtemp, rm} from 'node:fs/promises';
import {existsSync} from 'node:fs';
import {createServer} from 'node:http';
import {spawn, execFileSync} from 'node:child_process';
import {createHash, randomBytes} from 'node:crypto';
import {createInterface} from 'node:readline';
import {tmpdir, arch, release} from 'node:os';
import {resolve, dirname, extname, basename} from 'node:path';
import {fileURLToPath} from 'node:url';
import {parseArgs} from 'node:util';
import {summarize} from './metrics.mjs';
const here = dirname(fileURLToPath(import.meta.url)), root = resolve(here,'../../..');
const {values: args} = parseArgs({options: {
    host:{type:'string',default:'build/transport_benchmark_host'},
    chromium:{type:'string'}, headed:{type:'boolean',default:false},
    seconds:{type:'string',default:'12'}, trials:{type:'string',default:'3'},
    scene:{type:'string',default:'moving'}, fps:{type:'string',default:'30'},
    output:{type:'string',default:'build/benchmarks/browser/results.json'},
    'max-uncertainty-ms':{type:'string',default:'5'},
    'max-submit-ms':{type:'string',default:'16.7'}, 'max-proxy-ms':{type:'string'},
    'drift-ppm':{type:'string',default:'100'}, 'resolution-ms':{type:'string',default:'1'},
    'no-sandbox':{type:'boolean',default:false}
}});
const seconds = Number(args.seconds), trials = Number(args.trials), fps = Number(args.fps);
const options = {minFps:fps*.8,minFrames:Math.max(10,Math.floor(seconds*fps*.8)),
    maxUncertaintyMs:Number(args['max-uncertainty-ms']), maxSubmitMs:Number(args['max-submit-ms']),
    maxProxyMs:args['max-proxy-ms'] === undefined ? null : Number(args['max-proxy-ms']),
    driftPpm:Number(args['drift-ppm']), resolutionMs:Number(args['resolution-ms'])};
if (![seconds,trials,fps,...Object.values(options).filter(x=>x!==null)].every(x=>Number.isFinite(x)&&x>=0) ||
    seconds < 1 || seconds > 300 || trials < 1 || trials > 20 || !Number.isInteger(trials) || fps < 1 || fps > 120 ||
    !['moving','dynamic'].includes(args.scene)) throw Error('invalid benchmark options');
const output = resolve(args.output);
const report = {schema_version:1, status:'FAIL', scope:'local Chromium + native host on the same machine',
    environment:{arch:arch(),kernel:release(),node:process.version,headless:!args.headed},
    workload:{scene:args.scene,fps,seconds,trials,warmup_seconds:1,viewport:[1280,720],precision:'exact'},
    policy:options, limitations:{compositor_present:'unavailable',
        next_raf:'display-opportunity proxy only; no proof of scanout or presentation',
        clock:'interval bound conditional on configured drift and timer-resolution allowances'}, trials:[]};
let token = '';
const cleanError = e => String(e.message || e).replaceAll(token || '___no_token___','[redacted]').replace(/#wt-[^\s]*/g,'#[redacted]');
const delay = ms => new Promise(r=>setTimeout(r,ms));
async function trial(chromium, index) {
    const dir = await mkdtemp(resolve(tmpdir(),'imgui-browser-'));
    let host, browser, server, reader, pending, stderr='';
    const clocks = [], result = {index,status:'FAIL'};
    try {
        token = randomBytes(24).toString('hex');
        await writeFile(resolve(dir,'token'),token,{mode:0o600});
        execFileSync('openssl',['req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1',
            '-nodes','-keyout',resolve(dir,'key.pem'),'-out',resolve(dir,'cert.pem'),'-days','13',
            '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],{stdio:'ignore'});
        const cert = execFileSync('openssl',['x509','-in',resolve(dir,'cert.pem'),'-outform','DER']);
        const pin = createHash('sha256').update(cert).digest('hex');
        function probe() {
            if (pending) return Promise.reject(Error('overlapping clock probe'));
            return new Promise((accept,reject)=>{
                const timer = setTimeout(()=>{pending=null;reject(Error('native clock probe timeout'));},3000);
                pending = {accept:line=>{clearTimeout(timer);accept(line);},reject:e=>{clearTimeout(timer);reject(e);}};
                host.stdin.write('c');
            });
        }
        server = createServer(async (req,res)=>{
            try {
                if (req.url === '/clock') {
                    // Only our page can fetch this endpoint. No permissive CORS.
                    const native_ns = await probe();
                    res.writeHead(200,{'Content-Type':'application/json','Cache-Control':'no-store'});
                    res.end(JSON.stringify({native_ns})); return;
                }
                const name = req.url === '/' ? 'index.html' : req.url.slice(1);
                if (name !== basename(name) || !/^[a-zA-Z0-9_.-]+$/.test(name)) {res.writeHead(404);res.end();return;}
                const data = await readFile(resolve(root,'frontend',name));
                res.writeHead(200,{'Content-Type':({'.html':'text/html','.js':'text/javascript','.css':'text/css'})[extname(name)] || 'application/octet-stream'});
                res.end(data);
            } catch {res.writeHead(500);res.end('benchmark endpoint failed');}
        });
        await new Promise((yes,no)=>{server.once('error',no);server.listen(0,'127.0.0.1',yes);});
        const origin = `http://127.0.0.1:${server.address().port}`;
        const env = Object.fromEntries(Object.entries(process.env).filter(([key])=>!key.startsWith('IMGUI_QUIC_')&&!key.startsWith('IMGW_BENCH_')));
        Object.assign(env,{IMGUI_QUIC_CERT:resolve(dir,'cert.pem'),IMGUI_QUIC_KEY:resolve(dir,'key.pem'),
            IMGUI_QUIC_TOKEN_FILE:resolve(dir,'token'),IMGUI_QUIC_ORIGINS:origin,IMGW_BENCH_CLOCK_STDIO:'1'});
        host = spawn(resolve(args.host),[args.scene,args.fps],{env,stdio:['pipe','pipe','pipe']});
        host.on('error',e=>{pending?.reject(e);pending=null;});
        host.stdin.on('error',e=>{pending?.reject(e);pending=null;});
        host.on('exit',()=>{pending?.reject(Error('native host exited'));pending=null;});
        host.stderr.on('data',b=>{stderr=(stderr+b.toString()).slice(-4000);});
        reader=createInterface({input:host.stdout});
        reader.on('line',line=>{if (/^\d+$/.test(line)&&pending) {const p=pending;pending=null;p.accept(line);}});
        await probe(); // Initialization and side-channel readiness, bounded by timeout.
        browser = await chromium.launch({headless:!args.headed,executablePath:args.chromium,
            args:args['no-sandbox'] ? ['--no-sandbox'] : [], chromiumSandbox:!args['no-sandbox']});
        result.browser_version=browser.version();
        const page=await browser.newPage({viewport:{width:1280,height:720},deviceScaleFactor:1});
        const pageErrors=[];
        page.on('pageerror',e=>pageErrors.push(cleanError(e)));
        await page.addInitScript({path:resolve(here,'observer.js')});
        const fragment=new URLSearchParams({'wt-url':'https://127.0.0.1:19443/wt','wt-token':token,'wt-cert':pin,'draw-precision':'exact'});
        await page.goto(`${origin}/#${fragment}`,{waitUntil:'load',timeout:15000});
        await page.waitForFunction(()=>document.querySelector('#status')?.className==='connected',{},{timeout:20000});
        await delay(1000);
        result.renderer=await page.evaluate(()=>{
            const gl=document.querySelector('canvas').getContext('webgl'), debug=gl.getExtension('WEBGL_debug_renderer_info');
            return {vendor:gl.getParameter(gl.VENDOR),renderer:gl.getParameter(gl.RENDERER),
                unmasked:debug?gl.getParameter(debug.UNMASKED_RENDERER_WEBGL):null,attributes:gl.getContextAttributes()};
        });
        async function calibrate(count) {
            for(let i=0;i<count;i++) clocks.push(await page.evaluate(async()=>{
                const start_ms=performance.now();
                const r=await fetch('/clock',{cache:'no-store'});
                if (!r.ok) throw Error('clock endpoint');
                const {native_ns}=await r.json();
                return {start_ms,end_ms:performance.now(),native_ns};
            }));
        }
        await calibrate(12);
        await page.evaluate(()=>window.ImGuiBenchmark.start());
        const start=performance.now();
        while(performance.now()-start < seconds*1000) {
            await delay(Math.min(1000, Math.max(0,seconds*1000-(performance.now()-start))));
            await calibrate(1);
        }
        await page.evaluate(()=>window.ImGuiBenchmark.stop());
        await calibrate(12);
        await delay(250); // Complete pending RAF callbacks and nonblocking GPU queries.
        const snapshot=await page.evaluate(()=>window.ImGuiBenchmark.snapshot(document.querySelector('canvas').getContext('webgl')));
        snapshot.errors.push(...pageErrors);
        if(host.exitCode!==null) snapshot.errors.push('native host exited');
        Object.assign(result,summarize(snapshot,clocks,options),{clock_probes:clocks,samples:snapshot});
    } catch(e) {result.status='FAIL';result.failures=[cleanError(e)];result.native_stderr=cleanError(stderr);}
    finally {
        await browser?.close();
        if(host && host.exitCode===null) {
            host.kill('SIGTERM');
            await Promise.race([new Promise(r=>host.once('exit',r)),delay(2000)]);
            if(host.exitCode===null) host.kill('SIGKILL');
        }
        reader?.close();
        if(server) {server.closeAllConnections(); await new Promise(r=>server.close(r));}
        await rm(dir,{recursive:true,force:true});
    }
    return result;
}
try {
    const require = createRequire(import.meta.url);
    const candidate = process.env.IMGUI_BENCH_PLAYWRIGHT || resolve(root,'build/browser/node_modules/playwright');
    const {chromium} = require(existsSync(candidate) ? candidate : 'playwright');
    report.environment.commit=execFileSync('git',['rev-parse','HEAD'],{cwd:root,encoding:'utf8'}).trim();
    report.environment.dirty=!!execFileSync('git',['status','--porcelain'],{cwd:root,encoding:'utf8'}).trim();
    report.environment.host_sha256=createHash('sha256').update(await readFile(resolve(args.host))).digest('hex');
    for(let i=0;i<trials;i++) report.trials.push(await trial(chromium,i+1));
    report.status=report.trials.every(t=>t.status==='PASS')?'PASS':'FAIL';
} catch(e) {report.failure=cleanError(e);}
await mkdir(dirname(output),{recursive:true});
await writeFile(output,JSON.stringify(report,null,2)+'\n');
console.log(`${report.status}: ${output}`);
process.exitCode=report.status==='PASS'?0:1;
