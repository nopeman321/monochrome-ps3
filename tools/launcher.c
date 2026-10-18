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

static int FindPythonPath(char *outPath, size_t maxLen) {
    static const char *candidates[] = {
        "C:\\Program Files\\Python311\\pythonw.exe",
        "C:\\Program Files\\Python311\\python.exe",
        "C:\\Program Files\\Python312\\pythonw.exe",
        "C:\\Program Files\\Python312\\python.exe",
        "C:\\Program Files\\Python310\\pythonw.exe",
        "C:\\Program Files\\Python310\\python.exe",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
            strncpy(outPath, candidates[i], maxLen);
            return 0;
        }
    }

    /* Check PATH for pythonw or python */
    char found[MAX_PATH];
    if (SearchPathA(NULL, "pythonw.exe", NULL, sizeof(found), found, NULL) > 0) {
        strncpy(outPath, found, maxLen);
        return 0;
    }
    if (SearchPathA(NULL, "python.exe", NULL, sizeof(found), found, NULL) > 0) {
        strncpy(outPath, found, maxLen);
        return 0;
    }

    /* Fallback default command */
    strncpy(outPath, "python", maxLen);
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Detect repository directory */
    char projectDir[MAX_PATH] = "C:\\Users\\Juanjo\\.gemini\\antigravity\\scratch\\ps3-monochrome";

    /* Check local repo path */
    char testPath[MAX_PATH];
    snprintf(testPath, sizeof(testPath), "%s\\web\\index.html", projectDir);
    if (GetFileAttributesA(testPath) == INVALID_FILE_ATTRIBUTES) {
        /* Fallback to exe location parent directory */
        GetModuleFileNameA(NULL, projectDir, MAX_PATH);
        char *s = strrchr(projectDir, '\\');
        if (s) *s = '\0';
        /* If inside bin\, go one level up */
        char *sub = strrchr(projectDir, '\\');
        if (sub && _stricmp(sub + 1, "bin") == 0) {
            *sub = '\0';
        }
    }

    /* Find python executable */
    char pythonExe[MAX_PATH];
    FindPythonPath(pythonExe, sizeof(pythonExe));

    /* Path to desktop_app.py */
    char scriptPath[MAX_PATH];
    snprintf(scriptPath, sizeof(scriptPath), "%s\\tools\\desktop_app.py", projectDir);

    char cmd[MAX_PATH * 3];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", pythonExe, scriptPath);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION pi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    DWORD flags = CREATE_NO_WINDOW;
    CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL, projectDir, &si, &pi);

    if (pi.hProcess) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    return 0;
}
