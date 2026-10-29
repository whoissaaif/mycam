#include "winusb_bind.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <newdev.h>
#include <objbase.h>
#include <setupapi.h>

#include <functional>
#include <string>
#include <vector>

namespace mycam {

// Phones in Android Open Accessory mode enumerate as Google 18d1:2d00..2d05. 2d00 is the accessory alone and
// Windows has no driver for it. With USB debugging (or audio) on, the phone is a composite device instead
// (2d01 = accessory + adb, 2d04 = accessory + audio, 2d05 = all three); the accessory is always interface 0.
// 2d02/2d03 are audio only and have no accessory interface.
//
// How Windows names interface 0 depends on which bus driver claimed the composite parent:
//   - usbccgp (Windows' own):          USB\VID_18D1&PID_2D01&MI_00\<n>
//   - a vendor bus driver, e.g. the Samsung USB driver (dg_ssudbus), whose INF claims USB\VID_18D1&PID_2D01
//     on any PC that ever had Samsung's driver installed:
//                                      USB\VID_18D1&PID_2D01&MS_COMP_MTP&<model>\<n>  (and ...&ADB\<n>)
// so the instance ID can't be relied on. Both list "USB\VID_18D1&PID_2D01&MI_00" among the hardware IDs,
// though (it's the ID every composite bus driver publishes so function drivers can match one interface), and
// the parent is always USB\VID_18D1&PID_2D01. That pair is what identifies the accessory interface here.
//
// Interface 0 may also end up with a driver that can't use it: a vendor bus driver may hand it a compatible ID
// such as USB\MS_COMP_MTP (Samsung's does, the accessory is ff/ff/00 like Samsung's MTP), and a phone may
// report a Microsoft OS descriptor for it; Windows then loads the WPD MTP driver on the accessory.

namespace {

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = towupper(c);
    return s;
}

bool HexDigits(const std::wstring& s, size_t at, size_t n, unsigned* value) {
    if (s.size() < at + n) return false;
    unsigned v = 0;
    for (size_t i = at; i < at + n; ++i) {
        wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') v = v * 16 + (c - L'0');
        else if (c >= L'A' && c <= L'F') v = v * 16 + (c - L'A' + 10);
        else return false;
    }
    *value = v;
    return true;
}

struct UsbId {
    unsigned vid = 0, pid = 0;
    int mi = -1; // -1: the whole device.
};

// Parses "USB\VID_xxxx&PID_xxxx[&REV_xxxx][&MI_xx]" (a hardware ID, or the device-ID part of an instance ID).
// Anything with other parts (&MS_COMP_..., &Class_..., &ADB, ...) doesn't count: those name something else.
bool ParseUsbId(const std::wstring& raw, UsbId* out) {
    std::wstring id = Upper(raw);
    size_t second = id.find(L'\\', 4);
    if (second != std::wstring::npos) id.resize(second); // Instance ID: drop the instance part.
    if (id.rfind(L"USB\\VID_", 0) != 0) return false;
    UsbId r;
    if (!HexDigits(id, 8, 4, &r.vid) || id.compare(12, 5, L"&PID_") != 0 || !HexDigits(id, 17, 4, &r.pid)) return false;
    size_t at = 21;
    unsigned value = 0;
    if (id.compare(at, 5, L"&REV_") == 0) {
        if (!HexDigits(id, at + 5, 4, &value)) return false;
        at += 9;
    }
    if (id.compare(at, 4, L"&MI_") == 0) {
        if (!HexDigits(id, at + 4, 2, &value)) return false;
        r.mi = int(value);
        at += 6;
    }
    if (at != id.size()) return false;
    *out = r;
    return true;
}

constexpr unsigned kGoogleVid = 0x18D1;

bool IsCompositeAccessoryPid(unsigned pid) { return pid == 0x2D01 || pid == 0x2D04 || pid == 0x2D05; }

constexpr wchar_t kWpdClass[] = L"{EEC5AD98-8080-425F-922A-DABF3DE3F69A}";   // Portable devices (MTP)
constexpr wchar_t kImageClass[] = L"{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}"; // Still image (PTP)

// Registered for a composite child MyCam binds, so libusb (which finds composite interfaces by their device
// interface GUID) can see it. Any fixed GUID works; this one is MyCam's.
constexpr wchar_t kAccessoryInterfaceGuid[] = L"{ADA84E63-2CFD-4074-ADB4-CFD86103BD71}";

} // namespace

