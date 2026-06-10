#include "SshUmdLoader/Infrastructure.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

#include "INI/INIReader.h"

namespace sshumdloader {
namespace {

const int kSocketWaitTimeoutSeconds = 120;

class ConnectionStatusReporter {
 private:
  std::set<std::string> connecting_servers_;
  std::set<std::string> connected_servers_;

 public:
  void OnConnecting(const UmdServer& server) {
    const std::string key = server.name + "@" + server.endpoint.host + ":" +
                            std::to_string(server.endpoint.port);
    if (connecting_servers_.insert(key).second) {
      std::cerr << "Connecting to server " << server.name << " ("
                << server.endpoint.host << ":" << server.endpoint.port
                << ")..." << std::endl;
    }
  }

  void OnConnected(const UmdServer& server) {
    const std::string key = server.name + "@" + server.endpoint.host + ":" +
                            std::to_string(server.endpoint.port);
    if (connected_servers_.insert(key).second) {
      std::cerr << "Connected to server " << server.name << " ("
                << server.endpoint.host << ":" << server.endpoint.port
                << ")" << std::endl;
    }
  }
};

ConnectionStatusReporter& GetConnectionStatusReporter() {
  static ConnectionStatusReporter reporter;
  return reporter;
}

class Libssh2Global {
 public:
  Libssh2Global() {
    const int result = libssh2_init(0);
    if (result != 0) {
      throw std::runtime_error("libssh2_init failed: " +
                               std::to_string(result));
    }
  }

  ~Libssh2Global() { libssh2_exit(); }
};

struct SocketHandle {
  int fd = -1;

  ~SocketHandle() {
    if (fd >= 0) {
      close(fd);
    }
  }

  SocketHandle() = default;
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  int Release() {
    const int result = fd;
    fd = -1;
    return result;
  }
};

class Libssh2Session {
 private:
  int socket_fd_ = -1;
  LIBSSH2_SESSION* session_ = nullptr;
  LIBSSH2_SFTP* sftp_ = nullptr;

  int WaitSocket() {
    fd_set read_fds;
    fd_set write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);

