/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <procgrp.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <netioapi.h>
#include <xen.h>

#include "driver.h"
#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "frontend.h"
#include "names.h"
#include "mac.h"
#include "tcpip.h"
#include "receiver.h"
#include "transmitter.h"
#include "link.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef struct _XENVIF_FRONTEND_STATISTICS {
    ULONGLONG   Value[XENVIF_VIF_STATISTIC_COUNT];
} XENVIF_FRONTEND_STATISTICS, *PXENVIF_FRONTEND_STATISTICS;

struct _XENVIF_FRONTEND {
    PXENVIF_PDO                 Pdo;
    PCHAR                       Path;
    PCHAR                       Prefix;
    XENVIF_FRONTEND_STATE       State;
    BOOLEAN                     Online;
    KSPIN_LOCK                  Lock;
    PXENVIF_THREAD              MibThread;
    PXENVIF_THREAD              EjectThread;
    KEVENT                      EjectEvent;

    PCHAR                       BackendPath;
    USHORT                      BackendDomain;
    ULONG                       MaxQueues;
    ULONG                       NumQueues;

    PXENVIF_MAC                 Mac;
    PXENVIF_RECEIVER            Receiver;
    PXENVIF_TRANSMITTER         Transmitter;

    XENBUS_DEBUG_INTERFACE      DebugInterface;
    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    XENBUS_STORE_INTERFACE      StoreInterface;

    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackLate;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
    PXENBUS_STORE_WATCH         Watch;

    PXENVIF_FRONTEND_STATISTICS Statistics;
    ULONG                       StatisticsCount;
};

static const PCHAR
FrontendStateName(
    IN  XENVIF_FRONTEND_STATE   State
    )
{
#define _STATE_NAME(_State)     \
    case  FRONTEND_ ## _State:  \
        return #_State;

    switch (State) {
    _STATE_NAME(UNKNOWN);
    _STATE_NAME(CLOSED);
    _STATE_NAME(PREPARED);
    _STATE_NAME(CONNECTED);
    _STATE_NAME(ENABLED);
    default:
        break;
    }

    return "INVALID";

#undef  _STATE_NAME
}

#define FRONTEND_POOL    'NORF'

static FORCEINLINE PVOID
__FrontendAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, FRONTEND_POOL);
}

static FORCEINLINE VOID
__FrontendFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, FRONTEND_POOL);
}

static FORCEINLINE PXENVIF_PDO
__FrontendGetPdo(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->Pdo;
}

PXENVIF_PDO
FrontendGetPdo(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return __FrontendGetPdo(Frontend);
}

static FORCEINLINE PCHAR
__FrontendGetPath(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->Path;
}

PCHAR
FrontendGetPath(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return __FrontendGetPath(Frontend);
}

static FORCEINLINE PCHAR
__FrontendGetPrefix(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->Prefix;
}

PCHAR
FrontendGetPrefix(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return __FrontendGetPrefix(Frontend);
}

static FORCEINLINE PCHAR
__FrontendGetBackendPath(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->BackendPath;
}

PCHAR
FrontendGetBackendPath(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return __FrontendGetBackendPath(Frontend);
}

static FORCEINLINE USHORT
__FrontendGetBackendDomain(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->BackendDomain;
}

USHORT
FrontendGetBackendDomain(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return __FrontendGetBackendDomain(Frontend);
}

static VOID
FrontendSetMaxQueues(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    HANDLE                  ParametersKey;
    ULONG                   FrontendMaxQueues;
    NTSTATUS                status;

    Frontend->MaxQueues = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    ParametersKey = DriverGetParametersKey();

    status = RegistryQueryDwordValue(ParametersKey,
                                     "FrontendMaxQueues",
                                     &FrontendMaxQueues);
    if (NT_SUCCESS(status) && FrontendMaxQueues < Frontend->MaxQueues)
        Frontend->MaxQueues = FrontendMaxQueues;

    Info("%u\n", Frontend->MaxQueues);
}

static FORCEINLINE ULONG
__FrontendGetMaxQueues(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->MaxQueues;
}

ULONG
FrontendGetMaxQueues(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return __FrontendGetMaxQueues(Frontend);
}

PCHAR
FrontendFormatPath(
    IN  PXENVIF_FRONTEND    Frontend,
    IN  ULONG               Index
    )
{
    ULONG                   Length;
    PCHAR                   Path;
    NTSTATUS                status;

    Length = (ULONG)(strlen(__FrontendGetPath(Frontend)) +
                     strlen("/queue-XX") +
                     1) * sizeof (CHAR);

    Path = __FrontendAllocate(Length);
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path,
                                Length,
                                "%s/queue-%u",
                                __FrontendGetPath(Frontend),
                                Index);
    if (!NT_SUCCESS(status))
        goto fail2;

    return Path;

fail2:
    __FrontendFree(Path);

fail1:
    return NULL;
}

VOID
FrontendFreePath(
    IN  PXENVIF_FRONTEND    Frontend,
    IN  PCHAR               Path
    )
{
    UNREFERENCED_PARAMETER(Frontend);

    __FrontendFree(Path);
}

#define DEFINE_FRONTEND_GET_FUNCTION(_Function, _Type)  \
static FORCEINLINE _Type                                \
__FrontendGet ## _Function(                             \
    IN  PXENVIF_FRONTEND    Frontend                    \
    )                                                   \
{                                                       \
    return Frontend-> ## _Function;                     \
}                                                       \
                                                        \
_Type                                                   \
FrontendGet ## _Function(                               \
    IN  PXENVIF_FRONTEND    Frontend                    \
    )                                                   \
{                                                       \
    return __FrontendGet ## _Function ## (Frontend);    \
}

DEFINE_FRONTEND_GET_FUNCTION(Mac, PXENVIF_MAC)
DEFINE_FRONTEND_GET_FUNCTION(Transmitter, PXENVIF_TRANSMITTER)
DEFINE_FRONTEND_GET_FUNCTION(Receiver, PXENVIF_RECEIVER)

static BOOLEAN
FrontendIsOnline(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->Online;
}

