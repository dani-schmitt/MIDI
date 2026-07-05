// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================


#include "pch.h"
#include "midi2.IpMidiTransport.h"

#include "MidiEndpointNameTable.h"

using namespace wil;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

#define MAX_DEVICE_ID_LEN 200 // size in chars

namespace
{
    std::wstring MakeEndpointName(uint8_t portIndex)
    {
        wchar_t value[32]{};
        swprintf_s(value, L"%02u. Ethernet MIDI", static_cast<unsigned>(portIndex + 1));
        return value;
    }

    std::wstring MakeEndpointUniqueId(uint8_t portIndex)
    {
        wchar_t value[32]{};
        swprintf_s(value, L"%02u_ETHERNET_MIDI", static_cast<unsigned>(portIndex + 1));
        return value;
    }

    void LogActualPortsWriteFailure(HRESULT hr)
    {
        if (FAILED(hr))
        {
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"Unable to update ipMIDI ActualPorts registry value", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingHResult(hr, MIDI_TRACE_EVENT_HRESULT_FIELD));
        }
    }
}

GUID TransportLayerGUID = TRANSPORT_LAYER_GUID;


_Use_decl_annotations_
HRESULT
CMidi2IpMidiEndpointManager::Initialize(
    IMidiDeviceManager* midiDeviceManager,
    IMidiEndpointProtocolManager* midiEndpointProtocolManager
)
{
    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this")
    );

    RETURN_HR_IF(E_INVALIDARG, nullptr == midiDeviceManager);
    RETURN_HR_IF(E_INVALIDARG, nullptr == midiEndpointProtocolManager);

    RETURN_IF_FAILED(midiDeviceManager->QueryInterface(__uuidof(IMidiDeviceManager), (void**)&m_MidiDeviceManager));
    RETURN_IF_FAILED(midiEndpointProtocolManager->QueryInterface(__uuidof(IMidiEndpointProtocolManager), (void**)&m_MidiProtocolManager));


    m_TransportTransportId = TransportLayerGUID;    // this is needed so MidiSrv can instantiate the correct transport
    m_ContainerId = m_TransportTransportId;           // we use the transport ID as the container ID for convenience

    LogActualPortsWriteFailure(IpMidiRegistrySettings::WriteActualPorts(0));
    RETURN_IF_FAILED(CreateParentDevice());
    RETURN_IF_FAILED(TransportState::Current().InitializeNetworkEngine());

    auto networkEngine = TransportState::Current().GetNetworkEngine();
    RETURN_HR_IF_NULL(E_UNEXPECTED, networkEngine);

    m_initialized = true;
    const auto wantedPorts = IpMidiRegistrySettings::ReadWantedPorts();

    for (uint8_t portIndex = 0; portIndex < wantedPorts; ++portIndex)
    {
        const auto prepareResult = networkEngine->PreparePort(portIndex);
        if (FAILED(prepareResult))
        {
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"Stopping ipMIDI port creation after network preparation failure", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
                TraceLoggingHResult(prepareResult, MIDI_TRACE_EVENT_HRESULT_FIELD));
            break;
        }

        auto definition = std::make_shared<MidiIpMidiDeviceDefinition>();
        definition->PortIndex = portIndex;
        definition->UdpPort = static_cast<uint16_t>(IP_MIDI_BASE_UDP_PORT + portIndex);
        definition->AssociationId = IP_MIDI_ENDPOINT_ASSOCIATION_IDS[portIndex];
        definition->EndpointName = MakeEndpointName(portIndex);
        definition->EndpointDescription = L"ipMIDI Ethernet endpoint.";
        definition->EndpointUniqueIdentifier = MakeEndpointUniqueId(portIndex);
        definition->InstanceIdPrefix = MIDI_IP_MIDI_INSTANCE_ID_PREFIX;

        const auto createEndpointResult = CreateEndpoint(definition);
        if (FAILED(createEndpointResult))
        {
            networkEngine->RemoveLastPreparedPort();
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"Stopping ipMIDI port creation after endpoint activation failure", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
                TraceLoggingHResult(createEndpointResult, MIDI_TRACE_EVENT_HRESULT_FIELD));
            break;
        }

        m_createdEndpoints.push_back(std::move(definition));
    }

    const auto startResult = networkEngine->Start();
    if (FAILED(startResult))
    {
        for (auto endpoint = m_createdEndpoints.rbegin(); endpoint != m_createdEndpoints.rend(); ++endpoint)
        {
            LOG_IF_FAILED(DeleteEndpoint(*endpoint));
            TransportState::Current().GetEndpointTable()->RemoveDevice((*endpoint)->AssociationId);
        }
        m_createdEndpoints.clear();
        TransportState::Current().ShutdownNetworkEngine();
        LogActualPortsWriteFailure(IpMidiRegistrySettings::WriteActualPorts(0));
        m_initialized = false;
        return startResult;
    }

    LogActualPortsWriteFailure(IpMidiRegistrySettings::WriteActualPorts(
        static_cast<uint8_t>(m_createdEndpoints.size())));

    return S_OK;
}


