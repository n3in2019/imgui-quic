// Drive the actual DOM keyboard/paste handlers with browser-shaped events.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../../frontend/imgui_quic.js',import.meta.url),'utf8');
const events=new Map(),sent=[];
class Input {}
const ctx=vm.createContext({
    document:{addEventListener:(k,f)=>events.set(k,f)},
    window:{addEventListener:(k,f)=>events.set(k,f)},
    HTMLInputElement:Input,HTMLTextAreaElement:Input,
    sendU16:(...v)=>sent.push(v),sendU32:(...v)=>sent.push(v),
    sendClipboardText:t=>sent.push(['clipboard',t]),
});
const mapStart=source.indexOf('const IMGUI_KEY_MAP');
vm.runInContext(source.slice(mapStart,source.indexOf('\n};',mapStart)+3)+
    source.slice(source.indexOf('const keysDown ='),source.indexOf('window.addEventListener("resize", resize)')),ctx);
function event(code,extra={}){return {code,key:code.slice(-1).toLowerCase(),target:{},
    ctrlKey:false,shiftKey:false,metaKey:false,altKey:false,preventDefault(){this.prevented=true;},...extra};}
for(const mod of ['ctrlKey','metaKey']){
    sent.length=0;
    events.get('keydown')(event('KeyC',{[mod]:true}));
    assert(sent.some(([t,k])=>t===20&&k===4096),'aggregate Ctrl required for native shortcut');
    assert(!sent.some(([t,k])=>t===20&&k===32768),'Cmd is normalized to Ctrl, not Ctrl+Super');
    events.get('copy')(event(''));
    assert(sent.some(([t,k])=>t===20&&k===548),'copy event delivers C key');
    events.get('keyup')(event('KeyC'));
    assert(sent.some(([t,k])=>t===21&&k===4096),'modifier released');
}
sent.length=0;
const v=event('KeyV',{ctrlKey:true});events.get('keydown')(v);
assert(!v.prevented,'browser paste event must be allowed');
const paste=event('',{clipboardData:{getData:()=> 'hello\n世界 🌍'}});
events.get('paste')(paste);
assert(paste.prevented);
assert(sent.findIndex(v=>v[0]==='clipboard')<sent.findIndex(([t,k])=>t===20&&k===567),'clipboard must precede V');
assert(!sent.some(([t])=>t===22),'paste is one native editing action, not individual characters');
events.get('blur')();assert(sent.some(([t,k])=>t===21&&k===4096));
sent.length=0;events.get('cut')(event(''));
assert(sent.some(([t,k])=>t===20&&k===569),'Edit menu Cut works without keydown');
sent.length=0;
events.get('paste')(event('',{target:new Input(),clipboardData:{getData:()=> 'native'}}));
assert.equal(sent.length,0,'native fallback textarea keeps browser editing');
console.log('all clipboard_shortcuts passed (Ctrl/Cmd, Unicode, ordering, blur, native fallback)');
