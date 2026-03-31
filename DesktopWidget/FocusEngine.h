#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <uiautomation.h>
#pragma comment(lib, "uiautomationcore.lib")
#include <psapi.h>
#include <algorithm>
#include <unordered_map>
#include <deque>
#include <ctime>

struct TickAction {
    int tokenDelta;
    int nsfwDelta;
    std::wstring trackerKey;
};

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
    
    std::deque<TickAction> tickHistory;
    std::deque<POINT> mouseHistory;

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

    std::wstring saveFilePath;
    int ticksSinceLastSave = 0;
    time_t lastNsfwRefreshTime = 0;

    FocusEngine(const std::wstring& path) {
        saveFilePath = path;
        CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&pUIAutomation);
        LoadState();
        
        // 14-Day Strict Bi-Weekly Reset Bound natively
        time_t now = time(NULL);
        if (lastNsfwRefreshTime == 0 || (now - lastNsfwRefreshTime) >= 1209600) { // 1,209,600 seconds = 14 Days
            nsfwRemainingSeconds = 7200; // Exact 2 Hours Limit
            lastNsfwRefreshTime = now;
            SaveState(); // Instantly serialize the new epoch timeline
        }
    }
    
    ~FocusEngine() {
        SaveState();
        if (pUIAutomation) pUIAutomation->Release();
    }

    void SaveState() {
        if (saveFilePath.empty()) return;
        std::vector<uint8_t> buffer;
        
        auto pushInt = [&](int val) {
            uint8_t* p = (uint8_t*)&val;
            buffer.insert(buffer.end(), p, p + sizeof(int));
        };
        auto pushSize = [&](size_t val) {
            uint8_t* p = (uint8_t*)&val;
            buffer.insert(buffer.end(), p, p + sizeof(size_t));
        };
        auto pushTime = [&](time_t val) {
            uint8_t* p = (uint8_t*)&val;
            buffer.insert(buffer.end(), p, p + sizeof(time_t));
        };
        
        pushInt(earnedTokensSeconds);
        pushInt(nsfwRemainingSeconds);
        pushInt(totalActiveScreenTimeSeconds);
        pushTime(lastNsfwRefreshTime);
        pushSize(usageTracker.size());
        
        for (const auto& kv : usageTracker) {
            pushSize(kv.first.length());
            uint8_t* strP = (uint8_t*)kv.first.c_str();
            buffer.insert(buffer.end(), strP, strP + (kv.first.length() * sizeof(wchar_t)));
            pushInt(kv.second);
        }
        
        unsigned int checksum = 0;
        for (uint8_t b : buffer) checksum += b;
        
        uint8_t* chkP = (uint8_t*)&checksum;
        buffer.insert(buffer.end(), chkP, chkP + sizeof(unsigned int));
        
        // Anti-tamper XOR Encrypt
        const char key[] = "DEEPWORK_FOCUS_SECURE_KEY_2026!";
        size_t keyLen = sizeof(key) - 1;
        for (size_t i = 0; i < buffer.size(); i++) {
            buffer[i] ^= key[i % keyLen];
        }
        
        FILE* f;
        if (_wfopen_s(&f, saveFilePath.c_str(), L"wb") == 0) {
            fwrite(buffer.data(), 1, buffer.size(), f);
            fclose(f);
        }
    }

    void LoadState() {
        if (saveFilePath.empty()) return;
        FILE* f;
        if (_wfopen_s(&f, saveFilePath.c_str(), L"rb") != 0) return;
        
        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        rewind(f);
        if (fileSize < (sizeof(int)*3 + sizeof(size_t) + sizeof(unsigned int))) { fclose(f); return; }
        
        std::vector<uint8_t> buffer(fileSize);
        if (fread(buffer.data(), 1, fileSize, f) != fileSize) { fclose(f); return; }
        fclose(f);
        
        // Decrypt
        const char key[] = "DEEPWORK_FOCUS_SECURE_KEY_2026!";
        size_t keyLen = sizeof(key) - 1;
        for (size_t i = 0; i < buffer.size(); i++) {
            buffer[i] ^= key[i % keyLen];
        }
        
        // Integrity Checksum Validation
        unsigned int fileChecksum = 0;
        memcpy(&fileChecksum, buffer.data() + buffer.size() - sizeof(unsigned int), sizeof(unsigned int));
        
        unsigned int computed = 0;
        for (size_t i = 0; i < buffer.size() - sizeof(unsigned int); i++) computed += buffer[i];
        if (computed != fileChecksum) return; // Tampering detected, reject file natively!
        
        size_t offset = 0;
        auto popInt = [&]() -> int {
            int val = 0;
            if (offset + sizeof(int) <= buffer.size()) { memcpy(&val, buffer.data() + offset, sizeof(int)); offset += sizeof(int); }
            return val;
        };
        auto popSize = [&]() -> size_t {
            size_t val = 0;
            if (offset + sizeof(size_t) <= buffer.size()) { memcpy(&val, buffer.data() + offset, sizeof(size_t)); offset += sizeof(size_t); }
            return val;
        };
        auto popTime = [&]() -> time_t {
            time_t val = 0;
            if (offset + sizeof(time_t) <= buffer.size()) { memcpy(&val, buffer.data() + offset, sizeof(time_t)); offset += sizeof(time_t); }
            return val;
        };
        
        int inTokens = popInt();
        int inNsfw = popInt();
        int inTotal = popInt();
        time_t inEpoch = popTime();
        
        // Math Boundary Hard Limits
        if (inTokens < 0 || inTokens > 24 * 3600) return; // Cap at 24 hours
        if (inNsfw < 0 || inNsfw > 7200) return;         // Cap strictly at 2 hours
        if (inTotal < 0) return;
        
        size_t mapSize = popSize();
        if (mapSize > 10000) return; 
        
        std::unordered_map<std::wstring, int> tempMap;
        for (size_t i = 0; i < mapSize; i++) {
            size_t strLen = popSize();
            if (strLen == 0 || strLen > 5000 || offset + strLen * sizeof(wchar_t) > buffer.size() - sizeof(unsigned int)) return;
            
            std::wstring keyStr;
            keyStr.resize(strLen);
            memcpy(&keyStr[0], buffer.data() + offset, strLen * sizeof(wchar_t));
            offset += strLen * sizeof(wchar_t);
            
            tempMap[keyStr] = popInt();
        }
        
        earnedTokensSeconds = inTokens;
        nsfwRemainingSeconds = inNsfw;
        totalActiveScreenTimeSeconds = inTotal;
        lastNsfwRefreshTime = inEpoch;
        usageTracker = std::move(tempMap);
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
        // Track rolling mouse history (60 seconds)
        POINT pt;
        if (GetCursorPos(&pt)) {
            mouseHistory.push_back(pt);
            if (mouseHistory.size() > 60) mouseHistory.pop_front();
        }

        // -------------------------------------------------------------
        // PHASE 1: IMMUTABLE GLOBAL ANALYTICS (Never pauses)
        // -------------------------------------------------------------
        HWND hForeground = GetForegroundWindow();
        std::wstring exeName = L"";
        std::wstring trackingName = L"unknown";
        std::wstring titleStr = L"";
        bool isIncognito = false;

        if (hForeground) {
            exeName = GetActiveExeName(hForeground);
            trackingName = exeName;
            
            WCHAR windowTitle[MAX_PATH] = { 0 };
            GetWindowTextW(hForeground, windowTitle, MAX_PATH);
            titleStr = windowTitle;
            std::transform(titleStr.begin(), titleStr.end(), titleStr.begin(), ::towlower);

            isIncognito = (titleStr.find(L"incognito") != std::wstring::npos ||
                           titleStr.find(L"inprivate") != std::wstring::npos ||
                           titleStr.find(L"private browsing") != std::wstring::npos);

            if (exeName == L"chrome.exe" || exeName == L"msedge.exe" || exeName == L"firefox.exe" || exeName == L"brave.exe") {
                std::wstring url = ExtractUrlFromBrowser(hForeground);
                if (!url.empty()) trackingName = url;
                else trackingName = titleStr; // UIA Fallback
                
                // Privacy scrub logic (extract exact category strings unconditionally)
                auto MatchesTitle = [](const std::wstring& s, const std::wstring& t) {
                    if (t.find(s) != std::wstring::npos) return true;
                    // Strip extensions aggressively to locate root site name inside raw Chromium Window titles
                    std::wstring base = s;
                    size_t www = base.find(L"www.");
                    if (www == 0) base = base.substr(4);
                    size_t p = base.find_last_of(L".");
                    if (p != std::wstring::npos) base = base.substr(0, p);
                    if (base.length() > 3 && t.find(base) != std::wstring::npos) return true;
                    return false;
                };

                bool matchedCat = false;
                for (const auto& site : nsfwSites) {
                    if (trackingName.find(site) != std::wstring::npos || MatchesTitle(site, titleStr)) {
                        trackingName = L"NSFW Content"; matchedCat = true; break;
                    }
                }
                
                // Broad Keyword Heuristics for obscure domains (SEO adult keywords)
                if (!matchedCat) {
                    std::vector<std::wstring> nsfwKeywords = {
                        L"porn", L" xxx ", L"nude", L"hentai", L"nsfw", L"xvideos", L"pornhub", L"xhamster", L"chaturbate", L"spankbang", L"eporner", L"hqporner", L"beeg", L"redtube", L"youporn", L"camgirl", L"cams", L"jerk", L"slut", L"milf"
                    };
                    for (const auto& kw : nsfwKeywords) {
                        if (titleStr.find(kw) != std::wstring::npos || trackingName.find(kw) != std::wstring::npos) {
                            trackingName = L"NSFW Content"; matchedCat = true; break;
                        }
                    }
                }

                if (!matchedCat) {
                    for (const auto& site : socialSites) {
                        if (trackingName.find(site) != std::wstring::npos || MatchesTitle(site, titleStr)) {
                            trackingName = site; matchedCat = true; break;
                        }
                    }
                }
                if (!matchedCat) {
                    for (const auto& site : mediaSites) {
                        if (trackingName.find(site) != std::wstring::npos || MatchesTitle(site, titleStr)) {
                            trackingName = site; matchedCat = true; break;
                        }
                    }
                }
                if (!matchedCat) {
                    for (const auto& site : goodSites) {
                        if (trackingName.find(site) != std::wstring::npos || MatchesTitle(site, titleStr)) {
                            trackingName = site; matchedCat = true; break;
                        }
                    }
                }
                if (!matchedCat) {
                    trackingName = L"Web Browsing (" + exeName + L")"; 
                }
            } else {
                // Not a browser, leave trackingName as exeName unconditionally
            }
        }
        
        if (trackingName.empty()) trackingName = L"unknown";
        
        // Unconditionally evaluate Usage Matrix regardless of AFK status natively
        usageTracker[trackingName]++;
        totalActiveScreenTimeSeconds++;

        // -------------------------------------------------------------
        // PHASE 2: AFK DETECTION (Only gates Token Economy calculations)
        // -------------------------------------------------------------
        LASTINPUTINFO lii;
        lii.cbSize = sizeof(LASTINPUTINFO);
        bool forceAfk = false;

        if (GetLastInputInfo(&lii)) {
            DWORD currentTick = GetTickCount();
            DWORD idleTime = currentTick - lii.dwTime;

            // Bounding Box Anti-Cheat Check
            bool jigglerDetected = false;
            if (mouseHistory.size() >= 60) {
                long minX = mouseHistory[0].x, maxX = mouseHistory[0].x;
                long minY = mouseHistory[0].y, maxY = mouseHistory[0].y;
                for (const auto& p : mouseHistory) {
                    if (p.x < minX) minX = p.x;
                    if (p.x > maxX) maxX = p.x;
                    if (p.y < minY) minY = p.y;
                    if (p.y > maxY) maxY = p.y;
                }
                if ((maxX - minX) <= 30 && (maxY - minY) <= 30) {
                    jigglerDetected = true;
                }
            }

            DWORD afkThreshold = 90000;
            if (currentState == SPENDING_ENTERTAINMENT || currentState == SPENDING_NSFW) {
                afkThreshold = 3600000; // 1 Hour (Media)
            } else if (currentState == EARNING_GOOD || currentState == NEUTRAL) {
                afkThreshold = 1800000; // 30 Minutes
            }

            if (idleTime > afkThreshold || jigglerDetected) {
                forceAfk = true;
            } else {
                if (isAfk) {
                    activeTicksSinceAfk += 1000; 
                    if (activeTicksSinceAfk >= activityRequiredForWakeMs) {
                        isAfk = false;
                        tickHistory.clear(); 
                        mouseHistory.clear();
                    } else {
                        currentState = AFK_PAUSED;
                        goto SaveBlock; // Skip token math
                    }
                }
            }
        }

        if (forceAfk) {
            if (!isAfk) {
                DWORD rollbackSeconds = 90;
                if (currentState == SPENDING_ENTERTAINMENT || currentState == SPENDING_NSFW) rollbackSeconds = 3600;
                else if (currentState == EARNING_GOOD || currentState == NEUTRAL) rollbackSeconds = 1800;

                int itemsToPop = min(static_cast<int>(tickHistory.size()), static_cast<int>(rollbackSeconds)); 
                while (itemsToPop > 0 && !tickHistory.empty()) {
                    TickAction action = tickHistory.back();
                    tickHistory.pop_back();

                    // Specifically roll back the tokens ONLY (DO NOT roll back the raw analytics array)!
                    earnedTokensSeconds -= action.tokenDelta;
                    if (earnedTokensSeconds < 0) earnedTokensSeconds = 0;
                    
                    nsfwRemainingSeconds -= action.nsfwDelta;
                    if (nsfwRemainingSeconds < 0) nsfwRemainingSeconds = 0;
                    if (nsfwRemainingSeconds > 7200) nsfwRemainingSeconds = 7200;

                    itemsToPop--;
                }
                isAfk = true;
            }
            activeTicksSinceAfk = 0;
            currentState = AFK_PAUSED;
            goto SaveBlock; // Skip token math
        }

        // -------------------------------------------------------------
        // PHASE 3: THE TOKEN ECONOMY MATH (Executes exclusively while awake)
        // -------------------------------------------------------------
        {
            int initialTokens = earnedTokensSeconds;
            int initialNsfw = nsfwRemainingSeconds;
            
            bool isGoodApp = false;
            for (const auto& app : goodApps) {
                if (exeName == app) { isGoodApp = true; break; }
            }

            if (trackingName == L"NSFW Content") {
                currentState = SPENDING_NSFW;
                if (nsfwRemainingSeconds > 0) nsfwRemainingSeconds--;
                else CloseActiveBrowserTab();
            } else {
                bool matchedEco = false;
                for (const auto& site : socialSites) {
                    if (trackingName == site) { // trackingName is cleanly scrubbed by Phase 1 string assignments natively
                        currentState = SPENDING_ENTERTAINMENT;
                        if (earnedTokensSeconds >= 4) earnedTokensSeconds -= 4;
                        else { earnedTokensSeconds = 0; CloseActiveBrowserTab(); }
                        matchedEco = true; break;
                    }
                }
                if (!matchedEco) {
                    for (const auto& site : mediaSites) {
                        if (trackingName == site) {
                            currentState = SPENDING_ENTERTAINMENT;
                            if (earnedTokensSeconds >= 3) earnedTokensSeconds -= 3;
                            else { earnedTokensSeconds = 0; CloseActiveBrowserTab(); }
                            matchedEco = true; break;
                        }
                    }
                }
                if (!matchedEco) {
                    for (const auto& site : goodSites) {
                        if (trackingName == site) {
                            currentState = EARNING_GOOD;
                            earnedTokensSeconds++;
                            matchedEco = true; break;
                        }
                    }
                }
                if (!matchedEco && isGoodApp) {
                    currentState = EARNING_GOOD;
                    earnedTokensSeconds++;
                    matchedEco = true;
                }
                
                if (!matchedEco) {
                    currentState = NEUTRAL;
                }
            }

            // Warning visual triggers intelligently decoupled
            if (currentState == SPENDING_ENTERTAINMENT && earnedTokensSeconds > 0) {
                if (earnedTokensSeconds <= 300 && !warned5m) {
                    warningActive = true; warningMsg = L"Only 5 minutes of tokens remaining!"; warned5m = true;
                } else if (earnedTokensSeconds <= 60 && !warned1m) {
                    warningActive = true; warningMsg = L"Only 1 minute of tokens remaining!"; warned1m = true;
                }
            } else if (currentState == EARNING_GOOD) {
                if (earnedTokensSeconds > 300) warned5m = false;
                if (earnedTokensSeconds > 60) warned1m = false;
            }

            // Append exclusively token economy changes into the rollback engine
            int currentTokenDelta = earnedTokensSeconds - initialTokens;
            int currentNsfwDelta = nsfwRemainingSeconds - initialNsfw;
            tickHistory.push_back({ currentTokenDelta, currentNsfwDelta, L"" });
            if (tickHistory.size() > 3600) tickHistory.pop_front();
        }

    SaveBlock:
        ticksSinceLastSave++;
        if (ticksSinceLastSave >= 60) {
            SaveState();
            ticksSinceLastSave = 0;
        }
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
