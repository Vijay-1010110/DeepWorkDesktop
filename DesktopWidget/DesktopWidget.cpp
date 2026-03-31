// DesktopWidget.cpp : Defines the entry point for the application.

#include "framework.h"
#include "DesktopWidget.h"
#include <stdio.h>

// Required for GDI+ when WIN32_LEAN_AND_MEAN is defined
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")

// Hardware monitoring includes
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

#include <dxgi1_4.h>
#pragma comment(lib, "dxgi.lib")

#include <string>
#include <vector>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

#include <comdef.h>
#include <Wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")

using namespace Gdiplus;

// -------------------------------------------------------------------------
// Global Variables
// -------------------------------------------------------------------------
HWND g_hWnd = NULL;
ULONG_PTR g_gdiplusToken;
HWINEVENTHOOK g_hEventHook = NULL;

// -------------------------------------------------------------------------
// Hardware Monitor Class
// -------------------------------------------------------------------------
class HardwareMonitor {
private:
    PDH_HQUERY cpuQuery;
    PDH_HCOUNTER cpuTotal;

    PDH_HQUERY diskQuery;
    PDH_HCOUNTER diskTotal;

    PDH_HQUERY netQuery;
    PDH_HCOUNTER netTotal;

    PDH_HQUERY gpuQuery;
    PDH_HCOUNTER gpuTotal;

    IDXGIFactory4* dxgiFactory = nullptr;
    IDXGIAdapter3* dxgiAdapter = nullptr;