static BOOLEAN
FrontendIsBackendOnline(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    PCHAR                   Buffer;
    BOOLEAN                 Online;
    NTSTATUS                status;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          "online",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Online = FALSE;
    } else {
        Online = (BOOLEAN)strtol(Buffer, NULL, 2);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    return Online;
}

static DECLSPEC_NOINLINE NTSTATUS
FrontendEject(
    IN  PXENVIF_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENVIF_FRONTEND    Frontend = Context;
    PKEVENT             Event;

    Trace("%s: ====>\n", __FrontendGetPath(Frontend));

    Event = ThreadGetEvent(Self);

    for (;;) {
        KIRQL       Irql;

        KeWaitForSingleObject(Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        KeClearEvent(Event);

        if (ThreadIsAlerted(Self))
            break;

        KeAcquireSpinLock(&Frontend->Lock, &Irql);

        // It is not safe to use interfaces before this point
        if (Frontend->State == FRONTEND_UNKNOWN ||
            Frontend->State == FRONTEND_CLOSED)
            goto loop;

        if (!FrontendIsOnline(Frontend))
            goto loop;

        if (!FrontendIsBackendOnline(Frontend))
            PdoRequestEject(__FrontendGetPdo(Frontend));

loop:
        KeReleaseSpinLock(&Frontend->Lock, Irql);

        KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);

    Trace("%s: <====\n", __FrontendGetPath(Frontend));

    return STATUS_SUCCESS;
}

VOID
FrontendEjectFailed(
    IN PXENVIF_FRONTEND Frontend
    )
{
    KIRQL               Irql;
    ULONG               Length;
    PCHAR               Path;
    NTSTATUS            status;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    Info("%s: device eject failed\n", __FrontendGetPath(Frontend));

    Length = sizeof ("error/") + (ULONG)strlen(__FrontendGetPath(Frontend));
    Path = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path, 
                                Length,
                                "error/%s", 
                                __FrontendGetPath(Frontend));
    if (!NT_SUCCESS(status))
        goto fail2;

    (VOID) XENBUS_STORE(Printf,
                        &Frontend->StoreInterface,
                        NULL,
                        Path,
                        "error",
                        "UNPLUG FAILED: device is still in use");

    __FrontendFree(Path);

    KeReleaseSpinLock(&Frontend->Lock, Irql);
    return;

fail2:
    Error("fail2\n");

    __FrontendFree(Path);

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Frontend->Lock, Irql);
}

static NTSTATUS
FrontendGetInterfaceIndex(
    IN  PXENVIF_FRONTEND    Frontend,
    IN  PMIB_IF_TABLE2      Table,
    OUT PNET_IFINDEX        InterfaceIndex
    )
{
    ETHERNET_ADDRESS        PermanentPhysicalAddress;
    ULONG                   Index;
    PMIB_IF_ROW2            Row;

    MacQueryPermanentAddress(__FrontendGetMac(Frontend),
                             &PermanentPhysicalAddress);

    for (Index = 0; Index < Table->NumEntries; Index++) {
        Row = &Table->Table[Index];

        if (!(Row->InterfaceAndOperStatusFlags.HardwareInterface) ||
            !(Row->InterfaceAndOperStatusFlags.ConnectorPresent))
            continue;

        if (Row->OperStatus != IfOperStatusUp)
            continue;

        if (Row->PhysicalAddressLength != sizeof (ETHERNET_ADDRESS))
            continue;

        if (memcmp(Row->PermanentPhysicalAddress,
                   &PermanentPhysicalAddress,
                   sizeof (ETHERNET_ADDRESS)) != 0)
            continue;

        goto found;
    }

    return STATUS_UNSUCCESSFUL;

found:
    *InterfaceIndex = Row->InterfaceIndex;

    Trace("[%u]: %ws (%ws)",
          Row->InterfaceIndex,
          Row->Alias,
          Row->Description);

    return STATUS_SUCCESS;
}

