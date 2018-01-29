// D3D11NV12Rendering.cpp : Defines the entry point for the application.
//

#include "D3D11NV12Rendering.h"
#include "OutputManager.h"

#define MAX_LOADSTRING 100
char buf[1024];

struct NV12Frame
{
	UINT width;
	UINT height;
	UINT pitch;
	BYTE *Y;
	BYTE *UV;
};

//
// Globals
//
OUTPUTMANAGER OutMgr;

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
HWND hWnd;										// The window handle created

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

void WriteNV12ToTexture(NV12Frame *nv12Frame);
NV12Frame* ReadNV12FromFile();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_D3D11NV12RENDERING, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_D3D11NV12RENDERING));

	RECT DeskBounds;
	bool FirstTime = true;
	bool Occluded = true;

	MSG msg = { 0 };

	while (WM_QUIT != msg.message)
	{
		DUPL_RETURN Ret = DUPL_RETURN_SUCCESS;
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == OCCLUSION_STATUS_MSG)
			{
				// Present may not be occluded now so try again
				Occluded = false;
			}
			else
			{
				// Process window messages
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
		else if (FirstTime)
		{

			// First time through the loop so nothing to clean up
			FirstTime = false;

			// Re-initialize
			Ret = OutMgr.InitOutput(hWnd, &DeskBounds);

			// We start off in occluded state and we should immediate get a occlusion status window message
			Occluded = true;
		}
		else
		{
			// Nothing else to do, so try to present to write out to window if not occluded
			if (!Occluded)
			{
				NV12Frame *nv12Frame = ReadNV12FromFile();
				WriteNV12ToTexture(nv12Frame);
				free(nv12Frame->Y);
				free(nv12Frame->UV);
				free(nv12Frame);

				Ret = OutMgr.UpdateApplicationWindow(&Occluded);
			}
		}
	}

	if (msg.message == WM_QUIT)
	{
		OutMgr.CleanRefs();
		// For a WM_QUIT message we should return the wParam value
		return static_cast<INT>(msg.wParam);
	}

    return (int) msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_D3D11NV12RENDERING));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName = nullptr;// MAKEINTRESOURCEW(IDC_D3D11NV12RENDERING);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
	{
		PostQuitMessage(0);
		break;
	}
	case WM_SIZE:
	{
		// Tell output manager that window size has changed
		OutMgr.WindowResize();
		break;
	}
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

NV12Frame* ReadNV12FromFile()
{

	FILE *file = nullptr;
	sprintf_s(buf, "content\\16.nv12");
	fopen_s(&file, buf, "rb");

	int size = sizeof(NV12Frame);
	NV12Frame *nv12Frame = (NV12Frame*)malloc(size);
	int readBytes = fread(nv12Frame, size, 1, file);

	size = nv12Frame->pitch * nv12Frame->height;
	nv12Frame->Y = (BYTE *)malloc(size);
	readBytes = fread(nv12Frame->Y, size, 1, file);

	size = nv12Frame->pitch * nv12Frame->height / 2;
	nv12Frame->UV = (BYTE *)malloc(size);
	readBytes = fread(nv12Frame->UV, size, 1, file);

	fclose(file);

	return nv12Frame;
}

void WriteNV12ToTexture(NV12Frame *nv12Frame)
{
	// Copy from CPU access texture to bitmap buffer
	D3D11_MAPPED_SUBRESOURCE resource;
	UINT subresource = D3D11CalcSubresource(0, 0, 0);
	OutMgr.m_DeviceContext->Map(OutMgr.m_texture, subresource, D3D11_MAP_WRITE_DISCARD, 0, &resource);

	BYTE* dptr = reinterpret_cast<BYTE*>(resource.pData);

	for (int i = 0; i < nv12Frame->height; i++)
	{
		memcpy(dptr + resource.RowPitch * i, nv12Frame->Y + nv12Frame->pitch * i, nv12Frame->pitch);
	}

	for (int i = 0; i < nv12Frame->height / 2; i++)
	{
		memcpy(dptr + resource.RowPitch *(nv12Frame->height + i), nv12Frame->UV + nv12Frame->pitch * i, nv12Frame->pitch);
	}

	OutMgr.m_DeviceContext->Unmap(OutMgr.m_texture, subresource);
}