AccessoryKind ClassifyAccessory(const std::vector<std::wstring>& ids, const std::vector<std::wstring>& parentIds) {
    unsigned parentPid = 0;
    for (const auto& p : parentIds) {
        UsbId id;
        if (ParseUsbId(p, &id) && id.vid == kGoogleVid && id.mi < 0 && IsCompositeAccessoryPid(id.pid)) parentPid = id.pid;
    }
    for (const auto& s : ids) {
        UsbId id;
        if (!ParseUsbId(s, &id) || id.vid != kGoogleVid) continue;
        if (id.pid == 0x2D00 && id.mi < 0) return AccessoryKind::Device;
        if (id.mi == 0 && IsCompositeAccessoryPid(id.pid) && id.pid == parentPid) return AccessoryKind::Interface;
    }
    return AccessoryKind::None;
}

AccessoryDriver ClassifyAccessoryDriver(AccessoryKind kind, const AccessoryDriverInfo& info) {
    if (kind == AccessoryKind::None) return AccessoryDriver::Foreign;
    std::wstring service = Upper(info.service);
    if (service == L"WINUSB") {
        // A whole device is found through the USB hub's own interface; a composite child only through its GUID.
        return kind == AccessoryKind::Interface && !info.hasInterfaceGuid ? AccessoryDriver::NoInterfaceGuid
                                                                          : AccessoryDriver::Ready;
    }
    if (service == L"LIBUSBK" || service == L"LIBUSB0") return AccessoryDriver::Ready; // libusb can use these too.
    if (service.empty()) {
        bool failed = info.hasProblem && (info.problem == CM_PROB_FAILED_INSTALL || info.problem == CM_PROB_NOT_CONFIGURED);
        return failed ? AccessoryDriver::Missing : AccessoryDriver::Ready; // Otherwise Windows is still installing.
    }
    std::wstring cls = Upper(info.classGuid);
    if (cls == kWpdClass || cls == kImageClass || Upper(info.matchingId).rfind(L"USB\\MS_COMP_", 0) == 0) {
        return AccessoryDriver::WrongClass;
    }
    return AccessoryDriver::Foreign;
}

bool NeedsWinUsbBinding(AccessoryDriver state) {
    return state == AccessoryDriver::Missing || state == AccessoryDriver::WrongClass ||
           state == AccessoryDriver::NoInterfaceGuid;
}

const char* AccessoryDriverName(AccessoryDriver state) {
    switch (state) {
    case AccessoryDriver::Ready: return "ready";
    case AccessoryDriver::Missing: return "no driver";
    case AccessoryDriver::WrongClass: return "wrong class driver";
    case AccessoryDriver::NoInterfaceGuid: return "WinUSB without an interface GUID";
    case AccessoryDriver::Foreign: return "another driver (left alone)";
    }
    return "?";
}

namespace {

std::vector<std::wstring> SplitMultiSz(const wchar_t* p, size_t chars) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < chars && p[i];) {
        std::wstring s(p + i);
        i += s.size() + 1;
        out.push_back(std::move(s));
    }
    return out;
}

std::wstring InstanceId(HDEVINFO set, SP_DEVINFO_DATA* dev) {
    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    SetupDiGetDeviceInstanceIdW(set, dev, id, MAX_DEVICE_ID_LEN, nullptr);
    return id;
}