static NTSTATUS
FrontendInsertAddress(
    IN OUT  PSOCKADDR_INET      *AddressTable,
    IN      const SOCKADDR_INET *Address,
    IN OUT  PULONG              AddressCount
    )
{
    ULONG                       Index;
    PSOCKADDR_INET              Table;
    NTSTATUS                    status;

    Trace("====>\n");

    for (Index = 0; Index < *AddressCount; Index++) {
        if ((*AddressTable)[Index].si_family != Address->si_family)
            continue;

        if (Address->si_family == AF_INET) {
            if (RtlCompareMemory(&Address->Ipv4.sin_addr.s_addr,
                                 &(*AddressTable)[Index].Ipv4.sin_addr.s_addr,
                                 IPV4_ADDRESS_LENGTH) == IPV4_ADDRESS_LENGTH)
                goto done;
        } else {
            ASSERT3U(Address->si_family, ==, AF_INET6);

            if (RtlCompareMemory(&Address->Ipv6.sin6_addr.s6_addr,
                                 &(*AddressTable)[Index].Ipv6.sin6_addr.s6_addr,
                                 IPV6_ADDRESS_LENGTH) == IPV6_ADDRESS_LENGTH)
                goto done;
        }
    }

    // We have an address we've not seen before so grow the table
    Table = __FrontendAllocate(sizeof (SOCKADDR_INET) * (*AddressCount + 1));

    status = STATUS_NO_MEMORY;
    if (Table == NULL)
        goto fail1;

    RtlCopyMemory(Table, *AddressTable, sizeof (SOCKADDR_INET) * *AddressCount);
    Table[(*AddressCount)++] = *Address;

    if (*AddressTable != NULL)
        __FrontendFree(*AddressTable);

    *AddressTable = Table;

done:
    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
FrontendProcessAddressTable(
    IN  PXENVIF_FRONTEND            Frontend,
    IN  PMIB_UNICASTIPADDRESS_TABLE Table,
    IN  NET_IFINDEX                 InterfaceIndex,
    OUT PSOCKADDR_INET              *AddressTable,
    OUT PULONG                      AddressCount
    )
{
    ULONG                           Index;
    NTSTATUS                        status;

    UNREFERENCED_PARAMETER(Frontend);

    *AddressTable = NULL;
    *AddressCount = 0;

    for (Index = 0; Index < Table->NumEntries; Index++) {
        PMIB_UNICASTIPADDRESS_ROW   Row = &Table->Table[Index];

        if (Row->InterfaceIndex != InterfaceIndex)
            continue;

        if (Row->Address.si_family != AF_INET &&
            Row->Address.si_family != AF_INET6)
            continue;

        status = FrontendInsertAddress(AddressTable,
                                       &Row->Address,
                                       AddressCount);
        if (!NT_SUCCESS(status))
            goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    if (*AddressTable != NULL)
        __FrontendFree(*AddressTable);

    return status;
}

static NTSTATUS
FrontendDumpAddressTable(
    IN  PXENVIF_FRONTEND        Frontend,
    IN  PSOCKADDR_INET          AddressTable,
    IN  ULONG                   AddressCount
    )
{
    PXENBUS_STORE_TRANSACTION   Transaction;
    ULONG                       Index;
    ULONG                       IpVersion4Count;
    ULONG                       IpVersion6Count;
    NTSTATUS                    status;

    Trace("====>\n");

    status = XENBUS_STORE(TransactionStart,
                          &Frontend->StoreInterface,
                          &Transaction);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_STORE(Remove,
                          &Frontend->StoreInterface,
                          Transaction,
                          __FrontendGetPrefix(Frontend),
                          "ipv4");
    if (!NT_SUCCESS(status) &&
        status != STATUS_OBJECT_NAME_NOT_FOUND)
        goto fail2;

    status = XENBUS_STORE(Remove,
                          &Frontend->StoreInterface,
                          Transaction,
                          __FrontendGetPrefix(Frontend),
                          "ipv6");
    if (!NT_SUCCESS(status) &&
        status != STATUS_OBJECT_NAME_NOT_FOUND)
        goto fail3;

    IpVersion4Count = 0;
    IpVersion6Count = 0;

    for (Index = 0; Index < AddressCount; Index++) {
        switch (AddressTable[Index].si_family) {
        case AF_INET: {
            IPV4_ADDRESS    Address;
            CHAR            Node[sizeof ("ipv4/XXXXXXXX/addr")];

            RtlCopyMemory(Address.Byte,
                          &AddressTable[Index].Ipv4.sin_addr.s_addr,
                          IPV4_ADDRESS_LENGTH);

            status = RtlStringCbPrintfA(Node,
                                        sizeof (Node),
                                        "ipv4/%u/addr",
                                        IpVersion4Count);
            ASSERT(NT_SUCCESS(status));

            status = XENBUS_STORE(Printf,
                                  &Frontend->StoreInterface,
                                  Transaction,
                                  __FrontendGetPrefix(Frontend),
                                  Node,
                                  "%u.%u.%u.%u",
                                  Address.Byte[0],
                                  Address.Byte[1],
                                  Address.Byte[2],
                                  Address.Byte[3]);
            if (!NT_SUCCESS(status))
                goto fail4;

            Trace("%s: %u.%u.%u.%u\n",
                  __FrontendGetPrefix(Frontend),
                  Address.Byte[0],
                  Address.Byte[1],
                  Address.Byte[2],
                  Address.Byte[3]);

            IpVersion4Count++;
            break;
        }
        case AF_INET6: {
            IPV6_ADDRESS    Address;
            CHAR            Node[sizeof ("ipv6/XXXXXXXX/addr")];

            RtlCopyMemory(Address.Byte,
                          &AddressTable[Index].Ipv6.sin6_addr.s6_addr,
                          IPV6_ADDRESS_LENGTH);

            status = RtlStringCbPrintfA(Node,
                                        sizeof (Node),
                                        "ipv6/%u/addr",
                                        IpVersion6Count);
            ASSERT(NT_SUCCESS(status));

            status = XENBUS_STORE(Printf,
                                  &Frontend->StoreInterface,
                                  Transaction,
                                  __FrontendGetPrefix(Frontend),
                                  Node,
                                  "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                                  NTOHS(Address.Word[0]),
                                  NTOHS(Address.Word[1]),
                                  NTOHS(Address.Word[2]),
                                  NTOHS(Address.Word[3]),
                                  NTOHS(Address.Word[4]),
                                  NTOHS(Address.Word[5]),
                                  NTOHS(Address.Word[6]),
                                  NTOHS(Address.Word[7]));
            if (!NT_SUCCESS(status))
                goto fail4;

            Trace("%s: %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n",
                  __FrontendGetPrefix(Frontend),
                  NTOHS(Address.Word[0]),
                  NTOHS(Address.Word[1]),
                  NTOHS(Address.Word[2]),
                  NTOHS(Address.Word[3]),
                  NTOHS(Address.Word[4]),
                  NTOHS(Address.Word[5]),
                  NTOHS(Address.Word[6]),
                  NTOHS(Address.Word[7]));

            IpVersion6Count++;
            break;
        }
        default:
            break;
        }
    }

    status = XENBUS_STORE(TransactionEnd,
                          &Frontend->StoreInterface,
                          Transaction,
                          TRUE);

    Trace("<====\n");

    return status;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    (VOID) XENBUS_STORE(TransactionEnd,
                        &Frontend->StoreInterface,
                        Transaction,
                        FALSE);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FrontendIpAddressChange(
    IN  PVOID                       Context,
    IN  PMIB_UNICASTIPADDRESS_ROW   Row OPTIONAL,
    IN  MIB_NOTIFICATION_TYPE       NotificationType
    )
{
    PXENVIF_FRONTEND                Frontend = Context;

    UNREFERENCED_PARAMETER(Row);
    UNREFERENCED_PARAMETER(NotificationType);

    ThreadWake(Frontend->MibThread);
}

static DECLSPEC_NOINLINE NTSTATUS
FrontendMib(
    IN  PXENVIF_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENVIF_FRONTEND    Frontend = Context;
    PKEVENT             Event;
    NTSTATUS            (*__GetIfTable2)(PMIB_IF_TABLE2 *);
    NTSTATUS            (*__NotifyUnicastIpAddressChange)(ADDRESS_FAMILY,
                                                          PUNICAST_IPADDRESS_CHANGE_CALLBACK,
                                                          PVOID,    
                                                          BOOLEAN,
                                                          HANDLE *);
    NTSTATUS            (*__GetUnicastIpAddressTable)(ADDRESS_FAMILY,
                                                      PMIB_UNICASTIPADDRESS_TABLE *);

    VOID                (*__FreeMibTable)(PVOID);
    NTSTATUS            (*__CancelMibChangeNotify2)(HANDLE);
    HANDLE              Handle;
    NTSTATUS            status;

    Trace("====>\n");

    status = LinkGetRoutineAddress("netio.sys",
                                   "GetIfTable2",
                                   (PVOID *)&__GetIfTable2);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = LinkGetRoutineAddress("netio.sys",
                                   "NotifyUnicastIpAddressChange",
                                   (PVOID *)&__NotifyUnicastIpAddressChange);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = LinkGetRoutineAddress("netio.sys",
                                   "GetUnicastIpAddressTable",
                                   (PVOID *)&__GetUnicastIpAddressTable);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = LinkGetRoutineAddress("netio.sys",
                                   "FreeMibTable",
                                   (PVOID *)&__FreeMibTable);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = LinkGetRoutineAddress("netio.sys",
                                   "CancelMibChangeNotify2",
                                   (PVOID *)&__CancelMibChangeNotify2);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = __NotifyUnicastIpAddressChange(AF_UNSPEC,
                                            FrontendIpAddressChange,
                                            Frontend,
                                            TRUE,
                                            &Handle);
    if (!NT_SUCCESS(status))
        goto fail6;

    Event = ThreadGetEvent(Self);

    for (;;) { 
        PMIB_IF_TABLE2              IfTable;
        NET_IFINDEX                 InterfaceIndex;
        PMIB_UNICASTIPADDRESS_TABLE UnicastIpAddressTable;
        KIRQL                       Irql;
        PSOCKADDR_INET              AddressTable;
        ULONG                       AddressCount;

        Trace("waiting...\n");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        IfTable = NULL;
        UnicastIpAddressTable = NULL;

        status = __GetIfTable2(&IfTable);
        if (!NT_SUCCESS(status))
            goto loop;

        status = FrontendGetInterfaceIndex(Frontend,
                                           IfTable,
                                           &InterfaceIndex);
        if (!NT_SUCCESS(status))
            goto loop;

        status = __GetUnicastIpAddressTable(AF_UNSPEC,
                                            &UnicastIpAddressTable);
        if (!NT_SUCCESS(status))
            goto loop;

        KeAcquireSpinLock(&Frontend->Lock, &Irql);

        // It is not safe to use interfaces before this point
        if (Frontend->State != FRONTEND_CONNECTED &&
            Frontend->State != FRONTEND_ENABLED)
            goto unlock;

        status = FrontendProcessAddressTable(Frontend,
                                             UnicastIpAddressTable,
                                             InterfaceIndex,
                                             &AddressTable,
                                             &AddressCount);
        if (!NT_SUCCESS(status))
            goto unlock;

        TransmitterUpdateAddressTable(__FrontendGetTransmitter(Frontend),
                                      AddressTable,
                                      AddressCount);

        (VOID) FrontendDumpAddressTable(Frontend,
                                        AddressTable,
                                        AddressCount);

        if (AddressCount != 0)
            __FrontendFree(AddressTable);

unlock:
        KeReleaseSpinLock(&Frontend->Lock, Irql);

loop:
        if (UnicastIpAddressTable != NULL)
            __FreeMibTable(UnicastIpAddressTable);

        if (IfTable != NULL)
            __FreeMibTable(IfTable);
    }

    status = __CancelMibChangeNotify2(Handle);
    ASSERT(NT_SUCCESS(status));

    Trace("<====\n");

    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FrontendSetOnline(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = TRUE;

    Trace("<====\n");
}

static VOID
FrontendSetOffline(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = FALSE;
    PdoRequestEject(__FrontendGetPdo(Frontend));

    Trace("<====\n");
}

static VOID
FrontendSetXenbusState(
    IN  PXENVIF_FRONTEND    Frontend,
    IN  XenbusState         State
    )
{
    BOOLEAN                 Online;

    Trace("%s: ====> %s\n",
          __FrontendGetPath(Frontend),
          XenbusStateName(State));

    ASSERT(FrontendIsOnline(Frontend));

    Online = !PdoIsEjectRequested(__FrontendGetPdo(Frontend)) &&
             FrontendIsBackendOnline(Frontend);

    (VOID) XENBUS_STORE(Printf,
                        &Frontend->StoreInterface,
                        NULL,
                        __FrontendGetPath(Frontend),
                        "state",
                        "%u",
                        State);

    if (State == XenbusStateClosed && !Online)
        FrontendSetOffline(Frontend);

    Trace("%s: <==== %s\n",
          __FrontendGetPath(Frontend),
          XenbusStateName(State));
}

static NTSTATUS
FrontendAcquireBackend(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    PCHAR                   Buffer;
    NTSTATUS                status;

    Trace("=====>\n");

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetPath(Frontend),
                          "backend",
                          &Buffer);
    if (!NT_SUCCESS(status))
        goto fail1;

    Frontend->BackendPath = Buffer;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetPath(Frontend),
                          "backend-id",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Frontend->BackendDomain = 0;
    } else {
        Frontend->BackendDomain = (USHORT)strtol(Buffer, NULL, 10);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    Trace("<====\n");
    return status;
}

static VOID
FrontendWaitForBackendXenbusStateChange(
    IN      PXENVIF_FRONTEND    Frontend,
    IN OUT  XenbusState         *State
    )
{
    KEVENT                      Event;
    PXENBUS_STORE_WATCH         Watch;
    LARGE_INTEGER               Start;
    ULONGLONG                   TimeDelta;
    LARGE_INTEGER               Timeout;
    XenbusState                 Old = *State;
    NTSTATUS                    status;

    Trace("%s: ====> %s\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(*State));

    ASSERT(FrontendIsOnline(Frontend));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = XENBUS_STORE(WatchAdd,
                          &Frontend->StoreInterface,
                          __FrontendGetBackendPath(Frontend),
                          "state",
                          &Event,
                          &Watch);
    if (!NT_SUCCESS(status))
        Watch = NULL;

    KeQuerySystemTime(&Start);
    TimeDelta = 0;

    Timeout.QuadPart = 0;

    while (*State == Old && TimeDelta < 120000) {
        PCHAR           Buffer;
        LARGE_INTEGER   Now;

        if (Watch != NULL) {
            ULONG   Attempt = 0;

            while (++Attempt < 1000) {
                status = KeWaitForSingleObject(&Event,
                                               Executive,
                                               KernelMode,
                                               FALSE,
                                               &Timeout);
                if (status != STATUS_TIMEOUT)
                    break;

                // We are waiting for a watch event at DISPATCH_LEVEL so
                // it is our responsibility to poll the store ring.
                XENBUS_STORE(Poll,
                             &Frontend->StoreInterface);

                KeStallExecutionProcessor(1000);   // 1ms
            }

            KeClearEvent(&Event);
        }

        status = XENBUS_STORE(Read,
                              &Frontend->StoreInterface,
                              NULL,
                              __FrontendGetBackendPath(Frontend),
                              "state",
                              &Buffer);
        if (!NT_SUCCESS(status)) {
            *State = XenbusStateUnknown;
        } else {
            *State = (XenbusState)strtol(Buffer, NULL, 10);

            XENBUS_STORE(Free,
                         &Frontend->StoreInterface,
                         Buffer);
        }

        KeQuerySystemTime(&Now);

        TimeDelta = (Now.QuadPart - Start.QuadPart) / 10000ull;
    }

    if (Watch != NULL)
        (VOID) XENBUS_STORE(WatchRemove,
                            &Frontend->StoreInterface,
                            Watch);

    Trace("%s: <==== (%s)\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(*State));
}

static VOID
FrontendReleaseBackend(
    IN      PXENVIF_FRONTEND    Frontend
    )
{
    Trace("=====>\n");

    ASSERT(Frontend->BackendDomain != DOMID_INVALID);
    ASSERT(Frontend->BackendPath != NULL);

    Frontend->BackendDomain = DOMID_INVALID;

    XENBUS_STORE(Free,
                 &Frontend->StoreInterface,
                 Frontend->BackendPath);
    Frontend->BackendPath = NULL;

    Trace("<=====\n");
}

static VOID
FrontendClose(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    XenbusState             State;

    Trace("====>\n");

    ASSERT(Frontend->Watch != NULL);
    (VOID) XENBUS_STORE(WatchRemove,
                        &Frontend->StoreInterface,
                        Frontend->Watch);
    Frontend->Watch = NULL;

    State = XenbusStateUnknown;
    while (State != XenbusStateClosed) {
        if (!FrontendIsOnline(Frontend))
            break;

        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);

        switch (State) {
        case XenbusStateUnknown:
            FrontendSetOffline(Frontend);
            break;

        case XenbusStateConnected:
        case XenbusStateInitWait:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateClosing);
            break;

        case XenbusStateClosing:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateClosed);
            break;

        case XenbusStateClosed:
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    FrontendReleaseBackend(Frontend);

    XENBUS_STORE(Release, &Frontend->StoreInterface);

    Trace("<====\n");
}

static NTSTATUS
FrontendPrepare(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    XenbusState             State;
    NTSTATUS                status;

    Trace("====>\n");

    status = XENBUS_STORE(Acquire, &Frontend->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    FrontendSetOnline(Frontend);

    status = FrontendAcquireBackend(Frontend);
    if (!NT_SUCCESS(status))
        goto fail2;

    State = XenbusStateUnknown;
    while (State != XenbusStateInitWait) {
        if (!FrontendIsOnline(Frontend))
            break;

        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);

        status = STATUS_SUCCESS;
        switch (State) {
        case XenbusStateUnknown:
            FrontendSetOffline(Frontend);
            break;

        case XenbusStateClosed:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateInitialising);
            break;

        case XenbusStateClosing:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateClosed);
            break;

        case XenbusStateInitWait:
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    status = STATUS_UNSUCCESSFUL;
    if (State != XenbusStateInitWait)
        goto fail3;

    status = XENBUS_STORE(WatchAdd,
                          &Frontend->StoreInterface,
                          __FrontendGetBackendPath(Frontend),
                          "online",
                          ThreadGetEvent(Frontend->EjectThread),
                          &Frontend->Watch);
    if (!NT_SUCCESS(status))
        goto fail4;

    Trace("<====\n");
    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

    FrontendReleaseBackend(Frontend);

fail2:
    Error("fail2\n");

    FrontendSetOffline(Frontend);

    XENBUS_STORE(Release, &Frontend->StoreInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    Trace("<====\n");
    return status;
}

static FORCEINLINE VOID
__FrontendQueryStatistic(
    IN  PXENVIF_FRONTEND        Frontend,
    IN  XENVIF_VIF_STATISTIC    Name,
    OUT PULONGLONG              Value
    )
{
    ULONG                       Index;

    ASSERT(Name < XENVIF_VIF_STATISTIC_COUNT);

    *Value = 0;
    for (Index = 0; Index < Frontend->StatisticsCount; Index++) {
        PXENVIF_FRONTEND_STATISTICS Statistics;

        Statistics = &Frontend->Statistics[Index];
        *Value += Statistics->Value[Name];
    }
}

VOID
FrontendQueryStatistic(
    IN  PXENVIF_FRONTEND        Frontend,
    IN  XENVIF_VIF_STATISTIC    Name,
    OUT PULONGLONG              Value
    )
{
    __FrontendQueryStatistic(Frontend, Name, Value);
}

VOID
FrontendIncrementStatistic(
    IN  PXENVIF_FRONTEND        Frontend,
    IN  XENVIF_VIF_STATISTIC    Name,
    IN  ULONGLONG               Delta
    )
{
    ULONG                       Index;
    PXENVIF_FRONTEND_STATISTICS Statistics;

    ASSERT(Name < XENVIF_VIF_STATISTIC_COUNT);

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    Index = KeGetCurrentProcessorNumberEx(NULL);

    ASSERT3U(Index, <, Frontend->StatisticsCount);
    Statistics = &Frontend->Statistics[Index];

    Statistics->Value[Name] += Delta;
}

static FORCEINLINE const CHAR *
__FrontendStatisticName(
    IN  XENVIF_VIF_STATISTIC    Name
    )
{
#define _FRONTEND_STATISTIC_NAME(_Name)     \
    case XENVIF_ ## _Name:                  \
        return #_Name;

    switch (Name) {
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_PACKETS_DROPPED);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_BACKEND_ERRORS);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_FRONTEND_ERRORS);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_UNICAST_PACKETS);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_UNICAST_OCTETS);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_MULTICAST_PACKETS);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_MULTICAST_OCTETS);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_BROADCAST_PACKETS);
    _FRONTEND_STATISTIC_NAME(TRANSMITTER_BROADCAST_OCTETS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_PACKETS_DROPPED);
    _FRONTEND_STATISTIC_NAME(RECEIVER_BACKEND_ERRORS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_FRONTEND_ERRORS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_UNICAST_PACKETS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_UNICAST_OCTETS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_MULTICAST_PACKETS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_MULTICAST_OCTETS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_BROADCAST_PACKETS);
    _FRONTEND_STATISTIC_NAME(RECEIVER_BROADCAST_OCTETS);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _FRONTEND_STATISTIC_NAME
}

