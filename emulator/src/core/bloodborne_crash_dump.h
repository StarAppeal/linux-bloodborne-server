// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Bloodborne {

// The crash catcher for the random co-op crashes whose logs just stop: an unhandled exception,
// std::terminate or SIGABRT writes crash-<time>.txt (see FormatCrashLine) and, on Windows, a
// minidump crash-<time>.dmp into the log folder, without the (asynchronous) logger. Linux x86-64
// writes the same text (the system's core dump stands in for the minidump); the runtime's own
// fault handlers report through WriteCrashReport, so no SIGSEGV handler is installed here.
void InstallCrashCatcher(u64 image_base, u64 image_size);

// What a POSIX fault handler has: the signal, its siginfo_t* and ucontext_t*.
struct CrashSignal {
    int signal;
    const void* info;
    const void* context;
};

// Writes the report for an exception (Windows: EXCEPTION_POINTERS*; POSIX: CrashSignal*; nullptr
// outside one). Only the first call writes; later ones return at once.
void WriteCrashReport(void* exception_pointers);

// A freeze (see GameThreadStalled): freeze-<time>.txt with the stuck thread's registers and its
// stack's game addresses (Windows suspends the thread for the read and writes freeze-<time>.dmp;
// Linux has the thread copy them in a signal handler).
void WriteFreezeReport(u32 thread_id, s64 stalled_ms);

// The party tick's heartbeat (the game thread), and the window's close: a game stuck for
// GameStallMs writes the freeze report before the process ends.
void NoteGameThreadTick();
void ReportFreezeOnClose();

} // namespace Core::Bloodborne
