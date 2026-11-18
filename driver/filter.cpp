#include <ntddk.h>
#pragma warning(push)
#pragma warning(disable:4324) // WDK intentionally aligns request parameter unions.
#include <wdf.h>
#pragma warning(pop)
#include <ntddmou.h>
#include <kbdmou.h>
#include <ntstrsafe.h>
#include <new.h>
#include "directional/packet_filter.h"
#include "directional/driver_protocol.h"
#include "directional/filter_state.h"

namespace {
using namespace directional;
struct Extension {
    FilterState state;
    WDFSPINLOCK lock=nullptr;
    WDFQUEUE controls=nullptr;
    CONNECT_DATA upper{};
};
struct Child { WDFQUEUE parent_queue; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Extension,context)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Child,child_context)

EVT_WDF_DRIVER_DEVICE_ADD add_device;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL internal_control;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL profile_control;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL child_control;
EVT_WDF_DEVICE_D0_EXIT power_exit;
EVT_WDF_DEVICE_D0_ENTRY power_entry;
EVT_WDF_DEVICE_SURPRISE_REMOVAL surprise_removal;
EVT_WDF_REQUEST_COMPLETION_ROUTINE connected;

void send_down(WDFDEVICE device,WDFREQUEST request) {
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(request,WdfDeviceGetIoTarget(device),&options))
        WdfRequestComplete(request,WdfRequestGetStatus(request));
}

// Entire callback and all called math stay nonpaged. No allocations, logging,
// clocks or floating point. The lock serializes packets, profile publication,
// residual resets and counters. Each downstream-consumed packet commits state.
void service(PDEVICE_OBJECT object,PMOUSE_INPUT_DATA begin,PMOUSE_INPUT_DATA end,PULONG consumed) {
    auto* ext=context(WdfWdmDeviceGetWdfDeviceHandle(object));
    *consumed=0;
    WdfSpinLockAcquire(ext->lock);
    const auto callback=reinterpret_cast<PSERVICE_CALLBACK_ROUTINE>(ext->upper.ClassService);
    if (callback) {
        const auto result=forward_packets(begin,static_cast<size_type>(end-begin),ext->state.processor,
            [&](MOUSE_INPUT_DATA& packet) noexcept {
                ULONG used=0;
                callback(ext->upper.ClassDeviceObject,&packet,&packet+1,&used);
                return used==1;
            });
        *consumed=static_cast<ULONG>(result.consumed);
        ext->state.record(result.consumed,result.overflow_bypasses);
    }
    WdfSpinLockRelease(ext->lock);
}

void connected(WDFREQUEST request,WDFIOTARGET,PWDF_REQUEST_COMPLETION_PARAMS completion,WDFCONTEXT opaque) {
    auto* ext=static_cast<Extension*>(opaque);
    WdfSpinLockAcquire(ext->lock);
    ext->state.set_connected(NT_SUCCESS(completion->IoStatus.Status));
    if (!NT_SUCCESS(completion->IoStatus.Status)) {
        PCONNECT_DATA original=nullptr;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(request,sizeof(CONNECT_DATA),reinterpret_cast<void**>(&original),nullptr)))
            *original=ext->upper;
        ext->upper={};
    }
    WdfSpinLockRelease(ext->lock);
    WdfRequestCompleteWithInformation(request,completion->IoStatus.Status,completion->IoStatus.Information);
}