HRESULT
CMidi2IpMidiEndpointManager::ProcessWorkQueue()
{
    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Enter", MIDI_TRACE_EVENT_MESSAGE_FIELD)
        );

    uint32_t countItemsProcessed{ 0 };

    while (!TransportState::Current().GetEndpointWorkQueue()->IsEmpty())
    {
        TransportWorkItem item{ };

        if (TransportState::Current().GetEndpointWorkQueue()->GetNextWorkItem(item))
        {
            if (item.Type == TransportWorkItemWorkType::Create)
            {
                LOG_IF_FAILED(CreateEndpoint(item.Definition));

                countItemsProcessed++;
            }

            // TODO: Process other types of work items
        }
    }

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Exit", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt32(countItemsProcessed, "count items processed")
    );

    return S_OK;
}


HRESULT
CMidi2IpMidiEndpointManager::CreateParentDevice()
{
    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this")
    );

    // this happens before initialization is complete, so don't gate on m_initialized here

    RETURN_HR_IF_NULL(E_POINTER, m_MidiDeviceManager);

    // the parent device parameters are set by the transport (this)
    std::wstring parentDeviceName{ TRANSPORT_PARENT_DEVICE_NAME };
    std::wstring parentDeviceId{ internal::NormalizeDeviceInstanceIdWStringCopy(TRANSPORT_PARENT_ID) };

    SW_DEVICE_CREATE_INFO createInfo = {};
    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszInstanceId = parentDeviceId.c_str();
    createInfo.CapabilityFlags = SWDeviceCapabilitiesNone;
    createInfo.pszDeviceDescription = parentDeviceName.c_str();
    createInfo.pContainerId = &m_ContainerId;

    wil::unique_cotaskmem_string newDeviceId;

    RETURN_IF_FAILED(m_MidiDeviceManager->ActivateVirtualParentDevice(
        0,
        nullptr,
        &createInfo,
        &newDeviceId
    ));

    m_parentDeviceId = internal::NormalizeDeviceInstanceIdWStringCopy(newDeviceId.get());


    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(newDeviceId.get(), "New parent device instance id")
    );

    return S_OK;
}



