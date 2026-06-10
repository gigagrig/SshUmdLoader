#include "SshUmdLoader/UseCases.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace sshumdloader {
namespace {

std::string JoinRelativePath(const std::string& left, const std::string& right) {
  if (left.empty()) {
    return right;
  }
  return left + "/" + right;
}

bool HasZstExtension(const std::string& name) {
  const std::string suffix = ".zst";
  return name.size() >= suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool HasDateFormat(const std::string& name) {
  return name.size() == 10 && name[4] == '.' && name[7] == '.';
}

std::string StripZstExtension(const std::string& value) {
  const std::string suffix = ".zst";
  if (value.size() >= suffix.size() &&
      value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return value.substr(0, value.size() - suffix.size());
  }
  return value;
}

bool MatchesSecurity(const DownloadRequest& request,
                     const std::string& file_name) {
  if (request.security_tickers.empty()) {
    return true;
  }
  return request.security_tickers.count(StripZstExtension(file_name)) != 0;
}

bool IsRequestedDateDirectory(const DownloadRequest& request,
                              const std::string& name) {
  try {
    const MarketDataDate date = MarketDataDate::Parse(name);
    return request.date_range.start <= date && date <= request.date_range.end;
  } catch (const std::exception&) {
    return false;
  }
}

std::string JoinRemotePath(const std::string& left, const std::string& right) {
  if (left.empty() || left == "/") {
    return "/" + right;
  }
  if (left.back() == '/') {
    return left + right;
  }
  return left + "/" + right;
}

std::string ExtractBoard(const std::string& security) {
  const std::size_t at_pos = security.find('@');
  if (at_pos == std::string::npos || at_pos + 1 >= security.size()) {
    return std::string();
  }
  return security.substr(at_pos + 1);
}

std::vector<MarketDataDate> DatesInRange(const DateRange& date_range) {
  std::vector<MarketDataDate> result;
  for (MarketDataDate date = date_range.start; date <= date_range.end;
       date = date.NextDay()) {
    result.push_back(date);
  }
  return result;
}

struct SearchRoot {
  std::string remote_path;
  std::string relative_path;
};

std::string FormatSecurities(const std::vector<Security>& securities) {
  std::ostringstream stream;
  for (std::size_t i = 0; i < securities.size(); ++i) {
    if (i > 0) {
      stream << ", ";
    }
    stream << securities[i].ticker;
  }
  return stream.str();
}

std::vector<SearchRoot> BuildSearchRoots(const DownloadRequest& request) {
  std::set<std::string> boards;
  for (const Security& security : request.securities) {
    const std::string board = ExtractBoard(security.ticker);
    if (!board.empty()) {
      boards.insert(board);
    }
  }

  std::vector<SearchRoot> result;
  if (boards.empty()) {
    result.push_back(SearchRoot{request.server.remote_root_path, std::string()});
    return result;
  }

  for (const std::string& board : boards) {
    const std::string board_remote_path =
        JoinRemotePath(request.server.remote_root_path, board);
    for (const MarketDataDate& date : DatesInRange(request.date_range)) {
      const std::string date_string = date.ToString();
      result.push_back(SearchRoot{
          JoinRemotePath(board_remote_path, date_string),
          JoinRelativePath(board, date_string)});
    }
  }
  return result;
}

void WalkRemoteTree(const DownloadRequest& request,
                    IUmdRemoteRepository* remote_repository,
                    IPathMapper* path_mapper, ITransferPolicy* transfer_policy,
                    const std::string& remote_path,
                    const std::string& relative_path, bool inside_date_dir,
                    DownloadPlan* plan) {
  std::cerr << "Scanning remote directory: " << remote_path << std::endl;
  const std::vector<RemoteEntry> entries =
      remote_repository->ListDirectory(request.server, remote_path);
  for (const RemoteEntry& entry : entries) {
    const std::string child_relative_path =
        JoinRelativePath(relative_path, entry.name);
    if (entry.type == RemoteEntryType::kDirectory) {
      if (!inside_date_dir && HasDateFormat(entry.name) &&
          !IsRequestedDateDirectory(request, entry.name)) {
        continue;
      }
      const bool child_inside_date_dir =
          inside_date_dir || IsRequestedDateDirectory(request, entry.name);
      WalkRemoteTree(request, remote_repository, path_mapper, transfer_policy,
                     entry.absolute_path, child_relative_path,
                     child_inside_date_dir, plan);
      continue;
    }

    if (!inside_date_dir || !HasZstExtension(entry.name) ||
        !MatchesSecurity(request, entry.name)) {
      continue;
    }

    RemoteFile remote_file;
    remote_file.absolute_path = entry.absolute_path;
    remote_file.relative_path = child_relative_path;
    remote_file.size = entry.size;

    const std::string local_path = path_mapper->ToLocalPath(
        request.local_download_root, remote_file.relative_path);
    DownloadTask task;
    task.remote_file = remote_file;
    task.transfer = transfer_policy->Decide(remote_file, local_path);
    plan->tasks.push_back(task);
  }
}

}  // namespace

BuildDownloadPlanUseCase::BuildDownloadPlanUseCase(
    IUmdRemoteRepository* remote_repository, IPathMapper* path_mapper,
    ITransferPolicy* transfer_policy)
    : remote_repository_(remote_repository),
      path_mapper_(path_mapper),
      transfer_policy_(transfer_policy) {}

DownloadPlan BuildDownloadPlanUseCase::Execute(const DownloadRequest& request) {
  if (remote_repository_ == nullptr || path_mapper_ == nullptr ||
      transfer_policy_ == nullptr) {
    throw std::logic_error("BuildDownloadPlanUseCase dependencies are not set");
  }
  if (request.date_range.end < request.date_range.start) {
    throw std::invalid_argument("end_day must not be earlier than day");
  }

  std::cerr << "Building download plan for server " << request.server.name
            << ", dates " << request.date_range.start.ToString() << "..."
            << request.date_range.end.ToString() << ", securities "
            << FormatSecurities(request.securities) << std::endl;

  DownloadPlan plan;
  std::string last_search_error;
  for (const SearchRoot& search_root : BuildSearchRoots(request)) {
    std::cerr << "Search root: " << search_root.remote_path << std::endl;
    try {
      WalkRemoteTree(request, remote_repository_, path_mapper_, transfer_policy_,
                     search_root.remote_path, search_root.relative_path, true,
                     &plan);
    } catch (const std::exception& error) {
      if (search_root.relative_path.empty()) {
        throw;
      }
      last_search_error = std::string(" while scanning ") +
                          search_root.remote_path + ": " + error.what();
    }
  }
  std::sort(plan.tasks.begin(), plan.tasks.end(),
            [](const DownloadTask& left, const DownloadTask& right) {
              return left.remote_file.relative_path <
                     right.remote_file.relative_path;
            });
  if (plan.tasks.empty()) {
    throw std::runtime_error("No remote files matched the request" +
                             last_search_error);
  }
  std::cerr << "Download plan ready: " << plan.tasks.size() << " file(s)"
            << std::endl;
  return plan;
}

RunDownloadPlanUseCase::RunDownloadPlanUseCase(
    IDownloadScheduler* download_scheduler, IProgressSink* progress_sink)
    : download_scheduler_(download_scheduler), progress_sink_(progress_sink) {}

void RunDownloadPlanUseCase::Execute(const UmdServer& server,
                                     const DownloadPlan& plan,
                                     int max_concurrent_downloads) {
  if (download_scheduler_ == nullptr) {
    throw std::logic_error("RunDownloadPlanUseCase dependency is not set");
  }
  if (max_concurrent_downloads < 1) {
    throw std::invalid_argument("MaxConcurrentDownloads must be positive");
  }
  std::cerr << "Starting downloads: " << plan.tasks.size()
            << " file(s), max concurrency " << max_concurrent_downloads
            << std::endl;
  download_scheduler_->Run(server, plan.tasks, max_concurrent_downloads,
                           progress_sink_);
  std::cerr << std::endl;
}

SshUmdLoaderApplication::SshUmdLoaderApplication(
    IAppConfigRepository* config_repository, ICliOptionsParser* cli_parser,
    BuildDownloadPlanUseCase* build_plan, RunDownloadPlanUseCase* run_plan)
    : config_repository_(config_repository),
      cli_parser_(cli_parser),
      build_plan_(build_plan),
      run_plan_(run_plan) {}

int SshUmdLoaderApplication::Run(int argc, const char* const argv[]) {
  try {
    if (config_repository_ == nullptr || cli_parser_ == nullptr ||
        build_plan_ == nullptr || run_plan_ == nullptr) {
      throw std::logic_error("Application dependencies are not set");
    }

    std::cerr << "Loading configuration..." << std::endl;
    const AppConfig config = config_repository_->Load();
    std::cerr << "Configuration loaded: " << config.servers.size()
              << " server(s), MaxConcurrentDownloads="
              << config.max_concurrent_downloads << std::endl;

    std::cerr << "Parsing command line..." << std::endl;
    const DownloadRequest request = cli_parser_->Parse(argc, argv, config);
    std::cerr << "Command line parsed: server=" << request.server.name
              << ", securities=" << FormatSecurities(request.securities)
              << ", day=" << request.date_range.start.ToString();
    if (!(request.date_range.start == request.date_range.end)) {
      std::cerr << ", end_day=" << request.date_range.end.ToString();
    }
    std::cerr << std::endl;

    const DownloadPlan plan = build_plan_->Execute(request);
    run_plan_->Execute(request.server, plan, config.max_concurrent_downloads);
    std::cerr << "Download completed successfully" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "SshUmdLoader error: " << error.what() << std::endl;
    return 1;
  }
}

}  // namespace sshumdloader
