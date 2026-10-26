#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "infrastructure/detection_sample_stream_scratch_recovery.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <winioctl.h>
#endif

namespace recovery=signalstudio::sample_stream::scratch_recovery;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void checked(bool ok,const QString& message){if(!ok)throw std::runtime_error(message.toStdString());}
QString parentRoot;
QJsonArray results;
QByteArray read(const QString& path){QFile f(path);check(f.open(QIODevice::ReadOnly),"Cannot read test sentinel");return f.readAll();}
void write(const QString& path,const QByteArray& bytes){QFile f(path);check(f.open(QIODevice::WriteOnly|QIODevice::Truncate),"Cannot write owned test file");check(f.write(bytes)==bytes.size()&&f.flush(),"Test short write");}
QString templatePath(const QString& root={}){return (root.isEmpty()?QDir::tempPath():root)+"/sample-XXXXXX";}
QByteArray scalar(std::uint64_t samples,std::uint64_t origin,int payload=17) {
  QByteArray bytes(64,'\0');std::memcpy(bytes.data(),"SSSE0001",8);
  qToLittleEndian<quint64>(samples,bytes.data()+8);qToLittleEndian<quint64>(origin,bytes.data()+16);
  bytes.append(QByteArray(payload,'\x12'));return bytes; // incomplete crash write permitted
}
int ownerChild(const QStringList& args) {
  check(args.size()>=3,"owner-child root required");
  QTemporaryDir directory(templatePath(args[2]));check(directory.isValid(),"Cannot create child candidate");directory.setAutoRemove(false);
  recovery::OwnerGuard guard;QString error;
  checked(guard.begin(args[2],directory.path(),1000,123,8*1000+8192,error),error);
  write(directory.path()+"/energy.f32",scalar(1000,123));write(directory.path()+"/smooth.f32",scalar(1000,123,3));
  if(args.contains("--foreign"))write(directory.path()+"/foreign.bin","never delete");
  std::cout<<directory.path().toStdString()<<'\n'<<std::flush;
  return QCoreApplication::exec(); // parent really kills process: no destructors
}
struct Child {
  QProcess process;QString directory;
#ifdef Q_OS_WIN
  HANDLE pidPin=nullptr;
#endif
  Child(const QString& root,bool foreign=false) {
    QStringList args{"--owner-child",root};if(foreign)args<<"--foreign";
    process.start(QCoreApplication::applicationFilePath(),args);
    check(process.waitForStarted(10000),"Cannot start native owner child");
    QByteArray stdoutBytes;
    for(int i=0;i<100&&!stdoutBytes.contains('\n');++i){process.waitForReadyRead(100);stdoutBytes+=process.readAllStandardOutput();}
    directory=QString::fromUtf8(stdoutBytes).trimmed();
    check(!directory.isEmpty()&&QFile::exists(directory+"/owner.json"),"Child owner readiness missing");
#ifdef Q_OS_WIN
    pidPin=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(process.processId()));
    check(pidPin!=nullptr,"Cannot pin child PID against reuse");
#endif
  }
  void hardKill() {
    process.kill();check(process.waitForFinished(10000),"Hard-kill owner wait failed");
    check(process.exitStatus()==QProcess::CrashExit,"Owner termination was not actual hard kill");
  }
  ~Child() {
    if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(10000);}
#ifdef Q_OS_WIN
    if(pidPin)CloseHandle(pidPin);