static VOID
FrontendDebugCallback(
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
    )
{
    PXENVIF_FRONTEND        Frontend = Argument;
    XENVIF_VIF_STATISTIC    Name;

    UNREFERENCED_PARAMETER(Crashing);

    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "PATH: %s\n",
                 __FrontendGetPath(Frontend));

    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "STATISTICS:\n");

    for (Name = 0; Name < XENVIF_VIF_STATISTIC_COUNT; Name++) {
        ULONGLONG   Value;

        __FrontendQueryStatistic(Frontend, Name, &Value);

        XENBUS_DEBUG(Printf,
                     &Frontend->DebugInterface,
                     " - %40s %lu\n",
                     __FrontendStatisticName(Name),
                     Value);
    }
}

static VOID
FrontendSetNumQueues(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    PCHAR                   Buffer;
    ULONG                   BackendMaxQueues;
    NTSTATUS                status;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          "multi-queue-max-queues",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        BackendMaxQueues = (ULONG)strtoul(Buffer, NULL, 10);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    } else {
        BackendMaxQueues = 1;
    }

    Frontend->NumQueues = __min(Frontend->MaxQueues, BackendMaxQueues);

    Info("%u\n", Frontend->NumQueues);
}

static FORCEINLINE ULONG
__FrontendGetNumQueues(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return Frontend->NumQueues;
}

