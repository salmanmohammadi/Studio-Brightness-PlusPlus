//----------------  hid.cpp  ----------------
#include "hid.h"
#include "Log.h"
#define _WIN32_DCOM
#include <initguid.h>
#include <devpropdef.h>
#include <hidsdi.h>
#include <setupapi.h>
// SBPP_DEVPKEY_Device_ContainerId: {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2
DEFINE_DEVPROPKEY(SBPP_DEVPKEY_Device_ContainerId,
                  0x8c7ed206, 0x3f8a, 0x4827,
                  0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);
#include <shlwapi.h>
#include <vector>
#include <cstdint>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shlwapi.lib")

/* ---------- Apple vendor ID ---------- */
static const wchar_t kAppleVid[]     = L"vid_05ac";
// kColFilter removed: some displays (e.g. Studio Display XDR) expose
// brightness via a HID collection, not the top-level interface.

inline bool icontains(const wchar_t *hay, const wchar_t *needle) {
	return StrStrIW(hay, needle) != nullptr;
}

/* ---------- Known display profiles ---------- */
static const DisplayProfile kProfiles[] = {
	{0x1114, DisplayType::StudioDisplay,  L"Studio Display",          600.f},
	{0x1118, DisplayType::StudioDisplay2, L"Studio Display (Gen 2)",  600.f},
	{0x1116, DisplayType::StudioXDR,      L"Studio Display XDR",     1000.f},
	{0x9243, DisplayType::ProXDR,         L"Pro Display XDR",        1000.f},
};

static const DisplayProfile *findProfileByPid(uint16_t pid) {
	for (const auto &p : kProfiles)
		if (p.pid == pid)
			return &p;
	return nullptr;
}

/* ---------- Extract PID from device path ---------- */
static uint16_t extractPid(const wchar_t *path) {
	const wchar_t *p = StrStrIW(path, L"pid_");
	if (!p)
		return 0;
	return (uint16_t)wcstoul(p + 4, nullptr, 16);
}

/* ---------- Query ContainerId via SetupAPI ---------- */
static GUID queryContainerId(HDEVINFO set, PSP_DEVINFO_DATA devInfo) {
	GUID cid   = {};
	DWORD type = 0, size = 0;

	SetupDiGetDevicePropertyW(set, devInfo, &SBPP_DEVPKEY_Device_ContainerId,
	                          &type, nullptr, 0, &size, 0);
	if (size == sizeof(GUID)) {
		SetupDiGetDevicePropertyW(set, devInfo, &SBPP_DEVPKEY_Device_ContainerId,
		                          &type, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
	}
	return cid;
}

/* ============================================================ */
void DisplayDevice::close() {
	if (prep) {
		HidD_FreePreparsedData(prep);
		prep = nullptr;
	}
	if (hDev != INVALID_HANDLE_VALUE) {
		CloseHandle(hDev);
		hDev = INVALID_HANDLE_VALUE;
	}
}

int DisplayDevice::getBrightness(ULONG *val) {
	if (hDev == INVALID_HANDLE_VALUE || featCaps.len == 0)
		return -1;
	std::vector<uint8_t> buf(featCaps.len, 0);
	buf[0] = featCaps.id;
	if (!HidD_GetFeature(hDev, buf.data(), (ULONG)buf.size()))
		return -2;
	NTSTATUS s = HidP_GetUsageValue(HidP_Feature, featCaps.page, 0, featCaps.usage, val, prep,
	                                reinterpret_cast<PCHAR>(buf.data()), featCaps.len);
	return (s == HIDP_STATUS_SUCCESS) ? 0 : -3;
}

int DisplayDevice::setBrightness(ULONG v) {
	if (hDev == INVALID_HANDLE_VALUE || featCaps.len == 0)
		return -1;
	std::vector<uint8_t> buf(featCaps.len, 0);
	buf[0] = featCaps.id;
	if (!HidD_GetFeature(hDev, buf.data(), (ULONG)buf.size()))
		return -2;
	NTSTATUS s = HidP_SetUsageValue(HidP_Feature, featCaps.page, 0, featCaps.usage, v, prep,
	                                reinterpret_cast<PCHAR>(buf.data()), featCaps.len);
	if (s != HIDP_STATUS_SUCCESS)
		return -3;
	return HidD_SetFeature(hDev, buf.data(), featCaps.len) ? 0 : -4;
}

int DisplayDevice::getBrightnessRange(ULONG *mn, ULONG *mx) {
	if (!prep || hDev == INVALID_HANDLE_VALUE)
		return -1;
	// Use the stored featCaps (already resolved to the correct brightness cap)
	HIDP_CAPS caps{};
	if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS)
		return -2;
	USHORT numVals = caps.NumberFeatureValueCaps;
	if (numVals == 0) return -2;
	std::vector<HIDP_VALUE_CAPS> vcaps(numVals);
	if (HidP_GetValueCaps(HidP_Feature, vcaps.data(), &numVals, prep) != HIDP_STATUS_SUCCESS)
		return -2;
	for (USHORT i = 0; i < numVals; ++i) {
		USAGE u = vcaps[i].IsRange ? vcaps[i].Range.UsageMin : vcaps[i].NotRange.Usage;
		if (vcaps[i].UsagePage == featCaps.page && u == featCaps.usage) {
			*mn = vcaps[i].LogicalMin;
			*mx = vcaps[i].LogicalMax;
			return 0;
		}
	}
	return -2;
}

