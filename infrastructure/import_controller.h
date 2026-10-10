#pragma once
#include "infrastructure/source_loader.h"
#include <memory>
#include <vector>

namespace signalstudio {
enum class ImportStatus { Pending, Reading, Ready, Partial, Failed, Cancelled };
struct ImportRow {
    FileMetadata metadata;
    QString error,provenance,metadataError;
    bool confirmed=false,selected=true,acquisitionKnown=false,committed=false;
    ImportStatus status=ImportStatus::Pending;
    SourceLoadSnapshot load;
};
// Serial scheduling bounds disk pressure and memory; UI only polls immutable snapshots.
class ImportController {
public:
    std::vector<ImportRow>& rows(){return rows_;}
    const std::vector<ImportRow>& rows() const{return rows_;}
    bool add(ImportRow row,QString& error);
    bool start(const std::vector<std::size_t>& indices);
    void poll();
    void stopAll();
    void stopRow(std::size_t);
    bool running() const{return active_!=invalid||!pending_.empty();}
    bool canCommit() const;
    std::vector<FileState> sources() const;
    void restart();
    void markCommitted(const FileMetadata&);
private:
    void next();
    static constexpr std::size_t invalid=static_cast<std::size_t>(-1);
    std::vector<ImportRow> rows_;
    std::vector<std::size_t> pending_,lastRun_;
    std::size_t active_=invalid;
    std::unique_ptr<SourceLoader> loader_;
};
}
