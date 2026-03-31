#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <uiautomation.h>
#pragma comment(lib, "uiautomationcore.lib")
#include <psapi.h>
#include <algorithm>
#include <unordered_map>

enum FocusState {
    EARNING_GOOD,
    SPENDING_ENTERTAINMENT,
    SPENDING_NSFW,
    AFK_PAUSED,
    NEUTRAL
};

class FocusEngine {
public:
    int earnedTokensSeconds = 0;       // User earns tokens by doing 'Good Jobs'
    int nsfwRemainingSeconds = 7200;   // 2 hours limit (7200 seconds)
    
    int afkTimeoutMs = 120000;         // 120s of no keyboard/mouse -> AFK
    int activityRequiredForWakeMs = 60000; // Must be active for 60s to start earning again

    FocusState currentState = NEUTRAL;
    DWORD lastActiveTick = 0;
    int activeTicksSinceAfk = 0;
    bool isAfk = false;

    bool warningActive = false;
    std::wstring warningMsg = L"";
    bool warned5m = false;
    bool warned1m = false;

    std::unordered_map<std::wstring, int> usageTracker;
    int totalActiveScreenTimeSeconds = 0;

    std::vector<std::pair<std::wstring, int>> GetTopUsage(int limit) {
        std::vector<std::pair<std::wstring, int>> vec(usageTracker.begin(), usageTracker.end());
        std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
        if (vec.size() > limit) vec.resize(limit);
        return vec;
    }

    IUIAutomation* pUIAutomation = nullptr;
    
    // Categorization 
    std::vector<std::wstring> goodApps = { L"code.exe", L"devenv.exe", L"idea64.exe", L"studio64.exe", L"blender.exe", L"cursor.exe", L"antigravity.exe", L"code-insiders.exe" };
    std::vector<std::wstring> goodSites = { L"chatgpt.com", L"gemini.google.com", L"github.com", L"stackoverflow.com" };
    std::vector<std::wstring> socialSites = { L"x.com", L"instagram.com", L"reddit.com", L"discord.com", L"snapchat.com", L"twitter.com", L"facebook.com" };
    std::vector<std::wstring> mediaSites = { L"youtube.com", L"netflix.com", L"primevideo.com", L"hulu.com", L"disneyplus.com", L"twitch.tv" };
    std::vector<std::wstring> nsfwSites = { L"pornhub.com", L"xvideos.com", L"rule34.xxx", L"onlyfans.com" };