    const int directions = libssh2_session_block_directions(session_);
    if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) != 0) {
      FD_SET(socket_fd_, &read_fds);
    }
    if ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0) {
      FD_SET(socket_fd_, &write_fds);
    }
    if (directions == 0) {
      FD_SET(socket_fd_, &read_fds);
      FD_SET(socket_fd_, &write_fds);
    }

    timeval timeout;
    timeout.tv_sec = kSocketWaitTimeoutSeconds;
    timeout.tv_usec = 0;
    const int result =
        select(socket_fd_ + 1, &read_fds, &write_fds, nullptr, &timeout);
    if (result < 0) {
      throw std::runtime_error("select failed: " + std::string(strerror(errno)));
    }
    if (result == 0) {
      throw std::runtime_error("SSH socket wait timed out");
    }
    return result;
  }

  void Check(int result, const std::string& operation) {
    while (result == LIBSSH2_ERROR_EAGAIN) {
      WaitSocket();
      return;
    }
    if (result < 0) {
      char* error_message = nullptr;
      libssh2_session_last_error(session_, &error_message, nullptr, 0);
      throw std::runtime_error(operation + " failed: " +
                               (error_message == nullptr ? "" : error_message));
    }
  }

 public:
  Libssh2Session() = default;
  Libssh2Session(const Libssh2Session&) = delete;
  Libssh2Session& operator=(const Libssh2Session&) = delete;

  ~Libssh2Session() {
    if (sftp_ != nullptr) {
      while (libssh2_sftp_shutdown(sftp_) == LIBSSH2_ERROR_EAGAIN) {
      }
    }
    if (session_ != nullptr) {
      while (libssh2_session_disconnect(session_, "SshUmdLoader shutdown") ==
             LIBSSH2_ERROR_EAGAIN) {
      }
      libssh2_session_free(session_);
    }
    if (socket_fd_ >= 0) {
      close(socket_fd_);
    }
  }

  LIBSSH2_SESSION* session() { return session_; }
  LIBSSH2_SFTP* sftp() { return sftp_; }
  int socket_fd() const { return socket_fd_; }
  int BlockDirections() const {
    return libssh2_session_block_directions(session_);
  }

  void Wait() { WaitSocket(); }

  void Connect(const UmdServer& server, const std::string& password) {
    GetConnectionStatusReporter().OnConnecting(server);

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const std::string port = std::to_string(server.endpoint.port);
    const int gai_result =
        getaddrinfo(server.endpoint.host.c_str(), port.c_str(), &hints,
                    &results);
    if (gai_result != 0) {
      throw std::runtime_error("getaddrinfo failed for " + server.endpoint.host +
                               ": " + gai_strerror(gai_result));
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(results,
                                                                 freeaddrinfo);

    SocketHandle connected_socket;
    for (addrinfo* address = addresses.get(); address != nullptr;
         address = address->ai_next) {
      SocketHandle candidate;
      candidate.fd =
          socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate.fd < 0) {
        continue;
      }

      const int flags = fcntl(candidate.fd, F_GETFL, 0);
      if (flags < 0 ||
          fcntl(candidate.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        continue;
      }

      const int connect_result =
          connect(candidate.fd, address->ai_addr, address->ai_addrlen);
      if (connect_result == 0 || errno == EINPROGRESS) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(candidate.fd, &write_fds);
        timeval timeout;
        timeout.tv_sec = kSocketWaitTimeoutSeconds;
        timeout.tv_usec = 0;
        if (select(candidate.fd + 1, nullptr, &write_fds, nullptr, &timeout) >
            0) {
          int socket_error = 0;
          socklen_t socket_error_size = sizeof(socket_error);
          getsockopt(candidate.fd, SOL_SOCKET, SO_ERROR, &socket_error,
                     &socket_error_size);
          if (socket_error == 0) {
            connected_socket.fd = candidate.Release();
            break;
          }
        }
      }
    }

    if (connected_socket.fd < 0) {
      throw std::runtime_error("Cannot connect to " + server.endpoint.host +
                               ":" + std::to_string(server.endpoint.port));
    }

    socket_fd_ = connected_socket.Release();
    session_ = libssh2_session_init();
    if (session_ == nullptr) {
      throw std::runtime_error("libssh2_session_init failed");
    }
    libssh2_session_set_blocking(session_, 0);

    int result = 0;
    while ((result = libssh2_session_handshake(session_, socket_fd_)) ==
           LIBSSH2_ERROR_EAGAIN) {
      WaitSocket();
    }
    Check(result, "SSH handshake");

    while ((result = libssh2_userauth_password(
                session_, server.credentials.user.c_str(), password.c_str())) ==
           LIBSSH2_ERROR_EAGAIN) {
      WaitSocket();
    }
    Check(result, "SSH password authentication");

    while ((sftp_ = libssh2_sftp_init(session_)) == nullptr &&
           libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN) {
      WaitSocket();
    }
    if (sftp_ == nullptr) {
      Check(libssh2_session_last_errno(session_), "SFTP init");
    }

    GetConnectionStatusReporter().OnConnected(server);
  }
};

class SftpHandle {
 private:
  Libssh2Session* session_ = nullptr;
  LIBSSH2_SFTP_HANDLE* handle_ = nullptr;

 public:
  SftpHandle(Libssh2Session* session, LIBSSH2_SFTP_HANDLE* handle)
      : session_(session), handle_(handle) {}

  ~SftpHandle() {
    if (handle_ != nullptr) {
      while (libssh2_sftp_close(handle_) == LIBSSH2_ERROR_EAGAIN) {
        session_->Wait();
      }
    }
  }

  SftpHandle(const SftpHandle&) = delete;
  SftpHandle& operator=(const SftpHandle&) = delete;

  LIBSSH2_SFTP_HANDLE* get() { return handle_; }
};

