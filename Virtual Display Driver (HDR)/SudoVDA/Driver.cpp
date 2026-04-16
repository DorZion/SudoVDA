/*++

Copyright (c) Microsoft Corporation

Abstract:

	This module contains a sample implementation of an indirect display driver. See the included README.md file and the
	various TODO blocks throughout this file and all accompanying files for information on building a production driver.

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"
#include "edid.h"

#include <tuple>
#include <list>
#include <set>
#include <iostream>
#include <thread>
#include <mutex>
#include <cstdarg>

#include <AdapterOption.h>
#include <sudovda-ioctl.h>

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;
using namespace SUDOVDA;

LUID preferredAdapterLuid{};
bool preferredAdapterChanged = false;

// Set in SudoVDADeviceAdd. Used by persistence helpers to obtain a handle to
// the per-device PnP registry key (the only registry location Universal UMDF
// drivers can write to without tripping InfVerif "legacy AddReg" errors).
WDFDEVICE g_WdfDevice = nullptr;

std::recursive_mutex monitorListOp;
std::set<size_t> freeConnectorSlots;
std::list<IndirectMonitorContext*> monitorCtxList;

bool isHDRSupported = false;
bool testMode = false;
DWORD watchdogTimeout = 3; // seconds
DWORD watchdogCountdown = 0;
std::thread watchdogThread;

DWORD MaxVirtualMonitorCount = 10;
IDDCX_BITS_PER_COMPONENT SDRBITS = IDDCX_BITS_PER_COMPONENT_8;
IDDCX_BITS_PER_COMPONENT HDRBITS = IDDCX_BITS_PER_COMPONENT_10;

#pragma region SampleMonitors

static const UINT mode_scale_factors[] = {
	// Put 100 at the first for convenience and fool proof
	100,
	50,
	75,
	125,
	150,
};

// Default modes reported for edid-less monitors. The second mode is set as preferred
static const struct VirtualMonitorMode s_DefaultModes[] = {
	{800, 600, 30000},
	{800, 600, 59940},
	{800, 600, 60000},
	{800, 600, 72000},
	{800, 600, 90000},
	{800, 600, 120000},
	{800, 600, 144000},
	{800, 600, 240000},
	{1280, 720, 30000},
	{1280, 720, 59940},
	{1280, 720, 60000},
	{1280, 720, 72000},
	{1280, 720, 90000},
	{1280, 720, 120000},
	{1280, 720, 144000},
	{1366, 768, 30000},
	{1366, 768, 59940},
	{1366, 768, 60000},
	{1366, 768, 72000},
	{1366, 768, 90000},
	{1366, 768, 120000},
	{1366, 768, 144000},
	{1366, 768, 240000},
	{1920, 1080, 30000},
	{1920, 1080, 59940},
	{1920, 1080, 60000},
	{1920, 1080, 72000},
	{1920, 1080, 90000},
	{1920, 1080, 120000},
	{1920, 1080, 144000},
	{1920, 1080, 240000},
	{2560, 1440, 30000},
	{2560, 1440, 59940},
	{2560, 1440, 60000},
	{2560, 1440, 72000},
	{2560, 1440, 90000},
	{2560, 1440, 120000},
	{2560, 1440, 144000},
	{2560, 1440, 240000},
	{3840, 2160, 30000},
	{3840, 2160, 59940},
	{3840, 2160, 60000},
	{3840, 2160, 72000},
	{3840, 2160, 90000},
	{3840, 2160, 120000},
	{3840, 2160, 144000},
	{3840, 2160, 240000},

	// // Valve Index (1440x1600 per eye -> 2880x1600 combined)
	// {2880, 1600, 60000},
	// {2880, 1600, 72000},
	// {2880, 1600, 90000},
	// {2880, 1600, 120000},
	// {2880, 1600, 144000},
	// {2880, 1600, 240000},

	// // Meta Quest 2 (1832x1920 per eye -> 3664x1920 combined)
	// {3664, 1920, 60000},
	// {3664, 1920, 72000},
	// {3664, 1920, 90000},
	// {3664, 1920, 120000},
	// {3664, 1920, 144000},
	// {3664, 1920, 240000},

	// // Meta Quest 3 (2064x2208 per eye -> 4128x2208 combined)
	// {4128, 2208, 60000},
	// {4128, 2208, 72000},
	// {4128, 2208, 90000},
	// {4128, 2208, 120000},
	// {4128, 2208, 144000},
	// {4128, 2208, 240000},

	// // Apple Vision Pro (3660x3142 per eye -> 7320x3142 combined)
	// {7320, 3142, 60000},
	// {7320, 3142, 72000},
	// {7320, 3142, 90000},
	// {7320, 3142, 120000},
	// {7320, 3142, 144000},
	// {7320, 3142, 240000},
};

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
	Mode.totalSize.cx = Mode.activeSize.cx = Width;
	Mode.totalSize.cy = Mode.activeSize.cy = Height;

	// See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
	Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
	Mode.AdditionalSignalInfo.videoStandard = 255;

	DWORD Denominator = 1000;

	if (isHDRSupported) {
		if (VSync < 1000) {
			VSync *= 1000;
		}
	} else {
		if (VSync % 1000 > 500) {
			VSync = (VSync / 1000) + 1;
		} else {
			VSync /= 1000;
		}
		Denominator = 1;
	}

	Mode.vSyncFreq.Numerator = VSync;
	Mode.vSyncFreq.Denominator = Denominator;
	Mode.hSyncFreq.Numerator = VSync * Height;
	Mode.hSyncFreq.Denominator = Denominator;

	Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

	Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height) / Denominator;
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
	IDDCX_MONITOR_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_MONITOR_MODE2 CreateIddCxMonitorMode2(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
	IDDCX_MONITOR_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	Mode.BitsPerComponent.Rgb = SDRBITS | HDRBITS;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
	IDDCX_TARGET_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

static IDDCX_TARGET_MODE2 CreateIddCxTargetMode2(DWORD Width, DWORD Height, DWORD VSync)
{
	IDDCX_TARGET_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.BitsPerComponent.Rgb = SDRBITS | HDRBITS;
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD SudoVDADriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD SudoVDADeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY SudoVDADeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED SudoVDAAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES SudoVDAAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION SudoVDAParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES SudoVDAMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES SudoVDAMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN SudoVDAMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN SudoVDAMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO SudoVDAAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA SudoVDAMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 SudoVDAParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 SudoVDAMonitorQueryModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 SudoVDAAdapterCommitModes2;

EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP SudoVDAMonitorSetGammaRamp;

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

struct IndirectMonitorContextWrapper
{
	IndirectMonitorContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

// Cleanup callback for IDDCX_MONITOR objects
// This prevents using dangling pointers when attempting to reuse monitor handles
static void MonitorCleanupCallback(WDFOBJECT Object)
{
	auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(Object);
	if (pWrapper && pWrapper->pContext) {
		auto* ctx = pWrapper->pContext;

		std::lock_guard<std::recursive_mutex> lg(monitorListOp);

		ctx->SetMonitor(nullptr);

		// Mark as disconnected but DON'T return connector slot to the pool.
		// The slot stays reserved for this monitor so reconnect uses the same
		// ConnectorIndex, preserving Windows device identity (ICC profiles, etc).
		// The slot is only returned when REMOVE is called (permanent deletion).
		if (ctx->isConnected) {
			ctx->isConnected = false;
		}
	}
}

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);
	UNREFERENCED_PARAMETER(dwReason);

	return TRUE;
}

static void SudoVDALog(const wchar_t* fmt, ...) {
	wchar_t buf[1024];
	va_list args;
	va_start(args, fmt);
	int written = _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, args);
	va_end(args);
	if (written < 0) {
		return;
	}
	OutputDebugStringW(buf);
}

#define SUVDA_LOG(fmt, ...) SudoVDALog(L"[SudoVDA] " fmt L"\n", __VA_ARGS__)

// Extracts an EDID string field (serial string @ 0x4D or product name @ 0x71)
// which is stored as up to 13 bytes, optionally terminated by 0x0A and padded
// with spaces. Writes a trimmed, null-terminated copy to `out`.
static void ExtractEdidString(const uint8_t* edidData, size_t offset, char out[EDID_STRING_FIELD_SIZE + 1]) {
	memset(out, 0, EDID_STRING_FIELD_SIZE + 1);
	if (!edidData) return;
	for (size_t i = 0; i < EDID_STRING_FIELD_SIZE; i++) {
		uint8_t c = edidData[offset + i];
		if (c == 0x0A) break;
		out[i] = (char)c;
	}
	// Trim trailing spaces
	for (int i = EDID_STRING_FIELD_SIZE - 1; i >= 0; i--) {
		if (out[i] == ' ') out[i] = 0;
		else if (out[i]) break;
	}
}

// Forward declaration; the definition lives in the DisplayPersistence region.
void GuidToWString(const GUID& guid, wchar_t* buffer, size_t bufferSize);

// Emits one log line per successful monitor arrival. Across reboots these
// lines must be byte-for-byte identical for the same virtual monitor — that
// is what preserves ICC/HDR profile associations in Windows.
static void LogMonitorIdentity(const wchar_t* where,
	const GUID& containerId, DWORD connectorIndex, DWORD osTargetId,
	const uint8_t* edidData) {
	wchar_t guidStr[64];
	GuidToWString(containerId, guidStr, 64);
	DWORD edidSerial = 0;
	WORD edidModel = 0;
	char edidSerialStr[EDID_STRING_FIELD_SIZE + 1] = {};
	char edidProdName[EDID_STRING_FIELD_SIZE + 1] = {};
	if (edidData) {
		edidModel = (WORD)edidData[EDID_OFFSET_MODEL] | ((WORD)edidData[EDID_OFFSET_MODEL + 1] << 8);
		memcpy(&edidSerial, edidData + EDID_OFFSET_SERIAL, 4);
		ExtractEdidString(edidData, EDID_OFFSET_SERIALSTR, edidSerialStr);
		ExtractEdidString(edidData, EDID_OFFSET_PRODNAME, edidProdName);
	}
	SUVDA_LOG(L"arrival[%s] container=%s connector=%lu osTargetId=%lu edidModel=0x%04X edidSerial=0x%08lX edidSerialStr=\"%S\" edidProdName=\"%S\"",
		where, guidStr, connectorIndex, osTargetId, edidModel, edidSerial,
		edidSerialStr, edidProdName);
}

// Opens the driver's "Displays" persistence subkey under the per-device PnP
// registry key. Writable by the UMDF host (LocalService) without any ACL
// gymnastics, and compliant with Universal-driver InfVerif rules (no HKLM
// AddReg in the INF).
//
// On success, returns a raw HKEY and fills *outWdfKey with the owning WDFKEY.
// The caller MUST call WdfRegistryClose(*outWdfKey) when done — this also
// closes the HKEY.
static HKEY OpenPersistenceKey(ACCESS_MASK access, bool createIfMissing, WDFKEY* outWdfKey) {
	*outWdfKey = nullptr;
	if (!g_WdfDevice) {
		SUVDA_LOG(L"OpenPersistenceKey: g_WdfDevice is null");
		return nullptr;
	}

	WDFKEY hDevKey = nullptr;
	NTSTATUS s = WdfDeviceOpenRegistryKey(g_WdfDevice, PLUGPLAY_REGKEY_DEVICE,
		KEY_ALL_ACCESS, WDF_NO_OBJECT_ATTRIBUTES, &hDevKey);
	if (!NT_SUCCESS(s)) {
		SUVDA_LOG(L"OpenPersistenceKey: WdfDeviceOpenRegistryKey failed, status=0x%08lX", s);
		return nullptr;
	}

	DECLARE_CONST_UNICODE_STRING(displaysName, L"Displays");

	WDFKEY hDisplaysKey = nullptr;
	if (createIfMissing) {
		s = WdfRegistryCreateKey(hDevKey, &displaysName, KEY_ALL_ACCESS,
			REG_OPTION_NON_VOLATILE, NULL, WDF_NO_OBJECT_ATTRIBUTES, &hDisplaysKey);
	} else {
		s = WdfRegistryOpenKey(hDevKey, &displaysName, access,
			WDF_NO_OBJECT_ATTRIBUTES, &hDisplaysKey);
	}
	WdfRegistryClose(hDevKey);
	if (!NT_SUCCESS(s)) {
		if (createIfMissing) {
			SUVDA_LOG(L"OpenPersistenceKey: create Displays subkey failed, status=0x%08lX", s);
		}
		return nullptr;
	}

	*outWdfKey = hDisplaysKey;
	return (HKEY)WdfRegistryWdmGetHandle(hDisplaysKey);
}

void LoadSettings() {
	HKEY hKey;
	DWORD bufferSize;
	LONG lResult;

	// Open the registry key
	lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SudoMaker\\SudoVDA", 0, KEY_READ, &hKey);
	if (lResult != ERROR_SUCCESS) {
		return;
	}

	// Query gpuName
	wchar_t gpuName[128];
	bufferSize = sizeof(gpuName);
	lResult = RegQueryValueExW(hKey, L"gpuName", NULL, NULL, (LPBYTE)gpuName, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		AdapterOption adapterOpt = AdapterOption();
		adapterOpt.selectGPU(gpuName);

		preferredAdapterLuid = adapterOpt.adapterLuid;
		preferredAdapterChanged = adapterOpt.hasTargetAdapter;
	}

	// Query Test mode
	DWORD _testMode;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"testMode", NULL, NULL, (LPBYTE)&_testMode, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		testMode = !!_testMode;
	}

	// Query watchdog
	DWORD _watchdogTimeout;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"watchdog", NULL, NULL, (LPBYTE)&_watchdogTimeout, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		watchdogTimeout = _watchdogTimeout;
	}

	// Query Max monitor count
	DWORD _maxMonitorCount;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"maxMonitors", NULL, NULL, (LPBYTE)&_maxMonitorCount, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		MaxVirtualMonitorCount = _maxMonitorCount;
	}

	// Query SDRBits
	DWORD _sdrBits;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"sdrBits", NULL, NULL, (LPBYTE)&_sdrBits, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		if (_sdrBits == 10) {
			SDRBITS = IDDCX_BITS_PER_COMPONENT_10;
		}
	}

	// Query HDRBits
	DWORD _hdrBits;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"hdrBits", NULL, NULL, (LPBYTE)&_hdrBits, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		if (_hdrBits == 12) {
			HDRBITS = IDDCX_BITS_PER_COMPONENT_12;
		}
	}

	// Close the registry key
	RegCloseKey(hKey);
}

#pragma region DisplayPersistence

// Persistence lives under the per-device PnP registry key's "Displays" subkey.
// That full path is approximately:
//   HKLM\SYSTEM\CurrentControlSet\Enum\Root\SudoMaker\SudoVDA\<N>\Device Parameters\Displays\{GUID}
// It is writable by the UMDF host at runtime and allowed under InfVerif's
// Universal-driver rules (no HKLM AddReg in the INF).

// Convert GUID to registry-safe wide string
void GuidToWString(const GUID& guid, wchar_t* buffer, size_t bufferSize) {
	swprintf_s(buffer, bufferSize, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		guid.Data1, guid.Data2, guid.Data3,
		guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
		guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

bool WStringToGuid(const wchar_t* str, GUID& guid) {
	unsigned int data4[8];
	int result = swscanf_s(str, L"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		&guid.Data1, &guid.Data2, &guid.Data3,
		&data4[0], &data4[1], &data4[2], &data4[3],
		&data4[4], &data4[5], &data4[6], &data4[7]);
	if (result != 11) return false;
	for (int i = 0; i < 8; i++) {
		guid.Data4[i] = static_cast<unsigned char>(data4[i]);
	}
	return true;
}

bool SaveDisplayToRegistry(const GUID& guid, DWORD width, DWORD height, DWORD vsync,
                           const char* serialStr, const char* deviceName, DWORD connectorIndex) {
	wchar_t guidStr[64];
	GuidToWString(guid, guidStr, 64);

	WDFKEY hDisplaysWdfKey = nullptr;
	HKEY hDisplaysKey = OpenPersistenceKey(KEY_ALL_ACCESS, /*createIfMissing=*/true, &hDisplaysWdfKey);
	if (!hDisplaysKey) {
		SUVDA_LOG(L"SaveDisplay: open Displays persistence key failed, guid=%s", guidStr);
		return false;
	}

	HKEY hDisplayKey = nullptr;
	LONG result = RegCreateKeyExW(hDisplaysKey, guidStr, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hDisplayKey, NULL);
	if (result != ERROR_SUCCESS) {
		SUVDA_LOG(L"SaveDisplay: create per-display key failed, err=%ld guid=%s", result, guidStr);
		WdfRegistryClose(hDisplaysWdfKey);
		return false;
	}

	LONG setResult = ERROR_SUCCESS;
	LONG r;
	r = RegSetValueExW(hDisplayKey, L"Width", 0, REG_DWORD, (LPBYTE)&width, sizeof(DWORD));
	if (r != ERROR_SUCCESS) setResult = r;
	r = RegSetValueExW(hDisplayKey, L"Height", 0, REG_DWORD, (LPBYTE)&height, sizeof(DWORD));
	if (r != ERROR_SUCCESS) setResult = r;
	r = RegSetValueExW(hDisplayKey, L"VSync", 0, REG_DWORD, (LPBYTE)&vsync, sizeof(DWORD));
	if (r != ERROR_SUCCESS) setResult = r;
	r = RegSetValueExW(hDisplayKey, L"ConnectorIndex", 0, REG_DWORD, (LPBYTE)&connectorIndex, sizeof(DWORD));
	if (r != ERROR_SUCCESS) setResult = r;

	if (serialStr && serialStr[0]) {
		wchar_t wSerialStr[14];
		MultiByteToWideChar(CP_UTF8, 0, serialStr, -1, wSerialStr, 14);
		r = RegSetValueExW(hDisplayKey, L"SerialStr", 0, REG_SZ, (LPBYTE)wSerialStr, (DWORD)(wcslen(wSerialStr) + 1) * sizeof(wchar_t));
		if (r != ERROR_SUCCESS) setResult = r;
	}
	if (deviceName && deviceName[0]) {
		wchar_t wDeviceName[14];
		MultiByteToWideChar(CP_UTF8, 0, deviceName, -1, wDeviceName, 14);
		r = RegSetValueExW(hDisplayKey, L"DeviceName", 0, REG_SZ, (LPBYTE)wDeviceName, (DWORD)(wcslen(wDeviceName) + 1) * sizeof(wchar_t));
		if (r != ERROR_SUCCESS) setResult = r;
	}

	RegCloseKey(hDisplayKey);
	WdfRegistryClose(hDisplaysWdfKey);

	if (setResult != ERROR_SUCCESS) {
		SUVDA_LOG(L"SaveDisplay: at least one RegSetValueEx failed, err=%ld guid=%s", setResult, guidStr);
		return false;
	}

	SUVDA_LOG(L"SaveDisplay: ok guid=%s mode=%ux%u@%umHz connector=%lu serial=%S name=%S",
		guidStr, width, height, vsync, connectorIndex,
		serialStr ? serialStr : "", deviceName ? deviceName : "");
	return true;
}

