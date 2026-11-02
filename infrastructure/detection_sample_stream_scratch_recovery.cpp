#include "infrastructure/detection_sample_stream_scratch_recovery.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace signalstudio::sample_stream::scratch_recovery {
namespace {
constexpr auto ownerName="owner.json";
constexpr auto lockName="owner.lock";
constexpr auto magic="SignalStudioSampleScratchOwner";
constexpr std::uint64_t hardQuota=32ULL<<30;
constexpr qsizetype ownerMaxBytes=4096;
const QRegularExpression directoryPattern(QStringLiteral("^sample-[A-Za-z0-9]{6}$"));
QMutex activeMutex;
QHash<QString,QString> activeTokens; // hard cap256; do not trust PID equality after PID reuse
bool registerActive(const QString& token,const QString& directory) {
  QMutexLocker locker(&activeMutex);if(activeTokens.size()>=256)return false;
  activeTokens.insert(token,directory);return true;
}
void unregisterActive(const QString& token) {QMutexLocker locker(&activeMutex);activeTokens.remove(token);}
bool registeredActive(const QString& token,const QString& directory) {
  QMutexLocker locker(&activeMutex);return QString::compare(activeTokens.value(token),directory,Qt::CaseInsensitive)==0;
}
QString normalized(const QString& path) {return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));}
bool same(const QString& a,const QString& b) {
#ifdef Q_OS_WIN
  return QString::compare(normalized(a),normalized(b),Qt::CaseInsensitive)==0;
#else
  return normalized(a)==normalized(b);
#endif
}
bool stop(const Limits& limits,QElapsedTimer& timer,Report& r) {
  if(limits.cancelled && limits.cancelled()){r.cancelled=true;return true;}
  return timer.elapsed()>=limits.maximumMilliseconds;
}
void note(Report& r,QString text) {if(r.diagnostics.size()<32)r.diagnostics.push_back(text.left(512));}
bool unsignedText(const QJsonObject& object,const char* key,std::uint64_t& result) {
  const auto value=object.value(QLatin1String(key));if(!value.isString())return false;
  const auto text=value.toString();if(text.isEmpty() || text.size()>20)return false;
  for(auto ch:text)if(ch<'0'||ch>'9')return false;
  bool ok=false;result=text.toULongLong(&ok);return ok;
}
struct Owner {
  QString root,name,token,host;
  std::uint64_t pid=0,samples=0,origin=0,reserved=0,created=0;
};
QJsonObject json(const Owner& o) {
  return {{"magic",magic},{"version",1},{"root",o.root},{"directory",o.name},{"token",o.token},
      {"host",o.host},{"pid",QString::number(o.pid)},{"samples",QString::number(o.samples)},
      {"origin",QString::number(o.origin)},{"reservedBytes",QString::number(o.reserved)},
      {"createdUtcMs",QString::number(o.created)}};
}
bool parse(const QByteArray& bytes,const QString& root,const QString& name,Owner& o,QString& why) {
  QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
  if(error.error!=QJsonParseError::NoError || !doc.isObject()) {why="Invalid scratch owner JSON";return false;}
  const auto v=doc.object();
  if(v.size()!=11 || v.value("magic")!=magic || v.value("version")!=1 ||
     !v.value("root").isString() || !v.value("directory").isString() || !v.value("token").isString() || !v.value("host").isString() ||
     !unsignedText(v,"pid",o.pid) || !unsignedText(v,"samples",o.samples) || !unsignedText(v,"origin",o.origin) ||
     !unsignedText(v,"reservedBytes",o.reserved) || !unsignedText(v,"createdUtcMs",o.created)) {
    why="Owner magic/schema/fields rejected";return false;
  }
  o.root=v.value("root").toString();o.name=v.value("directory").toString();
  o.token=v.value("token").toString();o.host=v.value("host").toString();
  if(!same(o.root,root) || o.name!=name || !directoryPattern.match(name).hasMatch() ||
     QUuid(o.token).isNull() || o.token.size()!=36 || o.host.isEmpty() || o.host.size()>255 ||
     QString::compare(o.host,QSysInfo::machineHostName(),Qt::CaseInsensitive)!=0 ||
     !o.pid || o.pid>std::numeric_limits<std::uint32_t>::max() || !o.samples || o.samples>(std::uint64_t{1}<<34) ||
     o.origin>std::numeric_limits<std::uint64_t>::max()-o.samples || !o.created ||
     o.reserved<8*o.samples+8192 || o.reserved>hardQuota) {
    why="Owner identity/context/quota rejected";return false;
  }
  return true;
}
#ifdef Q_OS_WIN
// Handle-only deletion and ancestors opened WITHOUT FILE_SHARE_DELETE. This
// prevents a validated parent/candidate from being renamed out from under us.
class Pin {
 public:
  HANDLE handle=INVALID_HANDLE_VALUE;
  BY_HANDLE_FILE_INFORMATION info{};
  QString path;
  Pin()=default;~Pin(){close();}
  Pin(const Pin&)=delete;Pin& operator=(const Pin&)=delete;
  Pin(Pin&& other) noexcept:handle(std::exchange(other.handle,INVALID_HANDLE_VALUE)),info(other.info),path(std::move(other.path)){}
  Pin& operator=(Pin&& other) noexcept {if(this!=&other){close();handle=std::exchange(other.handle,INVALID_HANDLE_VALUE);info=other.info;path=std::move(other.path);}return *this;}
  void close(){if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}
  bool open(const QString& value,bool directory,bool deletable,QString& error,bool allowRename=false) {
    path=normalized(value);
    // Attribute-only Windows opens do not enforce rename sharing constraints.
    // Use the standard directory-list access so denying SHARE_DELETE pins it.
    const DWORD access=FILE_READ_ATTRIBUTES|(directory?FILE_LIST_DIRECTORY:GENERIC_READ)|(deletable?DELETE:0);
    const DWORD share=directory?(FILE_SHARE_READ|FILE_SHARE_WRITE|(allowRename?FILE_SHARE_DELETE:0)):FILE_SHARE_READ;
    handle=CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),access,share,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
    if(handle==INVALID_HANDLE_VALUE){error="Cannot pin owned path: "+path+"; win32="+QString::number(GetLastError());return false;}
    if(!GetFileInformationByHandle(handle,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
       bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory || (!directory&&info.nNumberOfLinks!=1)) {
      error="Reparse/type/hard-link identity rejected: "+path;return false;
    }
    std::array<wchar_t,32768> buffer{};
    const auto length=GetFinalPathNameByHandleW(handle,buffer.data(),static_cast<DWORD>(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!length || length>=buffer.size()){error="Cannot verify pinned final path";return false;}
    auto final=QString::fromWCharArray(buffer.data(),static_cast<qsizetype>(length));
    if(final.startsWith("\\\\?\\"))final.remove(0,4);
    if(!same(final,path)){error="Pinned parent/final path drift: "+path;return false;}
    return true;
  }
  std::uint64_t size() const {return (std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;}
  bool prefix(std::size_t maximum,QByteArray& bytes,QString& error) {
    const auto count=static_cast<DWORD>(std::min<std::uint64_t>(size(),maximum));
    if(!count){bytes.clear();return true;}
    bytes.resize(count);DWORD read=0;LARGE_INTEGER zero{};
    if(!SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN) || !ReadFile(handle,bytes.data(),count,&read,nullptr) || read!=count) {
      error="Cannot read pinned ownership/header";return false;
    }
    return true;
  }
  bool erase(QString& error) {
    FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
    if(!SetFileInformationByHandle(handle,FileDispositionInfo,&disposition,sizeof(disposition))) {
      error="Owned handle deletion failed: "+path+"; win32="+QString::number(GetLastError());return false;
    }
    close();return true;
  }
};
struct RootPins {
  QString root;
  std::vector<Pin> ancestors;
  bool open(const QString& value,QString& error) {
    root=normalized(value);
    if(root.size()<4 || root[1]!=':' || root[2]!='/' || root.startsWith("//")) {error="Only private local DOS-volume scratch roots supported";return false;}
    const auto drive=root.left(3);const auto type=GetDriveTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(drive).utf16()));
    if(type!=DRIVE_FIXED && type!=DRIVE_REMOVABLE){error="Remote/unknown scratch volume rejected";return false;}
    QStringList chain;auto path=root;
    for(;;) {
      if(chain.size()==128){error="Scratch ancestry exceeds bound";return false;}
      chain.prepend(path);const auto parent=QFileInfo(path).absolutePath();if(same(parent,path))break;path=parent;
    }
    for(const auto& directory:chain){Pin pin;if(!pin.open(directory,true,false,error))return false;ancestors.push_back(std::move(pin));}
    return true;
  }
};
enum class Life { Alive, Dead, Unknown };
Life life(std::uint64_t pid) {
  const auto process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(pid));
  if(!process)return GetLastError()==ERROR_INVALID_PARAMETER?Life::Dead:Life::Unknown;
  const auto status=WaitForSingleObject(process,0);CloseHandle(process);
  return status==WAIT_OBJECT_0?Life::Dead:(status==WAIT_TIMEOUT?Life::Alive:Life::Unknown);
}
bool ownerRead(const QString& directory,const QString& root,Owner& o,QString& error) {
  Pin file;if(!file.open(directory+"/"+ownerName,false,false,error))return false;
  if(file.size()==0 || file.size()>ownerMaxBytes){error="Owner JSON size rejected";return false;}
  QByteArray bytes;if(!file.prefix(ownerMaxBytes,bytes,error))return false;
  return parse(bytes,root,QFileInfo(directory).fileName(),o,error);
}
struct FileSet {
  std::vector<Pin> data;
  Pin owner;
  std::uint64_t bytes=0;
  std::uint32_t count=0;
};
bool whitelist(const QString& directory,const Owner& expected,bool requireOwner,
               std::uint32_t maximum,FileSet& set,QString& error,
               const std::function<bool()>& shouldStop={}) {
  QDirIterator entries(directory,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDirIterator::NoIteratorFlags);
  bool foundOwner=false,foundLock=false;
  while(entries.hasNext()) {
    if(shouldStop && shouldStop()){error="Recovery batch deadline/cancellation reached";return false;}
    entries.next();if(++set.count>maximum){error="Owned directory file-count bound exceeded";return false;}
    const auto name=entries.fileName();
    if(name!=ownerName && name!=lockName && name!="energy.f32" && name!="smooth.f32") {error="Foreign file/directory in owned candidate: "+name;return false;}
    if(name==lockName) {
      const auto path=entries.filePath();const auto attrs=GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
      if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
         entries.fileInfo().size()<0 || entries.fileInfo().size()>4096){error="Lock file type/size rejected";return false;}
      foundLock=true;set.bytes+=static_cast<std::uint64_t>(entries.fileInfo().size());continue;
    }
    Pin file;if(!file.open(entries.filePath(),false,true,error))return false;
    if(file.size()>expected.reserved || set.bytes>expected.reserved-file.size()){error="Owned file aggregate quota exceeded";return false;}
    set.bytes+=file.size();QByteArray head;
    if(name==ownerName) {
      foundOwner=true;
      if(file.size()>ownerMaxBytes || !file.prefix(ownerMaxBytes,head,error))return false;
      if(requireOwner) {
        Owner actual;if(!parse(head,expected.root,expected.name,actual,error) || actual.token!=expected.token ||
          actual.pid!=expected.pid || actual.samples!=expected.samples || actual.origin!=expected.origin || actual.reserved!=expected.reserved) {
          error="Owner changed while taking recovery lock";return false;
        }
      }
      set.owner=std::move(file);
    } else {
      if(file.size()>64+4*expected.samples || !file.prefix(24,head,error)){error="Owned scalar size/read rejected";return false;}
      const QByteArray tag("SSSE0001",8);
      if(head.left(std::min<qsizetype>(head.size(),8))!=tag.left(std::min<qsizetype>(head.size(),8)) ||
         (head.size()>=16 && qFromLittleEndian<quint64>(head.constData()+8)!=expected.samples) ||
         (head.size()>=24 && qFromLittleEndian<quint64>(head.constData()+16)!=expected.origin)) {
        error="Scalar header conflicts with owner context";return false;
      }
      set.data.push_back(std::move(file)); // partial crash write is allowed
    }
  }
  if(!foundLock || (requireOwner&&!foundOwner) || set.bytes>expected.reserved) {error="Owner/lock missing or final aggregate quota exceeded";return false;}
  return true;
}
bool cleanupSet(Pin& directory,QLockFile& lock,FileSet& files,QString& error,
                const std::function<bool()>& shouldStop={}) {
  // All files were validated/pinned BEFORE the first mutation. No recursive delete.
  for(auto& file:files.data) {
    if(shouldStop&&shouldStop()){error="Recovery stopped before scalar mutation";return false;}
    if(!file.erase(error))return false;
  }
  if(shouldStop&&shouldStop()){error="Recovery stopped before owner mutation";return false;}
  if(files.owner.handle!=INVALID_HANDLE_VALUE && !files.owner.erase(error))return false;
  lock.unlock(); // only OUR acquired lock; never force-remove someone else's
  return directory.erase(error); // exact anchored empty directory, not a path walk
}
#endif
} // namespace

