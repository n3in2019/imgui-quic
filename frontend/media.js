// Decode browser-local media; pixels are drawn exclusively by ImGui::Image.
(() => {
    const panel=document.getElementById('media-panel'), picker=document.getElementById('media-file');
    const error=document.getElementById('media-error');
    const decoders=document.getElementById('media-decoders');
    let videoCallback=false;
    let media=null,url=null,token=0,frameCallback=0,bridge=null,kind=0,name='',failure='';
    const TEXTURE=4294967294;
    function report() {
        bridge?.report({kind,width:media?.videoWidth||media?.naturalWidth||0,
            height:media?.videoHeight||media?.naturalHeight||0,
            position:Number.isFinite(media?.currentTime)?media.currentTime:0,
            duration:Number.isFinite(media?.duration)?media.duration:0,
            volume:media?.volume??1,paused:media?.paused??true,muted:media?.muted??true,
            error:!!failure,message:failure||name});
    }
    function release() {
        ++token;
        if(media && kind===2){
            if(frameCallback && videoCallback)media.cancelVideoFrameCallback(frameCallback);
            else cancelAnimationFrame(frameCallback);
            media.pause();media.removeAttribute('src');media.load();
        }
        frameCallback=0;decoders.replaceChildren();media=null;kind=0;name='';failure='';
        if(url)URL.revokeObjectURL(url);url=null;
        bridge?.remove(TEXTURE);
    }
    function upload() {
        if(!media || !(media.videoWidth||media.naturalWidth) || (kind===2&&media.readyState<2))return;
        try {bridge?.upload(TEXTURE,media);bridge?.redraw();}
        catch(e){failure='Unable to upload media texture: '+e.message;report();}
    }
    function animate(generation) {
        if(generation!==token || !media || kind!==2)return;
        upload();
        if(media.paused||media.ended){frameCallback=0;return;}
        videoCallback=!!media.requestVideoFrameCallback;
        frameCallback=videoCallback ? media.requestVideoFrameCallback(()=>animate(generation)) : requestAnimationFrame(()=>animate(generation));
    }
    function load(type,src,label,objectUrl=false) {
        release();const generation=token;
        kind=type;name=label;url=objectUrl?src:null;
        const element=document.createElement(type===2?'video':'img');media=element;
        decoders.replaceChildren(element);
        if(type===2){element.playsInline=true;element.preload='auto';element.muted=true;}
        element.addEventListener(type===2?'loadeddata':'load',()=>{if(generation!==token)return;upload();report();});
        for(const event of ['timeupdate','pause','volumechange','seeked','ended'])
            element.addEventListener(event,()=>{if(generation!==token)return;upload();report();});
        element.addEventListener('play',()=>{if(generation!==token)return;failure='';report();if(!frameCallback)animate(generation);});
        element.addEventListener('error',()=>{if(generation!==token)return;release();failure='Cannot decode this file. Try PNG/JPEG or a browser-supported MP4/WebM.';error.textContent=failure;report();});
        element.src=src;report();
    }
    function display(file) {
        const type=file.type.startsWith('image/')?1:file.type.startsWith('video/')?2:
            !file.type&&/\.(png|jpe?g|gif|webp|avif|bmp|svg)$/i.test(file.name)?1:
            !file.type&&/\.(mp4|m4v|webm|ogv|mov)$/i.test(file.name)?2:0;
        if(!type){error.textContent='Choose an image or video file.';open();return;}
        error.textContent='';load(type,URL.createObjectURL(file),file.name||'Clipboard image',true);
        if(panel.open)panel.close();
    }
    function open(){window.dispatchEvent(new Event('imgui-quic-release-input'));if(!panel.open)panel.showModal();}
    async function control(action,value) {
        if(action===7||action===8){
            // The tiny embedded samples use the same fully seekable blob
            // path as local files; static HTTP serving need not support Range.
            release();report();const generation=token;
            try {
                const response=await fetch(action===7?'samples/image.png':'samples/video.webm');
                if(!response.ok)throw new Error('sample unavailable');
                const file=await response.blob();
                if(generation!==token)return;
                load(action===7?1:2,URL.createObjectURL(file),action===7?'Sample image':'Sample video',true);
            } catch(e){if(generation===token){failure=e.message;report();}}
            return;
        }
        if(action===6){release();report();return;}
        if(!media||kind!==2)return;
        try {
            if(action===1){if(media.ended)media.currentTime=0;await media.play();}
            if(action===2)media.pause();
            if(action===3&&Number.isFinite(value))media.currentTime=Math.max(0,Math.min(value,Number.isFinite(media.duration)?media.duration:0));
            if(action===4&&Number.isFinite(value))media.volume=Math.max(0,Math.min(1,value));
            if(action===5)media.muted=!!value;
        } catch(e){failure='Playback blocked or unsupported: '+e.message;}
        report();
    }
    document.getElementById('media-open').addEventListener('click',open);
    document.getElementById('media-close').addEventListener('click',()=>panel.close());
    picker.addEventListener('change',()=>{if(picker.files[0])display(picker.files[0]);picker.value='';});
    document.addEventListener('paste',e=>{
        const item=Array.from(e.clipboardData?.items||[]).find(i=>i.kind==='file'&&/^(image|video)\//.test(i.type));
        const file=item?.getAsFile();if(!file)return;e.preventDefault();e.stopImmediatePropagation();display(file);
    },true);
    document.addEventListener('dragover',e=>{if(Array.from(e.dataTransfer?.types||[]).includes('Files')){e.preventDefault();e.dataTransfer.dropEffect='copy';}});
    document.addEventListener('drop',e=>{const file=e.dataTransfer?.files[0];if(file){e.preventDefault();display(file);}});
    window.addEventListener('pagehide',release);
    window.imgui_quicMedia={attach(b){bridge=b;upload();report();},report,control};
})();