    FocusEngine() {
        // CoInitialize is managed by the main WMI init, but we ensure UIA is created
        CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&pUIAutomation);
    }
    
    ~FocusEngine() {
        if (pUIAutomation) pUIAutomation->Release();
    }

    std::wstring GetActiveExeName(HWND hForeground) {
        DWORD pid;
        GetWindowThreadProcessId(hForeground, &pid);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        std::wstring exeName = L"";
        if (hProcess) {
            wchar_t processPath[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
                wchar_t* name = wcsrchr(processPath, L'\\');
                if (name) exeName = std::wstring(name + 1);
            }
            CloseHandle(hProcess);
        }
        std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::towlower);
        return exeName;
    }

    std::wstring ExtractUrlFromBrowser(HWND hWnd) {
        if (!pUIAutomation) return L"";
        
        IUIAutomationElement* pWindowElement = nullptr;
        if (FAILED(pUIAutomation->ElementFromHandle(hWnd, &pWindowElement)) || !pWindowElement) {
            return L"";
        }

        // We use TreeScope_Descendants to find the Edit/Document Control which represents the URL bar
        IUIAutomationCondition* pCondition = nullptr;
        VARIANT varEdit;
        varEdit.vt = VT_I4;
        varEdit.lVal = UIA_EditControlTypeId;
        
        std::wstring extractedUrl = L"";

        if (SUCCEEDED(pUIAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varEdit, &pCondition))) {
            IUIAutomationElement* pEditElement = nullptr;
            
            // Note: In Chrome/Edge, the URL bar is typically the first EditControl.
            if (SUCCEEDED(pWindowElement->FindFirst(TreeScope_Descendants, pCondition, &pEditElement)) && pEditElement) {
                IUIAutomationValuePattern* pValuePattern = nullptr;
                if (SUCCEEDED(pEditElement->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pValuePattern))) && pValuePattern) {
                    BSTR val = nullptr;
                    if (SUCCEEDED(pValuePattern->get_CurrentValue(&val)) && val) {
                        extractedUrl = val;
                        SysFreeString(val);
                        
                        // Strip protocol and www.
                        size_t pfx = extractedUrl.find(L"://");
                        if (pfx != std::wstring::npos) extractedUrl = extractedUrl.substr(pfx + 3);
                        if (extractedUrl.find(L"www.") == 0) extractedUrl = extractedUrl.substr(4);
                        
                        // Strip internal paths, queries, and fragments so "x.com/home" becomes just "x.com"
                        size_t trailing = extractedUrl.find_first_of(L"/?#");
                        if (trailing != std::wstring::npos) extractedUrl = extractedUrl.substr(0, trailing);
                    }
                    pValuePattern->Release();
                }
                pEditElement->Release();
            }
            pCondition->Release();
        }
        pWindowElement->Release();
        
        std::transform(extractedUrl.begin(), extractedUrl.end(), extractedUrl.begin(), ::towlower);
        return extractedUrl;
    }

    void CloseActiveBrowserTab() {
        INPUT inputs[4] = { 0 };

        // Press Ctrl
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_CONTROL;
        
        // Press W
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = 'W';
        
        // Release W
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wVk = 'W';
        inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
        
        // Release Ctrl
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wVk = VK_CONTROL;
        inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(4, inputs, sizeof(INPUT));
    }

    void ProcessTick() {
        // 1. AFK Detection via GetLastInputInfo
        LASTINPUTINFO lii;
        lii.cbSize = sizeof(LASTINPUTINFO);
        if (GetLastInputInfo(&lii)) {
            DWORD currentTick = GetTickCount();
            DWORD idleTime = currentTick - lii.dwTime;

            if (idleTime > afkTimeoutMs) {
                isAfk = true;
                activeTicksSinceAfk = 0;
                currentState = AFK_PAUSED;
                return;
            } else {
                if (isAfk) {
                    activeTicksSinceAfk += 1000; 
                    if (activeTicksSinceAfk >= activityRequiredForWakeMs) {
                        isAfk = false;
                    } else {
                        currentState = AFK_PAUSED;
                        return; // Lock active until threshold hit
                    }
                }
            }
        }

        // 2. Identify the active application
        HWND hForeground = GetForegroundWindow();
        if (!hForeground) {
            currentState = NEUTRAL;
            return;
        }

        std::wstring exeName = GetActiveExeName(hForeground);
        std::wstring trackingName = exeName;

        if (exeName == L"chrome.exe" || exeName == L"msedge.exe" || exeName == L"firefox.exe" || exeName == L"brave.exe") {
            std::wstring url = ExtractUrlFromBrowser(hForeground);
            if (!url.empty()) trackingName = url;
            
            for (const auto& site : nsfwSites) {
                if (trackingName.find(site) != std::wstring::npos) {
                    currentState = SPENDING_NSFW;
                    if (nsfwRemainingSeconds > 0) {
                        nsfwRemainingSeconds--;
                    } else {
                        CloseActiveBrowserTab(); // Surgically kill only the active bad tab
                    }
                    goto TrackerUpdate;
                }
            }
            for (const auto& site : socialSites) {
                if (trackingName.find(site) != std::wstring::npos) {
                    currentState = SPENDING_ENTERTAINMENT;
                    if (earnedTokensSeconds >= 4) {
                        earnedTokensSeconds -= 4;
                        if (earnedTokensSeconds <= 300 && !warned5m) {
                            warningActive = true;
                            warningMsg = L"Only 5 minutes of tokens remaining!";
                            warned5m = true;
                        } else if (earnedTokensSeconds <= 60 && !warned1m) {
                            warningActive = true;
                            warningMsg = L"Only 1 minute of tokens remaining!";
                            warned1m = true;
                        }
                    } else {
                        earnedTokensSeconds = 0;
                        CloseActiveBrowserTab(); // Surgically kill only the active bad tab
                    }
                    goto TrackerUpdate;
                }
            }
            for (const auto& site : mediaSites) {
                if (trackingName.find(site) != std::wstring::npos) {
                    currentState = SPENDING_ENTERTAINMENT;
                    if (earnedTokensSeconds >= 3) {
                        earnedTokensSeconds -= 3;
                        if (earnedTokensSeconds <= 300 && !warned5m) {
                            warningActive = true;
                            warningMsg = L"Only 5 minutes of tokens remaining!";
                            warned5m = true;
                        } else if (earnedTokensSeconds <= 60 && !warned1m) {
                            warningActive = true;
                            warningMsg = L"Only 1 minute of tokens remaining!";
                            warned1m = true;
                        }
                    } else {
                        earnedTokensSeconds = 0;
                        CloseActiveBrowserTab(); // Surgically kill only the active bad tab
                    }
                    goto TrackerUpdate;
                }
            }
            for (const auto& site : goodSites) {
                if (trackingName.find(site) != std::wstring::npos) {
                    currentState = EARNING_GOOD;
                    earnedTokensSeconds++;
                    if (earnedTokensSeconds > 300) warned5m = false;
                    if (earnedTokensSeconds > 60) warned1m = false;
                    goto TrackerUpdate;
                }
            }
            currentState = NEUTRAL;
            goto TrackerUpdate;
        }

        // 3. Evaluate Good Apps natively
        for (const auto& goodApp : goodApps) {
            if (exeName == goodApp) {
                currentState = EARNING_GOOD;
                earnedTokensSeconds++;
                if (earnedTokensSeconds > 300) warned5m = false;
                if (earnedTokensSeconds > 60) warned1m = false;
                goto TrackerUpdate;
            }
        }

        currentState = NEUTRAL;

    TrackerUpdate:
        if (trackingName.empty()) trackingName = L"unknown";
        usageTracker[trackingName]++;
        totalActiveScreenTimeSeconds++;
    }

    FocusState GetCategoryForName(const std::wstring& name) {
        for (const auto& site : nsfwSites) {
            if (name.find(site) != std::wstring::npos) return SPENDING_NSFW;
        }
        for (const auto& site : socialSites) {
            if (name.find(site) != std::wstring::npos) return SPENDING_ENTERTAINMENT;
        }
        for (const auto& site : mediaSites) {
            if (name.find(site) != std::wstring::npos) return SPENDING_ENTERTAINMENT;
        }
        for (const auto& site : goodSites) {
            if (name.find(site) != std::wstring::npos) return EARNING_GOOD;
        }
        for (const auto& app : goodApps) {
            if (name.find(app) != std::wstring::npos) return EARNING_GOOD;
        }
        return NEUTRAL;
    }
};
