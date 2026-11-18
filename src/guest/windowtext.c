#include "agent.h"

/* Centralize every tools window-text read, including top-level windows.
   Never send WM_GETTEXT to a password control: redaction must happen before
   credentials reach a response buffer or any host-side formatter. */
void v9x_window_text(HWND window, char *text, unsigned long size)
{
    DWORD copied = 0ul;
    char class_name[64];
    if (size == 0ul) return;
    text[0] = '\0';
    class_name[0] = '\0';
    (void)GetClassNameA(window, class_name, sizeof(class_name));
    if ((lstrcmpiA(class_name, "Edit") == 0 ||
         lstrcmpiA(class_name, "RichEdit20A") == 0 ||
         lstrcmpiA(class_name, "RichEdit50W") == 0 ||
         lstrcmpiA(class_name, "RichEdit") == 0) &&
        (GetWindowLongA(window, GWL_STYLE) & ES_PASSWORD) != 0) {
        lstrcpynA(text, "[password redacted]", (int)size);
        return;
    }
    if (SendMessageTimeoutA(window, WM_GETTEXT, (WPARAM)size, (LPARAM)text,
                            SMTO_ABORTIFHUNG, 500u, &copied) == 0) {
        text[0] = '\0';
    }
    text[size - 1ul] = '\0';
}
