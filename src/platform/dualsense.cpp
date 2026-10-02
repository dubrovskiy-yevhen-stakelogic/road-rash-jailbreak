// The DualSense over the Windows HID class driver: open, read, write, enumerate. Ported from gt2-play (MIT)
// src\platform\input\dualsense.cpp; the enumeration is ours (SetupAPI instead of DirectInput's device path).
#include "platform/dualsense.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace rr::platform {

namespace {
bool IsDualSense(USHORT vid, USHORT pid) { return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2); }
} // namespace

struct DualSenseDevice::Impl {
    std::wstring path;
    HANDLE device = INVALID_HANDLE_VALUE;
    std::atomic<bool> ok{false};
    bool bt = false, dirty = false;
    std::atomic<bool> stop{false}, inputFailed{false}, outputFailed{false};
    std::atomic<uint32_t> reports{0};
    std::atomic<uint8_t> lastId{0};
    DWORD inputLength = 0, outputLength = 0;
    bool haveInput = false, streaming = false;
    PhysicalPad input;
    std::chrono::steady_clock::time_point inputTime;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker, reader;
    DualSenseOutput state;
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();

    void Read() {
        OVERLAPPED io{};
        io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!io.hEvent) {
            inputFailed = true;
            return;
        }
        std::vector<uint8_t> bytes(inputLength);
        while (!stop) {
            ResetEvent(io.hEvent);
            DWORD count = 0;
            BOOL received = ReadFile(device, bytes.data(), inputLength, &count, &io);
            if (!received && GetLastError() == ERROR_IO_PENDING) {
                DWORD wait;
                do {
                    wait = WaitForSingleObject(io.hEvent, 50);
                } while (wait == WAIT_TIMEOUT && !stop);
                if (wait != WAIT_OBJECT_0) CancelIoEx(device, &io);
                received = GetOverlappedResult(device, &io, &count, TRUE);
            }
            if (stop) break;
            if (!received) {
                inputFailed = true;
                break;
            }
            PhysicalPad pad;
            if (count > 0 && DecodeDualSenseInput(bytes.data(), count, bt, pad)) {
                std::lock_guard lock(mutex);
                if (!haveInput)
                    std::printf("DualSense: native %s input active (report 0x%02X)\n", bt ? "Bluetooth" : "USB",
                                unsigned(bytes[0]));
                input = pad;
                haveInput = true;
                // the USB report and the Bluetooth 0x31 one stream continuously; the simple Bluetooth report is
                // sent on a change only, so it never goes stale
                streaming = !(bt && bytes[0] == 0x01);
                inputTime = std::chrono::steady_clock::now();
                ++reports;
                lastId = bytes[0];
            }
        }
        CloseHandle(io.hEvent);
    }

    void Run() {
        uint8_t sequence = 0;
        OVERLAPPED io{};
        io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!io.hEvent) {
            outputFailed = true;
            return;
        }
        DualSenseOutput previous;
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop || dirty; });
            const bool ending = stop;
            DualSenseOutput next = state;
            if (ending || std::chrono::steady_clock::now() - touched > std::chrono::milliseconds(500)) next = {};
            dirty = false;
            if (next == previous && !ending) continue;
            if (ending && previous == DualSenseOutput{}) break; // nothing on: leave the pad's mode as it is
            previous = next;
            lock.unlock();
            const auto report = DualSenseOutputReport(next, bt, sequence++);
            std::vector<uint8_t> packet(outputLength, 0);
            std::copy_n(report.begin(), bt ? 78 : 48, packet.begin());
            ResetEvent(io.hEvent);
            DWORD written = 0;
            // Win32 HID writes must be padded to the longest output report.
            BOOL sent = WriteFile(device, packet.data(), outputLength, &written, &io);
            if (!sent && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(io.hEvent, 100) != WAIT_OBJECT_0) CancelIoEx(device, &io);
                sent = GetOverlappedResult(device, &io, &written, TRUE);
            }
            if (!sent || written != outputLength) {
                outputFailed = true; // the input keeps working; no rumble / triggers until the pad is reconnected
                std::fprintf(stderr, "DualSense: HID output failed (%lu); reconnect the controller to retry\n",
                             GetLastError());
            }
            lock.lock();
            if (ending || outputFailed) break;
            // bound the USB / Bluetooth traffic when the game updates faster than the pad
            wake.wait_for(lock, std::chrono::milliseconds(8), [&] { return stop.load(); });
        }
        CloseHandle(io.hEvent);
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
        if (reader.joinable()) reader.join();
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
    }
};

