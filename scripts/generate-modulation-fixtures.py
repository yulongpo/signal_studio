"""Independent family/order transmitter. No receiver code or template input."""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.signal import upfirdn
import runpy
rrc = runpy.run_path(str(Path(__file__).with_name('generate-narrowband-fixtures.py')))['rrc']

def geometry(family, m):
    if family == 'psk':
        return np.exp(1j*(2*np.pi*np.arange(m)/m + (np.pi/4 if m == 4 else 0)))
    if family == 'qam':
        bits = int(np.log2(m))
        if bits % 2 == 0 or bits == 3:
            x, y = np.meshgrid(np.arange(2**((bits+1)//2)), np.arange(2**(bits//2)))
            points = (2*x.ravel()-x.shape[1]+1) + 1j*(2*y.ravel()-y.shape[0]+1)
        else:
            k = 2**((bits-5)//2)
            xy = np.array([(x,y) for y in range(6*k) for x in range(6*k)
                           if not ((x<k or x>=5*k) and (y<k or y>=5*k))])
            points = 2*xy[:,0]-6*k+1 + 1j*(2*xy[:,1]-6*k+1)
        return points/np.sqrt(np.mean(np.abs(points)**2))
    rings = {
        8: [(2,1,0),(6,2,0)],
        16: [(4,1,45),(12,2.85,15)],
        32: [(4,1,45),(12,2.84,15),(16,5.27,0)],
        64: [(4,1,45),(12,2.4,15),(20,3.7,9),(28,5.2,360/56)],
        128: [(8,1,0),(16,2,0),(24,3,0),(32,4,0),(48,5,0)],
        256: [(4,1,0),(12,2,0),(20,3,0),(28,4,0),(36,5,0),(44,6,0),(52,7,0),(60,8,0)],
    }[m]
    points = np.concatenate([r*np.exp(1j*(angle*np.pi/180 + 2*np.pi*np.arange(n)/n)) for n,r,angle in rings])
    return points/np.sqrt(np.mean(np.abs(points)**2))

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True);args=parser.parse_args()
    root=Path(args.output);root.mkdir(parents=True,exist_ok=True)
    rng=np.random.default_rng(2026102302);fs=20_000_000;rs=625_000;sps=32;count=8192
    records=[]
    for family, orders in [('psk',[2,4,8,16,32,64,128,256]),('qam',[4,8,16,32,64,128,256,512,1024,2048,4096]),('apsk',[8,16,32,64,128,256])]:
        for m in orders:
            points=geometry(family,m);symbols=rng.choice(points,count)
            pulse=rrc(np.arange(-6*sps,6*sps+1)/sps-.23,.35)
            base=.25*upfirdn(pulse,symbols,up=sps);cfo=rs*.004
            for noisy in (False,True):
                noise_db=40 if (family=='psk' and m>=64) or (family=='qam' and m>=512) else 30 if family=='psk' and m==32 else 20
                name=f'{family}{m}-'+(f'awgn{noise_db}' if noisy else 'ideal')
                x=base*np.exp(1j*(2*np.pi*(2_000_000+cfo)*np.arange(len(base))/fs+.43))
                if noisy:
                    variance=np.mean(np.abs(base)**2)*10**(-noise_db/10)
                    x+=np.sqrt(variance/2)*(rng.standard_normal(len(x))+1j*rng.standard_normal(len(x)))
                raw=np.stack((x.real,x.imag),axis=-1)
                np.rint(np.clip(raw,-1,32767/32768)*32768).astype('<i2').tofile(root/(name+'.raw'))
                np.stack((symbols.real,symbols.imag),axis=-1).astype('<f4').tofile(root/(name+'.symbols'))
                records.append(dict(name=name,family=family,order=m,rs=rs,fs=fs,cfo=cfo,snrDb=noise_db if noisy else None,
                                    symbolOrigin=6.23*sps,sps=sps,syntheticTransmitter=True))
    (root/'modulation-index.json').write_text(json.dumps(dict(schemaVersion=2,seed=2026102302,records=records),indent=2),encoding='utf-8')
    print(f'Generated {len(records)} independent family/order IQ records.')

if __name__=='__main__': main()