void internal_control(WDFQUEUE queue,WDFREQUEST request,size_t,size_t,ULONG code) {
    const auto device=WdfIoQueueGetDevice(queue);
    auto* ext=context(device);
    if (code==IOCTL_INTERNAL_MOUSE_DISCONNECT) {
        // Matches the mouse filter contract: dynamic disconnect is unsupported;
        // KMDF/PnP owns callback lifetime until the physical stack is removed.
        WdfRequestComplete(request,STATUS_NOT_IMPLEMENTED); return;
    }
    if (code!=IOCTL_INTERNAL_MOUSE_CONNECT) { send_down(device,request); return; }
    PCONNECT_DATA data=nullptr;
    auto status=WdfRequestRetrieveInputBuffer(request,sizeof(CONNECT_DATA),reinterpret_cast<void**>(&data),nullptr);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
    WdfSpinLockAcquire(ext->lock);
    if (ext->upper.ClassService) status=STATUS_SHARING_VIOLATION;
    else if (!data->ClassService || !data->ClassDeviceObject) status=STATUS_INVALID_PARAMETER;
    else {
        ext->upper=*data;
        data->ClassDeviceObject=WdfDeviceWdmGetDeviceObject(device);
        data->ClassService=reinterpret_cast<void*>(service);
    }
    WdfSpinLockRelease(ext->lock);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
    WdfRequestFormatRequestUsingCurrentType(request);
    WdfRequestSetCompletionRoutine(request,connected,ext);
    if (!WdfRequestSend(request,WdfDeviceGetIoTarget(device),nullptr)) {
        WdfSpinLockAcquire(ext->lock);
        *data=ext->upper; ext->upper={}; ext->state.set_connected(false);
        WdfSpinLockRelease(ext->lock);
        WdfRequestComplete(request,WdfRequestGetStatus(request));
    }
}

void profile_control(WDFQUEUE queue,WDFREQUEST request,size_t output_size,size_t input_size,ULONG code) {
    auto* ext=context(WdfIoQueueGetDevice(queue));
    NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information=0;
    if (code==driver::ioctl_get_state) {
        driver::State* output=nullptr;
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*output),reinterpret_cast<void**>(&output),nullptr);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(ext->lock);
            *output=ext->state.snapshot();
            WdfSpinLockRelease(ext->lock);
            information=sizeof(*output);
        }
    } else if (code==driver::ioctl_set_profile) {
        driver::SetProfile* input=nullptr;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*input),reinterpret_cast<void**>(&input),nullptr);
        if (NT_SUCCESS(status)) {
            const auto command=*input;
            if (input_size!=sizeof(command)) status=STATUS_INVALID_PARAMETER;
            else {
                WdfSpinLockAcquire(ext->lock);
                switch (ext->state.apply(command)) {
                    case ApplyStatus::ok: status=STATUS_SUCCESS; break;
                    case ApplyStatus::invalid: status=STATUS_INVALID_PARAMETER; break;
                    case ApplyStatus::stale: status=STATUS_REVISION_MISMATCH; break;
                    case ApplyStatus::not_ready: status=STATUS_DEVICE_NOT_READY; break;
                }
                WdfSpinLockRelease(ext->lock);
            }
        }
    }
    UNREFERENCED_PARAMETER(output_size);
    WdfRequestCompleteWithInformation(request,status,information);
}

void child_control(WDFQUEUE queue,WDFREQUEST request,size_t,size_t,ULONG) {
    const auto child=child_context(WdfIoQueueGetDevice(queue));
    WDF_REQUEST_FORWARD_OPTIONS options;
    WDF_REQUEST_FORWARD_OPTIONS_INIT(&options);
    const auto status=WdfRequestForwardToParentDeviceIoQueue(request,child->parent_queue,&options);
    if (!NT_SUCCESS(status)) WdfRequestComplete(request,status);
}

