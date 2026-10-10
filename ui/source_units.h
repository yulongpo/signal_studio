#pragma once
#include "domain/project.h"
#include <QString>
namespace signalstudio {
inline bool floatingSamples(const FileMetadata& metadata){return metadata.sampleFormat.componentEncoding>=ComponentEncoding::Float32;}
inline QString amplitudeUnit(const FileMetadata& metadata){return floatingSamples(metadata)?"原始幅度":"ADC 计数";}
inline QString powerUnit(const FileMetadata& metadata){return floatingSamples(metadata)?"dB(unit^2/Hz)":metadata.sampleFormat.normalizeIntegerAdc?"dBFS/Hz":"dB(ADC^2/Hz)";}
inline QString sourceUnits(QString label,const FileMetadata& metadata){label.replace("dBFS/Hz",powerUnit(metadata));if(floatingSamples(metadata))label.replace("ADC 计数",amplitudeUnit(metadata));return label;}
}