/* ---------- Candidate info collected in pass 1 (no device opened) ---------- */
struct HidCandidate {
	std::wstring     path;
	GUID             containerId = {};
	const DisplayProfile *profile = nullptr;
	uint16_t         pid        = 0;
	bool             exactMatch = false;
	// Which value-cap index matched, and its details
	UCHAR            reportId   = 0;
	USAGE            page       = 0;
	USAGE            usage      = 0;
};

/* ============================================================ */
std::vector<DisplayDevice> hid_enumerate() {
	std::vector<DisplayDevice> result;

	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);
	HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, 0,
	                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		Log::Error(L"SetupDiGetClassDevsW failed (%lu)", GetLastError());
		return result;
	}

	// --- Pass 1: scan all HID interfaces, classify candidates without opening ---
	// We avoid opening devices in this pass so that fallback interfaces don't
	// interfere with the display firmware before the correct interface is opened.
	std::vector<HidCandidate> candidates;

	SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		if (!need)
			continue;

		std::vector<BYTE> buf(need);
		auto det       = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
		det->cbSize    = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

		SP_DEVINFO_DATA devInfo{sizeof(devInfo)};
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &devInfo))
			continue;

		const wchar_t *path = det->DevicePath;

		// Filter: Apple VID only
		if (!icontains(path, kAppleVid))
			continue;

		uint16_t pid = extractPid(path);
		const DisplayProfile *profile = findProfileByPid(pid);

		if (profile) {
			Log::Info(L"Found %s (PID 0x%04X): %s", profile->name, pid, path);
		} else {
			Log::Info(L"Found unknown Apple HID (PID 0x%04X): %s", pid, path);
		}

		// Open device temporarily to read HID caps
		HANDLE hDev = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                          OPEN_EXISTING, 0, nullptr);
		if (hDev == INVALID_HANDLE_VALUE) {
			Log::Warn(L"  CreateFile failed (%lu), skipping", GetLastError());
			continue;
		}

		PHIDP_PREPARSED_DATA prep = nullptr;
		if (!HidD_GetPreparsedData(hDev, &prep)) {
			Log::Warn(L"  GetPreparsedData failed, skipping");
			CloseHandle(hDev);
			continue;
		}

		HIDP_CAPS caps{};
		if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) {
			Log::Warn(L"  GetCaps failed, skipping");
			HidD_FreePreparsedData(prep);
			CloseHandle(hDev);
			continue;
		}

		USHORT numFeatVals = caps.NumberFeatureValueCaps;
		if (numFeatVals == 0) {
			Log::Info(L"  No Feature value caps, skipping");
			HidD_FreePreparsedData(prep);
			CloseHandle(hDev);
			continue;
		}
		std::vector<HIDP_VALUE_CAPS> vcaps(numFeatVals);
		NTSTATUS vcStatus = HidP_GetValueCaps(HidP_Feature, vcaps.data(), &numFeatVals, prep);
		if (vcStatus != HIDP_STATUS_SUCCESS) {
			Log::Warn(L"  HidP_GetValueCaps failed (0x%08X), skipping", (unsigned)vcStatus);
			HidD_FreePreparsedData(prep);
			CloseHandle(hDev);
			continue;
		}

		// Log all Feature value caps for diagnostics
		for (USHORT vi = 0; vi < numFeatVals; ++vi) {
			auto &vc = vcaps[vi];
			USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
			Log::Info(L"  ValueCap[%u]: Page=0x%04X Usage=0x%04X ReportID=0x%02X BitSize=%u ReportCount=%u LogMin=%ld LogMax=%ld",
			          vi, vc.UsagePage, u, vc.ReportID, vc.BitSize, vc.ReportCount,
			          vc.LogicalMin, vc.LogicalMax);
		}

		// Search for brightness cap
		int brightIdx = -1;
		int fallbackIdx = -1;
		for (USHORT vi = 0; vi < numFeatVals; ++vi) {
			auto &vc = vcaps[vi];
			USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
			if (vc.UsagePage == 0x0082 && u == 0x0010) {
				brightIdx = vi;
				break;
			}
			if (fallbackIdx < 0 && vc.ReportCount == 1 && vc.LogicalMax >= 400)
				fallbackIdx = vi;
		}
		int chosen = (brightIdx >= 0) ? brightIdx : fallbackIdx;

		// Close the device — we'll reopen only the best candidate per display
		HidD_FreePreparsedData(prep);
		CloseHandle(hDev);

		if (chosen < 0) {
			Log::Warn(L"  No brightness value cap found among %u caps, skipping", numFeatVals);
			continue;
		}

		bool isExact = (brightIdx >= 0);
		if (isExact) {
			Log::Info(L"  Matched brightness cap by UsagePage/Usage (index %d)", chosen);
		} else {
			Log::Info(L"  Fallback candidate (index %d), deferring", chosen);
		}

		auto &bc = vcaps[chosen];
		HidCandidate cand;
		cand.path        = path;
		cand.containerId = queryContainerId(set, &devInfo);
		cand.profile     = profile;
		cand.pid         = pid;
		cand.exactMatch  = isExact;
		cand.reportId    = bc.ReportID;
		cand.page        = bc.UsagePage;
		cand.usage       = bc.IsRange ? bc.Range.UsageMin : bc.NotRange.Usage;
		candidates.push_back(std::move(cand));
	}

	SetupDiDestroyDeviceInfoList(set);

	// --- Pass 2: for each physical display, pick best candidate and open it ---
	// Prefer exact matches over fallbacks for the same ContainerId.
	static const GUID emptyGuid = {};
	std::vector<size_t> chosen; // indices into candidates

	for (size_t ci = 0; ci < candidates.size(); ++ci) {
		auto &cand = candidates[ci];
		bool dominated = false;

		// Check if we already selected a candidate for this ContainerId
		if (memcmp(&cand.containerId, &emptyGuid, sizeof(GUID)) != 0) {
			for (size_t si = 0; si < chosen.size(); ++si) {
				auto &existing = candidates[chosen[si]];
				if (memcmp(&existing.containerId, &cand.containerId, sizeof(GUID)) == 0) {
					// Same physical display — keep the better one
					if (cand.exactMatch && !existing.exactMatch) {
						Log::Info(L"  Preferring exact match over fallback for %s",
						          cand.path.c_str());
						chosen[si] = ci;
					} else {
						Log::Info(L"  Duplicate ContainerId, skipping %s",
						          cand.path.c_str());
					}
					dominated = true;
					break;
				}
			}
		}
		if (!dominated)
			chosen.push_back(ci);
	}

	// Now open only the selected candidates
	for (size_t idx : chosen) {
		auto &cand = candidates[idx];

		DisplayDevice dev;
		dev.devicePath = cand.path;
		dev.hDev = CreateFileW(cand.path.c_str(), GENERIC_READ | GENERIC_WRITE,
		                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                       OPEN_EXISTING, 0, nullptr);
		if (dev.hDev == INVALID_HANDLE_VALUE) {
			Log::Warn(L"  CreateFile failed on reopen (%lu), skipping", GetLastError());
			continue;
		}

		if (!HidD_GetPreparsedData(dev.hDev, &dev.prep)) {
			Log::Warn(L"  GetPreparsedData failed on reopen, skipping");
			dev.close();
			continue;
		}

		HIDP_CAPS caps{};
		if (HidP_GetCaps(dev.prep, &caps) != HIDP_STATUS_SUCCESS) {
			Log::Warn(L"  GetCaps failed on reopen, skipping");
			dev.close();
			continue;
		}

		dev.featCaps.len   = caps.FeatureReportByteLength;
		dev.featCaps.id    = cand.reportId;
		dev.featCaps.page  = cand.page;
		dev.featCaps.usage = cand.usage;
		dev.exactMatch     = cand.exactMatch;

		if (cand.profile) {
			dev.type    = cand.profile->type;
			dev.name    = cand.profile->name;
			dev.maxNits = cand.profile->maxNits;
		} else {
			dev.type    = DisplayType::AppleGeneric;
			dev.name    = L"Apple Display (Unknown)";
			dev.maxNits = 600.f;
			Log::Warn(L"  PID 0x%04X not in profiles, using generic mode", cand.pid);
		}

		dev.containerId = cand.containerId;

		Log::Info(L"  Opened: %s [Feature ID=0x%02X, %s]",
		          dev.name.c_str(), dev.featCaps.id,
		          dev.exactMatch ? L"exact" : L"fallback");

		result.push_back(std::move(dev));
	}

	if (!result.empty())
		Log::Info(L"Enumeration complete: %zu display(s) found", result.size());
	return result;
}