NTSTATUS create_child(WDFDEVICE parent,WDFQUEUE parent_queue) {
    auto* init=WdfPdoInitAllocate(parent);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    DECLARE_CONST_UNICODE_STRING(id,L"DirectionalSensitivity\\Control");
    DECLARE_CONST_UNICODE_STRING(instance,L"0");
    DECLARE_CONST_UNICODE_STRING(description,L"Directional sensitivity profile control");
    DECLARE_CONST_UNICODE_STRING(location,L"Mouse filter");
    DECLARE_CONST_UNICODE_STRING(security,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    NTSTATUS status=WdfPdoInitAssignRawDevice(init,&driver::raw_class_id);
    if (NT_SUCCESS(status)) status=WdfDeviceInitAssignSDDLString(init,&security);
    if (NT_SUCCESS(status)) status=WdfPdoInitAssignDeviceID(init,&id);
    if (NT_SUCCESS(status)) status=WdfPdoInitAssignInstanceID(init,&instance);
    if (NT_SUCCESS(status)) status=WdfPdoInitAddDeviceText(init,&description,&location,0x409);
    if (!NT_SUCCESS(status)) { WdfDeviceInitFree(init); return status; }
    WdfPdoInitSetDefaultLocale(init,0x409);
    WdfPdoInitAllowForwardingRequestToParent(init);
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs,Child);
    WDFDEVICE child=nullptr;
    status=WdfDeviceCreate(&init,&attrs,&child);
    if (!NT_SUCCESS(status)) { if (init) WdfDeviceInitFree(init); return status; }
    child_context(child)->parent_queue=parent_queue;
    WDF_DEVICE_PNP_CAPABILITIES caps;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
    caps.Removable=WdfTrue; caps.SurpriseRemovalOK=WdfTrue;
    caps.NoDisplayInUI=WdfTrue; caps.UniqueID=WdfFalse;
    WdfDeviceSetPnpCapabilities(child,&caps);
    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue,WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl=child_control;
    status=WdfIoQueueCreate(child,&queue,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);
    if (NT_SUCCESS(status)) status=WdfDeviceCreateDeviceInterface(child,&driver::interface_id,nullptr);
    if (NT_SUCCESS(status)) status=WdfFdoAddStaticChild(parent,child);
    if (!NT_SUCCESS(status)) WdfObjectDelete(child);
    return status;
}

NTSTATUS power_exit(WDFDEVICE device,WDF_POWER_DEVICE_STATE) {
    auto* ext=context(device);
    WdfSpinLockAcquire(ext->lock);
    ext->state.set_powered(false);
    WdfSpinLockRelease(ext->lock);
    return STATUS_SUCCESS;
}

NTSTATUS power_entry(WDFDEVICE device,WDF_POWER_DEVICE_STATE) {
    auto* ext=context(device);
    WdfSpinLockAcquire(ext->lock);
    ext->state.set_powered(true);
    WdfSpinLockRelease(ext->lock);
    return STATUS_SUCCESS;
}
void surprise_removal(WDFDEVICE device) {
    auto* ext=context(device);
    WdfSpinLockAcquire(ext->lock);
    ext->state.set_powered(false); ext->state.set_connected(false);
    WdfSpinLockRelease(ext->lock);
}

NTSTATUS add_device(WDFDRIVER,PWDFDEVICE_INIT init) {
    WdfFdoInitSetFilter(init);
    WdfDeviceInitSetDeviceType(init,FILE_DEVICE_MOUSE);
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Exit=power_exit;
    power.EvtDeviceD0Entry=power_entry;
    power.EvtDeviceSurpriseRemoval=surprise_removal;
    WdfDeviceInitSetPnpPowerEventCallbacks(init,&power);
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs,Extension);
    WDFDEVICE device=nullptr;
    auto status=WdfDeviceCreate(&init,&attrs,&device);
    if (!NT_SUCCESS(status)) return status;
    // WDF zeroes context memory but does not run C++ initializers. Placement new
    // begins the lifetime in WDF's nonpaged allocation; it does not allocate.
    auto* ext=new (context(device)) Extension{};
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs); attrs.ParentObject=device;
    status=WdfSpinLockCreate(&attrs,&ext->lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue,WdfIoQueueDispatchParallel);
    queue.EvtIoInternalDeviceControl=internal_control;
    status=WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT(&queue,WdfIoQueueDispatchParallel);
    queue.PowerManaged=WdfFalse; queue.EvtIoDeviceControl=profile_control;
    status=WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,&ext->controls);
    if (!NT_SUCCESS(status)) return status;
    return create_child(device,ext->controls);
}
} // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry_path) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config,add_device);
    return WdfDriverCreate(object,registry_path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
