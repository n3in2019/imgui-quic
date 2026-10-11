import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtempSync,readFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
const dir=mkdtempSync(join(tmpdir(),'benchmark-cli-'));
try {
    for(const [name,args] of [['invalid-duration',['--seconds','invalid']],
        ['unknown-option',['--unknown-option']],['missing-host',['--host',join(dir,'absent')]]]) {
        const output=join(dir,`${name}.json`);
        const child=spawnSync(process.execPath,['tools/benchmarks/browser/run.mjs',...args,'--output',output],
            {encoding:'utf8',timeout:10000});
        assert.equal(child.status,1,child.stderr);
        const report=JSON.parse(readFileSync(output,'utf8'));
        assert.equal(report.status,'FAIL');
        assert.equal(typeof report.failure,'string');
        assert.ok(report.failure.length>0);
        assert.deepEqual(report.trials,[]);
    }
    console.log('benchmark CLI: invalid options and missing host emit FAIL JSON and exit 1 PASS');
} finally {rmSync(dir,{recursive:true,force:true});}
