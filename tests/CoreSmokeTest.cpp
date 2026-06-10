#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "SshUmdLoader/Infrastructure.h"
#include "SshUmdLoader/UseCases.h"

namespace {

class FakeRemoteRepository final : public sshumdloader::IUmdRemoteRepository {
 private:
  std::map<std::string, std::vector<sshumdloader::RemoteEntry>> entries_;

 public:
  FakeRemoteRepository() {
    entries_["/home/"] = {
        {sshumdloader::RemoteEntryType::kDirectory, "CME", "/home/CME", 0}};
    entries_["/home/CME"] = {
        {sshumdloader::RemoteEntryType::kDirectory, "2026.03.26",
         "/home/CME/2026.03.26", 0},
        {sshumdloader::RemoteEntryType::kDirectory, "2026.03.27",
         "/home/CME/2026.03.27", 0}};
    entries_["/home/CME/2026.03.26"] = {
        {sshumdloader::RemoteEntryType::kDirectory, "orderbook1_1",
         "/home/CME/2026.03.26/orderbook1_1", 0}};
    entries_["/home/CME/2026.03.26/orderbook1_1"] = {
        {sshumdloader::RemoteEntryType::kFile, "ETHUSDC@CME.zst",
         "/home/CME/2026.03.26/orderbook1_1/ETHUSDC@CME.zst", 42},
        {sshumdloader::RemoteEntryType::kFile, "BTCUSDC@CME.zst",
         "/home/CME/2026.03.26/orderbook1_1/BTCUSDC@CME.zst", 84}};
    entries_["/home/CME/2026.03.27"] = {
        {sshumdloader::RemoteEntryType::kDirectory, "orderbook1_1",
         "/home/CME/2026.03.27/orderbook1_1", 0}};
    entries_["/home/CME/2026.03.27/orderbook1_1"] = {
        {sshumdloader::RemoteEntryType::kFile, "ETHUSDC@CME.zst",
         "/home/CME/2026.03.27/orderbook1_1/ETHUSDC@CME.zst", 21}};
  }

  std::vector<sshumdloader::RemoteEntry> ListDirectory(
      const sshumdloader::UmdServer& server,
      const std::string& remote_path) override {
    (void)server;
    return entries_[remote_path];
  }
};

}  // namespace

int main() {
  {
    FakeRemoteRepository remote_repository;
    sshumdloader::DefaultPathMapper path_mapper;
    sshumdloader::OverwriteTransferPolicy transfer_policy;
    sshumdloader::BuildDownloadPlanUseCase build_plan(
        &remote_repository, &path_mapper, &transfer_policy);

    sshumdloader::DownloadRequest request;
    request.server.remote_root_path = "/home/";
    request.local_download_root = "/tmp/umd";
    request.date_range.start =
        sshumdloader::MarketDataDate::Parse("2026.03.26");
    request.date_range.end = sshumdloader::MarketDataDate::Parse("2026.03.26");
    request.securities.push_back(sshumdloader::Security{"ETHUSDC@CME"});
    request.security_tickers.insert("ETHUSDC@CME");

    const sshumdloader::DownloadPlan plan = build_plan.Execute(request);
    assert(plan.tasks.size() == 1);
    assert(plan.tasks[0].remote_file.relative_path ==
           "CME/2026.03.26/orderbook1_1/ETHUSDC@CME.zst");
    assert(plan.tasks[0].remote_file.size == 42);
    assert(plan.tasks[0].transfer.local_path ==
           "/tmp/umd/CME/2026.03.26/orderbook1_1/ETHUSDC@CME.zst");
    assert(plan.tasks[0].transfer.offset == 0);
  }

  {
    const std::string config_path = "/tmp/sshumdloader_core_smoke_test.ini";
    {
      std::ofstream config_file(config_path.c_str(), std::ios::trunc);
      config_file << "DownloadPath = ~/umd/\n"
                  << "MaxConcurrentDownloads = 2\n"
                  << "\n"
                  << "[UmdServer0]\n"
                  << "name = MOEX\n"
                  << "host = example.invalid\n"
                  << "port = 23\n"
                  << "user = user\n"
                  << "pass_base64 = 'cGFzcw=='\n"
                  << "umd_path = /home/umd/\n"
                  << "\n"
                  << "[UmdServer1]\n"
                  << "name = UMD-CA-NEW\n"
                  << "host = example2.invalid\n"
                  << "port = 23\n"
                  << "user = user2\n"
                  << "pass_base64 = 'cGFzczI='\n"
                  << "umd_path = /home/\n";
    }

    sshumdloader::IniUmdConfigRepository config_repository(config_path);
    const sshumdloader::AppConfig config = config_repository.Load();
    assert(config.local_download_root == "~/umd/");
    assert(config.max_concurrent_downloads == 2);
    assert(config.servers.size() == 2);
    assert(config.servers[1].name == "UMD-CA-NEW");
    std::remove(config_path.c_str());
  }
  return 0;
}
