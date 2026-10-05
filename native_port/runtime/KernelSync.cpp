#include "Kernel.h"

#include "Log.h"
#include "Nt.h"
#include "Threads.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace cw::kernel {

namespace {

using namespace xbox;

constexpr UCHAR kDispatchLevel = 2;
constexpr UCHAR kNotificationEvent = 0;
constexpr UCHAR kSynchronizationEvent = 1;
constexpr UCHAR kTimerNotificationObject = 8;
constexpr UCHAR kTimerSynchronizationObject = 9;
constexpr SHORT kDpcObject = 19;

volatile ULONG g_keTickCount = 0;

// Raising IRQL to DISPATCH_LEVEL on the single-core Xbox stops other threads; a global lock reproduces that.
std::recursive_mutex g_dispatchLock;

std::mutex g_objectsMutex;
std::unordered_map<const void*, HANDLE> g_objectHandles;
std::unordered_map<HANDLE, DispatcherHeader*> g_handleObjects;

std::mutex g_timerMutex;
std::condition_variable g_timerSignal;
struct TimerState {
    std::chrono::steady_clock::time_point due;
    LONG periodMs;
    Kdpc* dpc;
};
std::map<Ktimer*, TimerState> g_timers;
std::deque<Kdpc*> g_dpcQueue;

UCHAR raiseIrql(UCHAR newIrql) {
    const UCHAR oldIrql = threads::currentIrql();
    if (oldIrql < kDispatchLevel && newIrql >= kDispatchLevel) {
        g_dispatchLock.lock();
    }
    threads::setCurrentIrql(newIrql);
    return oldIrql;
}

void lowerIrql(UCHAR newIrql) {
    const UCHAR oldIrql = threads::currentIrql();
    threads::setCurrentIrql(newIrql);
    if (oldIrql >= kDispatchLevel && newIrql < kDispatchLevel) {
        g_dispatchLock.unlock();
    }
}

// Returns the host event that backs an Xbox dispatcher object living in game memory.
HANDLE handleForObject(void* object) {
    std::lock_guard lock(g_objectsMutex);
    auto existing = g_objectHandles.find(object);
    if (existing != g_objectHandles.end()) {
        return existing->second;
    }

    auto* header = static_cast<DispatcherHeader*>(object);
    const bool manualReset = header->Type == kNotificationEvent || header->Type == kTimerNotificationObject;
    if (header->Type != kNotificationEvent && header->Type != kSynchronizationEvent &&
        header->Type != kTimerNotificationObject && header->Type != kTimerSynchronizationObject) {
        logf("dispatcher object %p has unsupported type %u; treating it as an event", object, header->Type);
    }
    HANDLE event = CreateEventW(nullptr, manualReset, header->SignalState != 0, nullptr);
    g_objectHandles[object] = event;
    return event;
}

LONG signalObject(DispatcherHeader* header) {
    const LONG previous = header->SignalState;
    header->SignalState = 1;
    SetEvent(handleForObject(header));
    return previous;
}

std::chrono::steady_clock::time_point dueTimeToTimePoint(LARGE_INTEGER dueTime) {
    const auto now = std::chrono::steady_clock::now();
    if (dueTime.QuadPart < 0) {
        return now + std::chrono::microseconds(-dueTime.QuadPart / 10);
    }
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);
    const LONGLONG current = (static_cast<LONGLONG>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
    const LONGLONG delta = dueTime.QuadPart > current ? dueTime.QuadPart - current : 0;
    return now + std::chrono::microseconds(delta / 10);
}

void queueDpc(Kdpc* dpc) {
    if (!dpc->Inserted) {
        dpc->Inserted = TRUE;
        g_dpcQueue.push_back(dpc);
    }
}

DWORD WINAPI dispatcherThread(void*) {
    threads::attachCurrentThread(0, 0);
    std::unique_lock lock(g_timerMutex);
    while (true) {
        auto nextDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        for (const auto& [timer, state] : g_timers) {
            nextDue = std::min(nextDue, state.due);
        }
        g_timerSignal.wait_until(lock, nextDue, [] { return !g_dpcQueue.empty(); });

        const auto now = std::chrono::steady_clock::now();
        for (auto it = g_timers.begin(); it != g_timers.end();) {
            Ktimer* timer = it->first;
            TimerState& state = it->second;
            if (state.due > now) {
                ++it;
                continue;
            }
            signalObject(&timer->Header);
            if (state.dpc != nullptr) {
                queueDpc(state.dpc);
            }
            if (state.periodMs > 0) {
                state.due += std::chrono::milliseconds(state.periodMs);
                ++it;
            } else {
                timer->Header.Inserted = FALSE;
                it = g_timers.erase(it);
            }
        }

        while (!g_dpcQueue.empty()) {
            Kdpc* dpc = g_dpcQueue.front();
            g_dpcQueue.pop_front();
            dpc->Inserted = FALSE;
            lock.unlock();
            const UCHAR oldIrql = raiseIrql(kDispatchLevel);
            dpc->Routine(dpc, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2);
            lowerIrql(oldIrql);
            lock.lock();
        }
    }
}

DWORD WINAPI tickThread(void*) {
    const ULONGLONG start = gameMilliseconds();
    while (true) {
        g_keTickCount = static_cast<ULONG>(gameMilliseconds() - start);
        Sleep(1);
    }
}

UCHAR __fastcall xKfRaiseIrql(UCHAR newIrql) {
    return raiseIrql(newIrql);
}

void __fastcall xKfLowerIrql(UCHAR newIrql) {
    lowerIrql(newIrql);
}

UCHAR __stdcall xKeRaiseIrqlToDpcLevel() {
    return raiseIrql(kDispatchLevel);
}

NTSTATUS __stdcall xNtCreateEvent(PHANDLE eventHandle, ObjectAttributes* objectAttributes, ULONG eventType, BOOLEAN initialState) {
    std::string name;
    if (objectAttributes != nullptr && objectAttributes->ObjectName != nullptr) {
        name = "Local\\cw_" + toString(objectAttributes->ObjectName);
    }
    HANDLE event = CreateEventA(nullptr, eventType == kNotificationEvent, initialState, name.empty() ? nullptr : name.c_str());
    if (event == nullptr) {
        return kStatusUnsuccessful;
    }
    *eventHandle = event;
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtSetEvent(HANDLE eventHandle, PLONG previousState) {
    if (previousState != nullptr) {
        *previousState = 0;
    }
    return SetEvent(eventHandle) ? kStatusSuccess : kStatusInvalidHandle;
}

NTSTATUS __stdcall xNtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout) {
    return NtWaitForSingleObject(handle, alertable, timeout);
}

NTSTATUS __stdcall xNtWaitForSingleObjectEx(HANDLE handle, CHAR waitMode, BOOLEAN alertable, PLARGE_INTEGER timeout) {
    return NtWaitForSingleObject(handle, alertable, timeout);
}

NTSTATUS __stdcall xObReferenceObjectByHandle(HANDLE handle, void* objectType, void** returnedObject) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return kStatusInvalidHandle;
    }
    auto* object = new DispatcherHeader{};
    object->Type = kNotificationEvent;
    std::lock_guard lock(g_objectsMutex);
    g_objectHandles[object] = duplicate;
    g_handleObjects[duplicate] = object;
    *returnedObject = object;
    return kStatusSuccess;
}

