#pragma once

#include "domain/project.h"

#include <unordered_map>

namespace signalstudio {

struct DeleteResult {
    int marks = 0;
    int channels = 0;
};

class Session {
public:
    Session();
    Project& project() { return project_; }
    const Project& project() const { return project_; }
    FileState* activeFile();
    const FileState* activeFile() const;
    void newProject();
    std::string addDemoFile();
    bool activateFile(const std::string& id);
    bool removeActiveFile();
    std::string addMark(ViewRange range);
    void selectMarks(std::vector<std::string> ids, std::string active = {});
    DeleteResult deleteSelectedMarks();
    bool focusMark(const std::string& id);
    bool setView(ViewRange range, bool record = true);
    bool back();
    bool forward();
    bool canBack() const;
    bool canForward() const;
    void resetView();
    bool createChannelFromActiveMark();
    void replaceProject(Project project);

private:
    struct History {
        std::vector<ViewRange> past;
        std::vector<ViewRange> future;
    };
    std::string nextId(const std::string& prefix);
    Project project_;
    std::unordered_map<std::string, History> histories_;
    std::uint64_t nextIdentifier_ = 1;
    std::uint64_t demoSequence_ = 0;
};

} // namespace signalstudio