bool RemoveDisplayFromRegistry(const GUID& guid) {
	wchar_t guidStr[64];
	GuidToWString(guid, guidStr, 64);

	WDFKEY hDisplaysWdfKey = nullptr;
	HKEY hDisplaysKey = OpenPersistenceKey(KEY_ALL_ACCESS, /*createIfMissing=*/false, &hDisplaysWdfKey);
	if (!hDisplaysKey) {
		// Nothing persisted yet — treat as a no-op.
		return false;
	}

	LONG result = RegDeleteKeyW(hDisplaysKey, guidStr);
	WdfRegistryClose(hDisplaysWdfKey);
	if (result != ERROR_SUCCESS) {
		SUVDA_LOG(L"RemoveDisplay: delete per-display key failed, err=%ld guid=%s", result, guidStr);
		return false;
	}
	SUVDA_LOG(L"RemoveDisplay: ok guid=%s", guidStr);
	return true;
}

int LoadPersistedDisplays(IDDCX_ADAPTER adapter) {
	WDFKEY hDisplaysWdfKey = nullptr;
	HKEY hDisplaysKey = OpenPersistenceKey(KEY_READ, /*createIfMissing=*/false, &hDisplaysWdfKey);
	if (!hDisplaysKey) {
		SUVDA_LOG(L"LoadPersistedDisplays: Displays subkey not present (ok if no persisted displays yet)");
		return 0;  // No displays to load
	}

	int loaded = 0;
	wchar_t guidStr[64];
	DWORD index = 0;
	DWORD guidStrSize;

	while (true) {
		guidStrSize = 64;
		if (RegEnumKeyExW(hDisplaysKey, index++, guidStr, &guidStrSize, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
			break;
		}

		GUID guid;
		if (!WStringToGuid(guidStr, guid)) continue;

		HKEY hDisplayKey;
		if (RegOpenKeyExW(hDisplaysKey, guidStr, 0, KEY_READ, &hDisplayKey) != ERROR_SUCCESS)
			continue;

		// Read values
		DWORD width = 0, height = 0, vsync = 0, connectorIndex = MAXDWORD;
		DWORD bufferSize;

		bufferSize = sizeof(DWORD);
		RegQueryValueExW(hDisplayKey, L"Width", NULL, NULL, (LPBYTE)&width, &bufferSize);
		bufferSize = sizeof(DWORD);
		RegQueryValueExW(hDisplayKey, L"Height", NULL, NULL, (LPBYTE)&height, &bufferSize);
		bufferSize = sizeof(DWORD);
		RegQueryValueExW(hDisplayKey, L"VSync", NULL, NULL, (LPBYTE)&vsync, &bufferSize);
		bufferSize = sizeof(DWORD);
		RegQueryValueExW(hDisplayKey, L"ConnectorIndex", NULL, NULL, (LPBYTE)&connectorIndex, &bufferSize);

		wchar_t wSerialStr[14] = {};
		wchar_t wDeviceName[14] = {};
		bufferSize = sizeof(wSerialStr);
		RegQueryValueExW(hDisplayKey, L"SerialStr", NULL, NULL, (LPBYTE)wSerialStr, &bufferSize);
		bufferSize = sizeof(wDeviceName);
		RegQueryValueExW(hDisplayKey, L"DeviceName", NULL, NULL, (LPBYTE)wDeviceName, &bufferSize);

		RegCloseKey(hDisplayKey);

		// Validate loaded values
		if (width == 0 || height == 0 || vsync == 0) continue;

		// Convert wide strings to char
		char serialStr[14] = {};
		char deviceName[14] = {};
		WideCharToMultiByte(CP_UTF8, 0, wSerialStr, -1, serialStr, 14, NULL, NULL);
		WideCharToMultiByte(CP_UTF8, 0, wDeviceName, -1, deviceName, 14, NULL, NULL);

		// Create context in disconnected state
		std::lock_guard<std::recursive_mutex> lg(monitorListOp);

		auto* ctx = new IndirectMonitorContext();  // Parameterless constructor
		ctx->monitorGuid = guid;
		ctx->preferredMode = {width, height, vsync};
		ctx->isConnected = false;
		strncpy(ctx->serialStr, serialStr, 13);
		strncpy(ctx->deviceName, deviceName, 13);
		ctx->pEdidData = generate_edid(guid.Data1, guid.Data2, serialStr, deviceName);
		ctx->m_Adapter = adapter;

		// Restore persisted connector index and reserve it from the free pool
		if (connectorIndex < MaxVirtualMonitorCount) {
			ctx->connectorId = connectorIndex;
			freeConnectorSlots.erase(connectorIndex);  // Reserve this slot
		}

		monitorCtxList.emplace_back(ctx);
		loaded++;

		SUVDA_LOG(L"LoadPersistedDisplays: restored guid=%s mode=%ux%u@%umHz connector=%lu serial=%S name=%S",
			guidStr, width, height, vsync, connectorIndex,
			serialStr, deviceName);
	}

	WdfRegistryClose(hDisplaysWdfKey);
	SUVDA_LOG(L"LoadPersistedDisplays: %d display(s) restored in disconnected state", loaded);
	return loaded;
}

#pragma endregion

void DisconnectAllMonitors(bool deleteContexts = false) {
	std::lock_guard<std::recursive_mutex> lg(monitorListOp);

	if (monitorCtxList.empty()) {
		return;
	}

	for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ) {
		auto* ctx = *it;

		// Disconnect if currently connected
		if (ctx->isConnected) {
			// Only return slot to pool if we're deleting contexts
			// (matches IOCTL_DISCONNECT behavior which preserves slots).
			// Connected contexts always have a valid slot, but guard anyway
			// against the MAXUINT sentinel for defense-in-depth.
			if (deleteContexts && ctx->connectorId < MaxVirtualMonitorCount) {
				freeConnectorSlots.insert(ctx->connectorId);
			}
			IddCxMonitorDeparture(ctx->GetMonitor());
			ctx->isConnected = false;
			ctx->SetMonitor(nullptr);
		}

		if (deleteContexts) {
			delete ctx;
			it = monitorCtxList.erase(it);
		} else {
			++it;  // Keep context in list for GUID matching
		}
	}
}