ULONG
FrontendGetNumQueues(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    return __FrontendGetNumQueues(Frontend);
}

static NTSTATUS
FrontendConnect(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    XenbusState             State;
    ULONG                   Attempt;
    NTSTATUS                status;

    Trace("====>\n");

    Frontend->StatisticsCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Frontend->Statistics = __FrontendAllocate(sizeof (XENVIF_FRONTEND_STATISTICS) * Frontend->StatisticsCount);

    status = STATUS_NO_MEMORY;
    if (Frontend->Statistics == NULL)
        goto fail1;

    status = XENBUS_DEBUG(Acquire, &Frontend->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_DEBUG(Register,
                          &Frontend->DebugInterface,
                          __MODULE__ "|FRONTEND",
                          FrontendDebugCallback,
                          Frontend,
                          &Frontend->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = MacConnect(__FrontendGetMac(Frontend));
    if (!NT_SUCCESS(status))
        goto fail4;

    FrontendSetNumQueues(Frontend);

    status = ReceiverConnect(__FrontendGetReceiver(Frontend));
    if (!NT_SUCCESS(status))
        goto fail5;

    status = TransmitterConnect(__FrontendGetTransmitter(Frontend));
    if (!NT_SUCCESS(status))
        goto fail6;

    Attempt = 0;
    do {
        PXENBUS_STORE_TRANSACTION   Transaction;

        status = XENBUS_STORE(TransactionStart,
                              &Frontend->StoreInterface,
                              &Transaction);
        if (!NT_SUCCESS(status))
            break;

        status = ReceiverStoreWrite(__FrontendGetReceiver(Frontend),
                                    Transaction);
        if (!NT_SUCCESS(status))
            goto abort;

        status = TransmitterStoreWrite(__FrontendGetTransmitter(Frontend),
                                       Transaction);
        if (!NT_SUCCESS(status))
            goto abort;

        status = XENBUS_STORE(Printf,
                              &Frontend->StoreInterface,
                              Transaction,
                              __FrontendGetPath(Frontend),
                              "multi-queue-num-queues",
                              "%u",
                              __FrontendGetNumQueues(Frontend));
        if (!NT_SUCCESS(status))
            goto abort;

        status = XENBUS_STORE(TransactionEnd,
                              &Frontend->StoreInterface,
                              Transaction,
                              TRUE);
        if (status != STATUS_RETRY || ++Attempt > 10)
            break;

        continue;

abort:
        (VOID) XENBUS_STORE(TransactionEnd,
                            &Frontend->StoreInterface,
                            Transaction,
                            FALSE);
        break;
    } while (status == STATUS_RETRY);

    if (!NT_SUCCESS(status))
        goto fail7;

    State = XenbusStateUnknown;
    while (State != XenbusStateConnected) {
        if (!FrontendIsOnline(Frontend))
            break;

        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);

        status = STATUS_SUCCESS;
        switch (State) {
        case XenbusStateUnknown:
            FrontendSetOffline(Frontend);
            break;

        case XenbusStateInitWait:
        case XenbusStateInitialised:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateConnected);
            break;

        case XenbusStateClosing:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateClosed);
            break;

        case XenbusStateConnected:
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    status = STATUS_UNSUCCESSFUL;
    if (State != XenbusStateConnected)
        goto fail8;

    ThreadWake(Frontend->MibThread);

    Trace("<====\n");
    return STATUS_SUCCESS;

fail8:
    Error("fail8\n");

fail7:
    Error("fail7\n");

    TransmitterDisconnect(__FrontendGetTransmitter(Frontend));

fail6:
    Error("fail6\n");

    ReceiverDisconnect(__FrontendGetReceiver(Frontend));

fail5:
    Error("fail5\n");

    MacDisconnect(__FrontendGetMac(Frontend));

    Frontend->NumQueues = 0;

fail4:
    Error("fail4\n");

    XENBUS_DEBUG(Deregister,
                 &Frontend->DebugInterface,
                 Frontend->DebugCallback);
    Frontend->DebugCallback = NULL;

fail3:
    Error("fail3\n");

    XENBUS_DEBUG(Release, &Frontend->DebugInterface);

fail2:
    Error("fail2\n");

    __FrontendFree(Frontend->Statistics);
    Frontend->Statistics = NULL;
    Frontend->StatisticsCount = 0;

fail1:
    Error("fail1 (%08x)\n", status);

    Trace("<====\n");
    return status;
}

static VOID
FrontendDisconnect(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    Trace("====>\n");

    TransmitterDisconnect(__FrontendGetTransmitter(Frontend));
    ReceiverDisconnect(__FrontendGetReceiver(Frontend));
    MacDisconnect(__FrontendGetMac(Frontend));

    Frontend->NumQueues = 0;

    XENBUS_DEBUG(Deregister,
                 &Frontend->DebugInterface,
                 Frontend->DebugCallback);
    Frontend->DebugCallback = NULL;

    XENBUS_DEBUG(Release, &Frontend->DebugInterface);

    __FrontendFree(Frontend->Statistics);
    Frontend->Statistics = NULL;
    Frontend->StatisticsCount = 0;

    Trace("<====\n");
}

static NTSTATUS
FrontendEnable(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    NTSTATUS                status;

    Trace("====>\n");

    status = MacEnable(__FrontendGetMac(Frontend));
    if (!NT_SUCCESS(status))
        goto fail1;

    status = ReceiverEnable(__FrontendGetReceiver(Frontend));
    if (!NT_SUCCESS(status))
        goto fail2;

    status = TransmitterEnable(__FrontendGetTransmitter(Frontend));
    if (!NT_SUCCESS(status))
        goto fail3;

    Trace("<====\n");
    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    ReceiverDisable(__FrontendGetReceiver(Frontend));

fail2:
    Error("fail2\n");

    MacDisable(__FrontendGetMac(Frontend));

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FrontendDisable(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    Trace("====>\n");

    TransmitterDisable(__FrontendGetTransmitter(Frontend));
    ReceiverDisable(__FrontendGetReceiver(Frontend));
    MacDisable(__FrontendGetMac(Frontend));

    Trace("<====\n");
}

NTSTATUS
FrontendSetState(
    IN  PXENVIF_FRONTEND        Frontend,
    IN  XENVIF_FRONTEND_STATE   State
    )
{
    BOOLEAN                     Failed;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    Trace("%s: ====> '%s' -> '%s'\n",
          __FrontendGetPath(Frontend),
          FrontendStateName(Frontend->State),
          FrontendStateName(State));

    Failed = FALSE;
    while (Frontend->State != State && !Failed) {
        NTSTATUS    status;

        switch (Frontend->State) {
        case FRONTEND_UNKNOWN:
            switch (State) {
            case FRONTEND_CLOSED:
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendPrepare(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_PREPARED;
                } else {
                    Failed = TRUE;
                }
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CLOSED:
            switch (State) {
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendPrepare(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_PREPARED;
                } else {
                    Failed = TRUE;
                }
                break;

            case FRONTEND_UNKNOWN:
                Frontend->State = FRONTEND_UNKNOWN;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_PREPARED:
            switch (State) {
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendConnect(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_CONNECTED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;

                    Failed = TRUE;
                }
                break;

            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendClose(Frontend);
                Frontend->State = FRONTEND_CLOSED;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CONNECTED:
            switch (State) {
            case FRONTEND_ENABLED:
                status = FrontendEnable(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_ENABLED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;

                    FrontendDisconnect(Frontend);
                    Failed = TRUE;
                }
                break;

            case FRONTEND_PREPARED:
            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendClose(Frontend);
                Frontend->State = FRONTEND_CLOSED;

                FrontendDisconnect(Frontend);
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_ENABLED:
            switch (State) {
            case FRONTEND_CONNECTED:
            case FRONTEND_PREPARED:
            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendDisable(Frontend);
                Frontend->State = FRONTEND_CONNECTED;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        default:
            ASSERT(FALSE);
            break;
        }

        Trace("%s in state '%s'\n",
              __FrontendGetPath(Frontend),
              FrontendStateName(Frontend->State));
    }

    KeReleaseSpinLock(&Frontend->Lock, Irql);

    Trace("%s: <=====\n", __FrontendGetPath(Frontend));

    return (!Failed) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static FORCEINLINE VOID
__FrontendResume(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    ASSERT3U(Frontend->State, ==, FRONTEND_UNKNOWN);
    (VOID) FrontendSetState(Frontend, FRONTEND_CLOSED);
}

static FORCEINLINE VOID
__FrontendSuspend(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    (VOID) FrontendSetState(Frontend, FRONTEND_UNKNOWN);
}

static DECLSPEC_NOINLINE VOID
FrontendSuspendCallbackLate(
    IN  PVOID           Argument
    )
{
    PXENVIF_FRONTEND    Frontend = Argument;
    NTSTATUS            status;

    FrontendReleaseBackend(Frontend);

    status = FrontendAcquireBackend(Frontend);
    ASSERT(NT_SUCCESS(status));

    __FrontendSuspend(Frontend);
    __FrontendResume(Frontend);
}

NTSTATUS
FrontendResume(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    KIRQL                   Irql;
    NTSTATUS                status;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_SUSPEND(Acquire, &Frontend->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FrontendResume(Frontend);

    status = XENBUS_SUSPEND(Register,
                            &Frontend->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            FrontendSuspendCallbackLate,
                            Frontend,
                            &Frontend->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID) KeWaitForSingleObject(&Frontend->EjectEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    Trace("<====\n");

    return STATUS_SUCCESS;
    
fail2:
    Error("fail2\n");

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

VOID
FrontendSuspend(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    KIRQL                   Irql;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_SUSPEND(Deregister,
                   &Frontend->SuspendInterface,
                   Frontend->SuspendCallbackLate);
    Frontend->SuspendCallbackLate = NULL;

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID) KeWaitForSingleObject(&Frontend->EjectEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    Trace("<====\n");
}

NTSTATUS
FrontendInitialize(
    IN  PXENVIF_PDO         Pdo,
    OUT PXENVIF_FRONTEND    *Frontend
    )
{
    PCHAR                   Name;
    ULONG                   Length;
    PCHAR                   Path;
    PCHAR                   Prefix;
    NTSTATUS                status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Name = PdoGetName(Pdo);

    Length = sizeof ("devices/vif/") + (ULONG)strlen(Name);
    Path = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path, 
                                Length,
                                "device/vif/%s", 
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    Length = sizeof ("data/vif/") + (ULONG)strlen(Name);
    Prefix = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Prefix == NULL)
        goto fail3;

    status = RtlStringCbPrintfA(Prefix, 
                                Length,
                                "data/vif/%s", 
                                Name);
    if (!NT_SUCCESS(status))
        goto fail4;

    *Frontend = __FrontendAllocate(sizeof (XENVIF_FRONTEND));

    status = STATUS_NO_MEMORY;
    if (*Frontend == NULL)
        goto fail5;

    (*Frontend)->Pdo = Pdo;
    (*Frontend)->Path = Path;
    (*Frontend)->Prefix = Prefix;
    (*Frontend)->BackendDomain = DOMID_INVALID;

    KeInitializeSpinLock(&(*Frontend)->Lock);

    (*Frontend)->Online = TRUE;

    FdoGetDebugInterface(PdoGetFdo(Pdo), &(*Frontend)->DebugInterface);
    FdoGetSuspendInterface(PdoGetFdo(Pdo), &(*Frontend)->SuspendInterface);
    FdoGetStoreInterface(PdoGetFdo(Pdo), &(*Frontend)->StoreInterface);

    FrontendSetMaxQueues(*Frontend);

    status = MacInitialize(*Frontend, &(*Frontend)->Mac);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = ReceiverInitialize(*Frontend, &(*Frontend)->Receiver);
    if (!NT_SUCCESS(status))
        goto fail7;

    status = TransmitterInitialize(*Frontend, &(*Frontend)->Transmitter);
    if (!NT_SUCCESS(status))
        goto fail8;

    KeInitializeEvent(&(*Frontend)->EjectEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FrontendEject, *Frontend, &(*Frontend)->EjectThread);
    if (!NT_SUCCESS(status))
        goto fail9;

    status = ThreadCreate(FrontendMib, *Frontend, &(*Frontend)->MibThread);
    if (!NT_SUCCESS(status))
        goto fail10;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail10:
    Error("fail10\n");

    ThreadAlert((*Frontend)->EjectThread);
    ThreadJoin((*Frontend)->EjectThread);
    (*Frontend)->EjectThread = NULL;

fail9:
    Error("fail9\n");

    RtlZeroMemory(&(*Frontend)->EjectEvent, sizeof (KEVENT));

    TransmitterTeardown(__FrontendGetTransmitter(*Frontend));
    (*Frontend)->Transmitter = NULL;

fail8:
    Error("fail8\n");

    ReceiverTeardown(__FrontendGetReceiver(*Frontend));
    (*Frontend)->Receiver = NULL;

fail7:
    Error("fail7\n");

    MacTeardown(__FrontendGetMac(*Frontend));
    (*Frontend)->Mac = NULL;

fail6:
    Error("fail6\n");

    (*Frontend)->MaxQueues = 0;

    RtlZeroMemory(&(*Frontend)->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&(*Frontend)->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&(*Frontend)->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    (*Frontend)->Online = FALSE;

    RtlZeroMemory(&(*Frontend)->Lock, sizeof (KSPIN_LOCK));

    (*Frontend)->BackendDomain = 0;
    (*Frontend)->Prefix = NULL;
    (*Frontend)->Path = NULL;
    (*Frontend)->Pdo = NULL;

    ASSERT(IsZeroMemory(*Frontend, sizeof (XENVIF_FRONTEND)));

    __FrontendFree(*Frontend);
    *Frontend = NULL;

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

    __FrontendFree(Prefix);

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    __FrontendFree(Path);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FrontendTeardown(
    IN  PXENVIF_FRONTEND    Frontend
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    ASSERT(Frontend->State == FRONTEND_UNKNOWN);

    ThreadAlert(Frontend->MibThread);
    ThreadJoin(Frontend->MibThread);
    Frontend->MibThread = NULL;

    ThreadAlert(Frontend->EjectThread);
    ThreadJoin(Frontend->EjectThread);
    Frontend->EjectThread = NULL;

    RtlZeroMemory(&Frontend->EjectEvent, sizeof (KEVENT));

    TransmitterTeardown(__FrontendGetTransmitter(Frontend));
    Frontend->Transmitter = NULL;

    ReceiverTeardown(__FrontendGetReceiver(Frontend));
    Frontend->Receiver = NULL;

    MacTeardown(__FrontendGetMac(Frontend));
    Frontend->Mac = NULL;

    Frontend->MaxQueues = 0;

    RtlZeroMemory(&Frontend->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Frontend->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Frontend->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    Frontend->Online = FALSE;

    RtlZeroMemory(&Frontend->Lock, sizeof (KSPIN_LOCK));

    Frontend->BackendDomain = 0;

    __FrontendFree(Frontend->Prefix);
    Frontend->Prefix = NULL;

    __FrontendFree(Frontend->Path);
    Frontend->Path = NULL;

    Frontend->Pdo = NULL;

    ASSERT(IsZeroMemory(Frontend, sizeof (XENVIF_FRONTEND)));

    __FrontendFree(Frontend);

    Trace("<====\n");
}
