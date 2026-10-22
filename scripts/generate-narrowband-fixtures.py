"""Independent transmit waveforms and readable truth; never used by the receiver."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.signal import upfirdn


def rrc(t, alpha):
    h = np.empty_like(t)
    zero = np.isclose(t, 0)
    singular = np.isclose(np.abs(t), 1 / (4 * alpha))
    regular = ~(zero | singular)
    x = t[regular]
    h[regular] = (np.sin(np.pi*x*(1-alpha)) + 4*alpha*x*np.cos(np.pi*x*(1+alpha))) / (np.pi*x*(1-(4*alpha*x)**2))
    h[zero] = 1 + alpha*(4/np.pi-1)
    h[singular] = alpha/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*alpha)) + (1-2/np.pi)*np.cos(np.pi/(4*alpha)))
    return h


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    root = Path(parser.parse_args().output)
    root.mkdir(parents=True, exist_ok=True)
    fs, rs, sps, count = 20_000_000, 625_000, 32, 8192
    rng = np.random.default_rng(20261022)
    records = []

    def write(name, x, truth, symbols=None):
        x = x.astype(np.complex64)
        raw = np.stack((x.real, x.imag), axis=-1)
        raw.astype('<f4').tofile(root / (name + '.sigmf-data'))
        np.rint(np.clip(raw, -1, 32767/32768)*32768).astype('<i2').tofile(root / (name + '.raw'))
        meta = {'global': {'core:datatype': 'cf32_le', 'core:sample_rate': fs, 'core:version': '1.0.0'},
                'captures': [{'core:sample_start': 0, 'core:frequency': 100_000_000}], 'annotations': []}
        (root / (name + '.sigmf-meta')).write_text(json.dumps(meta, indent=2), encoding='utf-8')
        truth.update(name=name, sourceRateHz=fs, sourceCenterHz=100_000_000,
                     channelCenterHz=102_000_000, channelBandwidthHz=1_000_000,
                     outputRateHz=2_000_000, sampleCount=len(x), syntheticTransmitter=True)
        if symbols is not None:
            np.stack((symbols.real, symbols.imag), axis=-1).astype('<f4').tofile(root / (name + '.symbols'))
        (root / (name + '.truth.json')).write_text(json.dumps(truth, indent=2), encoding='utf-8')
        records.append(truth)

    for modulation in ('BPSK', 'QPSK', '16QAM'):
        if modulation == 'BPSK':
            symbols = (2*rng.integers(0, 2, count)-1).astype(complex)
        elif modulation == 'QPSK':
            symbols = ((2*rng.integers(0, 2, count)-1) + 1j*(2*rng.integers(0, 2, count)-1))/np.sqrt(2)
        else:
            symbols = ((2*rng.integers(0, 4, count)-3) + 1j*(2*rng.integers(0, 4, count)-3))/np.sqrt(10)
        for alpha in (.2, .35, .5):
            # Fractional timing is synthesized in the transmit pulse itself.
            delay_ui, span = .23, 12
            taps = rrc(np.arange(-span*sps//2, span*sps//2+1)/sps-delay_ui, alpha)
            base = .25 * upfirdn(taps, symbols, up=sps)
            for snr in (None, 20):
                cfo = 0 if snr is None else rs*.025
                t = np.arange(len(base))/fs
                x = base*np.exp(1j*(2*np.pi*(2_000_000+cfo)*t+.43))
                if snr is not None:
                    variance = np.mean(np.abs(base)**2)*10**(-snr/10)
                    x += np.sqrt(variance/2)*(rng.standard_normal(len(x))+1j*rng.standard_normal(len(x)))
                name = f'{modulation.lower()}-a{int(alpha*100):02d}-' + ('ideal' if snr is None else 'awgn20')
                write(name, x, dict(modulation=modulation, symbolRateHz=rs, alpha=alpha,
                      cfoHz=cfo, initialPhase=.43, timingOffsetUi=delay_ui,
                      symbolOriginSourceSample=span*sps/2+delay_ui*sps,
                      symbolCount=count, snrDb=snr, gain=.25), symbols)

    n = 400_000
    t = np.arange(n)/fs
    write('tone', .25*np.exp(2j*np.pi*2_050_000*t), dict(modulation='Tone', basebandToneHz=50_000))
    noise = .2*(rng.standard_normal(n)+1j*rng.standard_normal(n))/np.sqrt(2)
    write('noise', noise, dict(modulation='Noise'))
    bits = rng.integers(0, 2, (n+sps-1)//sps)
    f = np.repeat((bits*2-1)*rs/4, sps)[:n]
    write('fsk', .25*np.exp(2j*np.pi*(2_000_000*t+np.cumsum(f)/fs)), dict(modulation='FSK', symbolRateHz=rs))
    from scipy.ndimage import gaussian_filter1d
    gf = gaussian_filter1d(f, sps*.3)
    write('gmsk', .25*np.exp(2j*np.pi*(2_000_000*t+np.cumsum(gf)/fs)), dict(modulation='GMSK', symbolRateHz=rs))
    carriers = np.zeros((1200, 64), complex)
    carriers[:, 20:44] = (rng.choice((-1, 1), (1200, 24))+1j*rng.choice((-1, 1), (1200, 24)))/np.sqrt(2)
    ofdm = np.fft.ifft(np.fft.ifftshift(carriers, axes=1), axis=1)*2
    ofdm = np.concatenate((ofdm[:, -16:], ofdm), axis=1).reshape(-1)
    ofdm = np.repeat(ofdm, 4)[:n]
    write('ofdm', ofdm*np.exp(2j*np.pi*2_000_000*np.arange(len(ofdm))/fs), dict(modulation='OFDM'))
    short = np.zeros(n, complex)
    short[10000:11024] = .25*np.exp(2j*np.pi*2_000_000*t[10000:11024])
    write('short-burst', short, dict(modulation='ShortBurst', activeSourceRange=[10000,11024]))
    write('hop', .25*np.exp(2j*np.pi*(2_000_000*t+np.cumsum(np.where(t < .01, -180_000, 180_000))/fs)), dict(modulation='Hopping'))
    thresholds = dict(cacheAbsoluteError=1e-6, interpolationNrmse=1e-3, rateRelativeError=.01,
        residualCfoOverRs=.001, timingIdealUi=.05, timingAwgn20Ui=.10, evmIdeal=.05, evmAwgn20=.18)
    (root/'index.json').write_text(json.dumps(dict(seed=20261022, thresholds=thresholds, records=records), indent=2), encoding='utf-8')
    print(f'Generated {len(records)} deterministic transmit records (CI16 RAW and CF32 SigMF); no RF capture claims. {root}')


if __name__ == '__main__':
    main()