#endif
  }
};
recovery::Report sweep(const QString& root,recovery::Limits limits={}) {
  recovery::Report r;QString error;checked(recovery::recoverOrphans(root,limits,r,error),error);check(r.supported,"Recovery unsupported on test platform");return r;
}
QString describe(const recovery::Report& r) {
  QJsonArray diagnostics;for(const auto& line:r.diagnostics)diagnostics.append(line);
  return QString::fromUtf8(QJsonDocument(QJsonObject{{"complete",r.complete},{"safeToAdmit",r.safeToAdmit},
      {"cancelled",r.cancelled},{"timeBudgetReached",r.timeBudgetReached},{"elapsedMs",QString::number(r.elapsedMilliseconds)},
      {"rootEntries",static_cast<int>(r.rootEntriesVisited)},{"candidates",static_cast<int>(r.candidatesVisited)},
      {"filesValidated",static_cast<int>(r.filesValidated)},{"removed",static_cast<int>(r.directoriesRemoved)},
      {"active",static_cast<int>(r.activeOwnersSkipped)},{"activeOther",static_cast<int>(r.activeOtherProcessOwners)},
      {"invalid",static_cast<int>(r.invalidOrForeignSkipped)},{"unknown",static_cast<int>(r.unknownProcessSkipped)},
      {"reclaimedBytes",QString::number(r.reclaimedBytes)},{"retainedReservation",QString::number(r.retainedOwnedReservationBytes)},
      {"diagnostics",diagnostics}}).toJson(QJsonDocument::Compact));
}
void activeThenHardKill() {
  QTemporaryDir root(templatePath(parentRoot));check(root.isValid(),"Test root unavailable");
  write(root.path()+"/reference-sentinel.wav","source/reference must remain");
  Child child(root.path());const auto e=read(child.directory+"/energy.f32");
  const auto active=sweep(root.path());
  check(active.activeOwnersSkipped==1 && active.directoriesRemoved==0 && active.activeOtherProcessOwners==1 && !active.safeToAdmit,
        "Recovery touched/forgot active other-process lease");
  check(read(child.directory+"/energy.f32")==e,"Active scalar changed");
  child.hardKill();const auto dead=sweep(root.path());
  const bool recovered=dead.complete&&dead.safeToAdmit&&dead.directoriesRemoved==1&&dead.reclaimedBytes>0;
  if(!recovered)results.append(QJsonObject{{"case","active-then-real-hard-kill"},{"hardKillMeasured",true},{"recoveryReport",describe(dead)}});
  checked(recovered,"Actual hard-killed owner's stale lock/scalars not recovered; report="+describe(dead));
  check(!QFile::exists(child.directory)&&read(root.path()+"/reference-sentinel.wav")=="source/reference must remain",
        "Owned directory retained or source sentinel touched");
  results.append(QJsonObject{{"case","active-then-real-hard-kill"},{"hardKillMeasured",true},
      {"activeOwnersSkipped",static_cast<int>(active.activeOwnersSkipped)},
      {"deadDirectoriesRemoved",static_cast<int>(dead.directoriesRemoved)},{"reclaimedBytes",QString::number(dead.reclaimedBytes)}});
}
void foreignAndCorruptGuards() {
  QTemporaryDir root(templatePath(parentRoot));check(root.isValid(),"Test root unavailable");
  const auto foreign=root.path()+"/sample-FOREIG";check(QDir().mkdir(foreign),"Cannot create foreign directory");
  write(foreign+"/energy.f32","foreign unmarked scalar");
  Child child(root.path(),true);child.hardKill();
  const auto before=read(child.directory+"/energy.f32");const auto r=sweep(root.path());
  check(r.invalidOrForeignSkipped==2&&!r.safeToAdmit&&!r.directoriesRemoved,"Foreign/unmarked candidate accepted");
  check(read(foreign+"/energy.f32")=="foreign unmarked scalar" && read(child.directory+"/energy.f32")==before &&
      QFile::exists(child.directory+"/foreign.bin"),"Foreign validation partially deleted scalar files");
  results.append(QJsonObject{{"case","foreign-and-unmarked"},{"pass",true}});
  // Teardown is this test's PRIVATE QTemporaryDir, not production recovery.
}
void invalidOwnerLockAndQuota() {
  for(int variant=0;variant<6;++variant) {
    QTemporaryDir root(templatePath(parentRoot));check(root.isValid(),"Test root unavailable");
    Child child(root.path());child.hardKill();
    if(variant==0)write(child.directory+"/owner.json","{\"magic\":\"foreign\"}");
    if(variant==1)write(child.directory+"/owner.lock","corrupt foreign lock");
    if(variant==2)write(child.directory+"/energy.f32",QByteArray(64+4*1000+1,'\0'));
    if(variant==3)write(child.directory+"/owner.lock",QByteArray(4097,'x'));
    if(variant==4||variant==5) {
      auto lines=read(child.directory+"/owner.lock").split('\n');check(lines.size()>3,"Qt lock identity lines absent");
      if(variant==4){lines[2].clear();lines[3]="foreign-machine-id";}
      else lines[2]="foreign-host";
      write(child.directory+"/owner.lock",lines.join('\n'));
    }
    const auto before=read(child.directory+"/energy.f32");const auto r=sweep(root.path());
    check(r.invalidOrForeignSkipped==1&&!r.directoriesRemoved&&!r.safeToAdmit,"Invalid owner/lock/size accepted");
    check(read(child.directory+"/energy.f32")==before,"Rejected candidate partly deleted");
  }
  results.append(QJsonObject{{"case","invalid-owner-lock-or-quota"},{"pass",true}});
  {
    QTemporaryDir root(templatePath(parentRoot));check(root.isValid(),"Test root unavailable");
    Child child(root.path());child.hardKill();
    auto owner=QJsonDocument::fromJson(read(child.directory+"/owner.json")).object();
    owner["pid"]=QString::number(QCoreApplication::applicationPid());
    write(child.directory+"/owner.json",QJsonDocument(owner).toJson(QJsonDocument::Compact));
    const auto bytes=read(child.directory+"/energy.f32");const auto r=sweep(root.path());
    check(r.unknownProcessSkipped==1&&!r.safeToAdmit&&!r.directoriesRemoved&&read(child.directory+"/energy.f32")==bytes,
          "PID equality incorrectly treated an unregistered old owner as accounted active lease");
    results.append(QJsonObject{{"case","same-PID-unregistered-owner-admission-guard"},{"pass",true}});
  }
  {
    QTemporaryDir root(templatePath(parentRoot));check(root.isValid(),"Test root unavailable");
    Child child(root.path());child.hardKill();check(QFile::remove(child.directory+"/owner.lock"),"Cannot replace dead test lock");
    QLockFile active(child.directory+"/owner.lock");active.setStaleLockTime(0);check(active.tryLock(0),"Cannot acquire test foreign active lease");
    const auto bytes=read(child.directory+"/energy.f32");const auto lockBytes=read(child.directory+"/owner.lock");
    const auto r=sweep(root.path());check(!r.directoriesRemoved&&r.invalidOrForeignSkipped==1&&active.isLocked(),"Foreign live lock stolen");
    check(read(child.directory+"/energy.f32")==bytes&&read(child.directory+"/owner.lock")==lockBytes,"Foreign active lease/scalars mutated");
    results.append(QJsonObject{{"case","foreign-current-PID-lock-over-dead-owner"},{"pass",true}});
  }
}
void sameProcessAndNormalCleanup() {
  QTemporaryDir root(templatePath(parentRoot));check(root.isValid(),"Test root unavailable");
  QTemporaryDir owned(templatePath(root.path()));check(owned.isValid(),"Candidate unavailable");owned.setAutoRemove(false);
  recovery::OwnerGuard guard;QString error;
  checked(guard.begin(root.path(),owned.path(),1000,123,16192,error),error);
  write(owned.path()+"/energy.f32",scalar(1000,123));
  const auto active=sweep(root.path());check(active.activeOwnersSkipped==1&&active.safeToAdmit&&!active.directoriesRemoved,
      "Same-process active owner misclassified or removed");
  write(owned.path()+"/foreign.bin","forbidden");
  check(!guard.cleanup(error)&&QFile::exists(owned.path()+"/energy.f32"),"Normal cleanup became recursive/unvalidated");
  check(QFile::remove(owned.path()+"/foreign.bin"),"Cannot remove test-owned injected foreign file");
  checked(guard.cleanup(error),error);check(!QFile::exists(owned.path()),"Normal scalar-only cleanup incomplete");
  results.append(QJsonObject{{"case","same-process-normal-cleanup"},{"pass",true}});
}
void finiteScanAndRenameGuard() {
  QTemporaryDir root(templatePath(parentRoot));check(root.isValid(),"Test root unavailable");
  check(QDir().mkdir(root.path()+"/sample-FOREI1")&&QDir().mkdir(root.path()+"/sample-FOREI2"),"Cannot create bounded scan inputs");
  recovery::Limits limit;limit.maximumRootEntries=1;
  const auto r=sweep(root.path(),limit);check(!r.complete&&!r.safeToAdmit&&r.rootEntriesVisited==1,"Root enumeration cap ignored");
  limit={};limit.cancelled=[](){return true;};
  const auto cancelled=sweep(root.path(),limit);check(cancelled.cancelled&&!cancelled.complete&&!cancelled.directoriesRemoved,"Cancellation changed owned data");
#ifdef Q_OS_WIN
  bool attempted=false;const auto moved=root.path()+"-moved";
  limit={};limit.cancelled=[&]{if(!attempted){attempted=true;
    const bool renamed=MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root.path()).utf16()),
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(moved).utf16()),0);
    const auto code=GetLastError();
    if(renamed)check(MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(moved).utf16()),
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root.path()).utf16()),0),"Cannot restore unexpectedly renamed private test directory");
    check(!renamed,"Pinned trusted root was renamed during sweep");
    check(code==ERROR_SHARING_VIOLATION||code==ERROR_ACCESS_DENIED,"Rename was not rejected by a Windows sharing/access guard");
  }return false;};
  sweep(root.path(),limit);check(attempted&&QDir(root.path()).exists()&&!QFile::exists(moved),"Root path-drift check was not exercised");
