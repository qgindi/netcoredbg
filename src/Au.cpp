//Au: added file.

#include <stdio.h>
#include "Au.h"

void print(const char* s) noexcept {
	//MessageBoxA(0, s, "Debugger", MB_TOPMOST);
	HWND w = FindWindow(L"QM_Editor", nullptr);
	SendMessageA(w, WM_SETTEXT, -1, (LPARAM)s);
}

void print2(const char *format, ...) noexcept {
	char buffer[2000];
	va_list pArguments;
	va_start(pArguments, format);
	vsprintf(buffer,format,pArguments);
	va_end(pArguments);
	print(buffer);
}
