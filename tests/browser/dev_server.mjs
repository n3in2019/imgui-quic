import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, mkdir, writeFile, symlink, rm, readFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import net from 'node:net';
import http from 'node:http';
import {once} from 'node:events';

const dir = await mkdtemp(path.join(tmpdir(), 'imgui-dev-'));
const probe = net.createServer();
probe.listen(0, '127.0.0.1'); await once(probe, 'listening');
const port = probe.address().port;
await new Promise(resolve => probe.close(resolve));
const root = path.join(dir, 'frontend');
await mkdir(root);
await writeFile(path.join(root, 'index.html'), '<h1>ImGuiQuic</h1>');
await writeFile(path.join(root, 'app.js'), 'const ready = true;');
await writeFile(path.join(dir, 'private'), 'SECRET');
await symlink(path.join(dir, 'private'), path.join(root, 'outside'));
const request = (url, method='GET') => new Promise((resolve, reject) => {
    const req = http.request({hostname:'127.0.0.1', port, path:url, method}, res => {
        let body=''; res.on('data', part => body += part);
        res.on('end', () => resolve({status:res.statusCode, headers:res.headers, body}));
    });
    req.on('error', reject); req.end();
});
const child = spawn(path.resolve('build/examples/imgui_quic_dev'),
    ['--example','minimal','--credentials',path.join(dir,'credentials'),
     '--frontend',root,'--port',String(port)], {stdio:'ignore'});
const exited = once(child,'exit');
try {
    let response;
    for (let i=0;i<100;i++) {
        try {response=await request('/'); break;} catch {}
        if (child.exitCode !== null) throw Error('Launcher exited before serving');
        await new Promise(resolve=>setTimeout(resolve,50));
    }
    assert.equal(response?.body,'<h1>ImGuiQuic</h1>');
    assert.match((await request('/app.js')).headers['content-type'], /javascript/);
    assert.equal((await request('/', 'HEAD')).body,'');
    assert.equal((await request('/', 'POST')).status,405);
    for (const url of ['/missing','/../private','/%2e%2e/private','/outside']) {
        assert.equal((await request(url)).status,404);
    }
    const url=await readFile(path.join(dir,'credentials/url.txt'),'utf8');
    assert.ok(url.startsWith(`http://127.0.0.1:${port}/#`));
    assert.ok(url.includes(`127.0.0.1%3A${port}%2Fwt`));
    await new Promise(resolve=>setTimeout(resolve,250));
    assert.equal(child.exitCode,null);
    child.kill('SIGTERM');
    await exited;
    await assert.rejects(request('/'));
    console.log('Development server: assets, MIME, HEAD, path containment, shared port and shutdown passed');
} finally {
    if (child.exitCode===null && child.signalCode===null) {child.kill('SIGTERM'); await exited;}
    await rm(dir,{recursive:true,force:true});
}