    double lastCpu = 0;
    double lastDisk = 0;
    double lastNet = 0;
    double lastGpu = 0;

public:
    HardwareMonitor() {
        PdhOpenQuery(NULL, NULL, &cpuQuery);
        PdhAddEnglishCounter(cpuQuery, L"\\Processor(_Total)\\% Processor Time", 0, &cpuTotal);
        PdhCollectQueryData(cpuQuery);

        PdhOpenQuery(NULL, NULL, &diskQuery);
        PdhAddEnglishCounter(diskQuery, L"\\PhysicalDisk(_Total)\\Disk Bytes/sec", 0, &diskTotal);
        PdhCollectQueryData(diskQuery);

        PdhOpenQuery(NULL, NULL, &netQuery);
        PdhAddEnglishCounter(netQuery, L"\\Network Interface(*)\\Bytes Total/sec", 0, &netTotal);
        PdhCollectQueryData(netQuery);

        PdhOpenQuery(NULL, NULL, &gpuQuery);
        PdhAddEnglishCounter(gpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuTotal);
        PdhCollectQueryData(gpuQuery);

        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)))) {
            IDXGIAdapter1* adapter;
            if (SUCCEEDED(dxgiFactory->EnumAdapters1(0, &adapter))) {
                adapter->QueryInterface(IID_PPV_ARGS(&dxgiAdapter));
                adapter->Release();
            }
        }
    }

    ~HardwareMonitor() {
        if (dxgiAdapter) dxgiAdapter->Release();
        if (dxgiFactory) dxgiFactory->Release();
        if (cpuQuery) PdhCloseQuery(cpuQuery);
        if (diskQuery) PdhCloseQuery(diskQuery);
        if (netQuery) PdhCloseQuery(netQuery);
        if (gpuQuery) PdhCloseQuery(gpuQuery);
    }

    void Update() {
        PdhCollectQueryData(cpuQuery);
        PdhCollectQueryData(diskQuery);
        PdhCollectQueryData(netQuery);
        PdhCollectQueryData(gpuQuery);
    }

    double GetCPU() {
        PDH_FMT_COUNTERVALUE counterVal;
        if (PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
            lastCpu = counterVal.doubleValue;
        }
        return lastCpu;
    }

    double GetDiskMBs() {
        PDH_FMT_COUNTERVALUE counterVal;
        if (PdhGetFormattedCounterValue(diskTotal, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
            lastDisk = counterVal.doubleValue / (1024.0 * 1024.0);
        }
        return lastDisk;
    }

    double GetNetMBs() {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PdhGetFormattedCounterArray(netTotal, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
        if (bufferSize > 0) {
            PDH_FMT_COUNTERVALUE_ITEM* items = (PDH_FMT_COUNTERVALUE_ITEM*)malloc(bufferSize);
            if (items && PdhGetFormattedCounterArray(netTotal, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
                double total = 0;
                for (DWORD i = 0; i < itemCount; i++) {
                    total += items[i].FmtValue.doubleValue;
                }
                lastNet = total / (1024.0 * 1024.0);
                free(items);
            }
        }
        return lastNet;
    }

    double GetGPU() {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PdhGetFormattedCounterArray(gpuTotal, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
        if (bufferSize > 0) {
            PDH_FMT_COUNTERVALUE_ITEM* items = (PDH_FMT_COUNTERVALUE_ITEM*)malloc(bufferSize);
            if (items && PdhGetFormattedCounterArray(gpuTotal, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
                double total = 0;
                // Summing all GPU engine utilization values.
                for (DWORD i = 0; i < itemCount; i++) {
                    total += items[i].FmtValue.doubleValue;
                }
                lastGpu = total; 
                free(items);
            }
        }
        // Clamp strictly to 100% since different engine nodes can overlap percentages
        return min(lastGpu, 100.0); 
    }

    void GetRAM(double& usedMB, double& totalMB) {
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        GlobalMemoryStatusEx(&memInfo);
        usedMB = (memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024.0 * 1024.0);
        totalMB = memInfo.ullTotalPhys / (1024.0 * 1024.0);
    }

    void GetVRAM(double& usedMB, double& totalMB) {
        if (dxgiAdapter) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info;
            if (SUCCEEDED(dxgiAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                usedMB = info.CurrentUsage / (1024.0 * 1024.0);
                totalMB = info.Budget / (1024.0 * 1024.0);
                return;
            }
        }
        usedMB = 0;
        totalMB = 0;
    }
};

HardwareMonitor* g_hwMonitor = nullptr;

// -------------------------------------------------------------------------
// WMI Temperature Monitor (Requires LibreHardwareMonitor or OpenHardwareMonitor)
// Natively querying precise CPU/GPU dies requires Ring0 sys drivers.
// -------------------------------------------------------------------------
class WmiTemperatureMonitor {
    IWbemServices* pSvc = nullptr;
    IWbemLocator* pLoc = nullptr;
    bool initialized = false;

public:
    WmiTemperatureMonitor() {
        HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
        // Attempt security init, fails safely if already initialized by runtime
        CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);

        hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
        if (FAILED(hres)) return;

        hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\OpenHardwareMonitor"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
        if (SUCCEEDED(hres)) {
            hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
            if (SUCCEEDED(hres)) {
                initialized = true;
            }
        }
    }

    ~WmiTemperatureMonitor() {
        if (pSvc) pSvc->Release();
        if (pLoc) pLoc->Release();
        CoUninitialize();
    }

    void GetTemperatures(double& cpuTemp, double& gpuTemp) {
        if (!initialized) return;

        IEnumWbemClassObject* pEnumerator = NULL;
        HRESULT hres = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT Name, Value FROM Sensor WHERE SensorType = 'Temperature'"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);

        if (FAILED(hres)) return;

        IWbemClassObject* pclsObj = NULL;
        ULONG uReturn = 0;

        while (pEnumerator) {
            HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
            if (0 == uReturn) break;

            VARIANT vtPropName, vtPropVal;
            pclsObj->Get(L"Name", 0, &vtPropName, 0, 0);
            pclsObj->Get(L"Value", 0, &vtPropVal, 0, 0);

            if (vtPropName.vt == VT_BSTR && (vtPropVal.vt == VT_R4 || vtPropVal.vt == VT_R8)) {
                std::wstring name(vtPropName.bstrVal);
                float val = (vtPropVal.vt == VT_R8) ? (float)vtPropVal.dblVal : vtPropVal.fltVal;
                if (name.find(L"CPU Package") != std::wstring::npos || name.find(L"CPU Core") != std::wstring::npos) {
                    cpuTemp = val;
                } else if (name.find(L"GPU Core") != std::wstring::npos) {
                    gpuTemp = val;
                }
            }

            VariantClear(&vtPropName);
            VariantClear(&vtPropVal);
            pclsObj->Release();
        }
        pEnumerator->Release();
    }
};

WmiTemperatureMonitor* g_wmiMonitor = nullptr;

// -------------------------------------------------------------------------
// Path Resolution
// -------------------------------------------------------------------------
std::wstring GetAssetsPath(const std::wstring& subFolder) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    
    std::wstring assetsPath = path;
    assetsPath += L"\\assets\\" + subFolder;
    return assetsPath;
}

// -------------------------------------------------------------------------
// Font Management
// -------------------------------------------------------------------------
PrivateFontCollection g_fontCollection;
FontFamily* g_fontFamilyTitle = nullptr;
FontFamily* g_fontFamilyItem = nullptr;

void InitializeFonts() {
    std::wstring fontDir = GetAssetsPath(L"fonts");
    std::wstring searchPath = fontDir + L"\\*.*";
    
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring fileName = findData.cFileName;
                if (fileName.find(L".ttf") != std::wstring::npos || fileName.find(L".otf") != std::wstring::npos) {
                    std::wstring fullPath = fontDir + L"\\" + fileName;
                    g_fontCollection.AddFontFile(fullPath.c_str());
                }
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    INT count = g_fontCollection.GetFamilyCount();
    if (count > 0) {
        FontFamily* pFontFamilies = new FontFamily[count];
        INT found = 0;
        g_fontCollection.GetFamilies(count, pFontFamilies, &found);
        
        if (found >= 1) {
             WCHAR familyNameTitle[LF_FACESIZE];
             pFontFamilies[0].GetFamilyName(familyNameTitle);
             g_fontFamilyTitle = new FontFamily(familyNameTitle, &g_fontCollection);
             
             WCHAR familyNameItem[LF_FACESIZE];
             pFontFamilies[found > 1 ? 1 : 0].GetFamilyName(familyNameItem);
             g_fontFamilyItem = new FontFamily(familyNameItem, &g_fontCollection);
        }
        delete[] pFontFamilies;
    }
}

void CleanupFonts() {
    if (g_fontFamilyTitle) {
        delete g_fontFamilyTitle;
        g_fontFamilyTitle = nullptr;
    }
    if (g_fontFamilyItem) {
        delete g_fontFamilyItem;
        g_fontFamilyItem = nullptr;
    }
}

// -------------------------------------------------------------------------
// UI Graphics Helpers
// -------------------------------------------------------------------------
void AddRoundRect(GraphicsPath* path, const RectF& rect, float radius) {
    float d = radius * 2.0f;
    if (rect.Width < d || rect.Height < d) {
        path->AddRectangle(rect);
        return;
    }
    path->AddArc(rect.X, rect.Y, d, d, 180, 90);
    path->AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270, 90);
    path->AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0, 90);
    path->AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90, 90);
    path->CloseFigure();
}

