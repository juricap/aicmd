#include "ssh_process.h"
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
extern char** environ;
#endif
namespace ac {
struct SshPipe::Impl {
#ifdef _WIN32
    HANDLE input = nullptr, output = nullptr, error = nullptr, process = nullptr, job = nullptr;
#else
    int input = -1, output = -1, error = -1; pid_t process = -1;
#endif
    std::thread errors;
    std::mutex mutex;
    std::string message;
};
int64_t SshPipe::now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
SshPipe::SshPipe(const std::string& host) : impl(std::make_unique<Impl>()) {
    if (!valid_ssh_host(host)) throw std::runtime_error("Invalid SSH host alias");
    std::vector<std::string> args = {"ssh","-T","-oBatchMode=yes","-oStrictHostKeyChecking=yes","-oClearAllForwardings=yes","-oConnectTimeout=10","-oServerAliveInterval=10","-oServerAliveCountMax=2","-s",host,"sftp"};
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE childIn{},childOut{},childError{};
    auto cleanup = [&] { for (auto h : {childIn,childOut,childError}) if (h) CloseHandle(h); };
    if (!CreatePipe(&childIn,&impl->input,&sa,65536) || !CreatePipe(&impl->output,&childOut,&sa,65536) || !CreatePipe(&impl->error,&childError,&sa,4096)) {
        cleanup(); for (auto h : {impl->input,impl->output,impl->error}) if (h) CloseHandle(h);
        throw std::runtime_error("Cannot create SSH pipes");
    }
    for (auto h : {impl->input,impl->output,impl->error}) SetHandleInformation(h,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childIn; startup.StartupInfo.hStdOutput = childOut; startup.StartupInfo.hStdError = childError;
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size); std::vector<char> attributes(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    BOOL initialized = InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&size);
    HANDLE handles[] = {childIn,childOut,childError};
    BOOL updated = initialized && UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr);
    // Arguments are fixed switches and a validated alias; no shell is involved.
    std::wstring command; for (const auto& arg : args) { if (!command.empty()) command += L' '; command += fs::u8path(arg).native(); }
    PROCESS_INFORMATION process{};
    BOOL started = updated && CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    cleanup();
    if (!started) { for (auto h : {impl->input,impl->output,impl->error}) CloseHandle(h); throw std::runtime_error("Cannot launch OpenSSH ssh.exe"); }
    impl->job = CreateJobObjectW(nullptr,nullptr);
    if (!impl->job || !AssignProcessToJobObject(impl->job,process.hProcess)) {
        TerminateProcess(process.hProcess,1); CloseHandle(process.hThread); CloseHandle(process.hProcess);
        if (impl->job) CloseHandle(impl->job);
        for (auto h : {impl->input,impl->output,impl->error}) CloseHandle(h);
        throw std::runtime_error("Cannot isolate SSH child process");
    }
    ResumeThread(process.hThread); CloseHandle(process.hThread); impl->process = process.hProcess;
#else
    // Blocking writes return EPIPE instead of terminating the application.
    signal(SIGPIPE,SIG_IGN);
    int in[2]{-1,-1},out[2]{-1,-1},err[2]{-1,-1};
    if (pipe(in) || pipe(out) || pipe(err)) { for (int fd : {in[0],in[1],out[0],out[1],err[0],err[1]}) if (fd >= 0) close(fd); throw std::runtime_error("Cannot create SSH pipes"); }
    for (int fd : {in[0],in[1],out[0],out[1],err[0],err[1]}) fcntl(fd,F_SETFD,FD_CLOEXEC);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,in[0],0); posix_spawn_file_actions_adddup2(&actions,out[1],1); posix_spawn_file_actions_adddup2(&actions,err[1],2);
    for (int fd : {in[0],in[1],out[0],out[1],err[0],err[1]}) posix_spawn_file_actions_addclose(&actions,fd);
    std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
    posix_spawnattr_t attr; posix_spawnattr_init(&attr); posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attr,0);
    int rc = posix_spawnp(&impl->process,"ssh",&actions,&attr,argv.data(),environ); posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attr);
    close(in[0]); close(out[1]); close(err[1]);
    if (rc) { close(in[1]); close(out[0]); close(err[0]); throw std::runtime_error("Cannot launch ssh"); }
    impl->input = in[1]; impl->output = out[0]; impl->error = err[0];
#endif
    activity = now();
    impl->errors = std::thread([this] {
        char data[1024];
        for (;;) {
#ifdef _WIN32
            DWORD count = 0; if (!ReadFile(impl->error,data,sizeof(data),&count,nullptr) || !count) break;
#else
            auto count = ::read(impl->error,data,sizeof(data)); if (count <= 0) break;
#endif
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->message.size() < 4096) impl->message.append(data,std::min<size_t>(count,4096-impl->message.size()));
        }
    });
}
void SshPipe::kill() {
#ifdef _WIN32
    if (impl->job) TerminateJobObject(impl->job,1);
#else
    if (impl->process > 0) ::kill(-impl->process,SIGKILL);
#endif
}
SshPipe::~SshPipe() {
    kill();
    if (impl->errors.joinable()) impl->errors.join();
#ifdef _WIN32
    for (auto h : {impl->input,impl->output,impl->error,impl->process,impl->job}) if (h) CloseHandle(h);
#else
    for (auto fd : {impl->input,impl->output,impl->error}) if (fd >= 0) close(fd);
    if (impl->process > 0) waitpid(impl->process,nullptr,0);
#endif
}
std::string SshPipe::diagnostic() { std::lock_guard<std::mutex> lock(impl->mutex); return safe_text(impl->message); }
void SshPipe::read(char* data,size_t size) {
    while (size) {
#ifdef _WIN32
        DWORD count = 0; BOOL ok = ReadFile(impl->output,data,static_cast<DWORD>(std::min<size_t>(size,65536)),&count,nullptr);
        if (!ok || !count) throw std::runtime_error("SSH disconnected. "+diagnostic());
#else
        auto count = ::read(impl->output,data,size); if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("SSH disconnected. "+diagnostic());
#endif
        data += count; size -= count; activity = now();
    }
}
void SshPipe::write(const char* data,size_t size) {
    while (size) {
#ifdef _WIN32
        DWORD count = 0; BOOL ok = WriteFile(impl->input,data,static_cast<DWORD>(std::min<size_t>(size,65536)),&count,nullptr);
        if (!ok || !count) throw std::runtime_error("SSH write failed. "+diagnostic());
#else
        auto count = ::write(impl->input,data,size); if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("SSH write failed. "+diagnostic());
#endif
        data += count; size -= count; activity = now();
    }
}
SshDeadline::SshDeadline(SshPipe& p,Cancel cancel) : pipe(p) {
    pipe.activity = SshPipe::now();
    monitor = std::thread([this,cancel] {
        std::unique_lock<std::mutex> lock(mutex);
        while (!cv.wait_for(lock,std::chrono::milliseconds(50),[this]{return finished;})) {
            if ((cancel && cancel->load()) || SshPipe::now()-pipe.activity.load() > 30000) { pipe.kill(); return; }
        }
    });
}
SshDeadline::~SshDeadline() { { std::lock_guard<std::mutex> lock(mutex); finished = true; } cv.notify_one(); monitor.join(); }
}