NTSTATUS __stdcall xObReferenceObjectByName(AnsiString* objectName, ULONG attributes, void* objectType, void* parseContext, void** object) {
    logf("ObReferenceObjectByName('%s') not supported", toString(objectName).c_str());
    return kStatusObjectNameNotFound;
}

void __fastcall xObfDereferenceObject(void* object) {
}

NTSTATUS __stdcall xKeWaitForSingleObject(void* object, ULONG waitReason, CHAR waitMode, BOOLEAN alertable, PLARGE_INTEGER timeout) {
    auto* header = static_cast<DispatcherHeader*>(object);
    const NTSTATUS status = NtWaitForSingleObject(handleForObject(object), alertable, timeout);
    if (status == kStatusSuccess && (header->Type == kSynchronizationEvent || header->Type == kTimerSynchronizationObject)) {
        header->SignalState = 0;
    }
    return status;
}

NTSTATUS __stdcall xKeWaitForMultipleObjects(ULONG count, void** objects, ULONG waitType, ULONG waitReason, CHAR waitMode,
    BOOLEAN alertable, PLARGE_INTEGER timeout, void* waitBlockArray) {
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    if (count > MAXIMUM_WAIT_OBJECTS) {
        return kStatusInvalidParameter;
    }
    for (ULONG index = 0; index < count; ++index) {
        handles[index] = handleForObject(objects[index]);
    }
    return NtWaitForMultipleObjects(count, handles, waitType, alertable, timeout);
}