void DrawProgressBar(Graphics& g, Gdiplus::Font* font, const WCHAR* label, const WCHAR* valueText, 
                     float x, float y, float width, float percentage,
                     Color colorStart, Color colorEnd) {
    
    // Draw Label (Left Aligned)
    SolidBrush textBrush(Color(255, 255, 255, 255));
    g.DrawString(label, -1, font, PointF(x, y), &textBrush);
    
    // Draw Value (Right Aligned)
    StringFormat format;
    format.SetAlignment(StringAlignmentFar);
    RectF layoutRect(x, y, width, 20.0f);
    g.DrawString(valueText, -1, font, layoutRect, &format, &textBrush);

    // Draw Bar Background Track
    float barY = y + 20.0f;
    float barHeight = 8.0f;
    RectF bgRect(x, barY, width, barHeight);
    
    GraphicsPath bgPath;
    AddRoundRect(&bgPath, bgRect, barHeight / 2.0f);
    SolidBrush bgBrush(Color(50, 255, 255, 255)); 
    g.FillPath(&bgBrush, &bgPath);

    // Draw Bar Foreground (Gradient)
    if (percentage > 0.0f) {
        float fillWidth = (width * percentage) / 100.0f;
        if (fillWidth < barHeight) fillWidth = barHeight; // Ensure corners remain rounded
        RectF fillRect(x, barY, fillWidth, barHeight);
        
        GraphicsPath fillPath;
        AddRoundRect(&fillPath, fillRect, barHeight / 2.0f);

        LinearGradientBrush fillBrush(fillRect, colorStart, colorEnd, LinearGradientModeHorizontal);
        g.FillPath(&fillBrush, &fillPath);
    }
}

