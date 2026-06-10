#ifndef SSHUMDLOADER_PORTS_H_
#define SSHUMDLOADER_PORTS_H_

#include <string>
#include <vector>

#include "SshUmdLoader/Domain.h"

namespace sshumdloader {

class IAppConfigRepository {
 public:
  virtual ~IAppConfigRepository() = default;

  virtual AppConfig Load() = 0;
};

class ICliOptionsParser {
 public:
  virtual ~ICliOptionsParser() = default;

  virtual DownloadRequest Parse(int argc, const char* const argv[],
                                const AppConfig& config) = 0;
};

class IUmdRemoteRepository {
 public:
  virtual ~IUmdRemoteRepository() = default;

  virtual std::vector<RemoteEntry> ListDirectory(
      const UmdServer& server, const std::string& remote_path) = 0;
};

class IPathMapper {
 public:
  virtual ~IPathMapper() = default;

  virtual std::string ToLocalPath(const std::string& local_root,
                                  const std::string& relative_remote_path) = 0;
};

class ITransferPolicy {
 public:
  virtual ~ITransferPolicy() = default;

  virtual TransferDecision Decide(const RemoteFile& remote_file,
                                  const std::string& local_path) = 0;
};

class IProgressSink {
 public:
  virtual ~IProgressSink() = default;

  virtual void OnProgress(const ProgressEvent& event) = 0;
};

class IDownloadScheduler {
 public:
  virtual ~IDownloadScheduler() = default;

  virtual void Run(const UmdServer& server, const std::vector<DownloadTask>& tasks,
                   int max_concurrent_downloads, IProgressSink* progress_sink) = 0;
};

}  // namespace sshumdloader

#endif  // SSHUMDLOADER_PORTS_H_

