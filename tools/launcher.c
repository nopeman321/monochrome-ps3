/**
 * Monochrome Desktop Native Launcher
 * Starts the local backend server (hidden) and opens the standalone Monochrome window.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int FindEdgePath(char *outPath, size_t maxLen) {
    static const char *candidates[] = {
        "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
            strncpy(outPath, candidates[i], maxLen);
            return 0;
        }
    }
    return -1;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Detect repository directory */
    char projectDir[MAX_PATH] = "C:\\Users\\Juanjo\\.gemini\\antigravity\\scratch\\ps3-monochrome";

    /* Check local repo path */
    char testPath[MAX_PATH];
    snprintf(testPath, sizeof(testPath), "%s\\web\\index.html", projectDir);
    if (GetFileAttributesA(testPath) == INVALID_FILE_ATTRIBUTES) {
        /* Fallback to exe location */
        GetModuleFileNameA(NULL, projectDir, MAX_PATH);
        char *s = strrchr(projectDir, '\\');
        if (s) *s = '\0';
        s = strrchr(projectDir, '\\');
        if (s) *s = '\0';
    }

    /* Start Python daemon in background (hidden) */
    char scriptPath[MAX_PATH];
    snprintf(scriptPath, sizeof(scriptPath), "%s\\tools\\desktop_app.py", projectDir);

    char cmdServer[MAX_PATH * 2];
    snprintf(cmdServer, sizeof(cmdServer), "python \"%s\"", scriptPath);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION pi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    CreateProcessA(NULL, cmdServer, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, projectDir, &si, &pi);

    /* Wait briefly for server */
    Sleep(800);

    /* Launch Edge in Dedicated App Mode */
    char edgeExe[MAX_PATH];
    if (FindEdgePath(edgeExe, sizeof(edgeExe)) == 0) {
        char userDataDir[MAX_PATH];
        snprintf(userDataDir, sizeof(userDataDir), "%s\\bin\\edge_user_data", projectDir);

        char appCmd[MAX_PATH * 3];
        snprintf(appCmd, sizeof(appCmd),
                 "\"%s\" --app=http://127.0.0.1:7890 --user-data-dir=\"%s\" --window-size=1280,820 --autoplay-policy=no-user-gesture-required",
                 edgeExe, userDataDir);

        STARTUPINFOA siApp = { sizeof(STARTUPINFOA) };
        PROCESS_INFORMATION piApp = {0};
        CreateProcessA(NULL, appCmd, NULL, NULL, FALSE, 0, NULL, projectDir, &siApp, &piApp);
    } else {
        ShellExecuteA(NULL, "open", "http://127.0.0.1:7890", NULL, NULL, SW_SHOWNORMAL);
    }

    return 0;
}
