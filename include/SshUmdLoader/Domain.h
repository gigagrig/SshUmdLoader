#ifndef SSHUMDLOADER_DOMAIN_H_
#define SSHUMDLOADER_DOMAIN_H_

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace sshumdloader {

struct MarketDataDate {
  int year = 0;
  int month = 0;
  int day = 0;

  static MarketDataDate Parse(const std::string& value);

  std::string ToString() const;
  MarketDataDate NextDay() const;
  bool IsValid() const;
};

bool operator<(const MarketDataDate& left, const MarketDataDate& right);
bool operator==(const MarketDataDate& left, const MarketDataDate& right);
bool operator<=(const MarketDataDate& left, const MarketDataDate& right);

struct DateRange {
  MarketDataDate start;
  MarketDataDate end;
};

struct Security {
  std::string ticker;
};

struct ServerCredentials {
  std::string user;
  std::string password_base64;
};

struct ServerEndpoint {
  std::string host;
  int port = 22;
};

struct UmdServer {
  std::string name;
  ServerEndpoint endpoint;
  ServerCredentials credentials;
  std::string remote_root_path;
};

struct AppConfig {
  std::string local_download_root;
  int max_concurrent_downloads = 1;
  std::vector<UmdServer> servers;
};

struct DownloadRequest {
  UmdServer server;
  std::vector<Security> securities;
  std::set<std::string> security_tickers;
  DateRange date_range;
  std::string local_download_root;
};

enum class RemoteEntryType {
  kDirectory,
  kFile,
};

struct RemoteEntry {
  RemoteEntryType type = RemoteEntryType::kFile;
  std::string name;
  std::string absolute_path;
  std::uint64_t size = 0;
};

struct RemoteFile {
  std::string absolute_path;
  std::string relative_path;
  std::uint64_t size = 0;
};

enum class TransferMode {
  kOverwrite,
  kResume,
};

struct TransferDecision {
  TransferMode mode = TransferMode::kOverwrite;
  std::string local_path;
  std::string temporary_path;
  std::uint64_t offset = 0;
};

struct DownloadTask {
  RemoteFile remote_file;
  TransferDecision transfer;
};

struct DownloadPlan {
  std::vector<DownloadTask> tasks;
};

enum class DownloadStatus {
  kStarted,
  kProgress,
  kCompleted,
  kFailed,
};

struct ProgressEvent {
  DownloadStatus status = DownloadStatus::kStarted;
  DownloadTask task;
  std::uint64_t bytes_transferred = 0;
  std::uint64_t bytes_total = 0;
  std::string message;
};

}  // namespace sshumdloader

#endif  // SSHUMDLOADER_DOMAIN_H_
