// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/sync/ntsync.hpp"
#include "winapi/kernel32/filesystem/hid_io_status.generated.hpp"
#include <chrono>
#include <thread>
#include <cstdlib>

namespace vitreo::winapi {
namespace {
u32 hid_completion_status(u32 error) {
    switch(error) {
        case error_codes::ERROR_SUCCESS: return 0;
        case error_codes::ERROR_OPERATION_ABORTED: return hid_io_status::kSTATUS_CANCELLED;
        case error_codes::ERROR_DEVICE_NOT_CONNECTED: return hid_io_status::kSTATUS_DEVICE_NOT_CONNECTED;
        case error_codes::ERROR_NOACCESS: return hid_io_status::kSTATUS_ACCESS_VIOLATION;
        case error_codes::ERROR_INVALID_HANDLE: return hid_io_status::kSTATUS_INVALID_HANDLE;
        case error_codes::ERROR_ACCESS_DENIED: return hid_io_status::kSTATUS_ACCESS_DENIED;
        default: return hid_io_status::kSTATUS_NOT_SUPPORTED;
    }
}
class NativeHidCompletionSignal final : public HidCompletionSignal {
public:
    // Fresh HID handles are signaled on Windows in both access modes/bitnesses
    // (hid_device_metadata oracle initialwait).
    NativeHidCompletionSignal() : m_id(ntsync::global().create_event(true,true)) {}
    ~NativeHidCompletionSignal() override {ntsync::global().destroy(m_id);}
    void reset() override {(void)ntsync::global().reset_event(m_id);}
    void signal() override {(void)ntsync::global().set_event(m_id);}
    u64 native_id() const override {return m_id;}
private:
    ntsync::ObjectId m_id;
};
// Pop only after guest writes succeed, so inaccessible memory cannot consume
// a report another duplicate handle was entitled to read.
u32 transfer_hid(GuestMemory& memory,const HidOpenFileHolder& file,GuestAddr buffer,u32& actual) {
    std::lock_guard lock(file->mutex);
    if(file->cleaned_up) return error_codes::ERROR_OPERATION_ABORTED;
    if(file->disconnected) return error_codes::ERROR_DEVICE_NOT_CONNECTED;
    if(file->reports.empty()) return error_codes::ERROR_IO_PENDING;
    const auto& report=file->reports.front();
    if(memory.write_bytes(buffer,report.data(),report.size()).is_err()) return error_codes::ERROR_NOACCESS;
    static const bool diagnostic=[] {const char* value=std::getenv("VITREO_INPUT_DIAG");return value && *value && *value!='0';}();
    if(diagnostic) {
        static std::atomic<unsigned> logged{0};
        if(logged.fetch_add(1,std::memory_order_relaxed)<96) {
                gamepad_hid::Report readback{};
                const bool valid=memory.read_bytes(buffer,readback.data(),readback.size()).is_ok();
                char hex[gamepad_hid::kReportBytes*2+1]{};constexpr char digits[]="0123456789abcdef";
                if(valid) for(usize i=0;i<readback.size();++i) {hex[i*2]=digits[readback[i]>>4];hex[i*2+1]=digits[readback[i]&15];}
                std::fprintf(stderr,"[input-delivery] HID filled slot=%u generation=%llu buffer=0x%llx bytes=%zu readback=%s\n",file->slot,
                    static_cast<unsigned long long>(file->generation),static_cast<unsigned long long>(buffer),report.size(),valid ? hex:"unavailable");
        }
    }
    actual=report.size();file->reports.pop_front();return 0;
}
}
void FileManager::initialize_hid_waitable(Handle handle,const HidOpenFileHolder& file) {
    u64 id=0;
    {std::lock_guard lock(file->mutex);
        if(!file->completion_signal) file->completion_signal=std::make_shared<NativeHidCompletionSignal>();
        id=file->completion_signal->native_id();
    }
    std::lock_guard lock(m_hid_io_mutex);
    if(m_handles.get_shared<HidOpenFile>(handle,HandleType::HidDevice)!=file) return;
    m_hid_open_files[handle]=file;
    if(m_register_hid_waitable) m_register_hid_waitable(handle,id);
    // A readable open is a real input consumer even between ReadFile calls.
    // Metadata-only opens never start this bounded-ring producer.
    if(m_handles.get_access(handle) & (GENERIC_READ|GENERIC_ALL|hid_sdk::kFILE_READ_DATA)) {
        m_hid_input_handles.insert(handle);
        if(!m_hid_io_worker.joinable()) m_hid_io_worker=std::jthread([this](std::stop_token stop){run_hid_io(stop);});
        m_hid_io_cv.notify_one();
    }
}
bool FileManager::read_hid(Handle handle,GuestAddr buffer,u32 length,GuestAddr count,u32* actual_out,bool signal_completion) {
    if(actual_out) *actual_out=0;
    if(!m_memory) {
        static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!ReadFile: HID guest memory binding unavailable\n");});
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
    }
    if(count && m_memory->write_u32(count,0).is_err()) {set_last_error(error_codes::ERROR_NOACCESS);return false;}
    auto file=m_handles.get_shared<HidOpenFile>(handle,HandleType::HidDevice);
    if(!file) {set_last_error(error_codes::ERROR_INVALID_HANDLE);return false;}
    if((m_handles.get_access(handle) & (GENERIC_READ|GENERIC_ALL|hid_sdk::kFILE_READ_DATA))==0) {set_last_error(error_codes::ERROR_ACCESS_DENIED);return false;}
    if(length<gamepad_hid::kReportBytes) {
        static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!ReadFile: HID undersized-buffer error contract\n");});
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
    }
    if(m_gamepad_catalog && !m_gamepad_catalog->acquire_input(file)) {
        static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!ReadFile: native background HID monitoring unavailable\n");});
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
    }
    if(file->completion_signal) file->completion_signal->reset();
    const u32 tid=ThreadManager::get_current_thread_id_static();
    auto cancel=begin_sync_io(tid);
    u32 actual=0,error=0;
    while(true) {
        if(cancel->load()) {error=error_codes::ERROR_OPERATION_ABORTED;break;}
        if(m_gamepad_catalog) (void)m_gamepad_catalog->devices();
        error=transfer_hid(*m_memory,file,buffer,actual);
        if(error!=error_codes::ERROR_IO_PENDING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    end_sync_io(tid,cancel);
    if(signal_completion && file->completion_signal && ((file->notification_modes.load() & hid_sdk::kFILE_SKIP_SET_EVENT_ON_HANDLE)==0 || error)) file->completion_signal->signal();
    if(error) {set_last_error(error);return false;}
    if(count && m_memory->write_u32(count,actual).is_err()) {set_last_error(error_codes::ERROR_NOACCESS);return false;}
    if(actual_out) *actual_out=actual;
    return true;
}
OverlappedPipeIoResult FileManager::begin_overlapped_hid_read(Handle handle,GuestAddr buffer,u32 length,
    GuestAddr overlapped,bool is64,u32& actual) {
    actual=0;
    auto file=m_handles.get_shared<HidOpenFile>(handle,HandleType::HidDevice);
    if(!file || !m_memory) {set_last_error(error_codes::ERROR_INVALID_HANDLE);return OverlappedPipeIoResult::Failed;}
    if((m_handles.get_access(handle)&(GENERIC_READ|GENERIC_ALL|hid_sdk::kFILE_READ_DATA))==0) {set_last_error(error_codes::ERROR_ACCESS_DENIED);return OverlappedPipeIoResult::Failed;}
    if(length<gamepad_hid::kReportBytes) {
        static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!ReadFile: overlapped HID undersized-buffer error contract\n");});
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);return OverlappedPipeIoResult::Failed;
    }
    if(m_gamepad_catalog && !m_gamepad_catalog->acquire_input(file)) {
        static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!ReadFile: native background HID monitoring unavailable\n");});
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);return OverlappedPipeIoResult::Failed;
    }
    if(!m_overlapped_completion) {
        static std::once_flag once;std::call_once(once,[]{std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!ReadFile: HID completion binding unavailable\n");});
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);return OverlappedPipeIoResult::Failed;
    }
    // Snapshot routing and publish the request while close/cancel cannot
    // overtake registration; release this lock before any synchronous wait.
    {
        std::lock_guard lock(m_hid_io_mutex);
        if(m_handles.get_shared<HidOpenFile>(handle,HandleType::HidDevice)!=file) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);return OverlappedPipeIoResult::Failed;
        }
        const u32 prepare_error=prepare_overlapped_io(handle,overlapped,is64);
        if(prepare_error) {set_last_error(prepare_error);return OverlappedPipeIoResult::Failed;}
        trace_overlapped({.stage=OverlappedIoTraceStage::Submitted,.handle=handle,
            .object_id=static_cast<u64>(reinterpret_cast<std::uintptr_t>(file.get())),
            .overlapped=overlapped,.buffer=buffer,.requested_bytes=length,.is_64bit=is64});
        if(file->completion_signal) file->completion_signal->reset();
        if(file->overlapped) {
            m_hid_reads.push_back({handle,file,buffer,overlapped,length,ThreadManager::get_current_thread_id_static(),is64,false});
            if(!m_hid_io_worker.joinable()) m_hid_io_worker=std::jthread([this](std::stop_token stop){run_hid_io(stop);});
        }
    }
    if(!file->overlapped) {
        const bool ok=read_hid(handle,buffer,length,0,&actual,false);
        const u32 error=ok ? 0:get_last_error();
        notify_overlapped_completion(handle,actual,overlapped,is64,hid_completion_status(error),error,true);
        return ok ? OverlappedPipeIoResult::Completed : OverlappedPipeIoResult::Failed;
    }
    m_hid_io_cv.notify_one();
    set_last_error(error_codes::ERROR_IO_PENDING);
    return OverlappedPipeIoResult::Pending;
}
usize FileManager::cancel_hid_io(Handle handle,GuestAddr overlapped,bool calling_thread_only) {
    const u32 tid=ThreadManager::get_current_thread_id_static();
    const auto file=m_handles.get_shared<HidOpenFile>(handle,HandleType::HidDevice);
    if(!file) return 0;
    std::lock_guard lock(m_hid_io_mutex);
    usize count=0;
    for(auto& request:m_hid_reads) if(request.file==file && (!overlapped || request.overlapped==overlapped) && (!calling_thread_only || tid==request.tid)) {
        if(!request.cancelled) {request.cancelled=true;++count;}
    }
    m_hid_io_cv.notify_one();return count;
}
void FileManager::run_hid_io(std::stop_token stop) {
    std::unique_lock lock(m_hid_io_mutex);
    while(!stop.stop_requested() || !m_hid_reads.empty()) {
        if(m_hid_reads.empty() && m_hid_input_handles.empty()) {
            m_hid_io_cv.wait(lock,[&]{return stop.stop_requested() || !m_hid_reads.empty() || !m_hid_input_handles.empty();});continue;
        }
        lock.unlock();
        if(m_gamepad_catalog) (void)m_gamepad_catalog->devices();
        lock.lock();
        for(auto it=m_hid_reads.begin();it!=m_hid_reads.end();) {
            u32 actual=0;
            const u32 error=stop.stop_requested() || it->cancelled ? error_codes::ERROR_OPERATION_ABORTED :
                transfer_hid(*m_memory,it->file,it->buffer,actual);
            if(error==error_codes::ERROR_IO_PENDING) {++it;continue;}
            const auto request=*it;it=m_hid_reads.erase(it);
            const u32 status=hid_completion_status(error);
            lock.unlock();
            // The completion owner finalizes Internal, signals the shared
            // file object, and then publishes explicit-event/IOCP delivery.
            notify_overlapped_completion(request.handle,actual,request.overlapped,request.is64,status,error,false);
            lock.lock();it=m_hid_reads.begin();
        }
        if(!stop.stop_requested() && (!m_hid_reads.empty() || !m_hid_input_handles.empty()))
            m_hid_io_cv.wait_for(lock,std::chrono::milliseconds(8),[&]{return stop.stop_requested() || (m_hid_reads.empty() && m_hid_input_handles.empty());});
    }
}
void FileManager::shutdown_hid_io() {
    std::jthread worker;
    {std::lock_guard lock(m_hid_io_mutex);m_hid_io_worker.request_stop();m_hid_io_cv.notify_all();worker=std::move(m_hid_io_worker);}
}
} // namespace vitreo::winapi