std::vector<std::wstring> HardwareIds(HDEVINFO set, SP_DEVINFO_DATA* dev, const std::wstring& instanceId) {
    wchar_t buf[2048] = {};
    std::vector<std::wstring> ids;
    if (SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(buf),
                                          sizeof(buf) - 2 * sizeof(wchar_t), nullptr)) {
        ids = SplitMultiSz(buf, ARRAYSIZE(buf));
    }
    ids.push_back(instanceId);
    return ids;
}

std::vector<std::wstring> ParentIds(DEVINST inst) {
    std::vector<std::wstring> ids;
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS) return ids;
    wchar_t buf[2048] = {};
    ULONG len = sizeof(buf) - 2 * sizeof(wchar_t);
    if (CM_Get_DevNode_Registry_PropertyW(parent, CM_DRP_HARDWAREID, nullptr, buf, &len, 0) == CR_SUCCESS) {
        ids = SplitMultiSz(buf, ARRAYSIZE(buf));
    }
    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) ids.push_back(id);
    return ids;
}

bool HasValue(HKEY key, const wchar_t* name) {
    return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

std::wstring StringValue(HKEY key, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS || type != REG_SZ) return L"";
    return buf;
}

AccessoryDriverInfo DriverInfo(HDEVINFO set, SP_DEVINFO_DATA* dev) {
    AccessoryDriverInfo info;
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, dev->DevInst, 0) == CR_SUCCESS) {
        info.hasProblem = (status & DN_HAS_PROBLEM) != 0;
        info.problem = problem;
    }
    wchar_t buf[512] = {};
    if (SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_SERVICE, nullptr, reinterpret_cast<BYTE*>(buf),
                                          sizeof(buf) - sizeof(wchar_t), nullptr)) {
        info.service = buf;
    }
    wchar_t guid[64] = {};
    if (StringFromGUID2(dev->ClassGuid, guid, ARRAYSIZE(guid))) info.classGuid = guid;
    HKEY drv = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (drv != INVALID_HANDLE_VALUE) {
        info.matchingId = StringValue(drv, L"MatchingDeviceId");
        RegCloseKey(drv);
    }
    HKEY params = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (params != INVALID_HANDLE_VALUE) {
        info.hasInterfaceGuid = HasValue(params, L"DeviceInterfaceGUIDs") || HasValue(params, L"DeviceInterfaceGUID");
        RegCloseKey(params);
    }
    return info;
}

struct AccessoryNode {
    std::wstring instanceId;
    AccessoryKind kind;
    AccessoryDriverInfo info;
    AccessoryDriver state;
};

// Calls `visit` for every present accessory node; stops when it returns false.
void ForEachAccessory(const std::function<bool(HDEVINFO, SP_DEVINFO_DATA*, const AccessoryNode&)>& visit) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        AccessoryNode node;
        node.instanceId = InstanceId(set, &dev);
        std::vector<std::wstring> ids = HardwareIds(set, &dev, node.instanceId);
        bool google = false; // Cheap filter first: only Google 18d1 IDs can be accessories (whatever the node's name).
        for (const auto& id : ids) {
            UsbId parsed;
            google |= ParseUsbId(id, &parsed) && parsed.vid == kGoogleVid;
        }
        if (!google) continue;
        node.kind = ClassifyAccessory(ids, ParentIds(dev.DevInst));
        if (node.kind == AccessoryKind::None) continue;
        node.info = DriverInfo(set, &dev);
        node.state = ClassifyAccessoryDriver(node.kind, node.info);
        if (!visit(set, &dev, node)) break;
    }
    SetupDiDestroyDeviceInfoList(set);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// Lets libusb find a composite child: WinUSB registers the device interfaces listed here when it starts.