DualSenseDevice::DualSenseDevice(const std::wstring& path) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    p.path = path;
    if (path.empty()) return;
    p.device = CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (p.device == INVALID_HANDLE_VALUE) return;
    HIDD_ATTRIBUTES a{};
    a.Size = sizeof(a);
    if (!HidD_GetAttributes(p.device, &a) || !IsDualSense(a.VendorID, a.ProductID)) return;
    PHIDP_PREPARSED_DATA prep = nullptr;
    if (!HidD_GetPreparsedData(p.device, &prep)) return;
    HIDP_CAPS caps{};
    const auto status = HidP_GetCaps(prep, &caps);
    HidD_FreePreparsedData(prep);
    if (status != HIDP_STATUS_SUCCESS || caps.UsagePage != 1 || caps.Usage != 5) return;
    const auto transport = DualSenseReportTransport(caps.InputReportByteLength, caps.OutputReportByteLength);
    if (transport == DualSenseTransport::Unsupported) return;
    p.bt = transport == DualSenseTransport::Bluetooth;
    p.outputLength = caps.OutputReportByteLength;
    p.inputLength = caps.InputReportByteLength;
    p.ok = true;
    p.reader = std::thread([&p] { p.Read(); });
    p.worker = std::thread([&p] { p.Run(); });
    std::printf("DualSense%s: native %s input, rumble and adaptive triggers available\n",
                a.ProductID == 0x0DF2 ? " Edge" : "", p.bt ? "Bluetooth" : "USB");
}
DualSenseDevice::~DualSenseDevice() = default;
bool DualSenseDevice::Ok() const { return impl_->ok && !impl_->inputFailed; }
bool DualSenseDevice::Bluetooth() const { return impl_->bt; }
const std::wstring& DualSenseDevice::Path() const { return impl_->path; }
uint32_t DualSenseDevice::ReportsRead() const { return impl_->reports; }
uint8_t DualSenseDevice::LastReportId() const { return impl_->lastId; }

int DualSenseDevice::ReadPad(PhysicalPad& pad) const {
    auto& p = *impl_;
    if (!p.ok || p.inputFailed) return -1;
    std::lock_guard lock(p.mutex);
    if (!p.haveInput) return 0;
    const bool stale = p.streaming && std::chrono::steady_clock::now() - p.inputTime > std::chrono::milliseconds(500);
    if (stale) {
        pad = PhysicalPad{};
        pad.connected = true;
        pad.style = PadStyle::PlayStation;
        pad.source = p.input.source;
    } else {
        pad = p.input;
    }
    return 1;
}

void DualSenseDevice::Motors(uint8_t small, uint8_t large) {
    auto& p = *impl_;
    {
        std::lock_guard lock(p.mutex);
        p.state.small = small;
        p.state.large = large;
        p.dirty = true;
        p.touched = std::chrono::steady_clock::now();
    }
    p.wake.notify_one();
}
void DualSenseDevice::Triggers(uint8_t rightForce, uint8_t leftForce) {
    auto& p = *impl_;
    {
        std::lock_guard lock(p.mutex);
        p.state.rightForce = rightForce;
        p.state.leftForce = leftForce;
        p.dirty = true;
        p.touched = std::chrono::steady_clock::now();
    }
    p.wake.notify_one();
}

std::vector<std::wstring> EnumerateDualSensePaths() {
    std::vector<std::wstring> out;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        // access 0: attributes and caps only, never refused by another reader
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof(a);
        bool pad = false;
        if (HidD_GetAttributes(h, &a) && IsDualSense(a.VendorID, a.ProductID)) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep)) {
                HIDP_CAPS caps{};
                pad = HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.Usage == 5;
                HidD_FreePreparsedData(prep);
            }
        }
        CloseHandle(h);
        if (pad) out.push_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

struct DualSenseHub::Impl {
    std::mutex mutex;
    std::vector<std::shared_ptr<DualSenseDevice>> devices;
    std::thread scanner;
    std::mutex stopMutex;
    std::condition_variable stopWake;
    bool stop = false;

    void Scan() {
        const std::vector<std::wstring> paths = EnumerateDualSensePaths();
        std::lock_guard lock(mutex);
        devices.erase(std::remove_if(devices.begin(), devices.end(), [](const auto& d) { return !d->Ok(); }),
                      devices.end());
        for (const std::wstring& path : paths) {
            bool open = false;
            for (const auto& d : devices) open = open || d->Path() == path;
            if (open) continue;
            auto d = std::make_shared<DualSenseDevice>(path);
            if (d->Ok()) devices.push_back(std::move(d));
        }
    }
};

DualSenseHub::DualSenseHub() : impl_(std::make_unique<Impl>()) {
    impl_->Scan();
    impl_->scanner = std::thread([this] {
        std::unique_lock lock(impl_->stopMutex);
        while (!impl_->stopWake.wait_for(lock, std::chrono::seconds(2), [this] { return impl_->stop; })) {
            lock.unlock();
            impl_->Scan();
            lock.lock();
        }
    });
}
DualSenseHub::~DualSenseHub() {
    {
        std::lock_guard lock(impl_->stopMutex);
        impl_->stop = true;
    }
    impl_->stopWake.notify_one();
    if (impl_->scanner.joinable()) impl_->scanner.join();
}
DualSenseHub& DualSenseHub::Get() {
    static DualSenseHub hub;
    return hub;
}
size_t DualSenseHub::Count() {
    std::lock_guard lock(impl_->mutex);
    return impl_->devices.size();
}
std::shared_ptr<DualSenseDevice> DualSenseHub::At(size_t i) {
    std::lock_guard lock(impl_->mutex);
    return i < impl_->devices.size() ? impl_->devices[i] : nullptr;
}

} // namespace rr::platform