std::string Base64Decode(const std::string& value) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  int accumulator = 0;
  int bits = -8;
  for (const unsigned char character : value) {
    if (character == '=') {
      break;
    }
    const std::size_t index = alphabet.find(character);
    if (index == std::string::npos) {
      continue;
    }
    accumulator = (accumulator << 6) | static_cast<int>(index);
    bits += 6;
    if (bits >= 0) {
      result.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return result;
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

void EnsureParentDirectory(const std::string& file_path) {
  const std::size_t slash_pos = file_path.find_last_of('/');
  if (slash_pos == std::string::npos) {
    return;
  }

  std::string current;
  const std::string directory = file_path.substr(0, slash_pos);
  for (std::size_t i = 0; i < directory.size(); ++i) {
    current.push_back(directory[i]);
    if (directory[i] != '/' || current.size() == 1) {
      continue;
    }
    if (mkdir(current.c_str(), 0775) < 0 && errno != EEXIST) {
      throw std::runtime_error("mkdir failed for " + current + ": " +
                               strerror(errno));
    }
  }
  if (!directory.empty() && mkdir(directory.c_str(), 0775) < 0 &&
      errno != EEXIST) {
    throw std::runtime_error("mkdir failed for " + directory + ": " +
                             strerror(errno));
  }
}

std::string Trim(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const std::size_t begin = value.find_first_not_of(whitespace);
  if (begin == std::string::npos) {
    return std::string();
  }
  const std::size_t end = value.find_last_not_of(whitespace);
  return value.substr(begin, end - begin + 1);
}

std::string Unquote(const std::string& value) {
  const std::string trimmed = Trim(value);
  if (trimmed.size() >= 2 &&
      ((trimmed.front() == '\'' && trimmed.back() == '\'') ||
       (trimmed.front() == '"' && trimmed.back() == '"'))) {
    return trimmed.substr(1, trimmed.size() - 2);
  }
  return trimmed;
}

std::string ExpandUserHome(const std::string& path) {
  if (path.empty() || path[0] != '~') {
    return path;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string(home).empty()) {
    return path;
  }
  if (path.size() == 1) {
    return std::string(home);
  }
  if (path[1] == '/') {
    return std::string(home) + path.substr(1);
  }
  return path;
}

std::string JoinPath(const std::string& left, const std::string& right) {
  if (left.empty()) {
    return right;
  }
  if (right.empty()) {
    return left;
  }
  if (left.back() == '/') {
    return left + right;
  }
  return left + "/" + right;
}

std::vector<std::string> SplitCsv(const std::string& value) {
  std::vector<std::string> result;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ',')) {
    item = Trim(item);
    if (!item.empty()) {
      result.push_back(item);
    }
  }
  return result;
}

const UmdServer& FindServer(const AppConfig& config,
                            const std::string& server_name) {
  for (const UmdServer& server : config.servers) {
    if (server.name == server_name) {
      return server;
    }
  }
  throw std::invalid_argument("Unknown UMD server: " + server_name);
}

void RequireValue(int argc, int index, const std::string& option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument("Missing value for " + option);
  }
}

RemoteEntryType GetEntryType(const LIBSSH2_SFTP_ATTRIBUTES& attributes) {
  if ((attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 &&
      LIBSSH2_SFTP_S_ISDIR(attributes.permissions)) {
    return RemoteEntryType::kDirectory;
  }
  return RemoteEntryType::kFile;
}

std::string BuildProgressLine(const ProgressEvent& event) {
  const double ratio =
      event.bytes_total == 0
          ? 0.0
          : static_cast<double>(event.bytes_transferred) /
                static_cast<double>(event.bytes_total);
  const int width = 30;
  const int filled = static_cast<int>(ratio * width);

  std::ostringstream line;
  line << event.task.remote_file.relative_path << " [";
  for (int i = 0; i < width; ++i) {
    line << (i < filled ? '#' : '.');
  }
  line << "] " << event.bytes_transferred << "/" << event.bytes_total;
  if (!event.message.empty()) {
    line << " " << event.message;
  }
  return line.str();
}

}  // namespace

std::string DefaultPathMapper::ToLocalPath(
    const std::string& local_root, const std::string& relative_remote_path) {
  return JoinPath(ExpandUserHome(local_root), relative_remote_path);
}

TransferDecision OverwriteTransferPolicy::Decide(
    const RemoteFile& remote_file, const std::string& local_path) {
  TransferDecision decision;
  decision.mode = TransferMode::kOverwrite;
  decision.local_path = local_path;
  decision.temporary_path = local_path + ".part";
  decision.offset = 0;
  (void)remote_file;
  return decision;
}

