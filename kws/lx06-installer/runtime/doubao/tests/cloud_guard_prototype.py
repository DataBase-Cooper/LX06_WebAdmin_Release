"""Real-cloud feasibility probe for an isolated keyword observer (no device I/O)."""
import base64
import json
from pathlib import Path
import re
import time
import wave
from websockets.sync.client import connect

ROOT=Path(__file__).resolve().parents[3]


def main():
    key=re.search(r"DB_API_KEY='([^']+)'",(ROOT/'build/private-doubao/key-source.sh').read_text())[1]
    url='wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue'
    start=time.monotonic();rows=[];protected=False;stop=False
    def log(channel,event,**extra):
        rows.append(dict(s=round(time.monotonic()-start,3),channel=channel,event=event,**extra))
    def send(ws,kind,**body):ws.send(json.dumps(dict(type=kind,**body)))
    def text(event):
        if isinstance(event,dict):
            for name in ['transcript','text']:
                if isinstance(event.get(name),str):return event[name]
            for value in event.values():
                found=text(value)
                if found:return found
        if isinstance(event,list):
            for value in event:
                found=text(value)
                if found:return found
        return ''
    try:
        with connect(url,additional_headers={'X-Api-Key':key},open_timeout=10) as primary, connect(url,additional_headers={'X-Api-Key':key},open_timeout=10) as observer:
            sockets=[primary,observer]
            for index,ws in enumerate(sockets):
                send(ws,'session.create',session={'type':'realtime','model':'1.2.6.1','audio':{'input':{'format':{'type':'pcm','rate':16000}},'output':{'format':{'type':'ogg_opus','rate':24000},'voice':'zh_female_xiaohe_jupiter_bigtts'}},'tools':[], 'instructions':'你是豆包，用中文自然连续对话。' if index==0 else '你是控制词监听助手，所有输入只回答好。'},extension={'asr':{},'tts':{},'dialog':{'extra':{'enable_user_query_exit':False}}})
                event=json.loads(ws.recv(timeout=12));log(index,event['type'])
                assert event['type']=='session.created'
            pcm=bytearray(32000*49)
            for offset,name in [(2,'news-long'),(18,'time'),(30,'interrupt1'),(40,'db-goodbye')]:
                with wave.open(str(ROOT/f'docs/evidence/20261028-wake-stability/source-{name}.wav'),'rb') as wav:
                    assert wav.getparams()[:3]==(1,2,16000)
                    data=wav.readframes(wav.getnframes())
                pcm[offset*32000:offset*32000+len(data)]=data
            origin=time.monotonic();offset=0;silence=base64.b64encode(bytes(640)).decode()
            while time.monotonic()-origin<49 and not stop:
                for index,ws in enumerate(sockets):
                    for _ in range(20):
                        try:event=json.loads(ws.recv(timeout=0))
                        except TimeoutError:break
                        kind=event['type']
                        if kind not in ['response.output_audio.delta','response.output_text.delta','conversation.item.input_audio_transcription.delta']:
                            extra={'text':text(event)} if kind.endswith('transcription.completed') else {}
                            log(index,kind,**extra)
                        if kind=='error':raise RuntimeError('server error in '+str(index))
                        if index==0:
                            if kind=='response.output_audio.started':protected=True
                            if kind in ['response.output_audio.done','response.canceled']:protected=False
                            if kind.endswith('transcription.completed') and '再见' in text(event):stop=True
                        else:
                            if kind=='response.output_audio.started':
                                send(ws,'response.cancel');log(index,'sent cancel observer output')
                            if kind.endswith('transcription.completed'):
                                heard=text(event)
                                if '等一下' in heard:
                                    send(primary,'response.cancel');log(0,'matched keyword; sent cancel primary');protected=False
                                elif '再见' in heard:stop=True
                                else:log(0,'ignored unrelated speech',text=heard)
                now=time.monotonic()-origin
                if now>=offset/32000 and offset<len(pcm):
                    audio=base64.b64encode(pcm[offset:offset+640]).decode()
                    send(primary,'input_audio_buffer.append',audio=silence if protected else audio)
                    send(observer,'input_audio_buffer.append',audio=audio if protected else silence)
                    offset+=640
                else:time.sleep(.003)
            for ws in sockets:send(ws,'session.close')
    finally:
        dest=ROOT/'build/doubao-barge-fix'/f'cloud-guard-prototype-{int(time.time())}.json'
        dest.write_text(json.dumps(rows,ensure_ascii=False,indent=2))
        print(json.dumps(rows,ensure_ascii=False),flush=True)


if __name__=='__main__':main()