#endif
  results.append(QJsonObject{{"case","finite-enumeration-cancel-parent-rename"},{"pass",true}});
}
#ifdef Q_OS_WIN
bool junction(const QString& link,const QString& target,QString& error) {
  if(!CreateDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16()),nullptr)){error="Cannot create junction leaf";return false;}
  const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16()),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  if(handle==INVALID_HANDLE_VALUE){QDir().rmdir(link);error="Cannot open test junction";return false;}
  const QString substitute="\\??\\"+QDir::toNativeSeparators(target),print=QDir::toNativeSeparators(target);
  struct Header {DWORD tag;WORD length,reserved,subOffset,subLength,printOffset,printLength;};
  const auto pathBytes=(substitute.size()+1+print.size()+1)*sizeof(wchar_t);
  std::vector<unsigned char> buffer(sizeof(Header)+static_cast<std::size_t>(pathBytes),0);
  auto* h=reinterpret_cast<Header*>(buffer.data());h->tag=IO_REPARSE_TAG_MOUNT_POINT;
  h->length=static_cast<WORD>(8+pathBytes);h->subLength=static_cast<WORD>(substitute.size()*2);
  h->printOffset=h->subLength+2;h->printLength=static_cast<WORD>(print.size()*2);
  std::memcpy(buffer.data()+sizeof(Header),substitute.utf16(),h->subLength);
  std::memcpy(buffer.data()+sizeof(Header)+h->printOffset,print.utf16(),h->printLength);
  DWORD returned=0;const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr,0,&returned,nullptr);
  const auto code=GetLastError();CloseHandle(handle);
  if(!ok){QDir().rmdir(link);error="Cannot create test junction: win32="+QString::number(code);return false;}return true;
}
void reparseGuards() {
  QTemporaryDir root(templatePath(parentRoot)),outside(templatePath(parentRoot));check(root.isValid()&&outside.isValid(),"Junction test roots unavailable");
  write(outside.path()+"/source.wav","outside source must survive");
  const auto link=root.path()+"/sample-LINKAA";QString error;
  checked(junction(link,outside.path(),error),error);
  struct JunctionCleanup {
    QString path;
    ~JunctionCleanup(){RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));}
  } cleanup{link}; // unlink junction before either private QTemporaryDir teardown
  const auto r=sweep(root.path());check(r.invalidOrForeignSkipped==1&&!r.directoriesRemoved&&!r.safeToAdmit,"Reparse candidate accepted");
  recovery::Report report;
  check(!recovery::recoverOrphans(link,{},report,error),"Reparse scratch root accepted");
  check(QDir().mkdir(outside.path()+"/nested-scratch"),"Cannot create ancestor-junction test leaf");
  write(outside.path()+"/nested-scratch/source.wav","ancestor source must survive");
  check(!recovery::recoverOrphans(link+"/nested-scratch",{},report,error),"Ancestor junction concealed by ordinary leaf accepted");
  check(read(outside.path()+"/source.wav")=="outside source must survive","Reparse traversal touched outside data");
  check(read(outside.path()+"/nested-scratch/source.wav")=="ancestor source must survive","Ancestor reparse traversal touched source");
  check(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16())),"Cannot remove test junction ONLY");
  results.append(QJsonObject{{"case","native-junction-candidate-and-root"},{"pass",true}});
}
#endif
} // namespace
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);const auto args=app.arguments();QString reportPath;
  const auto save=[&](bool success,QString error) {
    if(reportPath.isEmpty())return true;
    QFile f(reportPath);if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;
    const auto bytes=QJsonDocument(QJsonObject{{"schema","sample-scratch-native-hard-kill-v1"},
        {"success",success},{"error",error},{"cases",results}}).toJson();
    return f.write(bytes)==bytes.size()&&f.flush();
  };
  try {
    if(args.size()>1&&args[1]=="--owner-child")return ownerChild(args);
    const auto root=args.indexOf("--scratch-root");if(root>=0){check(root+1<args.size(),"Missing scratch root");parentRoot=args[root+1];check(QDir(parentRoot).exists(),"Scratch parent must exist");}
    const auto report=args.indexOf("--report");if(report>=0){check(report+1<args.size(),"Missing report path");reportPath=args[report+1];}
#ifdef Q_OS_WIN
    activeThenHardKill();foreignAndCorruptGuards();invalidOwnerLockAndQuota();sameProcessAndNormalCleanup();finiteScanAndRenameGuard();reparseGuards();
    check(save(true,{}),"Cannot persist native recovery report");std::cout<<"Native scratch recovery checks passed, including actual QProcess hard-kill\n";return 0;
#else
    save(false,"Windows anchored recovery not supported/verified here");std::cout<<"Windows-only scratch recovery tests unavailable\n";return 77;
#endif
  } catch(const std::exception& e){save(false,QString::fromUtf8(e.what()));std::cerr<<e.what()<<'\n';return 1;}
}