bool AddInterfaceGuid(HDEVINFO set, SP_DEVINFO_DATA* dev) {
    HKEY key = SetupDiCreateDevRegKeyW(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
    if (key == INVALID_HANDLE_VALUE) return false;
    std::wstring value = std::wstring(kAccessoryInterfaceGuid) + L'\0'; // REG_MULTI_SZ: one string, then "\0\0".
    LONG r = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

// Installs Windows' inbox "WinUsb Device" driver (winusb.inf) on one device, like picking it manually in
// Device Manager. The driver package is Microsoft-signed, so no custom signing is needed. Replaces whatever
// driver the device has; Windows keeps that choice for this device from then on.
bool InstallWinUsb(HDEVINFO set, SP_DEVINFO_DATA* dev) {
    wchar_t classGuid[] = L"{88BAE032-5A81-49f0-BC3D-A4FF138216D6}"; // USBDevice class (winusb.inf)
    if (!SetupDiSetDeviceRegistryPropertyW(set, dev, SPDRP_CLASSGUID, reinterpret_cast<BYTE*>(classGuid), sizeof(classGuid)))
        return false;
    dev->ClassGuid = {0x88bae032, 0x5a81, 0x49f0, {0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6}};

    SP_DEVINSTALL_PARAMS_W params = {sizeof(params)};
    if (!SetupDiGetDeviceInstallParamsW(set, dev, &params)) return false;
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    GetWindowsDirectoryW(params.DriverPath, MAX_PATH);
    wcscat_s(params.DriverPath, L"\\INF\\winusb.inf");
    if (!SetupDiSetDeviceInstallParamsW(set, dev, &params)) return false;
    if (!SetupDiBuildDriverInfoList(set, dev, SPDIT_CLASSDRIVER)) return false;

    SP_DRVINFO_DATA_W driver = {sizeof(driver)};
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDriverInfoW(set, dev, SPDIT_CLASSDRIVER, i, &driver); ++i) {
        if (wcsstr(driver.Description, L"WinUsb")) { found = true; break; }
    }
    if (!found || !SetupDiSetSelectedDriverW(set, dev, &driver)) return false;

    BOOL reboot = FALSE;
    return DiInstallDevice(nullptr, set, dev, &driver, 0, &reboot) != FALSE;
}

} // namespace

bool AccessoryNeedsDriver(std::string* detail) {
    bool needs = false;
    ForEachAccessory([&](HDEVINFO, SP_DEVINFO_DATA*, const AccessoryNode& node) {
        if (!NeedsWinUsbBinding(node.state)) return true;
        needs = true;
        if (detail) {
            *detail = Narrow(node.instanceId) + ": " + AccessoryDriverName(node.state);
            if (!node.info.service.empty()) *detail += " (" + Narrow(node.info.service) + ")";
        }
        return false;
    });
    return needs;
}

bool AccessoryForeignDriver(std::string* detail) {
    bool foreign = false;
    ForEachAccessory([&](HDEVINFO, SP_DEVINFO_DATA*, const AccessoryNode& node) {
        if (node.state != AccessoryDriver::Foreign) return true;
        foreign = true;
        if (detail) {
            *detail = Narrow(node.instanceId) + ": " + AccessoryDriverName(node.state);
            if (!node.info.service.empty()) *detail += " (" + Narrow(node.info.service) + ")";
        }
        return false;
    });
    return foreign;
}

int BindAccessoryDrivers() {
    int bound = 0;
    ForEachAccessory([&](HDEVINFO set, SP_DEVINFO_DATA* dev, const AccessoryNode& node) {
        if (!NeedsWinUsbBinding(node.state)) return true;
        if (node.kind == AccessoryKind::Interface && !node.info.hasInterfaceGuid && !AddInterfaceGuid(set, dev)) return true;
        if (node.state == AccessoryDriver::WrongClass) {
            // The old class driver's filters (WPD MTP puts WinUsb itself below it) must not stay in the new stack.
            SetupDiSetDeviceRegistryPropertyW(set, dev, SPDRP_UPPERFILTERS, nullptr, 0);
            SetupDiSetDeviceRegistryPropertyW(set, dev, SPDRP_LOWERFILTERS, nullptr, 0);
        }
        if (InstallWinUsb(set, dev)) ++bound;
        return true;
    });
    return bound;
}

} // namespace mycam
