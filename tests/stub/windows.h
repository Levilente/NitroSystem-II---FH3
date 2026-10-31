// Host-test stand-in for <windows.h>, <xinput.h>, <mmsystem.h>: declarations only, enough for a syntax check.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
typedef void *HANDLE,*LPVOID,*HWND,*HMODULE,*HRSRC,*HGLOBAL,*HDC,*HFONT,*HGDIOBJ,*HBRUSH,*HINSTANCE,*HICON,*HCURSOR,*HMENU,*PVOID;
typedef unsigned long DWORD; typedef long LONG; typedef int BOOL; typedef unsigned short WORD; typedef unsigned char BYTE; typedef unsigned int UINT;
typedef long long LONGLONG; typedef uintptr_t WPARAM; typedef intptr_t LPARAM; typedef intptr_t LRESULT;
typedef const wchar_t *LPCWSTR; typedef const char *LPCSTR; typedef wchar_t *LPWSTR; typedef char *LPSTR; typedef unsigned long ULONG;
typedef unsigned short ATOM; typedef unsigned long COLORREF; typedef short SHORT; typedef unsigned char UCHAR;
#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define MEM_COMMIT 0x1000
#define PAGE_GUARD 0x100
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define EXCEPTION_EXECUTE_HANDLER 1


#define ERROR_SUCCESS 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define GENERIC_READ 0x80000000
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
typedef struct {void *BaseAddress;void *AllocationBase;DWORD AllocationProtect;size_t RegionSize;DWORD State;DWORD Protect;DWORD Type;} MEMORY_BASIC_INFORMATION;
typedef union {struct{DWORD LowPart;LONG HighPart;};LONGLONG QuadPart;} LARGE_INTEGER;
size_t VirtualQuery(const void*,MEMORY_BASIC_INFORMATION*,size_t);
HANDLE CreateThread(void*,size_t,DWORD(WINAPI*)(LPVOID),void*,DWORD,DWORD*);
BOOL CloseHandle(HANDLE);
LONG InterlockedIncrement(volatile LONG*); LONG InterlockedDecrement(volatile LONG*); LONG InterlockedExchangeAdd(volatile LONG*,LONG);
HANDLE GetProcessHeap(void); void *HeapAlloc(HANDLE,DWORD,size_t); BOOL HeapFree(HANDLE,DWORD,void*);
void Sleep(DWORD); uint64_t GetTickCount64(void); DWORD GetCurrentThreadId(void);
unsigned GetPrivateProfileIntW(LPCWSTR,LPCWSTR,int,LPCWSTR);
DWORD GetPrivateProfileStringW(LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,DWORD,LPCWSTR);
BOOL WritePrivateProfileStringW(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR);
HMODULE LoadLibraryW(LPCWSTR); void *GetProcAddress(HMODULE,LPCSTR);
BOOL GetModuleHandleExW(DWORD,LPCWSTR,HMODULE*);
HANDLE CreateFileW(LPCWSTR,DWORD,DWORD,void*,DWORD,DWORD,HANDLE);
BOOL GetFileSizeEx(HANDLE,LARGE_INTEGER*); BOOL ReadFile(HANDLE,void*,DWORD,DWORD*,void*);
BOOL SetFileAttributesW(LPCWSTR,DWORD); BOOL DeleteFileW(LPCWSTR); BOOL RemoveDirectoryW(LPCWSTR);
// xinput
#define XINPUT_GAMEPAD_DPAD_UP 1
#define XINPUT_GAMEPAD_DPAD_DOWN 2
#define XINPUT_GAMEPAD_DPAD_LEFT 4
#define XINPUT_GAMEPAD_DPAD_RIGHT 8
#define XINPUT_GAMEPAD_START 0x10
#define XINPUT_GAMEPAD_BACK 0x20
#define XINPUT_GAMEPAD_LEFT_THUMB 0x40
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x80
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30
typedef struct {WORD wButtons;BYTE bLeftTrigger,bRightTrigger;SHORT sThumbLX,sThumbLY,sThumbRX,sThumbRY;} XINPUT_GAMEPAD;
typedef struct {DWORD dwPacketNumber;XINPUT_GAMEPAD Gamepad;} XINPUT_STATE;
typedef struct {WORD wLeftMotorSpeed,wRightMotorSpeed;} XINPUT_VIBRATION;
// winmm
#define SND_MEMORY 4
#define SND_NODEFAULT 2
#define SND_ASYNC 1
#define SND_LOOP 8
#define SND_PURGE 0x40
BOOL PlaySoundW(LPCWSTR,HMODULE,DWORD);
#ifndef __clang__
#define __except(x) catch(...)
#endif
int _wcsicmp(const wchar_t*,const wchar_t*);
int swprintf_s(wchar_t*,size_t,const wchar_t*,...);
long _InterlockedIncrement(volatile long*);
typedef struct {LONG left,top,right,bottom;} RECT; typedef struct {LONG x,y;} POINT;
typedef struct {UINT cbSize,style;void*lpfnWndProc;int cbClsExtra,cbWndExtra;HINSTANCE hInstance;HICON hIcon;HCURSOR hCursor;HBRUSH hbrBackground;LPCWSTR lpszMenuName,lpszClassName;HICON hIconSm;} WNDCLASSEXW;
typedef struct {HDC hdc;BOOL fErase;RECT rcPaint;} PAINTSTRUCT; typedef struct {HWND hwnd;UINT message;WPARAM wParam;LPARAM lParam;DWORD time;POINT pt;} MSG;
typedef union {struct{DWORD LowPart;LONG HighPart;};LONGLONG QuadPart;} LARGE_INTEGER_;
#define WS_EX_LAYERED 1
#define WS_EX_TRANSPARENT 2
#define WS_EX_TOPMOST 4
#define WS_EX_TOOLWINDOW 8
#define WS_EX_NOACTIVATE 16
#define WS_POPUP 32
#define WM_PAINT 15
#define WM_NCHITTEST 132
#define WM_ERASEBKGND 20
#define HTTRANSPARENT (-1)
#define SW_HIDE 0
#define SW_SHOWNOACTIVATE 4
#define SWP_SHOWWINDOW 1
#define SWP_NOSIZE 2
#define SWP_NOMOVE 4
#define SWP_NOACTIVATE 8
#define LWA_COLORKEY 1
#define TRANSPARENT 1
#define PM_REMOVE 1
#define FW_NORMAL 400
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define DEFAULT_PITCH 0
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define RGB(r,g,b) ((COLORREF)((r)|((g)<<8)|((b)<<16)))
BOOL GetClientRect(HWND,RECT*); BOOL GetWindowRect(HWND,RECT*); HDC BeginPaint(HWND,PAINTSTRUCT*); BOOL EndPaint(HWND,const PAINTSTRUCT*);
HGDIOBJ SelectObject(HDC,HGDIOBJ); BOOL DeleteObject(HGDIOBJ); HFONT CreateFontA(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCSTR);
int SetBkMode(HDC,int); COLORREF SetTextColor(HDC,COLORREF); BOOL TextOutA(HDC,int,int,LPCSTR,int); int FillRect(HDC,const RECT*,HBRUSH); HBRUSH CreateSolidBrush(COLORREF);
ATOM RegisterClassExW(const WNDCLASSEXW*); HWND CreateWindowExW(DWORD,LPCWSTR,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
BOOL SetLayeredWindowAttributes(HWND,COLORREF,BYTE,DWORD); BOOL ShowWindow(HWND,int); BOOL SetWindowPos(HWND,HWND,int,int,int,int,UINT);
BOOL PeekMessageW(MSG*,HWND,UINT,UINT,UINT); BOOL TranslateMessage(const MSG*); LRESULT DispatchMessageW(const MSG*); LRESULT DefWindowProcW(HWND,UINT,WPARAM,LPARAM);
BOOL InvalidateRect(HWND,const RECT*,BOOL); BOOL DestroyWindow(HWND); BOOL IsWindowVisible(HWND); BOOL IsIconic(HWND); BOOL ClientToScreen(HWND,POINT*);
BOOL EnumWindows(BOOL(CALLBACK*)(HWND,LPARAM),LPARAM); DWORD GetWindowThreadProcessId(HWND,DWORD*); DWORD GetCurrentProcessId(void); HWND GetWindow(HWND,UINT);
BOOL QueryPerformanceFrequency(LARGE_INTEGER*); BOOL QueryPerformanceCounter(LARGE_INTEGER*); HWND GetForegroundWindow(void);
