#include "SshUmdLoader/Composition.h"
#include "SshUmdLoader/Infrastructure.h"

int main(int argc, const char* const argv[]) {
  sshumdloader::IniUmdConfigRepository config_repository(
      "SshUmdLoader.ini");
  sshumdloader::CliOptionsParser cli_parser;
  sshumdloader::Libssh2UmdRemoteRepository remote_repository;
  sshumdloader::DefaultPathMapper path_mapper;
  sshumdloader::OverwriteTransferPolicy transfer_policy;
  sshumdloader::Libssh2DownloadScheduler download_scheduler;
  sshumdloader::ConsoleProgressSink progress_sink;

  sshumdloader::ApplicationDependencies dependencies;
  dependencies.config_repository = &config_repository;
  dependencies.cli_parser = &cli_parser;
  dependencies.remote_repository = &remote_repository;
  dependencies.path_mapper = &path_mapper;
  dependencies.transfer_policy = &transfer_policy;
  dependencies.download_scheduler = &download_scheduler;
  dependencies.progress_sink = &progress_sink;

  sshumdloader::ApplicationGraph graph =
      sshumdloader::ComposeApplication(dependencies);
  return graph.application.Run(argc, argv);
}