LONG __stdcall xKeSetEvent(DispatcherHeader* event, LONG increment, BOOLEAN wait) {
    return signalObject(event);
}

void __stdcall xKeInitializeDpc(Kdpc* dpc, DeferredRoutine routine, void* context) {
    dpc->Type = kDpcObject;
    dpc->Inserted = FALSE;
    dpc->Routine = routine;
    dpc->DeferredContext = context;
}

BOOLEAN __stdcall xKeInsertQueueDpc(Kdpc* dpc, void* argument1, void* argument2) {
    std::lock_guard lock(g_timerMutex);
    if (dpc->Inserted) {
        return FALSE;
    }
    dpc->SystemArgument1 = argument1;
    dpc->SystemArgument2 = argument2;
    queueDpc(dpc);
    g_timerSignal.notify_all();
    return TRUE;
}

BOOLEAN __stdcall xKeRemoveQueueDpc(Kdpc* dpc) {
    std::lock_guard lock(g_timerMutex);
    if (!dpc->Inserted) {
        return FALSE;
    }
    std::erase(g_dpcQueue, dpc);
    dpc->Inserted = FALSE;
    return TRUE;
}

void __stdcall xKeInitializeTimerEx(Ktimer* timer, ULONG type) {
    std::memset(timer, 0, sizeof(Ktimer));
    timer->Header.Type = static_cast<UCHAR>(kTimerNotificationObject + type);
    timer->Header.Size = sizeof(Ktimer) / sizeof(LONG);
    timer->Header.WaitListHead.Flink = timer->Header.WaitListHead.Blink = &timer->Header.WaitListHead;
    timer->TimerListEntry.Flink = timer->TimerListEntry.Blink = &timer->TimerListEntry;
}

BOOLEAN __stdcall xKeSetTimerEx(Ktimer* timer, LARGE_INTEGER dueTime, LONG period, Kdpc* dpc) {
    std::lock_guard lock(g_timerMutex);
    const BOOLEAN wasInserted = g_timers.erase(timer) != 0;
    timer->Header.SignalState = 0;
    timer->Header.Inserted = TRUE;
    timer->Dpc = dpc;
    timer->Period = period;
    ResetEvent(handleForObject(timer));
    g_timers[timer] = {dueTimeToTimePoint(dueTime), period, dpc};
    g_timerSignal.notify_all();
    return wasInserted;
}

BOOLEAN __stdcall xKeSetTimer(Ktimer* timer, LARGE_INTEGER dueTime, Kdpc* dpc) {
    return xKeSetTimerEx(timer, dueTime, 0, dpc);
}

BOOLEAN __stdcall xKeCancelTimer(Ktimer* timer) {
    std::lock_guard lock(g_timerMutex);
    timer->Header.Inserted = FALSE;
    return g_timers.erase(timer) != 0;
}

NTSTATUS __stdcall xKeDelayExecutionThread(CHAR waitMode, BOOLEAN alertable, PLARGE_INTEGER interval) {
    return NtDelayExecution(alertable, interval);
}