struct OwnerGuard::Impl {
  Owner owner;
  QString directory;
  std::unique_ptr<QLockFile> lock;
  bool created=false,ownerComplete=false;
#ifdef Q_OS_WIN
  RootPins roots;
  Pin pinnedDirectory;
#endif
};
OwnerGuard::OwnerGuard():impl_(std::make_unique<Impl>()){}
OwnerGuard::~OwnerGuard() {
  if(impl_){unregisterActive(impl_->owner.token);if(impl_->lock)impl_->lock->unlock();}
} // unlock while path anchors still held
bool OwnerGuard::begin(const QString& root,const QString& directory,std::uint64_t samples,
                       std::uint64_t origin,std::uint64_t reserved,QString& error) {
#ifndef Q_OS_WIN
  Q_UNUSED(root);Q_UNUSED(directory);Q_UNUSED(samples);Q_UNUSED(origin);Q_UNUSED(reserved);
  error="Anchored scratch ownership currently requires Windows local filesystem";return false;
#else
  if(impl_->created){error="OwnerGuard already active";return false;}
  auto& o=impl_->owner;o.root=normalized(root);o.name=QFileInfo(directory).fileName();
  o.token=QUuid::createUuid().toString(QUuid::WithoutBraces);o.host=QSysInfo::machineHostName();
  o.pid=static_cast<std::uint64_t>(QCoreApplication::applicationPid());o.samples=samples;o.origin=origin;o.reserved=reserved;
  o.created=static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
  Owner validated;
  if(!parse(QJsonDocument(json(o)).toJson(QJsonDocument::Compact),o.root,o.name,validated,error) ||
      !same(QFileInfo(directory).absolutePath(),o.root) || !impl_->roots.open(root,error) ||
      !impl_->pinnedDirectory.open(directory,true,true,error))return false;
  impl_->directory=normalized(directory);impl_->created=true;
  QDirIterator initial(directory,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDirIterator::NoIteratorFlags);
  if(initial.hasNext()) {
    error="OwnerGuard requires newly-created empty candidate directory";impl_->created=false;return false;
  }
  impl_->lock=std::make_unique<QLockFile>(impl_->directory+"/"+lockName);impl_->lock->setStaleLockTime(0);
  if(!impl_->lock->tryLock(0)){error="Cannot acquire private owner lock";return false;}
  const auto bytes=QJsonDocument(json(o)).toJson(QJsonDocument::Compact);
  if(bytes.size()>ownerMaxBytes){error="Owner record exceeds fixed size";return false;}
  QFile file(impl_->directory+"/"+ownerName);
  if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly) || file.write(bytes)!=bytes.size() || !file.flush()) {
    error="Cannot persist scratch owner before scalar allocation";return false;
  }
  file.close();impl_->ownerComplete=true;
  if(!registerActive(o.token,impl_->directory)){error="Active owner registry hard cap exceeded";return false;}
  return true;
