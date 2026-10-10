#pragma once
#include "domain/sample_format.h"
#include <QByteArray>
#include <QtEndian>
#include <bit>
#include <complex>
#include <vector>

namespace signalstudio::fixtures {
// Fixed deterministic values; no external files or random state.
inline std::complex<double> reference(int channel,int sample){return {double(-100+channel*17+sample),double(63-channel*11-sample)};}
inline void appendComponent(QByteArray& b,const SampleFormat& f,double value){
    char buffer[8]{};
    const auto store=[&]<typename T>(T x){if(f.byteOrder==ByteOrder::Big)qToBigEndian<T>(x,buffer);else qToLittleEndian<T>(x,buffer);};
    switch(f.componentEncoding){
    case ComponentEncoding::Int8:buffer[0]=static_cast<char>(static_cast<qint8>(value));break;
    case ComponentEncoding::UInt8:buffer[0]=static_cast<char>(static_cast<quint8>(value+128));break;
    case ComponentEncoding::Int16:store(static_cast<qint16>(value));break;
    case ComponentEncoding::Int32:store(static_cast<qint32>(value));break;
    case ComponentEncoding::Float32:store(std::bit_cast<quint32>(static_cast<float>(value)));break;
    case ComponentEncoding::Float64:store(std::bit_cast<quint64>(value));break;
    }
    b.append(buffer,static_cast<qsizetype>(bytesPerComponent(f)));
}
inline QByteArray generate(const SampleFormat& f,int samples=11){
    QByteArray b(static_cast<qsizetype>(f.headerBytes),'H');
    const bool complex=f.structure==SampleStructure::ComplexIQ;
    const auto component=[&](int c,int n,bool q){const auto z=reference(c,n);appendComponent(b,f,q?z.imag():z.real());};
    const auto sample=[&](int c,int n){if(complex&&f.iqLayout==IQLayout::QIInterleaved){component(c,n,true);component(c,n,false);}
        else {component(c,n,false);if(complex)component(c,n,true);}};
    if(complex&&f.iqLayout==IQLayout::PlanarIQ){
        if(f.channelLayout==ChannelLayout::TimeInterleaved){for(bool q:{false,true})for(int n=0;n<samples;++n)for(std::uint32_t c=0;c<f.channels;++c)component(c,n,q);}
        else for(std::uint32_t c=0;c<f.channels;++c)for(bool q:{false,true})for(int n=0;n<samples;++n)component(c,n,q);
    } else if(f.channelLayout==ChannelLayout::TimeInterleaved){for(int n=0;n<samples;++n)for(std::uint32_t c=0;c<f.channels;++c)sample(c,n);}
    else for(std::uint32_t c=0;c<f.channels;++c)for(int n=0;n<samples;++n)sample(c,n);
    b.append(QByteArray(static_cast<qsizetype>(f.trailerBytes),'T'));return b;
}
}
