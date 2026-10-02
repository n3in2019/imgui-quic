import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
class Element {
    constructor(){this.listeners=new Map();this.children=[];this.textContent='';this.files=[];}
    addEventListener(name,fn){this.listeners.set(name,fn);}
    emit(name,e={}){this.listeners.get(name)?.(e);}
    appendChild(child){this.children.push(child);}
    replaceChildren(...children){this.children=children;}
    setAttribute(name,value){this[name]=value;}
    removeAttribute(name){delete this[name];}
    showModal(){this.open=true;}
    close(){this.open=false;this.emit('close');}
}
class Video extends Element {
    constructor(){super();this.paused=true;this.volume=1;this.currentTime=0;this.duration=5;this.readyState=4;}
    pause(){this.paused=true;this.emit('pause');}
    async play(){this.paused=false;this.emit('play');}
    load(){this.unloaded=true;}
}
const ids=new Map(['media-decoders','media-panel','media-file','media-preview','media-info','media-error','media-open','media-close'].map(id=>[id,new Element()]));
const document=new Element(),window=new Element();
document.getElementById=id=>ids.get(id);
let decoded;document.createElement=tag=>decoded=tag==='video'?new Video():new Element();
window.dispatchEvent=e=>window.emit(e.type);
let releasedKeys=0;window.addEventListener('imgui-quic-release-input',()=>releasedKeys++);
const live=new Set();let next=0;
vm.runInNewContext(readFileSync(new URL('../../frontend/media.js',import.meta.url),'utf8'),{
    document,window,HTMLVideoElement:Video,Event:class {constructor(type){this.type=type;}},
    fetch:async()=>({ok:true,blob:async()=>({})}),
    requestAnimationFrame:()=>1,cancelAnimationFrame(){},
    URL:{createObjectURL(){const url=`blob:test/${++next}`;live.add(url);return url;},revokeObjectURL:url=>live.delete(url)},
});

const reports=[],uploads=[],removed=[];
window.imgui_quicMedia.attach({report:m=>reports.push(m),upload:(id,m)=>uploads.push({id,m}),remove:id=>removed.push(id),redraw(){}});
const el=id=>ids.get(id),current=()=>decoded;
function choose(name,type){el('media-file').files=[{name,type}];el('media-file').emit('change');}
el('media-open').emit('click');assert.equal(releasedKeys,1);
choose('photo.png','image/png');const image=current();image.naturalWidth=640;image.naturalHeight=480;image.emit('load');
assert.equal(reports.at(-1).width,640);assert.equal(uploads.at(-1).id,4294967294);
assert.equal(el('media-preview').children.length,0,'media must never appear in a DOM overlay');
assert.equal(el('media-panel').open,false);assert.equal(live.size,1);
choose('movie.webm','video/webm');const video=current();
assert(video.playsInline&&video.muted);assert(!video.controls,'controls belong to ImGui');
video.videoWidth=1280;video.videoHeight=720;video.emit('loadeddata');
assert.equal(reports.at(-1).kind,2);assert.equal(live.size,1);
await window.imgui_quicMedia.control(1,0);assert.equal(video.paused,false);
await window.imgui_quicMedia.control(2,0);assert.equal(video.paused,true);
await window.imgui_quicMedia.control(3,99);assert.equal(video.currentTime,5);
await window.imgui_quicMedia.control(4,-1);assert.equal(video.volume,0);
await window.imgui_quicMedia.control(5,0);assert.equal(video.muted,false);
await window.imgui_quicMedia.control(6,0);assert(video.paused&&video.unloaded);assert.equal(live.size,0);
const paste={clipboardData:{items:[{kind:'file',type:'image/png',getAsFile:()=>({name:'paste.png',type:'image/png'})}]},
    preventDefault(){this.prevented=true;},stopImmediatePropagation(){this.stopped=true;}};
document.emit('paste',paste);assert(paste.prevented&&paste.stopped);
const old=current();choose('replacement.jpg','image/jpeg');old.emit('error');
assert.equal(live.size,1,'late error from replaced media cannot clear current texture');
current().emit('error');assert.equal(live.size,0);assert(reports.at(-1).error);
choose('unsupported.txt','text/plain');assert.equal(live.size,0);
choose('type-omitted.mp4','');assert(current() instanceof Video);
window.emit('pagehide');assert.equal(live.size,0);
await window.imgui_quicMedia.control(7,0);assert(current().src.startsWith('blob:'));assert.equal(reports.at(-1).kind,1);
await window.imgui_quicMedia.control(8,0);assert(current().src.startsWith('blob:'));assert.equal(reports.at(-1).kind,2);
console.log('all media_viewer passed (ImGui texture upload, controls, paste, replacement, errors, cleanup)');