void RunWatchdog() {
	if (watchdogTimeout) {
		watchdogCountdown = watchdogTimeout;
		watchdogThread = std::thread([]{
			for (;;) {
				if (watchdogTimeout) {
					Sleep(1000);

					if (!watchdogCountdown || monitorCtxList.empty()) {
						continue;
					}

					watchdogCountdown -= 1;

					if (!watchdogCountdown) {
						DisconnectAllMonitors(false);  // Keep contexts for GUID matching
					}
				} else {
					DisconnectAllMonitors(false);  // Keep contexts for GUID matching
					return;
				}
			}
		});
	}
}

void SetHighPriority() {
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT  pDriverObject,
	PUNICODE_STRING pRegistryPath
)
{
	LoadSettings();

	WDF_DRIVER_CONFIG Config;
	NTSTATUS Status;

	WDF_OBJECT_ATTRIBUTES Attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

	WDF_DRIVER_CONFIG_INIT(&Config,
		SudoVDADeviceAdd
	);

	Config.EvtDriverUnload = SudoVDADriverUnload;

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	RunWatchdog();

	SetHighPriority();

	return Status;
}

_Use_decl_annotations_
void SudoVDADriverUnload(_In_ WDFDRIVER) {
	if (watchdogTimeout > 0) {
		watchdogTimeout = 0;
		watchdogThread.join();
	}
	DisconnectAllMonitors(true);  // Delete everything - driver is unloading
}