ConsoleProgressSink::~ConsoleProgressSink() {
  if (rendered_line_count_ > 0) {
    std::cerr << std::endl;
  }
}

void ConsoleProgressSink::OnProgress(const ProgressEvent& event) {
  const std::string& path = event.task.remote_file.relative_path;
  if (line_by_path_.count(path) == 0) {
    line_by_path_[path] = rendered_line_count_;
    if (rendered_line_count_ > 0) {
      std::cerr << std::endl;
    }
    ++rendered_line_count_;
  }

  const int line_index = line_by_path_[path];
  const int lines_up = rendered_line_count_ - 1 - line_index;
  if (lines_up > 0) {
    std::cerr << "\033[" << lines_up << "A";
  }

  std::cerr << "\r\033[K" << BuildProgressLine(event);

  if (lines_up > 0) {
    std::cerr << "\033[" << lines_up << "B";
  }
  std::cerr.flush();
}

DownloadRequest CliOptionsParser::Parse(int argc, const char* const argv[],
                                        const AppConfig& config) {
  if (argc < 2) {
    throw std::invalid_argument(
        "Usage: SshUmdLoader <server> --securities TICKER[,TICKER] --day "
        "YYYY.MM.DD [--end_day YYYY.MM.DD]");
  }

  const std::string server_name = argv[1];
  DownloadRequest request;
  request.server = FindServer(config, server_name);
  request.local_download_root = config.local_download_root;

  bool has_day = false;
  bool has_securities = false;
  for (int i = 2; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--securities" || option == "-s") {
      RequireValue(argc, i, option);
      for (const std::string& ticker : SplitCsv(argv[++i])) {
        const std::string normalized_ticker =
            ticker.size() > 4 &&
                    ticker.compare(ticker.size() - 4, 4, ".zst") == 0
                ? ticker.substr(0, ticker.size() - 4)
                : ticker;
        request.securities.push_back(Security{normalized_ticker});
        request.security_tickers.insert(normalized_ticker);
      }
      has_securities = true;
    } else if (option == "--day" || option == "-d") {
      RequireValue(argc, i, option);
      request.date_range.start = MarketDataDate::Parse(argv[++i]);
      request.date_range.end = request.date_range.start;
      has_day = true;
    } else if (option == "--end_day" || option == "-end_day" ||
               option == "-e") {
      RequireValue(argc, i, option);
      request.date_range.end = MarketDataDate::Parse(argv[++i]);
    } else {
      throw std::invalid_argument("Unknown CLI option: " + option);
    }
  }

  if (!has_day) {
    throw std::invalid_argument("--day is required");
  }
  if (!has_securities || request.securities.empty()) {
    throw std::invalid_argument("--securities is required");
  }
  return request;
}

IniUmdConfigRepository::IniUmdConfigRepository(const std::string& config_path)
    : config_path_(config_path) {}

AppConfig IniUmdConfigRepository::Load() {
  INIReader reader(config_path_);
  if (reader.ParseError() != 0) {
    throw std::runtime_error("Cannot parse config: " + config_path_);
  }

  AppConfig config;
  config.local_download_root = Unquote(reader.Get("", "DownloadPath", ""));
  config.max_concurrent_downloads = static_cast<int>(
      reader.GetInteger("", "MaxConcurrentDownloads", 1));
  if (config.local_download_root.empty()) {
    throw std::runtime_error("DownloadPath is not configured");
  }
  if (config.max_concurrent_downloads < 1) {
    throw std::runtime_error("MaxConcurrentDownloads must be positive");
  }

  for (const std::string& section : reader.GetSections()) {
    if (section.find("UmdServer") != 0) {
      continue;
    }

    UmdServer server;
    server.name = Unquote(reader.Get(section, "name", ""));
    server.endpoint.host = Unquote(reader.Get(section, "host", ""));
    server.endpoint.port =
        static_cast<int>(reader.GetInteger(section, "port", 22));
    server.credentials.user = Unquote(reader.Get(section, "user", ""));
    server.credentials.password_base64 =
        Unquote(reader.Get(section, "pass_base64", ""));
    server.remote_root_path = Unquote(reader.Get(section, "umd_path", "/"));

    if (server.name.empty() || server.endpoint.host.empty() ||
        server.credentials.user.empty()) {
      throw std::runtime_error("Incomplete server config section: " + section);
    }
    config.servers.push_back(server);
  }

  if (config.servers.empty()) {
    throw std::runtime_error("No UMD servers configured");
  }
  return config;
}

