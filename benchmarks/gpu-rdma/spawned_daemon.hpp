// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mailbox_support.hpp"
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

// The child owns duplicated DMA-BUF/lease-read descriptors only. EOF on the
// parent-only lease writer requests ordinary daemon teardown, including its MRs.
class SpawnedDaemon {
    pid_t pid_ = -1;
    int lease_ = -1, status_ = 0;
    bool reaped_ = false;
public:
    ~SpawnedDaemon() { if (lease_ >= 0) ::close(lease_); }
    void start(const std::string &path, const std::string &peer, const std::string &socket_path, int buffer_fd) {
        mailbox_require(pid_ < 0, "daemon already started");
        int pipe_fds[2];
        mailbox_require(!pipe(pipe_fds), "cannot create daemon parent lease");
        int buffer = fcntl(buffer_fd, F_DUPFD_CLOEXEC, 10);
        int reader = fcntl(pipe_fds[0], F_DUPFD_CLOEXEC, 10);
        lease_ = fcntl(pipe_fds[1], F_DUPFD_CLOEXEC, 10);
        ::close(pipe_fds[0]); ::close(pipe_fds[1]);
        if (buffer < 0 || reader < 0 || lease_ < 0) {
            if (buffer >= 0) ::close(buffer);
            if (reader >= 0) ::close(reader);
            if (lease_ >= 0) ::close(lease_);
            lease_ = -1;
            throw std::runtime_error("cannot duplicate daemon descriptors");
        }
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        bool initialized = !error;
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, buffer, 3);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, reader, 4);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, buffer);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, reader);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, lease_);
        std::vector<std::string> environment;
        for (char **item = environ; *item; ++item)
            if (std::strncmp(*item, "MCDMA_RPCD_SOCKET=", 18)) environment.emplace_back(*item);
        environment.emplace_back("MCDMA_RPCD_SOCKET=" + socket_path);
        std::vector<char *> env;
        for (auto &item : environment) env.push_back(item.data());
        env.push_back(nullptr);
        const char *arguments[] = {path.c_str(), "connect", "--buffer-fd", "3", "--parent-fd", "4", peer.c_str(), nullptr};
        if (!error) error = posix_spawn(&pid_, path.c_str(), &actions, nullptr,
                                       const_cast<char *const *>(arguments), env.data());
        if (initialized) posix_spawn_file_actions_destroy(&actions);
        ::close(buffer); ::close(reader);
        if (error) {
            ::close(lease_); lease_ = -1; pid_ = -1;
            throw std::runtime_error("cannot spawn daemon");
        }
        std::puts("GPU_MAILBOX_DAEMON_STARTED buffer_fd_inherited=1 parent_lease=1");
    }
    bool running() {
        if (pid_ < 0 || reaped_) return false;
        pid_t result = waitpid(pid_, &status_, WNOHANG);
        if (result == pid_) { reaped_ = true; return false; }
        if (result < 0 && errno != EINTR) throw std::runtime_error("cannot inspect daemon child");
        return true;
    }
    bool stop(unsigned timeout) {
        if (lease_ >= 0) { ::close(lease_); lease_ = -1; }
        if (pid_ < 0) return true;
        uint64_t deadline = mailbox_now() + static_cast<uint64_t>(timeout) * 1000000000ull;
        while (running()) {
            if (mailbox_now() >= deadline) {
                std::puts("GPU_MAILBOX_DAEMON_EXIT clean=0 child_running=1 backing_retained=1");
                return false;
            }
            timespec pause{0, 10000000}; nanosleep(&pause, nullptr);
        }
        bool clean = WIFEXITED(status_) && WEXITSTATUS(status_) == 0;
        std::printf("GPU_MAILBOX_DAEMON_EXIT clean=%u child_running=0 exit_status=%d\n",
                    clean, WIFEXITED(status_) ? WEXITSTATUS(status_) : -1);
        return clean;
    }
};