VOID SudoVDAIoDeviceControl(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
);

_Use_decl_annotations_
NTSTATUS SudoVDADeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

	UNREFERENCED_PARAMETER(Driver);

	// Register for power callbacks - in this sample only power-on is needed
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = SudoVDADeviceD0Entry;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	// If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
	// redirects IoDeviceControl requests to an internal queue.
	IddConfig.EvtIddCxDeviceIoControl = SudoVDAIoDeviceControl;

	IddConfig.EvtIddCxAdapterInitFinished = SudoVDAAdapterInitFinished;

	IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = SudoVDAMonitorGetDefaultModes;
	IddConfig.EvtIddCxMonitorAssignSwapChain = SudoVDAMonitorAssignSwapChain;
	IddConfig.EvtIddCxMonitorUnassignSwapChain = SudoVDAMonitorUnassignSwapChain;

	if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))
	{
		isHDRSupported = true;
		IddConfig.EvtIddCxAdapterQueryTargetInfo = SudoVDAAdapterQueryTargetInfo;
		IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = SudoVDAMonitorSetDefaultHdrMetadata;
		IddConfig.EvtIddCxParseMonitorDescription2 = SudoVDAParseMonitorDescription2;
		IddConfig.EvtIddCxMonitorQueryTargetModes2 = SudoVDAMonitorQueryModes2;
		IddConfig.EvtIddCxAdapterCommitModes2 = SudoVDAAdapterCommitModes2;
		IddConfig.EvtIddCxMonitorSetGammaRamp = SudoVDAMonitorSetGammaRamp;
	} else {
		IddConfig.EvtIddCxParseMonitorDescription = SudoVDAParseMonitorDescription;
		IddConfig.EvtIddCxMonitorQueryTargetModes = SudoVDAMonitorQueryModes;
		IddConfig.EvtIddCxAdapterCommitModes = SudoVDAAdapterCommitModes;
	}

	Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		// Automatically cleanup the context when the WDF object is about to be deleted
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
		if (pContext)
		{
			pContext->Cleanup();
		}
	};

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}
	g_WdfDevice = Device;

	Status = WdfDeviceCreateDeviceInterface(
		Device,
		&SUVDA_INTERFACE_GUID,
		NULL
	);

	if (!NT_SUCCESS(Status)) {
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);

	// Create a new device context object and attach it to the WDF device object
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);

	return Status;
}

_Use_decl_annotations_
NTSTATUS SudoVDADeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	UNREFERENCED_PARAMETER(PreviousState);

	// This function is called by WDF to start the device in the fully-on power state.

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext->InitAdapter();

	return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
	AdapterLuid = {};
}

HRESULT Direct3DDevice::Init()
{
	// The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
	// created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		return hr;
	}

	// Find the specified render adapter
	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		return hr;
	}

	// Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
	if (FAILED(hr))
	{
		// If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
		// system is in a transient state.
		return hr;
	}

	return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
	// Alert the swap-chain processing thread to terminate
	SetEvent(m_hTerminateEvent.Get());

	if (m_hThread.Get())
	{
		// Wait for the thread to terminate
		WaitForSingleObject(m_hThread.Get(), INFINITE);
	}
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &AvTask);

	RunCore();

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	WdfObjectDelete((WDFOBJECT)m_hSwapChain);
	m_hSwapChain = nullptr;

	AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	if (FAILED(hr))
	{
		return;
	}

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		IDXGIResource* pSurface;

		if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
			IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
			BufferInArgs.Size = sizeof(BufferInArgs);
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		else
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}

		// Ask for the next buffer from the producer
		// IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
		// hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles [] =
			{
				m_hAvailableBufferEvent,
				m_hTerminateEvent.Get()
			};
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
			if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
			{
				// We have a new buffer, so try the AcquireBuffer again
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				hr = HRESULT_FROM_WIN32(WaitResult);
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			// We have new frame to process, the surface has a reference on it that the driver has to release
			AcquiredBuffer.Attach(pSurface);

			// ==============================
			// TODO: Process the frame here
			//
			// This is the most performance-critical section of code in an IddCx driver. It's important that whatever
			// is done with the acquired surface be finished as quickly as possible. This operation could be:
			//  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
			//  * a GPU encode operation
			//  * a GPU VPBlt to another surface
			//  * a GPU custom compute shader encode operation
			// ==============================

			// We have finished processing this frame hence we release the reference on it.
			// If the driver forgets to release the reference to the surface, it will be leaked which results in the
			// surfaces being left around after swapchain is destroyed.
			// NOTE: Although in this sample we release reference to the surface here; the driver still
			// owns the Buffer.MetaData.pSurface surface until IddCxSwapChainReleaseAndAcquireBuffer returns
			// S_OK and gives us a new frame, a driver may want to use the surface in future to re-encode the desktop
			// for better quality if there is no new frame for a while
			AcquiredBuffer.Reset();

			// Indicate to OS that we have finished inital processing of the frame, it is a hint that
			// OS could start preparing another frame
			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}

			// ==============================
			// TODO: Report frame statistics once the asynchronous encode/send work is completed
			//
			// Drivers should report information about sub-frame timings, like encode time, send time, etc.
			// ==============================
			// IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
		}
		else
		{
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
	m_WdfDevice(WdfDevice)
{
	m_Adapter = {};
	for (size_t i = 0; i < MaxVirtualMonitorCount; i++) {
		freeConnectorSlots.insert(i);
	}
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
	// ==============================
	// TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
	// numbers are used for telemetry and may be displayed to the user in some situations.
	//
	// This is also where static per-adapter capabilities are determined.
	// ==============================

	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);

	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
	}

	// Declare basic feature support for the adapter (required)
	AdapterCaps.MaxMonitorsSupported = MaxVirtualMonitorCount;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	// Declare your device strings for telemetry (required)
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"SudoMaker Virtual Display Adapter";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"SudoMaker";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"SudoVDA";

	// Declare your hardware and firmware versions (required)
	IDDCX_ENDPOINT_VERSION Version = {};
	Version.Size = sizeof(Version);
	Version.MajorVer = 1;
	AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
	AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

	// Initialize a WDF context that can store a pointer to the device context object
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	// Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	if (NT_SUCCESS(Status))
	{
		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;

		// Store the device context object into the WDF object context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		pContext->pContext = this;
	}
}

void IndirectDeviceContext::SetRenderAdapter(const LUID& AdapterLuid) {
	IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{AdapterLuid};
	IddCxAdapterSetRenderAdapter(m_Adapter, &inArgs);
}