_Use_decl_annotations_
HRESULT
CMidi2IpMidiEndpointManager::UpdateEndpointMutedStateProperty(
    _In_ std::shared_ptr<MidiIpMidiDeviceDefinition> definition)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, definition);

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Enter", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingWideString(definition->CreatedEndpointInterfaceId.c_str(), MIDI_TRACE_EVENT_DEVICE_SWD_ID_FIELD),
        TraceLoggingBool(definition->IsMuted, "is muted")
    );

    DEVPROP_BOOLEAN devPropTrue = DEVPROP_TRUE;
    DEVPROP_BOOLEAN devPropFalse = DEVPROP_FALSE;

    std::vector<DEVPROPERTY> interfaceDevProperties{};

    // see if this is going to start off as muted
    if (definition->IsMuted)
    {
        interfaceDevProperties.push_back(DEVPROPERTY{ {PKEY_MIDI_IsMuted, DEVPROP_STORE_SYSTEM, nullptr},
            DEVPROP_TYPE_BOOLEAN, (ULONG)(sizeof(DEVPROP_BOOLEAN)), &devPropTrue });
    }
    else
    {
        interfaceDevProperties.push_back(DEVPROPERTY{ {PKEY_MIDI_IsMuted, DEVPROP_STORE_SYSTEM, nullptr},
            DEVPROP_TYPE_BOOLEAN, (ULONG)(sizeof(DEVPROP_BOOLEAN)), &devPropFalse });
    }

    RETURN_IF_FAILED(m_MidiDeviceManager->UpdateEndpointProperties(
        definition->CreatedEndpointInterfaceId.c_str(),
        static_cast<ULONG>(interfaceDevProperties.size()),
        interfaceDevProperties.data()
        ));

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Exit", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingWideString(definition->CreatedEndpointInterfaceId.c_str(), MIDI_TRACE_EVENT_DEVICE_SWD_ID_FIELD),
        TraceLoggingBool(definition->IsMuted, "is muted")
    );

    return S_OK;
}


_Use_decl_annotations_
HRESULT
CMidi2IpMidiEndpointManager::DeleteEndpoint(
    _In_ std::shared_ptr<MidiIpMidiDeviceDefinition> const definition)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, definition);

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this")
    );

    RETURN_IF_FAILED(m_MidiDeviceManager->RemoveEndpoint(definition->CreatedShortClientInstanceId.c_str()));

    return S_OK;
}