// -------------------------------------------------------------------------
// DrawWidget: Renders the UI to an in-memory GDI+ Bitmap and pushes it 
// directly to the screen via UpdateLayeredWindow natively. 
// -------------------------------------------------------------------------
void DrawWidget(HWND hWnd) {
    int width = 400;
    int height = 500; // Increased height to comfortably fit all graphical bars including temperatures

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // Provide a scope for GDI+ objects to be properly destructed
    // before we call UpdateLayeredWindow
    {
        Graphics graphics(hdcMem);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.Clear(Color(0, 0, 0, 0)); // Pure transparent background

        // Draw a seamless dark, cleanly rounded background panel
        RectF panelRect(0, 0, (float)width, (float)height);
        GraphicsPath panelPath;
        AddRoundRect(&panelPath, panelRect, 15.0f);
        SolidBrush bgBrush(Color(220, 15, 15, 20)); // Deep dark smooth background
        graphics.FillPath(&bgBrush, &panelPath);

        // Fallback to Segoe UI if custom fonts failed to load
        FontFamily defaultFamily(L"Segoe UI");
        
        FontFamily* useTitleFamily = g_fontFamilyTitle ? g_fontFamilyTitle : &defaultFamily;
        FontFamily* useItemFamily = g_fontFamilyItem ? g_fontFamilyItem : &defaultFamily;

        Gdiplus::Font fontTitle(useTitleFamily, 16, FontStyleBold, UnitPixel);
        Gdiplus::Font fontItem(useItemFamily, 13, FontStyleRegular, UnitPixel);
        
        SolidBrush accentBrush(Color(255, 100, 200, 255));

        graphics.DrawString(L"SYSTEM STATUS", -1, &fontTitle, PointF(20.0f, 15.0f), &accentBrush);

        if (g_hwMonitor) {
            g_hwMonitor->Update();

            double cpu = g_hwMonitor->GetCPU();
            double gpu = g_hwMonitor->GetGPU();
            double disk = g_hwMonitor->GetDiskMBs();
            double net = g_hwMonitor->GetNetMBs();

            double ramUsed, ramTotal;
            g_hwMonitor->GetRAM(ramUsed, ramTotal);

            double vramUsed, vramTotal;
            g_hwMonitor->GetVRAM(vramUsed, vramTotal);

            WCHAR buf[256];
            float y = 50.0f;
            float spacing = 45.0f;
            float barWidth = 360.0f;
            float x = 20.0f;

            // CPU Bar (Orange/Red)
            swprintf_s(buf, L"%.1f %%", cpu);
            DrawProgressBar(graphics, &fontItem, L"CPU Usage", buf, x, y, barWidth, (float)cpu, 
                Color(255, 255, 150, 50), Color(255, 255, 50, 50)); 
            y += spacing;

            // GPU Bar (Purple/Pink)
            swprintf_s(buf, L"%.1f %%", gpu);
            DrawProgressBar(graphics, &fontItem, L"GPU Usage", buf, x, y, barWidth, (float)gpu, 
                Color(255, 200, 100, 255), Color(255, 255, 50, 200)); 
            y += spacing;

            // RAM Bar (Cyan/Blue)
            double ramPct = (ramTotal > 0) ? (ramUsed / ramTotal * 100.0) : 0;
            swprintf_s(buf, L"%.0f / %.0f MB", ramUsed, ramTotal);
            DrawProgressBar(graphics, &fontItem, L"RAM Usage", buf, x, y, barWidth, (float)ramPct, 
                Color(255, 100, 255, 255), Color(255, 50, 150, 255)); 
            y += spacing;

            // VRAM Bar (Green/Teal)
            double vramPct = (vramTotal > 0) ? (vramUsed / vramTotal * 100.0) : 0;
            swprintf_s(buf, L"%.0f / %.0f MB", vramUsed, vramTotal);
            DrawProgressBar(graphics, &fontItem, L"VRAM Usage", buf, x, y, barWidth, (float)vramPct, 
                Color(255, 100, 255, 150), Color(255, 50, 200, 100)); 
            y += spacing;

            // Disk Bar (Adaptive scaling, arbitrarily maxes at 100MB/s visually)
            double maxDisk = 100.0;
            double diskPct = min((disk / maxDisk) * 100.0, 100.0);
            swprintf_s(buf, L"%.2f MB/s", disk);
            DrawProgressBar(graphics, &fontItem, L"Disk Activity", buf, x, y, barWidth, (float)diskPct, 
                Color(255, 200, 200, 255), Color(255, 100, 100, 255)); 
            y += spacing;

            // Network Bar (Adaptive scaling, arbitrarily maxes at 50MB/s visually)
            double maxNet = 50.0;
            double netPct = min((net / maxNet) * 100.0, 100.0);
            swprintf_s(buf, L"%.2f MB/s", net);
            DrawProgressBar(graphics, &fontItem, L"Network Activity", buf, x, y, barWidth, (float)netPct, 
                Color(255, 255, 200, 100), Color(255, 255, 150, 50)); 
            y += spacing;

            // Temperatures (WMI via OpenHardwareMonitor/LibreHardwareMonitor)
            double cpuTemp = 0, gpuTemp = 0;
            if (g_wmiMonitor) g_wmiMonitor->GetTemperatures(cpuTemp, gpuTemp);
            
            // CPU Temp (Yellow/Red)
            if (cpuTemp > 0) swprintf_s(buf, L"%.1f °C", cpuTemp);
            else swprintf_s(buf, L"N/A (Run LHM/OHM)");
            DrawProgressBar(graphics, &fontItem, L"CPU Temp", buf, x, y, barWidth, (float)min((cpuTemp / 100.0) * 100.0, 100.0), 
                Color(255, 255, 200, 50), Color(255, 255, 50, 50)); 
            y += spacing;

            // GPU Temp (Orange/Red)
            if (gpuTemp > 0) swprintf_s(buf, L"%.1f °C", gpuTemp);
            else swprintf_s(buf, L"N/A (Run LHM/OHM)");
            DrawProgressBar(graphics, &fontItem, L"GPU Temp", buf, x, y, barWidth, (float)min((gpuTemp / 100.0) * 100.0, 100.0), 
                Color(255, 255, 150, 50), Color(255, 255, 80, 50)); 
            y += spacing;
        }
    }

    // Push the memory bitmap strictly via UpdateLayeredWindow (Alpha composition)
    POINT ptSrc = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = { 0 };
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hWnd, hdcScreen, NULL, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    // Resource cleanup
    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// -------------------------------------------------------------------------
// WinEventHook Callback: Evading Windows 11 24H2 DWM Bug
// "The Rainmeter Instantaneous Z-Order Juggle"
// -------------------------------------------------------------------------
void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (!hwnd || hwnd == g_hWnd || !g_hWnd) return;

        WCHAR className[256];
        if (GetClassNameW(hwnd, className, 256)) {
            // If the user clicks "Show Desktop" (Win+D) or the desktop background, 
            // the OS will sweep and blindly hide all overlaid windows.
            if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0) {
                // Instantly vault the widget to TOPMOST so the DWM ignores it
                SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else {
                // Focus shifted to a normal application, safely drop down
                SetWindowPos(g_hWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }
}

// -------------------------------------------------------------------------
// Window Procedure
// -------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            // Initialize Hardware Monitor
            g_hwMonitor = new HardwareMonitor();
            g_wmiMonitor = new WmiTemperatureMonitor();
            // 1) 1000ms strictly event-driven Timer - keeps CPU at 0%
            SetTimer(hWnd, 1, 1000, NULL);
            // 2) Initial Draw
            DrawWidget(hWnd);
            break;

        case WM_TIMER:
            if (wParam == 1) {
                DrawWidget(hWnd);
            }
            break;

        case WM_WINDOWPOSCHANGING: {
            // 3) Strip SWP_HIDEWINDOW to aggressively block DWM from natively hiding our widget
            WINDOWPOS* pos = (WINDOWPOS*)lParam;
            if (pos->flags & SWP_HIDEWINDOW) {
                pos->flags &= ~SWP_HIDEWINDOW;
            }
            break;
        }

        // We override mouse clicks so you can drag the window if needed (optional bonus)
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT) return HTCAPTION;
            return hit;
        }

        case WM_DESTROY:
            KillTimer(hWnd, 1);
            if (g_hwMonitor) {
                delete g_hwMonitor;
                g_hwMonitor = nullptr;
            }
            if (g_wmiMonitor) {
                delete g_wmiMonitor;
                g_wmiMonitor = nullptr;
            }
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// -------------------------------------------------------------------------
// Application Entry Point: wWinMain
// -------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 1) Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    // Initialize custom fonts
    InitializeFonts();

    // 2) Register strict Window Class
    const wchar_t CLASS_NAME[] = L"DesktopWidgetClass";
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    // 3) Create floating, unowned window (Parent = NULL)
    // WS_EX_LAYERED: Required for UpdateLayeredWindow
    // WS_EX_TOOLWINDOW: Hides from Alt-Tab
    // WS_EX_NOACTIVATE: Prevents stealing focus
    // WS_POPUP: Barebones popup window with no border
    g_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLASS_NAME,
        L"Desktop Widget",
        WS_POPUP,
        100, 100, 400, 500,   // Initial size/location
        NULL,                 // *Very* important: Unowned (No parent)
        NULL,                 // No menu
        hInstance,
        NULL
    );

    if (g_hWnd == NULL) {
        CleanupFonts();
        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    // 4) Register the EVENT_SYSTEM_FOREGROUND WinEventHook
    g_hEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    // 5) Show but do NOT steal focus (SW_SHOWNA)
    ShowWindow(g_hWnd, SW_SHOWNA);

    // 6) Sleep-friendly Event Loop (0% CPU overhead)
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Teardown
    if (g_hEventHook) UnhookWinEvent(g_hEventHook);
    if (g_hwMonitor) {
        delete g_hwMonitor;
        g_hwMonitor = nullptr;
    }
    if (g_wmiMonitor) {
        delete g_wmiMonitor;
        g_wmiMonitor = nullptr;
    }
    
    CleanupFonts();
    GdiplusShutdown(g_gdiplusToken);

    return (int) msg.wParam;
}