NTSTATUS IndirectDeviceContext::CreateMonitor(IndirectMonitorContext*& pMonitorContext, uint8_t* edidData, const GUID& containerId, const VirtualMonitorMode& preferredMode) {
	// ==============================
	// TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDIDs
	// provided here are purely for demonstration.
	// Monitor manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS
	// to optimize settings like viewing distance and scale factor. Manufacturers should also use a unique serial
	// number every single device to ensure the OS can tell the monitors apart.
	// ==============================

	if (freeConnectorSlots.empty()) {
		// Avoid dereferencing begin() on an empty set. Callers in the IOCTL_ADD
		// path pre-check this, but _TestCreateMonitor does not — and users can
		// set MaxVirtualMonitorCount arbitrarily low.
		pMonitorContext = nullptr;
		free(edidData);
		return STATUS_TOO_MANY_NODES;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
	Attr.EvtCleanupCallback = MonitorCleanupCallback;

	// In the sample driver, we report a monitor right away but a real driver would do this when a monitor connection event occurs
	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = (UINT)*freeConnectorSlots.begin();

	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	MonitorInfo.MonitorDescription.DataSize = sizeof(edid_base);
	MonitorInfo.MonitorDescription.pData = edidData;
	MonitorInfo.MonitorContainerId = containerId;

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	// Create a monitor object with the specified monitor descriptor
	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (NT_SUCCESS(Status))
	{
		freeConnectorSlots.erase(freeConnectorSlots.begin());
		// Create a new monitor context object and attach it to the Idd monitor object
		auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
		pMonitorContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);
		pMonitorContextWrapper->pContext = pMonitorContext;

		pMonitorContext->monitorGuid = containerId;
		pMonitorContext->connectorId = MonitorInfo.ConnectorIndex;
		pMonitorContext->pEdidData = edidData;
		pMonitorContext->preferredMode = preferredMode;
		pMonitorContext->m_Adapter = m_Adapter;

		// Tell the OS that the monitor has been plugged in
		IDARG_OUT_MONITORARRIVAL ArrivalOut;
		Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
		if (NT_SUCCESS(Status)) {
			pMonitorContext->adapterLuid = ArrivalOut.OsAdapterLuid;
			pMonitorContext->targetId = ArrivalOut.OsTargetId;
			pMonitorContext->isConnected = true;

			// Add to list ONLY after arrival succeeded - context is now fully valid
			monitorCtxList.emplace_back(pMonitorContext);

			LogMonitorIdentity(L"fresh", pMonitorContext->monitorGuid,
				(DWORD)pMonitorContext->connectorId, pMonitorContext->targetId,
				pMonitorContext->pEdidData);
		} else {
			// Arrival failed - clean up to avoid leak and invalid state
			delete pMonitorContext;
			pMonitorContext = nullptr;
		}
	} else {
		// Avoid memory leak
		free(edidData);
	}

	return Status;
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) :
	m_Monitor(Monitor)
{
	// Don't add to monitorCtxList here - caller will add after full initialization
	// This prevents race conditions where Windows callbacks access partially initialized contexts
}

IndirectMonitorContext::IndirectMonitorContext() :
	m_Monitor(nullptr)
{
	// Don't add to monitorCtxList - caller will do it for persisted displays
}

IndirectMonitorContext::~IndirectMonitorContext()
{
	m_ProcessingThread.reset();
	if (pEdidData && pEdidData != edid_base) {
		free(pEdidData);
	}
}

IDDCX_MONITOR IndirectMonitorContext::GetMonitor() const {
	return m_Monitor;
}

void IndirectMonitorContext::AssignSwapChain(const IDDCX_MONITOR& MonitorObject, const IDDCX_SWAPCHAIN& SwapChain, const LUID& RenderAdapter, const HANDLE& NewFrameEvent)
{
	m_ProcessingThread.reset();

	auto Device = make_shared<Direct3DDevice>(RenderAdapter);
	if (FAILED(Device->Init()))
	{
		// It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
		// swap-chain and try again.
		WdfObjectDelete(SwapChain);
	}
	else
	{
		// Create a new swap-chain processing thread
		m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));

		//create an event to get notified new cursor data
		HANDLE mouseEvent = CreateEventA(
			nullptr, //TODO set proper SECURITY_ATTRIBUTES
			false,
			false,
			"arbitraryMouseEventName"
		);

		if (!mouseEvent)
		{
			//do error handling
			return;
		}

		//set up cursor capabilities
		IDDCX_CURSOR_CAPS cursorInfo = {};
		cursorInfo.Size = sizeof(cursorInfo);
		cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL; //TODO play around with XOR cursors
		cursorInfo.AlphaCursorSupport = true;
		cursorInfo.MaxX = 64; //TODO figure out correct maximum value
		cursorInfo.MaxY = 64; //TODO figure out correct maximum value

		//prepare IddCxMonitorSetupHardwareCursor arguments
		IDARG_IN_SETUP_HWCURSOR hwCursor = {};
		hwCursor.CursorInfo = cursorInfo;
		hwCursor.hNewCursorDataAvailable = mouseEvent; //this event will be called when new cursor data is available

		NTSTATUS Status = IddCxMonitorSetupHardwareCursor(
			MonitorObject, //handle to the monitor we want to enable hardware mouse on
			&hwCursor
		);

		if (FAILED(Status))
		{
			//do error handling
		}
	}
}

void IndirectMonitorContext::UnassignSwapChain()
{
	// Stop processing the last swap-chain
	m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

void IndirectDeviceContext::_TestCreateMonitor() {
	auto connectorIndex = *freeConnectorSlots.begin();
	std::string idx = std::to_string(connectorIndex);
	std::string serialStr = "VDD2408";
	serialStr += idx;
	std::string dispName = "SudoVDD #";
	dispName += idx;
	GUID containerId;
	CoCreateGuid(&containerId);
	uint8_t* edidData = generate_edid(containerId.Data1, containerId.Data2, serialStr.c_str(), dispName.c_str());

	VirtualMonitorMode mode{3000 + (DWORD)connectorIndex * 2, 2120 + (DWORD)connectorIndex, 120 + (DWORD)connectorIndex};

	IndirectMonitorContext* pContext;
	CreateMonitor(pContext, edidData, containerId, mode);
}

_Use_decl_annotations_
NTSTATUS SudoVDAAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
	// UNREFERENCED_PARAMETER(AdapterObject);
	// UNREFERENCED_PARAMETER(pInArgs);

	if (NT_SUCCESS(pInArgs->AdapterInitStatus)) {
		if (preferredAdapterChanged) {
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
			IddCxAdapterSetRenderAdapter(AdapterObject, &inArgs);
			preferredAdapterChanged = false;
		}

		// Load persisted displays (in disconnected state) for cross-reboot recovery
		LoadPersistedDisplays(AdapterObject);
	}

	if (testMode) {
		auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
		if (NT_SUCCESS(pInArgs->AdapterInitStatus))
		{
			for (size_t i = 0; i < 3; i++) {
				pDeviceContextWrapper->pContext->_TestCreateMonitor();
			}
		}
	}

	// return STATUS_SUCCESS;
	return pInArgs->AdapterInitStatus;
}

