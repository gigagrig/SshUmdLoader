#ifndef SSHUMDLOADER_INFRASTRUCTURE_H_
#define SSHUMDLOADER_INFRASTRUCTURE_H_

#include <map>
#include <string>

#include "SshUmdLoader/Ports.h"

namespace sshumdloader {

class DefaultPathMapper final : public IPathMapper {
 public:
  std::string ToLocalPath(const std::string& local_root,
                          const std::string& relative_remote_path) override;
};

class OverwriteTransferPolicy final : public ITransferPolicy {
 public:
  TransferDecision Decide(const RemoteFile& remote_file,
                          const std::string& local_path) override;
};

class ConsoleProgressSink final : public IProgressSink {
 private:
  std::map<std::string, int> line_by_path_;
  int rendered_line_count_ = 0;

 public:
  ~ConsoleProgressSink() override;

  void OnProgress(const ProgressEvent& event) override;
};

class CliOptionsParser final : public ICliOptionsParser {
 public:
  DownloadRequest Parse(int argc, const char* const argv[],
                        const AppConfig& config) override;
};

class IniUmdConfigRepository final : public IAppConfigRepository {
 private:
  std::string config_path_;

 public:
  explicit IniUmdConfigRepository(const std::string& config_path);

  AppConfig Load() override;
};

class Libssh2UmdRemoteRepository final : public IUmdRemoteRepository {
 public:
  std::vector<RemoteEntry> ListDirectory(
      const UmdServer& server, const std::string& remote_path) override;
};

class Libssh2DownloadScheduler final : public IDownloadScheduler {
 public:
  void Run(const UmdServer& server, const std::vector<DownloadTask>& tasks,
           int max_concurrent_downloads, IProgressSink* progress_sink) override;
};

}  // namespace sshumdloader

#endif  // SSHUMDLOADER_INFRASTRUCTURE_H_