std::vector<RemoteEntry> Libssh2UmdRemoteRepository::ListDirectory(
    const UmdServer& server, const std::string& remote_path) {
  static Libssh2Global libssh2_global;
  Libssh2Session session;
  session.Connect(server, Base64Decode(server.credentials.password_base64));

  LIBSSH2_SFTP_HANDLE* raw_handle = nullptr;
  while ((raw_handle = libssh2_sftp_opendir(session.sftp(),
                                           remote_path.c_str())) == nullptr &&
         libssh2_session_last_errno(session.session()) ==
             LIBSSH2_ERROR_EAGAIN) {
    session.Wait();
  }
  if (raw_handle == nullptr) {
    throw std::runtime_error("SFTP opendir failed: " + remote_path);
  }
  SftpHandle directory(&session, raw_handle);

  std::vector<RemoteEntry> result;
  for (;;) {
    char name_buffer[512] = {};
    LIBSSH2_SFTP_ATTRIBUTES attributes = {};
    int read_result = 0;
    while ((read_result = libssh2_sftp_readdir_ex(
                directory.get(), name_buffer, sizeof(name_buffer), nullptr, 0,
                &attributes)) == LIBSSH2_ERROR_EAGAIN) {
      session.Wait();
    }

    if (read_result == 0) {
      break;
    }
    if (read_result < 0) {
      throw std::runtime_error("SFTP readdir failed: " + remote_path);
    }

    const std::string name(name_buffer, read_result);
    if (name == "." || name == "..") {
      continue;
    }

    RemoteEntry entry;
    entry.name = name;
    entry.absolute_path = JoinRemotePath(remote_path, name);
    entry.type = GetEntryType(attributes);
    if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0) {
      entry.size = attributes.filesize;
    }
    result.push_back(entry);
  }
  return result;
}