_Use_decl_annotations_
NTSTATUS SudoVDAAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	// For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

	// ==============================
	// TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
	// through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
	// should be turned off).
	// ==============================

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAAdapterCommitModes2(
	IDDCX_ADAPTER AdapterObject,
	const IDARG_IN_COMMITMODES2* pInArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	if (pInArgs->MonitorDescription.DataSize != sizeof(edid_base))
		return STATUS_INVALID_PARAMETER;

	pOutArgs->MonitorModeBufferOutputCount = std::size(s_DefaultModes);

	VirtualMonitorMode* pPreferredMode = nullptr;

	for (auto &it: monitorCtxList) {
		if (it->pEdidData && memcmp(pInArgs->MonitorDescription.pData, it->pEdidData, sizeof(edid_base)) == 0) {
			if (it->preferredMode.Width) {
				// We're adding 10 different modes, 1 original and 4 scaled x doubled refresh rate
				pOutArgs->MonitorModeBufferOutputCount += std::size(mode_scale_factors) * 2;
				pPreferredMode = &it->preferredMode;
			}
			break;
		}
	}

	if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		float vsyncMultiplier = 1;

		if (pPreferredMode && pPreferredMode->VSync) {
			float fVSync = (float)pPreferredMode->VSync / 1000;
			vsyncMultiplier = fVSync / round(fVSync);
			if (vsyncMultiplier > 1) {
				vsyncMultiplier = 1;
			}
		}

		for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
			auto vsyncTarget = s_DefaultModes[ModeIndex].VSync;
			if (vsyncMultiplier != 1 && !(vsyncTarget % 1000)) {
				vsyncTarget = (DWORD)(vsyncTarget * vsyncMultiplier);
			}
			pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
				s_DefaultModes[ModeIndex].Width,
				s_DefaultModes[ModeIndex].Height,
				vsyncTarget,
				IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
			);
		}

		if (pPreferredMode && pPreferredMode->Width) {
			auto width = pPreferredMode->Width;
			auto height = pPreferredMode->Height;
			auto vsync = pPreferredMode->VSync;

			for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
				auto scalc_factor = mode_scale_factors[idx];
				auto _width = width * scalc_factor / 100;
				auto _height = height * scalc_factor / 100;

				pInArgs->pMonitorModes[std::size(s_DefaultModes) + idx * 2] = CreateIddCxMonitorMode(
					_width,
					_height,
					vsync,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
				);

				pInArgs->pMonitorModes[std::size(s_DefaultModes) + idx * 2 + 1] = CreateIddCxMonitorMode(
					_width,
					_height,
					vsync * 2,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
				);
			}

			pOutArgs->PreferredMonitorModeIdx = std::size(s_DefaultModes);
		} else {
			pOutArgs->PreferredMonitorModeIdx = 1;
		}

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS SudoVDAParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs
)
{
	if (pInArgs->MonitorDescription.DataSize != sizeof(edid_base))
		return STATUS_INVALID_PARAMETER;

	pOutArgs->MonitorModeBufferOutputCount = std::size(s_DefaultModes);

	VirtualMonitorMode* pPreferredMode = nullptr;

	for (auto &it: monitorCtxList) {
		if (it->pEdidData && memcmp(pInArgs->MonitorDescription.pData, it->pEdidData, sizeof(edid_base)) == 0) {
			if (it->preferredMode.Width) {
				// We're adding 10 different modes, 1 original and 4 scaled x doubled refresh rate
				pOutArgs->MonitorModeBufferOutputCount += std::size(mode_scale_factors) * 2;
				pPreferredMode = &it->preferredMode;
			}
			break;
		}
	}

	if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		float vsyncMultiplier = 1;

		if (pPreferredMode && pPreferredMode->VSync) {
			float fVSync = (float)pPreferredMode->VSync / 1000;
			vsyncMultiplier = fVSync / round(fVSync);
			if (vsyncMultiplier > 1) {
				vsyncMultiplier = 1;
			}
		}

		for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
			auto vsyncTarget = s_DefaultModes[ModeIndex].VSync;
			if (vsyncMultiplier != 1 && !(vsyncTarget % 1000)) {
				vsyncTarget = (DWORD)(vsyncTarget * vsyncMultiplier);
			}
			pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode2(
				s_DefaultModes[ModeIndex].Width,
				s_DefaultModes[ModeIndex].Height,
				vsyncTarget,
				IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
			);
		}

		if (pPreferredMode && pPreferredMode->Width) {
			auto width = pPreferredMode->Width;
			auto height = pPreferredMode->Height;
			auto vsync = pPreferredMode->VSync;

			for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
				auto scalc_factor = mode_scale_factors[idx];
				auto _width = width * scalc_factor / 100;
				auto _height = height * scalc_factor / 100;

				pInArgs->pMonitorModes[std::size(s_DefaultModes) + idx * 2] = CreateIddCxMonitorMode2(
					_width,
					_height,
					vsync,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
				);

				pInArgs->pMonitorModes[std::size(s_DefaultModes) + idx * 2 + 1] = CreateIddCxMonitorMode2(
					_width,
					_height,
					vsync,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
				);
			}

			pOutArgs->PreferredMonitorModeIdx = std::size(s_DefaultModes);
		} else {
			pOutArgs->PreferredMonitorModeIdx = 1;
		}

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS SudoVDAMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for a monitor with no EDID.
	// Drivers should report modes that are guaranteed to be supported by the transport protocol and by nearly all
	// monitors (such 640x480, 800x600, or 1024x768). If the driver has access to monitor modes from a descriptor other
	// than an EDID, those modes would also be reported here.
	// ==============================

	UNREFERENCED_PARAMETER(MonitorObject);

	pOutArgs->DefaultMonitorModeBufferOutputCount = std::size(s_DefaultModes);
	pOutArgs->PreferredMonitorModeIdx = 1;

	if (pInArgs->DefaultMonitorModeBufferInputCount == 0) {
		return STATUS_SUCCESS;
	} else if (pInArgs->DefaultMonitorModeBufferInputCount < pOutArgs->DefaultMonitorModeBufferOutputCount) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
		pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
			s_DefaultModes[ModeIndex].Width,
			s_DefaultModes[ModeIndex].Height,
			s_DefaultModes[ModeIndex].VSync,
			IDDCX_MONITOR_MODE_ORIGIN_DRIVER
		);
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);

	pOutArgs->TargetModeBufferOutputCount = (UINT) std::size(s_DefaultModes);
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	if (pMonitorContextWrapper->pContext->preferredMode.Width) {
		pOutArgs->TargetModeBufferOutputCount += std::size(mode_scale_factors) * 2;
	}

	if (pInArgs->TargetModeBufferInputCount >= pOutArgs->TargetModeBufferOutputCount) {
		vector<IDDCX_TARGET_MODE> TargetModes;

		auto width = pMonitorContextWrapper->pContext->preferredMode.Width;
		auto height = pMonitorContextWrapper->pContext->preferredMode.Height;
		auto vsync = pMonitorContextWrapper->pContext->preferredMode.VSync;

		float vsyncMultiplier = 1;

		{
			float fVSync = (float)vsync / 1000;
			vsyncMultiplier = fVSync / round(fVSync);
			if (vsyncMultiplier > 1) {
				vsyncMultiplier = 1;
			}
		}

		// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
		// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
		// report the available set of modes for a given output as the intersection of monitor modes with target modes.

		for (size_t i = 0; i < std::size(s_DefaultModes); i++) {
			auto vsyncTarget = s_DefaultModes[i].VSync;
			if (vsyncMultiplier != 1 && !(vsyncTarget % 1000)) {
				vsyncTarget = (DWORD)(vsyncTarget * vsyncMultiplier);
			}
			TargetModes.push_back(CreateIddCxTargetMode(
				s_DefaultModes[i].Width,
				s_DefaultModes[i].Height,
				vsyncTarget
			));
		}

		for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
			auto scalc_factor = mode_scale_factors[idx];
			auto _width = width * scalc_factor / 100;
			auto _height = height * scalc_factor / 100;

			TargetModes.push_back(CreateIddCxTargetMode(
				_width,
				_height,
				vsync
			));

			TargetModes.push_back(CreateIddCxTargetMode(
				_width,
				_height,
				vsync * 2
			));
		}

		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	} else if (pInArgs->TargetModeBufferInputCount != 0) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAMonitorQueryModes2(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES2* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);

	pOutArgs->TargetModeBufferOutputCount = (UINT) std::size(s_DefaultModes);

	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);

	if (pMonitorContextWrapper->pContext->preferredMode.Width) {
		pOutArgs->TargetModeBufferOutputCount += std::size(mode_scale_factors) * 2;
	}

	if (pInArgs->TargetModeBufferInputCount >= pOutArgs->TargetModeBufferOutputCount) {
		vector<IDDCX_TARGET_MODE2> TargetModes;

		auto width = pMonitorContextWrapper->pContext->preferredMode.Width;
		auto height = pMonitorContextWrapper->pContext->preferredMode.Height;
		auto vsync = pMonitorContextWrapper->pContext->preferredMode.VSync;

		float vsyncMultiplier = 1;

		{
			float fVSync = (float)vsync / 1000;
			vsyncMultiplier = fVSync / round(fVSync);
			if (vsyncMultiplier > 1) {
				vsyncMultiplier = 1;
			}
		}

		for (size_t i = 0; i < std::size(s_DefaultModes); i++) {
			auto vsyncTarget = s_DefaultModes[i].VSync;
			if (vsyncMultiplier != 1 && !(vsyncTarget % 1000)) {
				vsyncTarget = (DWORD)(vsyncTarget * vsyncMultiplier);
			}
			TargetModes.push_back(CreateIddCxTargetMode2(
				s_DefaultModes[i].Width,
				s_DefaultModes[i].Height,
				vsyncTarget
			));
		}

		for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
			auto scalc_factor = mode_scale_factors[idx];
			auto _width = width * scalc_factor / 100;
			auto _height = height * scalc_factor / 100;

			TargetModes.push_back(CreateIddCxTargetMode2(
				_width,
				_height,
				vsync
			));

			TargetModes.push_back(CreateIddCxTargetMode2(
				_width,
				_height,
				vsync * 2
			));
		}

		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	} else if (pInArgs->TargetModeBufferInputCount != 0) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);

	if (preferredAdapterChanged) {
		if (memcmp(&pInArgs->RenderAdapterLuid, &preferredAdapterLuid, sizeof(LUID))) {
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
			IddCxAdapterSetRenderAdapter(pMonitorContextWrapper->pContext->m_Adapter, &inArgs);
			return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
		}
		preferredAdapterChanged = false;
	}

	pMonitorContextWrapper->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	pMonitorContextWrapper->pContext->UnassignSwapChain();
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAAdapterQueryTargetInfo(
	IDDCX_ADAPTER AdapterObject,
	IDARG_IN_QUERYTARGET_INFO* pInArgs,
	IDARG_OUT_QUERYTARGET_INFO* pOutArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);
	pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;
	pOutArgs->DitheringSupport.Rgb = HDRBITS;

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAMonitorSetDefaultHdrMetadata(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SudoVDAMonitorSetGammaRamp(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_SET_GAMMARAMP* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

#pragma endregion

VOID SudoVDAIoDeviceControl(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
)
{
	// Reset watchdog
	if (IoControlCode != IOCTL_GET_WATCHDOG) {
		watchdogCountdown = watchdogTimeout;
	}

	NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytesReturned = 0;

	switch (IoControlCode) {
	case IOCTL_ADD_VIRTUAL_DISPLAY: {
		if (InputBufferLength < sizeof(VIRTUAL_DISPLAY_ADD_PARAMS) || OutputBufferLength < sizeof(VIRTUAL_DISPLAY_ADD_OUT)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PVIRTUAL_DISPLAY_ADD_PARAMS params;
		PVIRTUAL_DISPLAY_ADD_OUT output;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(VIRTUAL_DISPLAY_ADD_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VIRTUAL_DISPLAY_ADD_OUT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		IndirectMonitorContext* existingCtx = nullptr;

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				existingCtx = ctx;
				break;
			}
		}

		if (existingCtx) {
			if (existingCtx->isConnected) {
				// Already connected - return existing info
				output->AdapterLuid = existingCtx->adapterLuid;
				output->TargetId = existingCtx->targetId;
				bytesReturned = sizeof(VIRTUAL_DISPLAY_ADD_OUT);
				Status = STATUS_SUCCESS;
			} else {
				// Found but disconnected (persisted display) - need to reconnect
				std::lock_guard<std::recursive_mutex> lg(monitorListOp);

				IDARG_OUT_MONITORARRIVAL ArrivalOut = {};

				if (existingCtx->GetMonitor() != nullptr) {
					// Has monitor handle - try to call arrival
					Status = IddCxMonitorArrival(existingCtx->GetMonitor(), &ArrivalOut);
					if (!NT_SUCCESS(Status)) {
						// Handle is stale (Windows destroyed the object but cleanup callback hasn't run yet)
						// Invalidate the handle and fall through to create a new monitor
						existingCtx->SetMonitor(nullptr);
					}
				}

				if (existingCtx->GetMonitor() == nullptr) {
					// No monitor handle (persisted from registry or stale handle) - need to create one
					UINT connectorIndex;
					if (existingCtx->connectorId < MaxVirtualMonitorCount) {
						connectorIndex = (UINT)existingCtx->connectorId;
						freeConnectorSlots.erase(existingCtx->connectorId);
					} else {
						if (freeConnectorSlots.empty()) {
							Status = STATUS_TOO_MANY_NODES;
							break;
						}
						connectorIndex = (UINT)*freeConnectorSlots.begin();
						freeConnectorSlots.erase(freeConnectorSlots.begin());
					}

					WDF_OBJECT_ATTRIBUTES Attr;
					WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
					Attr.EvtCleanupCallback = MonitorCleanupCallback;

					IDDCX_MONITOR_INFO MonitorInfo = {};
					MonitorInfo.Size = sizeof(MonitorInfo);
					MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
					MonitorInfo.ConnectorIndex = connectorIndex;
					MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
					MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
					MonitorInfo.MonitorDescription.DataSize = sizeof(edid_base);
					MonitorInfo.MonitorDescription.pData = existingCtx->pEdidData;
					MonitorInfo.MonitorContainerId = existingCtx->monitorGuid;

					IDARG_IN_MONITORCREATE MonitorCreate = {};
					MonitorCreate.ObjectAttributes = &Attr;
					MonitorCreate.pMonitorInfo = &MonitorInfo;

					IDARG_OUT_MONITORCREATE MonitorCreateOut;
					Status = IddCxMonitorCreate(existingCtx->m_Adapter, &MonitorCreate, &MonitorCreateOut);

					if (!NT_SUCCESS(Status)) {
						freeConnectorSlots.insert(connectorIndex);
						break;
					}

					auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
					pMonitorContextWrapper->pContext = existingCtx;

					existingCtx->SetMonitor(MonitorCreateOut.MonitorObject);
					existingCtx->connectorId = connectorIndex;

					Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
				}

				if (NT_SUCCESS(Status)) {
					existingCtx->adapterLuid = ArrivalOut.OsAdapterLuid;
					existingCtx->targetId = ArrivalOut.OsTargetId;
					existingCtx->isConnected = true;

					output->AdapterLuid = existingCtx->adapterLuid;
					output->TargetId = existingCtx->targetId;
					bytesReturned = sizeof(VIRTUAL_DISPLAY_ADD_OUT);

					LogMonitorIdentity(L"add-reconnect", existingCtx->monitorGuid,
						(DWORD)existingCtx->connectorId, existingCtx->targetId,
						existingCtx->pEdidData);

					// Self-heal: re-persist current values so a manually-cleared
					// registry entry gets rebuilt on the next successful reconnect.
					SaveDisplayToRegistry(existingCtx->monitorGuid,
						existingCtx->preferredMode.Width,
						existingCtx->preferredMode.Height,
						existingCtx->preferredMode.VSync,
						existingCtx->serialStr, existingCtx->deviceName,
						(DWORD)existingCtx->connectorId);
				}
			}
			break;
		}

		// Check if we have free slots before creating a new display
		if (freeConnectorSlots.empty()) {
			Status = STATUS_TOO_MANY_NODES;
			break;
		}

		// Validate and add the virtual display
		if (params->Width > 0 && params->Height > 0 && params->RefreshRate > 0) {
			std::lock_guard<std::recursive_mutex> lg(monitorListOp);

			auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);

			IndirectMonitorContext* pMonitorContext;
			uint8_t* edidData = generate_edid(params->MonitorGuid.Data1, params->MonitorGuid.Data2, params->SerialNumber, params->DeviceName);
			VirtualMonitorMode preferredMode = {params->Width, params->Height, params->RefreshRate};
			if (preferredMode.VSync < 1000) {
				preferredMode.VSync *= 1000;
			}
			Status = pDeviceContextWrapper->pContext->CreateMonitor(pMonitorContext, edidData, params->MonitorGuid, preferredMode);

			if (!NT_SUCCESS(Status)) {
				break;
			}

			// Store EDID params in context for persistence
			strncpy(pMonitorContext->serialStr, params->SerialNumber, 13);
			strncpy(pMonitorContext->deviceName, params->DeviceName, 13);

			// Persist to registry for cross-reboot recovery
			SaveDisplayToRegistry(params->MonitorGuid,
				preferredMode.Width, preferredMode.Height, preferredMode.VSync,
				params->SerialNumber, params->DeviceName,
				(DWORD)pMonitorContext->connectorId);

			output->AdapterLuid = pMonitorContext->adapterLuid;
			output->TargetId = pMonitorContext->targetId;
			bytesReturned = sizeof(VIRTUAL_DISPLAY_ADD_OUT);
		}
		else {
			Status = STATUS_INVALID_PARAMETER;
		}

		break;
	}
	case IOCTL_REMOVE_VIRTUAL_DISPLAY: {
		if (InputBufferLength < sizeof(VIRTUAL_DISPLAY_REMOVE_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PVIRTUAL_DISPLAY_REMOVE_PARAMS params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(VIRTUAL_DISPLAY_REMOVE_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::recursive_mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				// Remove from registry (permanent deletion)
				RemoveDisplayFromRegistry(ctx->monitorGuid);

				// Only disconnect if currently connected
				if (ctx->isConnected) {
					IddCxMonitorDeparture(ctx->GetMonitor());
				}

				// Return connector slot to the pool on permanent removal
				// (This is the only place where slots should be returned).
				// Guard against leaking the MAXUINT sentinel (set on persisted
				// contexts whose ConnectorIndex was missing/invalid) into the pool.
				if (ctx->connectorId < MaxVirtualMonitorCount) {
					freeConnectorSlots.insert(ctx->connectorId);
				}

				monitorCtxList.erase(it);
				delete ctx;  // Free memory to avoid leak
				Status = STATUS_SUCCESS;
				break;
			}
		}

		break;
	}
	case IOCTL_DISCONNECT_VIRTUAL_DISPLAY: {
		if (InputBufferLength < sizeof(VIRTUAL_DISPLAY_DISCONNECT_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PVIRTUAL_DISPLAY_DISCONNECT_PARAMS params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(VIRTUAL_DISPLAY_DISCONNECT_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::recursive_mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				if (!ctx->isConnected) {
					// Already disconnected
					Status = STATUS_SUCCESS;
					break;
				}

				// Disconnect: call IddCxMonitorDeparture but keep context in memory
				// Keep the monitor handle and connector slot - we'll reuse them on reconnect
				// to preserve Windows device identity (same handle = same device)
				NTSTATUS departureStatus = IddCxMonitorDeparture(ctx->GetMonitor());
				if (NT_SUCCESS(departureStatus)) {
					ctx->isConnected = false;
					// Don't return connector slot - it's bound to this monitor's handle
					Status = STATUS_SUCCESS;
				} else {
					Status = departureStatus;
				}
				break;
			}
		}

		break;
	}
	case IOCTL_RECONNECT_VIRTUAL_DISPLAY: {
		if (InputBufferLength < sizeof(VIRTUAL_DISPLAY_RECONNECT_PARAMS) ||
			OutputBufferLength < sizeof(VIRTUAL_DISPLAY_RECONNECT_OUT)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PVIRTUAL_DISPLAY_RECONNECT_PARAMS params;
		PVIRTUAL_DISPLAY_RECONNECT_OUT output;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(VIRTUAL_DISPLAY_RECONNECT_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VIRTUAL_DISPLAY_RECONNECT_OUT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::recursive_mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				if (ctx->isConnected) {
					// Already connected - just return the existing info
					output->AdapterLuid = ctx->adapterLuid;
					output->TargetId = ctx->targetId;
					bytesReturned = sizeof(VIRTUAL_DISPLAY_RECONNECT_OUT);
					Status = STATUS_SUCCESS;
					break;
				}

				IDARG_OUT_MONITORARRIVAL ArrivalOut = {};

				if (ctx->GetMonitor() != nullptr) {
					// Runtime-disconnected display: try to reuse existing monitor handle
					// This preserves Windows device identity (same handle = same device)
					Status = IddCxMonitorArrival(ctx->GetMonitor(), &ArrivalOut);
					if (!NT_SUCCESS(Status)) {
						// Handle is stale (Windows destroyed the object but cleanup callback hasn't run yet)
						// Invalidate the handle and fall through to create a new monitor
						ctx->SetMonitor(nullptr);
					}
				}

				if (ctx->GetMonitor() == nullptr) {
					// Persisted display or stale handle: no valid handle exists, need to create one
					// Use preserved connector index if available, otherwise allocate new
					UINT connectorIndex;
					if (ctx->connectorId < MaxVirtualMonitorCount) {
						// Use the preserved connector index for display persistence
						connectorIndex = (UINT)ctx->connectorId;
						// Remove from free pool if it's there (it might have been freed on driver restart)
						freeConnectorSlots.erase(ctx->connectorId);
					} else {
						// No preserved connector, need to allocate a new one
						if (freeConnectorSlots.empty()) {
							Status = STATUS_TOO_MANY_NODES;
							break;
						}
						connectorIndex = (UINT)*freeConnectorSlots.begin();
						freeConnectorSlots.erase(freeConnectorSlots.begin());
					}

					WDF_OBJECT_ATTRIBUTES Attr;
					WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
					Attr.EvtCleanupCallback = MonitorCleanupCallback;

					IDDCX_MONITOR_INFO MonitorInfo = {};
					MonitorInfo.Size = sizeof(MonitorInfo);
					MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
					MonitorInfo.ConnectorIndex = connectorIndex;

					MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
					MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
					MonitorInfo.MonitorDescription.DataSize = sizeof(edid_base);
					MonitorInfo.MonitorDescription.pData = ctx->pEdidData;
					MonitorInfo.MonitorContainerId = ctx->monitorGuid;

					IDARG_IN_MONITORCREATE MonitorCreate = {};
					MonitorCreate.ObjectAttributes = &Attr;
					MonitorCreate.pMonitorInfo = &MonitorInfo;

					IDARG_OUT_MONITORCREATE MonitorCreateOut;
					Status = IddCxMonitorCreate(ctx->m_Adapter, &MonitorCreate, &MonitorCreateOut);

					if (!NT_SUCCESS(Status)) {
						// Return the connector slot on failure
						freeConnectorSlots.insert(connectorIndex);
						break;
					}

					// Update wrapper context to point to existing context (reuse it)
					auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
					pMonitorContextWrapper->pContext = ctx;

					// Update context with new monitor handle and connector
					ctx->SetMonitor(MonitorCreateOut.MonitorObject);
					ctx->connectorId = connectorIndex;

					// Notify OS of monitor arrival
					Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
				}

				if (NT_SUCCESS(Status)) {
					ctx->adapterLuid = ArrivalOut.OsAdapterLuid;
					ctx->targetId = ArrivalOut.OsTargetId;
					ctx->isConnected = true;

					output->AdapterLuid = ctx->adapterLuid;
					output->TargetId = ctx->targetId;
					bytesReturned = sizeof(VIRTUAL_DISPLAY_RECONNECT_OUT);

					LogMonitorIdentity(L"reconnect", ctx->monitorGuid,
						(DWORD)ctx->connectorId, ctx->targetId,
						ctx->pEdidData);

					// Self-heal: re-persist current values so a manually-cleared
					// registry entry gets rebuilt on the next successful reconnect.
					SaveDisplayToRegistry(ctx->monitorGuid,
						ctx->preferredMode.Width,
						ctx->preferredMode.Height,
						ctx->preferredMode.VSync,
						ctx->serialStr, ctx->deviceName,
						(DWORD)ctx->connectorId);
				}

				break;
			}
		}

		break;
	}
	case IOCTL_SET_RENDER_ADAPTER: {
		PVIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(VIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);

		preferredAdapterLuid = params->AdapterLuid;
		pDeviceContextWrapper->pContext->SetRenderAdapter(params->AdapterLuid);
		preferredAdapterChanged = true;

		break;
	}
	case IOCTL_GET_WATCHDOG: {
		Status = STATUS_SUCCESS;
		PVIRTUAL_DISPLAY_GET_WATCHDOG_OUT output;

		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VIRTUAL_DISPLAY_GET_WATCHDOG_OUT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		output->Timeout = watchdogTimeout;
		output->Countdown = watchdogCountdown;
		bytesReturned = sizeof(VIRTUAL_DISPLAY_GET_WATCHDOG_OUT);
		break;
	}
	case IOCTL_DRIVER_PING: {
		Status = STATUS_SUCCESS;
		break;
	}
	case IOCTL_GET_PROTOCOL_VERSION: {
		Status = STATUS_SUCCESS;
		PVIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT output;

		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		output->Version = VDAProtocolVersion;
		bytesReturned = sizeof(VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT);
		break;
	}
	default:
		break;
	}

	WdfRequestCompleteWithInformation(Request, Status, bytesReturned);
}
