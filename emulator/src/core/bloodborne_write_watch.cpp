// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_write_watch.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
// After windows.h, which it needs.
#include <tlhelp32.h>
#elif defined(__linux__)
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace Core::Bloodborne {

#ifdef _WIN32
namespace {

std::atomic<WriteObserver> write_observer{nullptr};
std::once_flag handler_once;

LONG WINAPI OnWatchedWrite(EXCEPTION_POINTERS* exception) noexcept {
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP ||
        (exception->ContextRecord->Dr6 & 1) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    exception->ContextRecord->Dr6 = 0;
    if (const WriteObserver observer = write_observer.load(); observer != nullptr) {
        observer(exception->ContextRecord->Rip, exception->ContextRecord->Rsp);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

size_t ReadableBytesAt(u64 address, size_t most) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return 0;
    }
    const u64 end = reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize;
    return end > address ? static_cast<size_t>(std::min<u64>(end - address, most)) : 0;
}

bool WatchByteWrites(u64 address, WriteObserver observer) {
    std::call_once(handler_once, [] { AddVectoredExceptionHandler(1, OnWatchedWrite); });
    write_observer.store(observer);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    int armed = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                       entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = address;
                // L0 enabled, R/W0 = 01 (writes), LEN0 = 00 (one byte).
                context.Dr7 = (context.Dr7 & ~u64{0xF0003}) | 0x10001;
                if (SetThreadContext(thread, &context)) {
                    ++armed;
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return armed > 0;
}
#else
#ifdef __linux__
// process_vm_readv on this process copies until the first byte it cannot read (unmapped, PROT_NONE,
// a hole between mappings) and returns how far it got, which is the answer. It is one system
// call, so it is safe in the signal handler the write watch's observer will run in (no
// /proc/self/maps, no allocation). When the call itself is refused (seccomp, no such call),
// nothing counts as readable, as Windows answers when VirtualQuery fails.
size_t ReadableBytesAt(u64 address, size_t most) {
    // One page per call, each ending on a page boundary: partial transfers are only promised per
    // iovec, so a whole page either counts or does not.
    constexpr size_t Page = 4096;
    unsigned char chunk[Page];
    size_t readable = 0;
    while (readable < most) {
        const u64 at = address + readable;
        const size_t want = std::min<size_t>(Page - (at & (Page - 1)), most - readable);
        iovec local{chunk, want};
        iovec remote{reinterpret_cast<void*>(at), want};
        const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (got <= 0) {
            break;
        }
        readable += static_cast<size_t>(got);
        if (static_cast<size_t>(got) < want) {
            break; // stopped at an unreadable page
        }
    }
    return readable;
}
#else
// No cheap, signal-safe query here (macOS, FreeBSD): nothing counts as readable, so a scan reads
// nothing rather than past a stack's end.
size_t ReadableBytesAt(u64, size_t) {
    return 0;
}
#endif

#if defined(__linux__) && defined(__x86_64__)
namespace {

// Windows' debug register 0 per thread, here one perf_event hardware breakpoint per thread
// (PERF_TYPE_BREAKPOINT, a one-byte write watch, user space only: allowed for the process's own
// threads at the default perf_event_paranoid 2). Each event signals its own thread (F_SETOWN_EX
// with F_OWNER_TID, F_SETSIG) right after the write, as the debug exception does on Windows:
// PERF_EVENT_IOC_REFRESH gives it a budget of overflows, each notifying (POLL_IN) while the event
// stays on; with a budget of 1 it would pause after the first and miss a write right behind it.
// The handler tops the budget up. A real-time signal (queued, not merged) far from the
// runtime's own (SIGSEGV/BUS/ILL/TRAP, SIGALRM, SIGPROF, SIGUSR2, SIGVTALRM).
const int WatchSignal = SIGRTMAX - 1;
constexpr size_t MaxWatchedThreads = 1024;
constexpr int OverflowBudget = 1 << 30;

std::atomic<WriteObserver> write_observer{nullptr};
std::mutex arm_lock;
u64 armed_address = 0;
size_t armed_count = 0;
pid_t armed_tids[MaxWatchedThreads];
int armed_fds[MaxWatchedThreads];

void OnWatchedWrite(int, siginfo_t* info, void* context) {
    if (info == nullptr || context == nullptr) {
        return;
    }
    const auto* uc = static_cast<const ucontext_t*>(context);
    if (const WriteObserver observer = write_observer.load(); observer != nullptr) {
        observer(static_cast<u64>(uc->uc_mcontext.gregs[REG_RIP]),
                 static_cast<u64>(uc->uc_mcontext.gregs[REG_RSP]));
    }
    // si_fd is the event's descriptor (F_SETSIG): the budget back to where it was.
    ioctl(info->si_fd, PERF_EVENT_IOC_REFRESH, 1);
}

int OpenWriteBreakpoint(pid_t tid, u64 address) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_W;
    attr.bp_addr = address;
    attr.bp_len = HW_BREAKPOINT_LEN_1;
    attr.sample_period = 1;
    attr.wakeup_events = 1;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    const int fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, tid, -1, -1,
                                            PERF_FLAG_FD_CLOEXEC));
    if (fd < 0) {
        return -1;
    }
    f_owner_ex owner{F_OWNER_TID, tid};
    if (fcntl(fd, F_SETFL, O_ASYNC) != 0 || fcntl(fd, F_SETSIG, WatchSignal) != 0 ||
        fcntl(fd, F_SETOWN_EX, &owner) != 0 ||
        ioctl(fd, PERF_EVENT_IOC_REFRESH, OverflowBudget) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool IsArmed(pid_t tid) {
    for (size_t i = 0; i < armed_count; ++i) {
        if (armed_tids[i] == tid) {
            return true;
        }
    }
    return false;
}

} // namespace

bool WatchByteWrites(u64 address, WriteObserver observer) {
    static std::once_flag handler_once;
    std::call_once(handler_once, [] {
        struct sigaction action{};
        action.sa_sigaction = OnWatchedWrite;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(WatchSignal, &action, nullptr);
    });
    write_observer.store(observer);
    std::lock_guard lock{arm_lock};
    if (address != armed_address) {
        // Another byte: the old events go (Windows overwrites Dr0 the same way).
        for (size_t i = 0; i < armed_count; ++i) {
            close(armed_fds[i]);
        }
        armed_count = 0;
        armed_address = address;
    }
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) {
        return armed_count > 0;
    }
    // Every thread but this one, as on Windows (which cannot set a running thread's registers).
    const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
    while (const dirent* entry = readdir(tasks)) {
        char* end = nullptr;
        const long tid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || tid == self || IsArmed(tid) ||
            armed_count == MaxWatchedThreads) {
            continue;
        }
        if (const int fd = OpenWriteBreakpoint(static_cast<pid_t>(tid), address); fd >= 0) {
            armed_tids[armed_count] = static_cast<pid_t>(tid);
            armed_fds[armed_count] = fd;
            ++armed_count;
        }
    }
    closedir(tasks);
    return armed_count > 0;
}
#else
bool WatchByteWrites(u64, WriteObserver) {
    return false;
}
#endif
#endif

} // namespace Core::Bloodborne