void __stdcall xKeStallExecutionProcessor(ULONG microseconds) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart < microseconds);
}

ULONGLONG __stdcall xKeQueryPerformanceCounter() {
    // Game time (CW_TIME_SCALE) at the host counter's rate.
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<ULONGLONG>(gameSeconds() * frequency);
}

ULONGLONG __stdcall xKeQueryPerformanceFrequency() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

void __stdcall xKeQuerySystemTime(PLARGE_INTEGER currentTime) {
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);
    currentTime->LowPart = fileTime.dwLowDateTime;
    currentTime->HighPart = static_cast<LONG>(fileTime.dwHighDateTime);
}

struct XboxThreadStart {
    void* startRoutine;
    void* startContext;
    void* systemRoutine;
    ULONG tlsDataSize;
};

DWORD WINAPI xboxThreadEntry(void* parameter) {
    XboxThreadStart start = *static_cast<XboxThreadStart*>(parameter);
    delete static_cast<XboxThreadStart*>(parameter);
    threads::attachCurrentThread(start.tlsDataSize, 0);

    if (start.systemRoutine != nullptr) {
        reinterpret_cast<void(__stdcall*)(void*, void*)>(start.systemRoutine)(start.startRoutine, start.startContext);
    } else {
        reinterpret_cast<void(__stdcall*)(void*)>(start.startRoutine)(start.startContext);
    }
    return 0;
}

NTSTATUS __stdcall xPsCreateSystemThreadEx(PHANDLE threadHandle, ULONG threadExtensionSize, ULONG kernelStackSize, ULONG tlsDataSize,
    PHANDLE threadId, void* startRoutine, void* startContext, BOOLEAN createSuspended, BOOLEAN debuggerThread, void* systemRoutine) {
    auto* start = new XboxThreadStart{startRoutine, startContext, systemRoutine, tlsDataSize};
    const SIZE_T stackSize = std::max<SIZE_T>(kernelStackSize, 0x100000);
    DWORD hostThreadId = 0;
    HANDLE thread = CreateThread(nullptr, stackSize, &xboxThreadEntry, start, createSuspended ? CREATE_SUSPENDED : 0, &hostThreadId);
    if (thread == nullptr) {
        delete start;
        return kStatusNoMemory;
    }
    logf("PsCreateSystemThreadEx(start=%p, tls=%lu) -> thread %lu", startRoutine, tlsDataSize, hostThreadId);
    *threadHandle = thread;
    if (threadId != nullptr) {
        *threadId = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(hostThreadId));
    }
    return kStatusSuccess;
}

void __stdcall xPsTerminateSystemThread(NTSTATUS exitStatus) {
    ExitThread(static_cast<DWORD>(exitStatus));
}

std::mutex g_criticalSectionsMutex;
std::unordered_map<const void*, CRITICAL_SECTION*> g_criticalSections;

// Statically initialized Xbox critical sections already use every field, so host locks live in a side table.
CRITICAL_SECTION* hostCriticalSection(void* xboxCriticalSection) {
    std::lock_guard lock(g_criticalSectionsMutex);
    CRITICAL_SECTION*& entry = g_criticalSections[xboxCriticalSection];
    if (entry == nullptr) {
        entry = new CRITICAL_SECTION;
        InitializeCriticalSection(entry);
    }
    return entry;
}

void __stdcall xRtlInitializeCriticalSection(void* criticalSection) {
    hostCriticalSection(criticalSection);
}

void __stdcall xRtlEnterCriticalSection(void* criticalSection) {
    EnterCriticalSection(hostCriticalSection(criticalSection));
}

void __stdcall xRtlLeaveCriticalSection(void* criticalSection) {
    LeaveCriticalSection(hostCriticalSection(criticalSection));
}

BOOLEAN __stdcall xKeSynchronizeExecution(void* interrupt, BOOLEAN(__stdcall* routine)(void*), void* context) {
    std::lock_guard lock(g_dispatchLock);
    return routine(context);
}