#endif
}
bool OwnerGuard::cleanup(QString& error) {
  if(!impl_->created)return true;
#ifndef Q_OS_WIN
  error="Anchored scratch cleanup unsupported";return false;
#else
  if(!impl_->lock){error="Owned candidate lock unavailable";return false;}
  if(!impl_->lock->isLocked() && !impl_->lock->tryLock(0)){error="Cannot reacquire own cleanup lock";return false;}
  FileSet files;
  if(!whitelist(impl_->directory,impl_->owner,impl_->ownerComplete,4,files,error) ||
     !cleanupSet(impl_->pinnedDirectory,*impl_->lock,files,error))return false;
  unregisterActive(impl_->owner.token);
  impl_->created=false;impl_->lock.reset();impl_->roots.ancestors.clear();return true;
#endif
}
bool OwnerGuard::ownsDirectory() const {return impl_->created;}
std::uint64_t OwnerGuard::metadataBytes() const {
  if(!impl_->created)return 0;
  const auto owner=QFileInfo(impl_->directory+"/"+ownerName).size(),lock=QFileInfo(impl_->directory+"/"+lockName).size();
  return static_cast<std::uint64_t>(std::max<qint64>(0,owner)+std::max<qint64>(0,lock));
}

bool prepareTrustedRoot(const QString& root,QString& error) {
#ifndef Q_OS_WIN
  Q_UNUSED(root);error="Anchored scratch roots currently require Windows local filesystem";return false;
#else
  const auto absolute=normalized(root);
  if(QFileInfo::exists(absolute)){RootPins pins;return pins.open(absolute,error);}
  const auto parent=QFileInfo(absolute).absolutePath();RootPins parentPins;
  if(!parentPins.open(parent,error))return false;
  if(!CreateDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(absolute).utf16()),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) {
    error="Cannot create final trusted scratch root leaf";return false;
  }
  RootPins childPins;return childPins.open(absolute,error);
