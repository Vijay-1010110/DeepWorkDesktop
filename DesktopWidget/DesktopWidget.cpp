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

#include "FocusEngine.h"

using namespace Gdiplus;

// -------------------------------------------------------------------------
// Global Variables
// -------------------------------------------------------------------------
HWND g_hWnd = NULL;
HWND g_hDotWnd = NULL;
HWND g_hFocusWidgetWnd = NULL;
HWND g_hNotifyWnd = NULL;
int g_notifyFrames = 0;
ULONG_PTR g_gdiplusToken;
HWINEVENTHOOK g_hEventHook = NULL;
FocusEngine* g_focusEngine = nullptr;

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
                     Color colorStart, Color colorEnd, float barHeight = 8.0f) {
    
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
// Dot Indicator Window
// -------------------------------------------------------------------------
LRESULT CALLBACK DotWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void DrawDotIndicator(HWND hDotWnd, FocusState state) {
    if (!hDotWnd) return;
    int size = 16;
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, size, size);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    {
        Graphics graphics(hdcMem);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.Clear(Color(0, 0, 0, 0));

        Color dotColor;
        switch (state) {
            case EARNING_GOOD: dotColor = Color(255, 50, 255, 50); break; // Green
            case SPENDING_ENTERTAINMENT: dotColor = Color(255, 255, 100, 50); break; // Orange
            case SPENDING_NSFW: dotColor = Color(255, 255, 0, 255); break; // Deep Magenta/Fuschia
            case AFK_PAUSED: dotColor = Color(255, 255, 255, 50); break; // Yellow
            case NEUTRAL:
            default: dotColor = Color(255, 100, 100, 100); break; // Gray
        }

        SolidBrush brush(dotColor);
        graphics.FillEllipse(&brush, 2, 2, size - 4, size - 4);
    }

    POINT ptSrc = { 0, 0 };
    SIZE winSize = { size, size };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    
    // Top Left Corner
    POINT ptDst = { 10, 10 };
    UpdateLayeredWindow(hDotWnd, hdcScreen, &ptDst, &winSize, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void DrawNotifyWidget(HWND hWnd, const WCHAR* msg) {
    if (!hWnd) return;
    int width = 350; int height = 90;
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
    {
        Graphics graphics(hdcMem);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.Clear(Color(0,0,0,0));
        
        RectF panelRect(0, 0, (float)width, (float)height);
        GraphicsPath panelPath;
        AddRoundRect(&panelPath, panelRect, 10.0f);
        SolidBrush bg(Color(250, 40, 10, 20));
        graphics.FillPath(&bg, &panelPath);
        
        FontFamily defaultFamily(L"Segoe UI");
        Gdiplus::Font fontTitle(g_fontFamilyTitle ? g_fontFamilyTitle : &defaultFamily, 16, FontStyleBold, UnitPixel);
        Gdiplus::Font fontMsg(g_fontFamilyItem ? g_fontFamilyItem : &defaultFamily, 14, FontStyleRegular, UnitPixel);
        
        SolidBrush titleBrush(Color(255, 255, 200, 50));
        SolidBrush txtBrush(Color(255, 255, 255, 255));
        
        graphics.DrawString(L"⚠ TOKENS DEPLETING", -1, &fontTitle, PointF(15.0f, 15.0f), &titleBrush);
        graphics.DrawString(msg, -1, &fontMsg, PointF(15.0f, 45.0f), &txtBrush);
    }
    POINT ptSrc = {0,0}; SIZE sz = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hWnd, hdcScreen, NULL, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    SelectObject(hdcMem, hOldBitmap); DeleteObject(hBitmap); DeleteDC(hdcMem); ReleaseDC(NULL, hdcScreen);
}

// -------------------------------------------------------------------------
// Focus & Analytics Widget Window
// -------------------------------------------------------------------------
LRESULT CALLBACK FocusWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) {
        LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
        if (hit == HTCLIENT) return HTCAPTION;
        return hit;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void DrawFocusWidget(HWND hWnd) {
    int width = 400;
    int height = 480; 

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    {
        Graphics graphics(hdcMem);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.Clear(Color(0, 0, 0, 0));

        RectF panelRect(0, 0, (float)width, (float)height);
        GraphicsPath panelPath;
        AddRoundRect(&panelPath, panelRect, 15.0f);
        SolidBrush bgBrush(Color(220, 15, 20, 30)); // Deep dark blueish tint
        graphics.FillPath(&bgBrush, &panelPath);

        FontFamily defaultFamily(L"Segoe UI");
        FontFamily* useTitleFamily = g_fontFamilyTitle ? g_fontFamilyTitle : &defaultFamily;
        FontFamily* useItemFamily = g_fontFamilyItem ? g_fontFamilyItem : &defaultFamily;
        Gdiplus::Font fontTitle(useTitleFamily, 16, FontStyleBold, UnitPixel);
        Gdiplus::Font fontItem(useItemFamily, 13, FontStyleRegular, UnitPixel);
        
        SolidBrush accentBrush(Color(255, 100, 255, 200));
        graphics.DrawString(L"FOCUS & ANALYTICS", -1, &fontTitle, PointF(20.0f, 15.0f), &accentBrush);

        if (g_focusEngine) {
            WCHAR buf[256];
            float y = 50.0f;
            float spacing = 45.0f;
            float barWidth = 360.0f;
            float x = 20.0f;

            const WCHAR* stateStr = L"Neutral";
            Color stateCol = Color(255, 100, 100, 100);
            switch(g_focusEngine->currentState) {
                case EARNING_GOOD: stateStr = L"Earning Tokens"; stateCol = Color(255, 50, 255, 50); break;
                case SPENDING_ENTERTAINMENT: stateStr = L"Spending Tokens"; stateCol = Color(255, 255, 150, 50); break;
                case SPENDING_NSFW: stateStr = L"NSFW Warning"; stateCol = Color(255, 255, 0, 255); break;
                case AFK_PAUSED: stateStr = L"AFK Paused"; stateCol = Color(255, 255, 255, 50); break;
                default: break;
            }
            
            swprintf_s(buf, L"%s", stateStr);
            DrawProgressBar(graphics, &fontItem, L"Engine State", buf, x, y, barWidth, 100.0f, stateCol, stateCol);
            y += spacing;

            int hr = g_focusEngine->earnedTokensSeconds / 3600;
            int mn = (g_focusEngine->earnedTokensSeconds % 3600) / 60;
            swprintf_s(buf, L"%02d h %02d m", hr, mn);
            DrawProgressBar(graphics, &fontItem, L"Earned Tokens", buf, x, y, barWidth, 100.0f, Color(255, 50, 200, 255), Color(255, 50, 200, 255));
            y += spacing;
            
            int nsfwHr = g_focusEngine->nsfwRemainingSeconds / 3600;
            int nsfwMn = (g_focusEngine->nsfwRemainingSeconds % 3600) / 60;
            swprintf_s(buf, L"%02d h %02d m", nsfwHr, nsfwMn);
            DrawProgressBar(graphics, &fontItem, L"NSFW Allowance", buf, x, y, barWidth, 100.0f, Color(255, 200, 50, 100), Color(255, 200, 50, 100));
            y += spacing + 10.0f;

            graphics.DrawString(L"ACTIVE SCREEN TIME", -1, &fontTitle, PointF(20.0f, y), &accentBrush);
            y += 30.0f;

            int totalActive = max(1, g_focusEngine->totalActiveScreenTimeSeconds);
            int totHr = totalActive / 3600;
            int totMn = (totalActive % 3600) / 60;
            swprintf_s(buf, L"%d:%02d hr", totHr, totMn);
            
            // Premium Platinum/Gold Thick Bar for Benchmarking
            DrawProgressBar(graphics, &fontItem, L"Total Screen Time", buf, x, y, barWidth, 100.0f, Color(255, 255, 215, 0), Color(255, 255, 140, 0), 14.0f);
            y += spacing;

            // Tiny Subtle Divider Line
            Pen dividerPen(Color(100, 255, 255, 255), 1.0f); 
            graphics.DrawLine(&dividerPen, x + 30.0f, y, x + barWidth - 30.0f, y);
            y += 15.0f; // Padding below the line before top apps render

            auto topUsage = g_focusEngine->GetTopUsage(4);
            
            for (const auto& usage : topUsage) {
                int sec = usage.second;
                int uhr = sec / 3600;
                int umn = (sec % 3600) / 60;
                float pct = ((float)sec / (float)totalActive) * 100.0f;
                swprintf_s(buf, L"%d:%02d hr", uhr, umn);
                
                std::wstring label = usage.first;
                if (label.length() > 25) label = label.substr(0, 22) + L"...";

                FocusState category = g_focusEngine->GetCategoryForName(usage.first);
                Color startCol, endCol;
                if (category == EARNING_GOOD) {
                    startCol = Color(255, 50, 150, 255); endCol = Color(255, 50, 200, 255); // Cold (Blue)
                } else if (category == SPENDING_ENTERTAINMENT) {
                    startCol = Color(255, 255, 120, 50); endCol = Color(255, 255, 150, 50); // Warm (Orange)
                } else if (category == SPENDING_NSFW) {
                    startCol = Color(255, 255, 0, 150); endCol = Color(255, 255, 0, 255); // Danger (Magenta)
                } else {
                    startCol = Color(255, 150, 150, 150); endCol = Color(255, 100, 100, 100); // Neutral (Gray)
                }

                DrawProgressBar(graphics, &fontItem, label.c_str(), buf, x, y, barWidth, pct, startCol, endCol);
                y += spacing;
            }
        }
    }

    POINT ptSrc = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hWnd, hdcScreen, NULL, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// -------------------------------------------------------------------------
// DrawWidget: Renders the UI to an in-memory GDI+ Bitmap and pushes it 
// directly to the screen via UpdateLayeredWindow natively. 
// -------------------------------------------------------------------------
void DrawWidget(HWND hWnd) {
    int width = 340;
    int height = 430; // Original height for hardware stats

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
            float barWidth = 300.0f;
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
        if (!hwnd) return;

        WCHAR className[256];
        if (GetClassNameW(hwnd, className, 256)) {
            if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0) {
                if (g_hWnd) SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (g_hFocusWidgetWnd) SetWindowPos(g_hFocusWidgetWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else {
                if (hwnd == g_hWnd || hwnd == g_hFocusWidgetWnd || hwnd == g_hDotWnd) return;
                
                if (g_hWnd) SetWindowPos(g_hWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (g_hFocusWidgetWnd) SetWindowPos(g_hFocusWidgetWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
            g_focusEngine = new FocusEngine();
            // 1) 1000ms strictly event-driven Timer - keeps CPU at 0%
            SetTimer(hWnd, 1, 1000, NULL);
            // 2) Initial Draw
            DrawWidget(hWnd);
            break;

        case WM_TIMER:
            if (wParam == 1) {
                if (g_focusEngine) {
                    g_focusEngine->ProcessTick();
                    
                    if (g_focusEngine->warningActive) {
                        g_focusEngine->warningActive = false;
                        g_notifyFrames = 7; // Display notification for 7 seconds
                        if (!g_hNotifyWnd) {
                            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                            // Slide in slightly above the taskbar on the right side
                            g_hNotifyWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                L"DotIndicatorClass", L"Notify", WS_POPUP, screenWidth - 350 - 20, screenHeight - 150, 350, 90, NULL, NULL, GetModuleHandle(NULL), NULL);
                            ShowWindow(g_hNotifyWnd, SW_SHOWNA);
                        }
                        DrawNotifyWidget(g_hNotifyWnd, g_focusEngine->warningMsg.c_str());
                    }
                }
                
                if (g_notifyFrames > 0) {
                    g_notifyFrames--;
                    if (g_notifyFrames == 0 && g_hNotifyWnd) {
                        DestroyWindow(g_hNotifyWnd);
                        g_hNotifyWnd = NULL;
                    }
                }

                if (g_hDotWnd && g_focusEngine) DrawDotIndicator(g_hDotWnd, g_focusEngine->currentState);
                DrawWidget(hWnd);
                if (g_hFocusWidgetWnd) DrawFocusWidget(g_hFocusWidgetWnd);
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
            if (g_focusEngine) {
                delete g_focusEngine;
                g_focusEngine = nullptr;
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

    // Register Dot Indicator Window Class
    const wchar_t DOT_CLASS_NAME[] = L"DotIndicatorClass";
    WNDCLASSW dwc = { 0 };
    dwc.lpfnWndProc = DotWndProc;
    dwc.hInstance = hInstance;
    dwc.lpszClassName = DOT_CLASS_NAME;
    RegisterClassW(&dwc);

    // Register Focus Widget Window Class
    const wchar_t FOCUS_CLASS_NAME[] = L"FocusWidgetClass";
    WNDCLASSW fwc = { 0 };
    fwc.lpfnWndProc = FocusWndProc;
    fwc.hInstance = hInstance;
    fwc.lpszClassName = FOCUS_CLASS_NAME;
    fwc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&fwc);

    // 3) Create floating, unowned window (Parent = NULL)
    // WS_EX_LAYERED: Required for UpdateLayeredWindow
    // WS_EX_TOOLWINDOW: Hides from Alt-Tab
    // WS_EX_NOACTIVATE: Prevents stealing focus
    // WS_POPUP: Barebones popup window with no border
    
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int hwX = screenWidth - 340 - 20;

    g_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLASS_NAME,
        L"Desktop Widget",
        WS_POPUP,
        hwX, 20, 340, 430,   // Hardware widget parked top right
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

    g_hDotWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        DOT_CLASS_NAME,
        L"Dot",
        WS_POPUP,
        0, 0, 32, 32,
        NULL, NULL, hInstance, NULL
    );

    int focusX = screenWidth - 400 - 20;
    g_hFocusWidgetWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        FOCUS_CLASS_NAME,
        L"Focus Analytics",
        WS_POPUP,
        focusX, 470, 400, 480,   // Window height extended slightly for subtle divider separator element
        NULL, NULL, hInstance, NULL
    );

    // 4) Register the EVENT_SYSTEM_FOREGROUND WinEventHook
    g_hEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    // 5) Show but do NOT steal focus (SW_SHOWNA)
    ShowWindow(g_hWnd, SW_SHOWNA);
    if (g_hFocusWidgetWnd) ShowWindow(g_hFocusWidgetWnd, SW_SHOWNA);
    if (g_hDotWnd) ShowWindow(g_hDotWnd, SW_SHOWNA);

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
    if (g_focusEngine) {
        delete g_focusEngine;
        g_focusEngine = nullptr;
    }
    
    CleanupFonts();
    GdiplusShutdown(g_gdiplusToken);

    return (int) msg.wParam;
}