_Use_decl_annotations_
HRESULT
CMidi2IpMidiEndpointManager::CreateEndpoint(
    std::shared_ptr<MidiIpMidiDeviceDefinition> definition
    )
{
    RETURN_HR_IF(E_UNEXPECTED, !m_initialized);
    RETURN_HR_IF_NULL(E_POINTER, m_MidiDeviceManager);

    RETURN_HR_IF_NULL(E_INVALIDARG, definition);

    RETURN_HR_IF_MSG(E_INVALIDARG, definition->EndpointName.empty(), "Empty endpoint name");
    RETURN_HR_IF_MSG(E_INVALIDARG, definition->InstanceIdPrefix.empty(), "Empty endpoint prefix");
    RETURN_HR_IF_MSG(E_INVALIDARG, definition->EndpointUniqueIdentifier.empty(), "Empty endpoint unique id");


    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Enter", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingGuid(definition->AssociationId, "association id"),
        TraceLoggingWideString(definition->InstanceIdPrefix.c_str(), "prefix"),
        TraceLoggingWideString(definition->EndpointUniqueIdentifier.c_str(), "unique identifier")
        );

    DEVPROP_BOOLEAN devPropTrue = DEVPROP_TRUE;
    DEVPROP_BOOLEAN devPropFalse = DEVPROP_FALSE;


    std::wstring transportCode(TRANSPORT_CODE);

    //DEVPROP_BOOLEAN devPropTrue = DEVPROP_TRUE;
    //   DEVPROP_BOOLEAN devPropFalse = DEVPROP_FALSE;

    std::wstring endpointName = definition->EndpointName;
    std::wstring endpointDescription = definition->EndpointDescription;

    std::vector<DEVPROPERTY> interfaceDevProperties{};

    // no user or in-protocol data in this case
    std::wstring friendlyName = internal::CalculateEndpointDevicePrimaryName(endpointName, L"", L"");


    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Adding endpoint properties", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingGuid(definition->AssociationId, "association id"),
        TraceLoggingWideString(definition->EndpointUniqueIdentifier.c_str(), "unique identifier"),
        TraceLoggingWideString(transportCode.c_str(), "transport code")
    );

    std::wstring associationIdString = internal::GuidToString(definition->AssociationId);

    interfaceDevProperties.push_back(DEVPROPERTY{ {PKEY_MIDI_VirtualMidiEndpointAssociator, DEVPROP_STORE_SYSTEM, nullptr},
        DEVPROP_TYPE_STRING, (ULONG)(sizeof(wchar_t) * (associationIdString.length() + 1)), (PVOID)associationIdString.c_str() });


    // see if this is going to start off as muted
    if (definition->IsMuted)
    {
        interfaceDevProperties.push_back(DEVPROPERTY{ {PKEY_MIDI_IsMuted, DEVPROP_STORE_SYSTEM, nullptr},
            DEVPROP_TYPE_BOOLEAN, (ULONG)(sizeof(DEVPROP_BOOLEAN)), &devPropTrue });
    }
    else
    {
        interfaceDevProperties.push_back(DEVPROPERTY{ {PKEY_MIDI_IsMuted, DEVPROP_STORE_SYSTEM, nullptr},
            DEVPROP_TYPE_BOOLEAN, (ULONG)(sizeof(DEVPROP_BOOLEAN)), &devPropFalse });
    }


    // Device properties


    SW_DEVICE_CREATE_INFO createInfo = {};
    createInfo.cbSize = sizeof(createInfo);

    // build the instance id, which becomes the middle of the SWD id
    std::wstring instanceId = internal::NormalizeDeviceInstanceIdWStringCopy(
        definition->InstanceIdPrefix + definition->EndpointUniqueIdentifier);

    createInfo.pszInstanceId = instanceId.c_str();
    createInfo.CapabilityFlags = SWDeviceCapabilitiesNone;
    createInfo.pszDeviceDescription = friendlyName.c_str();

    wil::unique_cotaskmem_string newDeviceInterfaceId;

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Activating endpoint", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingGuid(definition->AssociationId, "association id"),
        TraceLoggingWideString(definition->EndpointUniqueIdentifier.c_str(), "unique identifier"),
        TraceLoggingWideString(instanceId.c_str(), "instance id")
    );

    MIDIENDPOINTCOMMONPROPERTIES commonProperties{};
    commonProperties.TransportId = m_TransportTransportId;
    commonProperties.EndpointDeviceType = MidiEndpointDeviceType::MidiEndpointDeviceType_Normal;
    commonProperties.FriendlyName = friendlyName.c_str();
    commonProperties.TransportCode = transportCode.c_str();
    commonProperties.EndpointName = endpointName.c_str();
    commonProperties.EndpointDescription = nullptr; // TODO: Should generate a default description here
    commonProperties.CustomEndpointName = friendlyName.c_str(); // technically, the user supplied this so we put it in both spots
    commonProperties.CustomEndpointDescription = endpointDescription.size() > 0 ? endpointDescription.c_str() : nullptr;
    commonProperties.UniqueIdentifier = definition->EndpointUniqueIdentifier.c_str();
    commonProperties.SupportedDataFormats = MidiDataFormats::MidiDataFormats_UMP;
    commonProperties.NativeDataFormat = MidiDataFormats::MidiDataFormats_UMP;

    UINT32 capabilities {0};
    capabilities |= MidiEndpointCapabilities_SupportsMidi1Protocol;
    capabilities |= MidiEndpointCapabilities_SupportsMultiClient;
    commonProperties.Capabilities = (MidiEndpointCapabilities) capabilities;


    // add a single group terminal block in each direction to support MIDI 1.0 port creation without creating 16 ins and 16 outs.

    std::vector<internal::GroupTerminalBlockInternal> blocks{ };

    internal::GroupTerminalBlockInternal gtb1;
    gtb1.Number = 1;             // gtb indexes start at 1
    gtb1.GroupCount = 1;         // todo: we could get this from properties
    gtb1.FirstGroupIndex = 0;    // group indexes start at 0
    gtb1.Protocol = 0x01;        // 0x01 = MIDI 1.0
    gtb1.Direction = MIDI_GROUP_TERMINAL_BLOCK_INPUT;   // MIDI Out from user's perspective
    gtb1.Name = endpointName;
    blocks.push_back(gtb1);

    internal::GroupTerminalBlockInternal gtb2;
    gtb2.Number = 1;             // gtb indexes start at 1
    gtb2.GroupCount = 1;         // todo: we could get this from properties
    gtb2.FirstGroupIndex = 0;    // group indexes start at 0
    gtb2.Protocol = 0x01;        // 0x01 = MIDI 1.0
    gtb2.Direction = MIDI_GROUP_TERMINAL_BLOCK_OUTPUT;  // MIDI In from user's perspective
    gtb2.Name = endpointName;
    blocks.push_back(gtb2);


    std::vector<std::byte> groupTerminalBlockData;
    if (internal::WriteGroupTerminalBlocksToPropertyDataPointer(blocks, groupTerminalBlockData))
    {
        interfaceDevProperties.push_back({ { PKEY_MIDI_GroupTerminalBlocks, DEVPROP_STORE_SYSTEM, nullptr },
            DEVPROP_TYPE_BINARY, (ULONG)groupTerminalBlockData.size(), (PVOID)groupTerminalBlockData.data() });

    }

    // Sort out MIDI 1.0 endpoint names
    WindowsMidiServicesNamingLib::MidiEndpointNameTable nameTable{};

    RETURN_IF_FAILED(nameTable.PopulateAllEntriesForNativeUmpDevice(L"", blocks));
    const winrt::hstring endpointNameHString{ endpointName };
    RETURN_HR_IF(E_FAIL, !nameTable.UpdateSourceEntryCustomName(0, endpointNameHString));
    RETURN_HR_IF(E_FAIL, !nameTable.UpdateDestinationEntryCustomName(0, endpointNameHString));
    RETURN_IF_FAILED(nameTable.WriteProperties(interfaceDevProperties));


    RETURN_IF_FAILED(m_MidiDeviceManager->ActivateEndpoint(
        (PCWSTR)m_parentDeviceId.c_str(),                       // parent instance Id
        false,                                                  // UMP-only. When set to false, WinMM MIDI 1.0 ports are created
        MidiFlow::MidiFlowBidirectional,                        // MIDI Flow
        &commonProperties,
        (ULONG)interfaceDevProperties.size(),
        (ULONG)0,
        interfaceDevProperties.data(),
        nullptr,
        &createInfo,
        &newDeviceInterfaceId));


    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Endpoint activated", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingGuid(definition->AssociationId, "association id"),
        TraceLoggingWideString(definition->EndpointUniqueIdentifier.c_str(), "unique identifier"),
        TraceLoggingWideString(newDeviceInterfaceId.get(), "new device interface id")
    );


    // we need this for removal later
    definition->CreatedShortClientInstanceId = instanceId;
    definition->CreatedEndpointInterfaceId = internal::NormalizeEndpointInterfaceIdWStringCopy(newDeviceInterfaceId.get());

    // store for tracking
    auto device = std::make_shared<MidiIpMidiDevice>();
    device->Definition = definition;

    TransportState::Current().GetEndpointTable()->SetDevice(definition->AssociationId, device);

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Done", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingGuid(definition->AssociationId, "association id"),
        TraceLoggingWideString(definition->EndpointUniqueIdentifier.c_str(), "unique identifier")
    );

    return S_OK;
}



HRESULT
CMidi2IpMidiEndpointManager::Shutdown()
{
    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this")
    );


    TransportState::Current().ShutdownNetworkEngine();

    for (auto endpoint = m_createdEndpoints.rbegin(); endpoint != m_createdEndpoints.rend(); ++endpoint)
    {
        LOG_IF_FAILED(DeleteEndpoint(*endpoint));
        TransportState::Current().GetEndpointTable()->RemoveDevice((*endpoint)->AssociationId);
    }
    m_createdEndpoints.clear();

    m_MidiDeviceManager.reset();
    m_MidiProtocolManager.reset();

    m_initialized = false;

    return S_OK;
}
