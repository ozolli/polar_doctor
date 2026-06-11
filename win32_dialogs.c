#include "polar_doctor.h"

#ifdef _WIN32

// Convertit UTF-8 vers wide char pour Windows
wchar_t* utf8_to_wchar(const char *utf8) {
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len == 0) return NULL;
    wchar_t *wstr = (wchar_t*)malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, len);
    return wstr;
}

// Convertit wide char vers UTF-8
char* wchar_to_utf8(const wchar_t *wstr) {
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len == 0) return NULL;
    char *utf8 = (char*)malloc(len);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, len, NULL, NULL);
    return utf8;
}

// Dialogue "Enregistrer sous" natif Windows
char* win32_save_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                                const char *filter_pattern, const char *default_name) {
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};

    // Convertir le titre
    wchar_t *wtitle = utf8_to_wchar(title);

    // Préparer le nom par défaut
    if (default_name) {
        wchar_t *wdefault = utf8_to_wchar(default_name);
        if (wdefault) {
            wcsncpy(szFile, wdefault, MAX_PATH - 1);
            free(wdefault);
        }
    }

    // Construire le filtre (format: "Description\0*.ext\0\0")
    wchar_t filter[256];
    if (filter_name && filter_pattern) {
        wchar_t *wfilter_name = utf8_to_wchar(filter_name);
        wchar_t *wfilter_pattern = utf8_to_wchar(filter_pattern);
        swprintf(filter, 256, L"%s%c%s%c%c", wfilter_name, L'\0', wfilter_pattern, L'\0', L'\0');
        free(wfilter_name);
        free(wfilter_pattern);
    } else {
        wcscpy(filter, L"All Files (*.*)\0*.*\0\0");
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;  // Pas de parent pour éviter les problèmes GTK
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    char *result = NULL;
    if (GetSaveFileNameW(&ofn) == TRUE) {
        result = wchar_to_utf8(szFile);
    }

    if (wtitle) free(wtitle);
    return result;
}

// Dialogue "Ouvrir" natif Windows
char* win32_open_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                                const char *filter_pattern) {
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};

    wchar_t *wtitle = utf8_to_wchar(title);

    // Construire le filtre
    wchar_t filter[256];
    if (filter_name && filter_pattern) {
        wchar_t *wfilter_name = utf8_to_wchar(filter_name);
        wchar_t *wfilter_pattern = utf8_to_wchar(filter_pattern);
        swprintf(filter, 256, L"%s%c%s%c%c", wfilter_name, L'\0', wfilter_pattern, L'\0', L'\0');
        free(wfilter_name);
        free(wfilter_pattern);
    } else {
        wcscpy(filter, L"All Files (*.*)\0*.*\0\0");
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    char *result = NULL;
    if (GetOpenFileNameW(&ofn) == TRUE) {
        result = wchar_to_utf8(szFile);
    }

    if (wtitle) free(wtitle);
    return result;
}

// Dialogue "Ouvrir" multi-sélection natif Windows
GSList* win32_open_multi_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                                        const char *filter_pattern) {
    OPENFILENAMEW ofn;
    wchar_t szFile[8192] = {0};  // Buffer plus grand pour multi-sélection

    wchar_t *wtitle = utf8_to_wchar(title);

    // Construire le filtre
    wchar_t filter[512];
    if (filter_name && filter_pattern) {
        wchar_t *wfilter_name = utf8_to_wchar(filter_name);
        wchar_t *wfilter_pattern = utf8_to_wchar(filter_pattern);
        swprintf(filter, 512, L"%s%c%s%c%c", wfilter_name, L'\0', wfilter_pattern, L'\0', L'\0');
        free(wfilter_name);
        free(wfilter_pattern);
    } else {
        wcscpy(filter, L"All Files (*.*)\0*.*\0\0");
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    GSList *file_list = NULL;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        wchar_t *ptr = szFile;
        wchar_t dir[MAX_PATH];

        // Premier élément : répertoire ou fichier unique
        wcscpy(dir, ptr);
        ptr += wcslen(ptr) + 1;

        // Si ptr pointe sur une chaîne vide, c'est un fichier unique
        if (*ptr == L'\0') {
            char *filepath = wchar_to_utf8(dir);
            if (filepath) {
                file_list = g_slist_append(file_list, filepath);
            }
        } else {
            // Multi-sélection : dir contient le répertoire, puis les noms de fichiers
            while (*ptr) {
                wchar_t fullpath[MAX_PATH];
                swprintf(fullpath, MAX_PATH, L"%s\\%s", dir, ptr);
                char *filepath = wchar_to_utf8(fullpath);
                if (filepath) {
                    file_list = g_slist_append(file_list, filepath);
                }
                ptr += wcslen(ptr) + 1;
            }
        }
    }

    if (wtitle) free(wtitle);
    return file_list;
}

#endif  // _WIN32