void __stdcall xKeInitializeInterrupt(void* interrupt, void* serviceRoutine, void* serviceContext, ULONG vector, UCHAR irql, ULONG mode, BOOLEAN shareVector) {
    logf("KeInitializeInterrupt(vector=%lu) recorded; hardware interrupts are not delivered", vector);
}

BOOLEAN __stdcall xKeConnectInterrupt(void* interrupt) {
    return TRUE;
}

BOOLEAN __stdcall xKeDisconnectInterrupt(void* interrupt) {
    return TRUE;
}

} // namespace

void startSystemThreads() {
    CloseHandle(CreateThread(nullptr, 0, &tickThread, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0x100000, &dispatcherThread, nullptr, 0, nullptr));
}

void registerSyncExports() {
    registerExport(98, reinterpret_cast<void*>(&xKeConnectInterrupt));
    registerExport(97, reinterpret_cast<void*>(&xKeCancelTimer));
    registerExport(99, reinterpret_cast<void*>(&xKeDelayExecutionThread));
    registerExport(100, reinterpret_cast<void*>(&xKeDisconnectInterrupt));
    registerExport(107, reinterpret_cast<void*>(&xKeInitializeDpc));
    registerExport(109, reinterpret_cast<void*>(&xKeInitializeInterrupt));
    registerExport(113, reinterpret_cast<void*>(&xKeInitializeTimerEx));
    registerExport(119, reinterpret_cast<void*>(&xKeInsertQueueDpc));
    registerExport(126, reinterpret_cast<void*>(&xKeQueryPerformanceCounter));
    registerExport(127, reinterpret_cast<void*>(&xKeQueryPerformanceFrequency));
    registerExport(128, reinterpret_cast<void*>(&xKeQuerySystemTime));
    registerExport(129, reinterpret_cast<void*>(&xKeRaiseIrqlToDpcLevel));
    registerExport(137, reinterpret_cast<void*>(&xKeRemoveQueueDpc));
    registerExport(145, reinterpret_cast<void*>(&xKeSetEvent));
    registerExport(149, reinterpret_cast<void*>(&xKeSetTimer));
    registerExport(150, reinterpret_cast<void*>(&xKeSetTimerEx));
    registerExport(151, reinterpret_cast<void*>(&xKeStallExecutionProcessor));
    registerExport(153, reinterpret_cast<void*>(&xKeSynchronizeExecution));
    registerExport(156, const_cast<ULONG*>(&g_keTickCount));
    registerExport(158, reinterpret_cast<void*>(&xKeWaitForMultipleObjects));
    registerExport(159, reinterpret_cast<void*>(&xKeWaitForSingleObject));
    registerExport(160, reinterpret_cast<void*>(&xKfRaiseIrql));
    registerExport(161, reinterpret_cast<void*>(&xKfLowerIrql));
    registerExport(189, reinterpret_cast<void*>(&xNtCreateEvent));
    registerExport(225, reinterpret_cast<void*>(&xNtSetEvent));
    registerExport(233, reinterpret_cast<void*>(&xNtWaitForSingleObject));
    registerExport(234, reinterpret_cast<void*>(&xNtWaitForSingleObjectEx));
    registerExport(246, reinterpret_cast<void*>(&xObReferenceObjectByHandle));
    registerExport(247, reinterpret_cast<void*>(&xObReferenceObjectByName));
    registerExport(250, reinterpret_cast<void*>(&xObfDereferenceObject));
    registerExport(255, reinterpret_cast<void*>(&xPsCreateSystemThreadEx));
    registerExport(258, reinterpret_cast<void*>(&xPsTerminateSystemThread));
    registerExport(277, reinterpret_cast<void*>(&xRtlEnterCriticalSection));
    registerExport(291, reinterpret_cast<void*>(&xRtlInitializeCriticalSection));
    registerExport(294, reinterpret_cast<void*>(&xRtlLeaveCriticalSection));
}

} // namespace cw::kernel
