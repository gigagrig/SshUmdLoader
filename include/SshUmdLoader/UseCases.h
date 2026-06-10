#ifndef SSHUMDLOADER_USE_CASES_H_
#define SSHUMDLOADER_USE_CASES_H_

#include "SshUmdLoader/Domain.h"
#include "SshUmdLoader/Ports.h"

namespace sshumdloader {

class BuildDownloadPlanUseCase {
 private:
  IUmdRemoteRepository* remote_repository_;
  IPathMapper* path_mapper_;
  ITransferPolicy* transfer_policy_;

 public:
  BuildDownloadPlanUseCase(IUmdRemoteRepository* remote_repository,
                           IPathMapper* path_mapper,
                           ITransferPolicy* transfer_policy);

  DownloadPlan Execute(const DownloadRequest& request);
};

class RunDownloadPlanUseCase {
 private:
  IDownloadScheduler* download_scheduler_;
  IProgressSink* progress_sink_;

 public:
  RunDownloadPlanUseCase(IDownloadScheduler* download_scheduler,
                         IProgressSink* progress_sink);

  void Execute(const UmdServer& server, const DownloadPlan& plan,
               int max_concurrent_downloads);
};

class SshUmdLoaderApplication {
 private:
  IAppConfigRepository* config_repository_;
  ICliOptionsParser* cli_parser_;
  BuildDownloadPlanUseCase* build_plan_;
  RunDownloadPlanUseCase* run_plan_;

 public:
  SshUmdLoaderApplication(IAppConfigRepository* config_repository,
                          ICliOptionsParser* cli_parser,
                          BuildDownloadPlanUseCase* build_plan,
                          RunDownloadPlanUseCase* run_plan);

  int Run(int argc, const char* const argv[]);
};

}  // namespace sshumdloader

#endif  // SSHUMDLOADER_USE_CASES_H_