#endif
}

bool recoverOrphans(const QString& root,const Limits& limits,Report& output,QString& error) {
  Report r;
  if(!limits.maximumRootEntries || limits.maximumRootEntries>4096 || !limits.maximumCandidateDirectories || limits.maximumCandidateDirectories>256 ||
     !limits.maximumFilesPerDirectory || limits.maximumFilesPerDirectory>4 || !limits.maximumMilliseconds || limits.maximumMilliseconds>5000 ||
     !limits.maximumReclaimedBytes || limits.maximumReclaimedBytes>hardQuota) {error="Invalid finite scratch recovery limits";return false;}
#ifndef Q_OS_WIN
  Q_UNUSED(root);r.supported=false;output=r;error="Anchored recovery currently supports Windows local filesystem only";return true;
#else
  QElapsedTimer timer;timer.start();RootPins roots;
  if(!roots.open(root,error))return false;
  r.supported=true;
  QDirIterator entries(roots.root,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDirIterator::NoIteratorFlags);
  bool bounded=false;
  while(entries.hasNext()) {
    if(stop(limits,timer,r) || r.rootEntriesVisited>=limits.maximumRootEntries || r.candidatesVisited>=limits.maximumCandidateDirectories) {bounded=true;break;}
    entries.next();++r.rootEntriesVisited;
    const auto name=entries.fileName();
    if(!name.startsWith("sample-"))continue; // NEVER enter noncandidate entries
    ++r.candidatesVisited;
    if(!directoryPattern.match(name).hasMatch()){++r.invalidOrForeignSkipped;note(r,name+": invalid fixed-prefix name");continue;}
    Pin inspected;QString why;
    if(!inspected.open(entries.filePath(),true,false,why,true)){++r.invalidOrForeignSkipped;note(r,why);continue;}
    Owner o;
    if(!ownerRead(entries.filePath(),roots.root,o,why)){++r.invalidOrForeignSkipped;note(r,name+": "+why);continue;}
    const auto ownerLife=life(o.pid);
    if(ownerLife==Life::Alive) {
      if(o.pid==static_cast<std::uint64_t>(QCoreApplication::applicationPid()) && !registeredActive(o.token,normalized(entries.filePath()))) {
        ++r.unknownProcessSkipped;note(r,name+": same PID is not a registered live owner (PID reuse/untracked owner)");
      } else {
        ++r.activeOwnersSkipped;r.activeOtherProcessOwners+=o.pid!=static_cast<std::uint64_t>(QCoreApplication::applicationPid());
      }
      r.retainedOwnedReservationBytes+=o.reserved;continue;
    }
    if(ownerLife!=Life::Dead){++r.unknownProcessSkipped;r.retainedOwnedReservationBytes+=o.reserved;note(r,name+": cannot prove owner process dead");continue;}
    // Acquire the non-renamable candidate anchor BEFORE constructing QLockFile,
    // so every lock destructor also runs while the anchored directory is held.
    Pin candidate;
    if(!candidate.open(entries.filePath(),true,true,why) ||
       candidate.info.dwVolumeSerialNumber!=inspected.info.dwVolumeSerialNumber ||
       candidate.info.nFileIndexHigh!=inspected.info.nFileIndexHigh || candidate.info.nFileIndexLow!=inspected.info.nFileIndexLow) {
      ++r.invalidOrForeignSkipped;r.retainedOwnedReservationBytes+=o.reserved;note(r,name+": candidate identity drift while locking");continue;
    }
    const auto lockPath=entries.filePath()+"/"+lockName;
    const auto attrs=GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(lockPath).utf16()));
    const auto attributeError=attrs==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
    QLockFile lock(lockPath);lock.setStaleLockTime(0);
    if(attrs!=INVALID_FILE_ATTRIBUTES) {
      Pin oldLock;
      if(!oldLock.open(lockPath,false,false,why) || oldLock.size()>4096) {
        ++r.invalidOrForeignSkipped;r.retainedOwnedReservationBytes+=o.reserved;note(r,name+": lock metadata size/link bound rejected");continue;
      }
      qint64 pid=0;QString host,app;
      const bool lockInfoRead=lock.getLockInfo(&pid,&host,&app);
      QByteArray metadata;
      if(!oldLock.prefix(4096,metadata,why)) {
        ++r.invalidOrForeignSkipped;r.retainedOwnedReservationBytes+=o.reserved;note(r,name+": cannot read bounded lock metadata");continue;
      }
      // Qt Windows can leave hostname empty and record machineUniqueId instead.
      // Empty hostname alone is NOT proof that a stale lock belongs locally.
      const auto lines=metadata.split('\n');const auto machine=QSysInfo::machineUniqueId();
      const bool machineMatches=!machine.isEmpty()&&lines.size()>3&&lines[3].trimmed()==machine;
      const bool identityMatches=host.isEmpty()?machineMatches:QString::compare(host,o.host,Qt::CaseInsensitive)==0;
      if((attrs&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) || !lockInfoRead ||
         pid!=static_cast<qint64>(o.pid) || !identityMatches) {
        ++r.invalidOrForeignSkipped;r.retainedOwnedReservationBytes+=o.reserved;
        note(r,QString("%1: foreign/invalid owner lock; read=%2 lockPid=%3 ownerPid=%4 hostEmpty=%5 machineMatches=%6")
            .arg(name).arg(lockInfoRead).arg(pid).arg(o.pid).arg(host.isEmpty()).arg(machineMatches));continue;
      }
    } else if(attributeError!=ERROR_FILE_NOT_FOUND) {
      ++r.unknownProcessSkipped;r.retainedOwnedReservationBytes+=o.reserved;continue;
    }
    // OS PID is independently dead. Qt may remove its stale lock internally;
    // NEVER call removeStaleLockFile(), never steal an age-expired active lease.
    if(!lock.tryLock(0)){++r.unknownProcessSkipped;r.retainedOwnedReservationBytes+=o.reserved;note(r,name+": dead owner lock could not be acquired");continue;}
    FileSet files;
    if(!whitelist(entries.filePath(),o,true,limits.maximumFilesPerDirectory,files,why,[&]{return stop(limits,timer,r);})) {
      ++r.invalidOrForeignSkipped;r.retainedOwnedReservationBytes+=o.reserved;note(r,name+": "+why);
      if(stop(limits,timer,r)){bounded=true;break;}
      continue;
    }
    r.filesValidated+=files.count;
    if(files.bytes>limits.maximumReclaimedBytes-r.reclaimedBytes){bounded=true;r.retainedOwnedReservationBytes+=o.reserved;break;}
    if(stop(limits,timer,r)){bounded=true;r.retainedOwnedReservationBytes+=o.reserved;break;}
    if(!cleanupSet(candidate,lock,files,why,[&]{return stop(limits,timer,r);})) {
      ++r.invalidOrForeignSkipped;r.retainedOwnedReservationBytes+=o.reserved;note(r,name+": "+why);continue;
    }
    r.reclaimedBytes+=files.bytes;++r.directoriesRemoved;
  }
  r.complete=!bounded&&!r.cancelled;
  r.elapsedMilliseconds=static_cast<std::uint64_t>(timer.elapsed());
  r.timeBudgetReached=r.elapsedMilliseconds>=limits.maximumMilliseconds;
  r.safeToAdmit=r.complete && !r.invalidOrForeignSkipped && !r.unknownProcessSkipped && !r.activeOtherProcessOwners;
  output=std::move(r);return true;
#endif
}
} // namespace signalstudio::sample_stream::scratch_recovery