void Libssh2DownloadScheduler::Run(const UmdServer& server,
                                   const std::vector<DownloadTask>& tasks,
                                   int max_concurrent_downloads,
                                   IProgressSink* progress_sink) {
  static Libssh2Global libssh2_global;
  if (max_concurrent_downloads < 1) {
    throw std::invalid_argument("MaxConcurrentDownloads must be positive");
  }

  struct ActiveTransfer {
    DownloadTask task;
    std::unique_ptr<Libssh2Session> session;
    std::unique_ptr<SftpHandle> remote_file;
    std::ofstream output;
    bool completed = false;
  };

  std::vector<std::unique_ptr<ActiveTransfer>> active_transfers;
  std::size_t next_task_index = 0;

  auto start_transfer = [&]() {
    const DownloadTask& task = tasks[next_task_index++];
    EnsureParentDirectory(task.transfer.temporary_path);

    std::unique_ptr<ActiveTransfer> transfer(new ActiveTransfer());
    transfer->task = task;
    transfer->session.reset(new Libssh2Session());
    transfer->session->Connect(server,
                               Base64Decode(server.credentials.password_base64));

    LIBSSH2_SFTP_HANDLE* raw_handle = nullptr;
    while ((raw_handle = libssh2_sftp_open(
                transfer->session->sftp(), task.remote_file.absolute_path.c_str(),
                LIBSSH2_FXF_READ, 0)) == nullptr &&
           libssh2_session_last_errno(transfer->session->session()) ==
               LIBSSH2_ERROR_EAGAIN) {
      transfer->session->Wait();
    }
    if (raw_handle == nullptr) {
      throw std::runtime_error("SFTP open failed: " +
                               task.remote_file.absolute_path);
    }
    transfer->remote_file.reset(new SftpHandle(transfer->session.get(),
                                               raw_handle));

    if (task.transfer.offset > 0) {
      libssh2_sftp_seek64(raw_handle, task.transfer.offset);
    }

    transfer->output.open(task.transfer.temporary_path.c_str(),
                          std::ios::binary | std::ios::trunc);
    if (!transfer->output) {
      throw std::runtime_error("Cannot open local file: " +
                               task.transfer.temporary_path);
    }

    if (progress_sink != nullptr) {
      progress_sink->OnProgress(ProgressEvent{DownloadStatus::kStarted, task, 0,
                                              task.remote_file.size,
                                              "started"});
    }
    active_transfers.push_back(std::move(transfer));
  };

  while (next_task_index < tasks.size() &&
         static_cast<int>(active_transfers.size()) < max_concurrent_downloads) {
    start_transfer();
  }

  std::vector<char> buffer(64 * 1024);
  while (!active_transfers.empty()) {
    for (std::unique_ptr<ActiveTransfer>& transfer : active_transfers) {
      if (transfer->completed) {
        continue;
      }

      ssize_t read_result = libssh2_sftp_read(
          transfer->remote_file->get(), buffer.data(), buffer.size());
      if (read_result == LIBSSH2_ERROR_EAGAIN) {
        continue;
      }
      if (read_result < 0) {
        if (progress_sink != nullptr) {
          progress_sink->OnProgress(
              ProgressEvent{DownloadStatus::kFailed, transfer->task, 0,
                            transfer->task.remote_file.size, "failed"});
        }
        throw std::runtime_error("SFTP read failed: " +
                                 transfer->task.remote_file.absolute_path);
      }
      if (read_result == 0) {
        transfer->output.close();
        if (std::rename(transfer->task.transfer.temporary_path.c_str(),
                        transfer->task.transfer.local_path.c_str()) != 0) {
          throw std::runtime_error(
              "rename failed from " + transfer->task.transfer.temporary_path +
              " to " + transfer->task.transfer.local_path + ": " +
              strerror(errno));
        }
        transfer->completed = true;
        if (progress_sink != nullptr) {
          progress_sink->OnProgress(ProgressEvent{
              DownloadStatus::kCompleted, transfer->task,
              transfer->task.remote_file.size, transfer->task.remote_file.size,
              "completed"});
        }
        continue;
      }

      transfer->output.write(buffer.data(), read_result);
      if (!transfer->output) {
        throw std::runtime_error("Cannot write local file: " +
                                 transfer->task.transfer.temporary_path);
      }
      const libssh2_uint64_t position =
          libssh2_sftp_tell64(transfer->remote_file->get());
      if (progress_sink != nullptr) {
        progress_sink->OnProgress(
            ProgressEvent{DownloadStatus::kProgress, transfer->task, position,
                          transfer->task.remote_file.size, std::string()});
      }
    }

    active_transfers.erase(
        std::remove_if(active_transfers.begin(), active_transfers.end(),
                       [](const std::unique_ptr<ActiveTransfer>& transfer) {
                         return transfer->completed;
                       }),
        active_transfers.end());

    while (next_task_index < tasks.size() &&
           static_cast<int>(active_transfers.size()) <
               max_concurrent_downloads) {
      start_transfer();
    }

    if (active_transfers.empty()) {
      break;
    }

    fd_set read_fds;
    fd_set write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    int max_fd = -1;
    for (const std::unique_ptr<ActiveTransfer>& transfer : active_transfers) {
      const int fd = transfer->session->socket_fd();
      const int directions = transfer->session->BlockDirections();
      if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) != 0 ||
          directions == 0) {
        FD_SET(fd, &read_fds);
      }
      if ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0 ||
          directions == 0) {
        FD_SET(fd, &write_fds);
      }
      if (fd > max_fd) {
        max_fd = fd;
      }
    }
    timeval timeout;
    timeout.tv_sec = kSocketWaitTimeoutSeconds;
    timeout.tv_usec = 0;
    const int select_result =
        select(max_fd + 1, &read_fds, &write_fds, nullptr, &timeout);
    if (select_result < 0) {
      throw std::runtime_error("download select failed: " +
                               std::string(strerror(errno)));
    }
    if (select_result == 0) {
      throw std::runtime_error("download socket wait timed out");
    }
  }
}

}  // namespace sshumdloader
