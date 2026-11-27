"""Validate the C MFCC model against an independent vectorized NumPy reference.

Reference parameters: local-wake c7afdec, librosa defaults (Slaney, 128 mel,
centered 512 FFT/400 periodic Hann, 160 hop, 13 orthonormal DCT-II, top_db=80).
NumPy FFT and matrix DCT do not use the C extraction implementation.
"""
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path
import numpy as np

def reference(pcm,dimensions=13,robust=False):
    y=np.asarray(pcm,dtype=np.float64)/32768
    padded=np.pad(y,(256,256))
    frames=np.lib.stride_tricks.sliding_window_view(padded,512)[::160]
    window=np.pad(.5-.5*np.cos(2*np.pi*np.arange(400)/400),(56,56))
    power=np.abs(np.fft.rfft(frames*window))**2
    mels=np.linspace(0,15+np.log(8)/(np.log(6.4)/27),130)
    hz=np.where(mels<15,mels*(200/3),1000*np.exp((mels-15)*(np.log(6.4)/27)))
    fft_hz=np.arange(257)*16000/512
    up=(fft_hz[None,:]-hz[:-2,None])/(hz[1:-1]-hz[:-2])[:,None]
    down=(hz[2:,None]-fft_hz[None,:])/(hz[2:]-hz[1:-1])[:,None]
    filters=np.maximum(0,np.minimum(up,down))*2/(hz[2:]-hz[:-2])[:,None]
    db=10*np.log10(np.maximum(1e-10,power@filters.T))
    db=np.maximum(db,db.max()-80)
    dct=np.cos(np.pi*np.arange(dimensions)[:,None]*(np.arange(128)+.5)/128)*np.sqrt(2/128)
    dct[0]/=np.sqrt(2)
    result=db@dct.T
    if robust:
        scale=1+11*np.sin(np.pi*np.arange(dimensions)/44);scale[0]=0
        result*=scale
    return result

def main():
    binary=sys.argv[1]
    rng=np.random.default_rng(20261028)
    signals=[np.zeros(16000,dtype=np.int16),
             (10000*np.sin(2*np.pi*440*np.arange(14400)/16000)).astype(np.int16),
             rng.integers(-20000,20001,17600,dtype=np.int16)]
    for source in sys.argv[2:]:
        with wave.open(source,'rb') as w: signals.append(np.frombuffer(w.readframes(w.getnframes()),dtype='<i2'))
    with tempfile.TemporaryDirectory() as tmp:
        for index,pcm in enumerate(signals):
            wav=Path(tmp)/'test.wav'; model=Path(tmp)/'test.lwt'
            with wave.open(str(wav),'wb') as w:
                w.setparams((1,2,16000,0,'NONE','not compressed')); w.writeframes(pcm.tobytes())
            subprocess.run([binary,'train','test',str(model),str(wav)],check=True,capture_output=True)
            data=model.read_bytes()
            magic,version,count,labellen=struct.unpack_from('<4I',data)
            assert magic==0x3154574c and version in (4,8) and count==1
            frames,dim=struct.unpack_from('<2I',data,16+labellen)
            actual=np.frombuffer(data,dtype='<f4',offset=24+labellen).reshape(frames,dim)
            expected=reference(pcm,dim,version==8)
            assert actual.shape==expected.shape
            err=float(np.max(np.abs(actual-expected)))
            assert err<(.03 if version==8 else .003),(index,err)
            print(f'PASS MFCC reference case={index} frames={frames} max_abs_error={err:.6f}')

if __name__=='__main__': main()
