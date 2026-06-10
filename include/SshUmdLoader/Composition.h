#ifndef SSHUMDLOADER_COMPOSITION_H_
#define SSHUMDLOADER_COMPOSITION_H_

#include "SshUmdLoader/Ports.h"
#include "SshUmdLoader/UseCases.h"

namespace sshumdloader {

struct ApplicationDependencies {
  IAppConfigRepository* config_repository = nullptr;
  ICliOptionsParser* cli_parser = nullptr;
  IUmdRemoteRepository* remote_repository = nullptr;
  IPathMapper* path_mapper = nullptr;
  ITransferPolicy* transfer_policy = nullptr;
  IDownloadScheduler* download_scheduler = nullptr;
  IProgressSink* progress_sink = nullptr;
};

struct ApplicationGraph {
  BuildDownloadPlanUseCase build_plan;
  RunDownloadPlanUseCase run_plan;
  SshUmdLoaderApplication application;

  explicit ApplicationGraph(const ApplicationDependencies& dependencies)
      : build_plan(dependencies.remote_repository, dependencies.path_mapper,
                   dependencies.transfer_policy),
        run_plan(dependencies.download_scheduler, dependencies.progress_sink),
        application(dependencies.config_repository, dependencies.cli_parser,
                    &build_plan, &run_plan) {}
};

inline ApplicationGraph ComposeApplication(
    const ApplicationDependencies& dependencies) {
  return ApplicationGraph(dependencies);
}

}  // namespace sshumdloader

#endif  // SSHUMDLOADER_COMPOSITION_H_
