/*
 * Wordpad implementation - Printing functions
 *
 * Copyright 2007-2008 by Alexander N. Sørnes <alex@thehandofagony.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <wingdi.h>
#include <winspool.h>
#include <winuser.h>
#include <richedit.h>
#include <commctrl.h>
#include <commdlg.h>

#include "wordpad.h"

static HGLOBAL devMode;
static HGLOBAL devNames;
static BOOL rwpPrinterDefaultsRequested;

static RECT margins;


static const WCHAR var_pagemargin[] = {'P','a','g','e','M','a','r','g','i','n',0};

static LPWSTR get_print_file_filter(HWND hMainWnd)
{
    static WCHAR wszPrintFilter[MAX_STRING_LEN*2+6+4+1];
    const WCHAR files_prn[] = {'*','.','P','R','N',0};
    const WCHAR files_all[] = {'*','.','*','\0'};
    LPWSTR p;
    HINSTANCE hInstance = GetModuleHandleW(0);

    p = wszPrintFilter;
    LoadStringW(hInstance, STRING_PRINTER_FILES_PRN, p, MAX_STRING_LEN);
    p += lstrlenW(p) + 1;
    lstrcpyW(p, files_prn);
    p += lstrlenW(p) + 1;
    LoadStringW(hInstance, STRING_ALL_FILES, p, MAX_STRING_LEN);
    p += lstrlenW(p) + 1;
    lstrcpyW(p, files_all);
    p += lstrlenW(p) + 1;
    *p = 0;

    return wszPrintFilter;
}

void registry_set_pagemargins(HKEY hKey)
{
    RegSetValueExW(hKey, var_pagemargin, 0, REG_BINARY, (LPBYTE)&margins, sizeof(RECT));
}

void registry_read_pagemargins(HKEY hKey)
{
    DWORD size = sizeof(RECT);

    if(!hKey || RegQueryValueExW(hKey, var_pagemargin, 0, NULL, (LPBYTE)&margins,
                     &size) != ERROR_SUCCESS || size != sizeof(RECT))
        SetRect(&margins, 1757, 1417, 1757, 1417);
}

void get_default_printer_opts(void);

static HDC make_dc(void)
{
    if(devNames && devMode)
    {
        LPDEVNAMES dn = GlobalLock(devNames);
        LPDEVMODEW dm = GlobalLock(devMode);
        HDC ret;

        ret = CreateDCW((LPWSTR)dn + dn->wDriverOffset,
                         (LPWSTR)dn + dn->wDeviceOffset, 0, dm);

        GlobalUnlock(dn);
        GlobalUnlock(dm);

        return ret;
    } else
    {
        return 0;
    }
}

static LONG twips_to_centmm(int twips)
{
    return MulDiv(twips, CENTMM_PER_INCH, TWIPS_PER_INCH);
}

static LONG centmm_to_twips(int mm)
{
    return MulDiv(mm, TWIPS_PER_INCH, CENTMM_PER_INCH);
}

static LONG twips_to_pixels(int twips, int dpi)
{
    return MulDiv(twips, dpi, TWIPS_PER_INCH);
}

static LONG devunits_to_twips(int units, int dpi)
{
    return MulDiv(units, TWIPS_PER_INCH, dpi);
}


static RECT get_print_rect(HDC hdc)
{
    RECT rc;
    int width, height;

    if(hdc)
    {
        int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        width = devunits_to_twips(GetDeviceCaps(hdc, PHYSICALWIDTH), dpiX);
        height = devunits_to_twips(GetDeviceCaps(hdc, PHYSICALHEIGHT), dpiY);
    } else
    {
        width = centmm_to_twips(18500);
        height = centmm_to_twips(27000);
    }

    SetRect(&rc, margins.left, margins.top, width - margins.right, height - margins.bottom);

    return rc;
}

void target_device(HWND hMainWnd, DWORD wordWrap)
{
    HWND hEditorWnd = GetDlgItem(hMainWnd, IDC_EDITOR);

    if(wordWrap == ID_WORDWRAP_MARGIN)
    {
        int width = 0;
        LRESULT result;
        HDC hdc;
        RECT rc;
        /* Only printer-margin wrapping requires printer initialization. */
        get_default_printer_opts();
        hdc = make_dc();
        rc = get_print_rect(hdc);

        width = rc.right - rc.left;
        if(!hdc)
        {
            HDC hMaindc = GetDC(hMainWnd);
            hdc = CreateCompatibleDC(hMaindc);
            ReleaseDC(hMainWnd, hMaindc);
        }
        result = SendMessageW(hEditorWnd, EM_SETTARGETDEVICE, (WPARAM)hdc, width);
        DeleteDC(hdc);
        if (result)
            return;
        /* otherwise EM_SETTARGETDEVICE failed, so fall back on wrapping
         * to window using the NULL DC. */
    }

    if (wordWrap != ID_WORDWRAP_NONE) {
        SendMessageW(hEditorWnd, EM_SETTARGETDEVICE, 0, 0);
    } else {
        SendMessageW(hEditorWnd, EM_SETTARGETDEVICE, 0, 1);
    }

}

static LPWSTR dialog_print_to_file(HWND hMainWnd)
{
    OPENFILENAMEW ofn;
    static WCHAR file[MAX_PATH] = {'O','U','T','P','U','T','.','P','R','N',0};
    static const WCHAR defExt[] = {'P','R','N',0};
    static LPWSTR file_filter;

    if(!file_filter)
        file_filter = get_print_file_filter(hMainWnd);

    ZeroMemory(&ofn, sizeof(ofn));

    ofn.lStructSize = sizeof(ofn);
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
    ofn.hwndOwner = hMainWnd;
    ofn.lpstrFilter = file_filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;

    if(GetSaveFileNameW(&ofn))
        return file;
    else
        return FALSE;
}

static void char_from_pagenum(HWND hEditorWnd, FORMATRANGE *fr, int page)
{
    int i;

    fr->chrg.cpMin = 0;

    for(i = 1; i < page; i++)
    {
        int bottom = fr->rc.bottom;
        fr->chrg.cpMin = SendMessageW(hEditorWnd, EM_FORMATRANGE, FALSE, (LPARAM)fr);
        fr->rc.bottom = bottom;
    }
}

static HWND get_ruler_wnd(HWND hMainWnd)
{
    return GetDlgItem(GetDlgItem(hMainWnd, IDC_REBAR), IDC_RULER);
}

void redraw_ruler(HWND hRulerWnd)
{
    RECT rc;

    GetClientRect(hRulerWnd, &rc);
    InvalidateRect(hRulerWnd, &rc, TRUE);
}

static void update_ruler(HWND hRulerWnd)
{
     SendMessageW(hRulerWnd, WM_USER, 0, 0);
     redraw_ruler(hRulerWnd);
}

/* Draw dark ruler markings directly to the paint DC.
 * The previous bitmap used a monochrome memory DC and SRCPAINT,
 * which made its marks disappear against the dark background. */
static void paint_ruler(HWND hWnd, LONG EditLeftmost, BOOL NewMetrics)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hWnd, &ps);
    HDC printDC = make_dc();
    RECT view, track, printRect;
    HBRUSH barBrush = CreateSolidBrush(RGB(41, 44, 49));
    HBRUSH trackBrush = CreateSolidBrush(RGB(51, 55, 62));
    HPEN linePen = CreatePen(PS_SOLID, 1, RGB(209, 219, 233));
    HPEN oldPen;
    HFONT rulerFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
    HFONT oldFont;
    int width, cmPixels, quarter, x, i, tick, midY;
    (void)NewMetrics;
    if (!dc) return;
    GetClientRect(hWnd, &view);
    FillRect(dc, &view, barBrush);
    printRect = get_print_rect(printDC);
    track = view;
    InflateRect(&track, 0, -4);
    track.left = EditLeftmost;
    track.right = twips_to_pixels(printRect.right - margins.left,
                                  GetDeviceCaps(dc, LOGPIXELSX));
    if (track.right > view.right) track.right = view.right;
    if (track.left < 0) track.left = 0;
    if (track.right > track.left)
    {
        FillRect(dc, &track, trackBrush);
        /* Thin outlines instead of DrawEdge's light-theme bevel. */
        FrameRect(dc, &track, barBrush);
    }
    oldPen = (HPEN)SelectObject(dc, linePen);
    oldFont = (HFONT)SelectObject(dc, rulerFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 239, 244));
    SetTextAlign(dc, TA_CENTER | TA_TOP);
    cmPixels = twips_to_pixels(centmm_to_twips(1000), GetDeviceCaps(dc, LOGPIXELSX));
    if (cmPixels < 4) cmPixels = 4;
    quarter = cmPixels / 4;
    if (quarter < 1) quarter = 1;
    midY = (view.top + view.bottom) / 2;
    width = track.right;
    for (i = 1, x = EditLeftmost; x < width && i <= 500; ++i, x += cmPixels)
    {
        WCHAR number[12];
        int j;
        for (j = 1; j <= 3; ++j)
        {
            tick = x + j * quarter;
            if (tick >= width) break;
            MoveToEx(dc, tick, midY - (j == 2 ? 3 : 1), NULL);
            LineTo(dc, tick, midY + (j == 2 ? 4 : 2));
        }
        tick = x + cmPixels;
        if (tick >= width) break;
        wsprintfW(number, L"%d", i);
        TextOutW(dc, tick, track.top + 2, number, lstrlenW(number));
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldPen);
    DeleteObject(rulerFont);
    DeleteObject(linePen);
    DeleteObject(trackBrush);
    DeleteObject(barBrush);
    if (printDC) DeleteDC(printDC);
    EndPaint(hWnd, &ps);
}

LRESULT CALLBACK ruler_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static WNDPROC pPrevRulerProc;
    static LONG EditLeftmost;
    static BOOL NewMetrics;

    switch(msg)
    {
        case WM_USER:
            if(wParam)
            {
                EditLeftmost = ((POINTL*)wParam)->x;
                pPrevRulerProc = (WNDPROC)lParam;
            }
            NewMetrics = TRUE;
            break;

        case WM_PAINT:
            paint_ruler(hWnd, EditLeftmost, NewMetrics);
            break;

        default:
            return CallWindowProcW(pPrevRulerProc, hWnd, msg, wParam, lParam);
    }

    return 0;
}

/* Inspect the document's RTF table cell boundaries without modifying the
 * document.  \cellx values are absolute table-column positions in twips.
 * Streaming rather than building a second copy keeps even image-heavy RTF
 * documents inexpensive to inspect at print time.
 */
typedef struct _RWP_TABLE_WIDTH_SCAN
{
    int matched;
    BOOL readingValue;
    BOOL hasDigits;
    LONG value;
    LONG widestCell;
} RWP_TABLE_WIDTH_SCAN;

static DWORD CALLBACK rwp_print_scan_table_width(DWORD_PTR cookie, LPBYTE data,
                                                   LONG size, LONG *consumed)
{
    static const char prefix[] = "\\cellx";
    RWP_TABLE_WIDTH_SCAN *scan = (RWP_TABLE_WIDTH_SCAN *)cookie;
    LONG i;

    for (i = 0; i < size; ++i)
    {
        unsigned char ch = data[i];
        if (scan->readingValue)
        {
            if (ch >= '0' && ch <= '9')
            {
                /* Real-world cell positions are much smaller than this cap. */
                if (scan->value < 100000000)
                    scan->value = scan->value * 10 + (ch - '0');
                scan->hasDigits = TRUE;
                continue;
            }
            if (scan->hasDigits && scan->value > scan->widestCell)
                scan->widestCell = scan->value;
            scan->readingValue = FALSE;
            scan->hasDigits = FALSE;
            scan->value = 0;
            scan->matched = 0;
        }

        if (ch == (unsigned char)prefix[scan->matched])
        {
            scan->matched++;
            if (scan->matched == (int)(sizeof(prefix) - 1))
            {
                scan->readingValue = TRUE;
                scan->matched = 0;
            }
        }
        else
            scan->matched = (ch == '\\') ? 1 : 0;
    }
    *consumed = size;
    return 0;
}

static LONG rwp_print_widest_table(HWND editor)
{
    RWP_TABLE_WIDTH_SCAN scan;
    EDITSTREAM stream;
    ZeroMemory(&scan, sizeof(scan));
    ZeroMemory(&stream, sizeof(stream));
    stream.dwCookie = (DWORD_PTR)&scan;
    stream.pfnCallback = rwp_print_scan_table_width;
    SendMessageW(editor, EM_STREAMOUT, SF_RTF, (LPARAM)&stream);
    if (stream.dwError)
        return 0;
    if (scan.readingValue && scan.hasDigits && scan.value > scan.widestCell)
        scan.widestCell = scan.value;
    return scan.widestCell;
}

/* Win32 uses 0.1 mm for custom DEVMODE paper dimensions.  Request the
 * document-sized sheet from the selected printer *before* StartDoc, then
 * verify its actual physical page dimensions.  Some PDF/printer drivers
 * silently ignore custom sizes; a successful ResetDC alone is NOT proof.
 * This changes only the current job's DC, not global printer defaults.
 */
static BOOL rwp_print_set_custom_paper(LPPRINTDLGW pd, LONG widthTwips,
                                        LONG heightTwips)
{
    LPDEVMODEW original;
    LPDEVMODEW requested;
    DWORD allocationSize;
    int widthTenths, heightTenths;
    int variant;
    BOOL supported = FALSE;

    if (!pd->hDC || !pd->hDevMode)
        return FALSE;
    widthTenths = MulDiv(widthTwips, 254, 1440);
    heightTenths = MulDiv(heightTwips, 254, 1440);
    if (widthTenths <= 0 || heightTenths <= 0 ||
        widthTenths > 32760 || heightTenths > 32760)
        return FALSE;

    original = GlobalLock(pd->hDevMode);
    if (!original)
        return FALSE;
    allocationSize = original->dmSize + original->dmDriverExtra;
    if (original->dmSize < sizeof(DEVMODEW) ||
        allocationSize > GlobalSize(pd->hDevMode))
    {
        GlobalUnlock(pd->hDevMode);
        return FALSE;
    }
    requested = HeapAlloc(GetProcessHeap(), 0, allocationSize);
    if (!requested)
    {
        GlobalUnlock(pd->hDevMode);
        return FALSE;
    }
    memcpy(requested, original, allocationSize);
    GlobalUnlock(pd->hDevMode);

    requested->dmFields |= DM_PAPERSIZE | DM_PAPERWIDTH | DM_PAPERLENGTH |
                           DM_ORIENTATION | DM_SCALE;
    requested->dmPaperWidth = (short)widthTenths;
    requested->dmPaperLength = (short)heightTenths;
    requested->dmOrientation = DMORIENT_PORTRAIT;
    requested->dmScale = 100;
    requested->dmFormName[0] = 0;
    requested->dmFields &= ~DM_FORMNAME;

    /* Different drivers expect either 0 or DMPAPER_USER for custom forms. */
    for (variant = 0; variant < 2 && !supported; ++variant)
    {
        HDC newDC;
        int actualWidth, actualHeight;
        requested->dmPaperSize = (variant == 0) ? 0 : DMPAPER_USER;
        newDC = ResetDCW(pd->hDC, requested);
        if (!newDC)
            continue;
        pd->hDC = newDC;
        actualWidth = MulDiv(GetDeviceCaps(pd->hDC, PHYSICALWIDTH), 254,
                             GetDeviceCaps(pd->hDC, LOGPIXELSX));
        actualHeight = MulDiv(GetDeviceCaps(pd->hDC, PHYSICALHEIGHT), 254,
                              GetDeviceCaps(pd->hDC, LOGPIXELSY));
        /* Allow rounding and small driver measurement differences (2 mm). */
        if (abs(actualWidth - widthTenths) <= 20 &&
            abs(actualHeight - heightTenths) <= 20)
            supported = TRUE;
    }
    HeapFree(GetProcessHeap(), 0, requested);
    return supported;
}

/* Some drivers (including some Microsoft Print to PDF configurations)
 * ignore or reject arbitrary paper dimensions.  Rather than squeezing a
 * document into A4, ask the driver which *real supported forms* exist and
 * select the smallest one large enough for the document at 100% scale.
 * Both portrait and landscape are considered.  The DEVMODE changes apply
 * only to this print job; they do not change the Windows printer defaults.
 */
static BOOL rwp_print_set_supported_paper(LPPRINTDLGW pd, LONG widthTwips,
                                           LONG heightTwips)
{
    LPDEVNAMES names;
    LPDEVMODEW current, requested;
    LPCWSTR printerName, portName;
    WORD *paperIds = NULL;
    POINT *paperSizes = NULL;
    BYTE *attempted = NULL;
    DWORD modeSize;
    int paperCount, sizeCount, idCount;
    int requestedWidth, requestedHeight;
    int tryNum;
    BOOL supported = FALSE;

    if (!pd->hDC || !pd->hDevNames || !pd->hDevMode)
        return FALSE;
    requestedWidth = MulDiv(widthTwips, 254, 1440);
    requestedHeight = MulDiv(heightTwips, 254, 1440);
    if (requestedWidth <= 0 || requestedHeight <= 0)
        return FALSE;

    current = GlobalLock(pd->hDevMode);
    if (!current)
        return FALSE;
    modeSize = current->dmSize + current->dmDriverExtra;
    if (current->dmSize < sizeof(DEVMODEW) ||
        modeSize > GlobalSize(pd->hDevMode))
    {
        GlobalUnlock(pd->hDevMode);
        return FALSE;
    }
    requested = HeapAlloc(GetProcessHeap(), 0, modeSize);
    if (!requested)
    {
        GlobalUnlock(pd->hDevMode);
        return FALSE;
    }
    memcpy(requested, current, modeSize);
    GlobalUnlock(pd->hDevMode);

    names = GlobalLock(pd->hDevNames);
    if (!names)
        goto cleanup;
    printerName = (LPCWSTR)names + names->wDeviceOffset;
    portName = (LPCWSTR)names + names->wOutputOffset;
    paperCount = DeviceCapabilitiesW(printerName, portName, DC_PAPERS,
                                     NULL, requested);
    if (paperCount <= 0 || paperCount > 1024)
    {
        GlobalUnlock(pd->hDevNames);
        goto cleanup;
    }
    paperIds = HeapAlloc(GetProcessHeap(), 0, paperCount * sizeof(*paperIds));
    paperSizes = HeapAlloc(GetProcessHeap(), 0, paperCount * sizeof(*paperSizes));
    attempted = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, paperCount * 2);
    if (!paperIds || !paperSizes || !attempted)
    {
        GlobalUnlock(pd->hDevNames);
        goto cleanup;
    }
    idCount = DeviceCapabilitiesW(printerName, portName, DC_PAPERS,
                                  (LPWSTR)paperIds, requested);
    sizeCount = DeviceCapabilitiesW(printerName, portName, DC_PAPERSIZE,
                                    (LPWSTR)paperSizes, requested);
    GlobalUnlock(pd->hDevNames);
    if (idCount != paperCount || sizeCount != paperCount)
        goto cleanup;

    requested->dmFields |= DM_PAPERSIZE | DM_ORIENTATION | DM_SCALE;
    requested->dmFields &= ~(DM_PAPERWIDTH | DM_PAPERLENGTH | DM_FORMNAME);
    requested->dmScale = 100;
    requested->dmFormName[0] = 0;

    /* Try suitable forms from smallest area to largest. Some advertised
     * forms can still be refused by ResetDC, so keep searching if needed. */
    for (tryNum = 0; tryNum < paperCount * 2; ++tryNum)
    {
        ULONGLONG bestArea = ~(ULONGLONG)0;
        int bestIndex = -1, bestOrientation = 0;
        int bestWidth = 0, bestHeight = 0;
        int i, orientation;

        for (i = 0; i < paperCount; ++i)
        {
            if (paperSizes[i].x <= 0 || paperSizes[i].y <= 0)
                continue;
            for (orientation = 0; orientation < 2; ++orientation)
            {
                int attemptIndex = 2*i + orientation;
                int w = orientation ? paperSizes[i].y : paperSizes[i].x;
                int h = orientation ? paperSizes[i].x : paperSizes[i].y;
                ULONGLONG area;
                if (attempted[attemptIndex] || w < requestedWidth ||
                    h < requestedHeight)
                    continue;
                area = (ULONGLONG)(unsigned int)w * (unsigned int)h;
                if (area < bestArea ||
                    (area == bestArea && w < bestWidth))
                {
                    bestArea = area;
                    bestIndex = i;
                    bestOrientation = orientation;
                    bestWidth = w;
                    bestHeight = h;
                }
            }
        }
        if (bestIndex < 0)
            break;
        attempted[2*bestIndex + bestOrientation] = 1;
        requested->dmPaperSize = paperIds[bestIndex];
        requested->dmOrientation = bestOrientation ? DMORIENT_LANDSCAPE :
                                                      DMORIENT_PORTRAIT;
        {
            HDC resized = ResetDCW(pd->hDC, requested);
            if (resized)
            {
                int actualW, actualH;
                pd->hDC = resized;
                actualW = MulDiv(GetDeviceCaps(resized, PHYSICALWIDTH), 254,
                                 GetDeviceCaps(resized, LOGPIXELSX));
                actualH = MulDiv(GetDeviceCaps(resized, PHYSICALHEIGHT), 254,
                                 GetDeviceCaps(resized, LOGPIXELSY));
                /* A printer may advertise a paper but ignore orientation.
                 * Check the real device, not just ResetDC's return value. */
                if (abs(actualW - bestWidth) <= 30 &&
                    abs(actualH - bestHeight) <= 30 &&
                    actualW >= requestedWidth - 10 &&
                    actualH >= requestedHeight - 10)
                {
                    supported = TRUE;
                    break;
                }
            }
        }
    }

cleanup:
    if (attempted) HeapFree(GetProcessHeap(), 0, attempted);
    if (paperSizes) HeapFree(GetProcessHeap(), 0, paperSizes);
    if (paperIds) HeapFree(GetProcessHeap(), 0, paperIds);
    HeapFree(GetProcessHeap(), 0, requested);
    return supported;
}

/* RichEdit's EM_FORMATRANGE paints its own white paper rectangle and also
 * fills otherwise unshaded table cells white, even when a document declares a
 * dark page background.  The rectangles overwrite a background painted on the
 * printer DC before EM_FORMATRANGE.
 *
 * Record a page as vector EMF, then replay it without the large white opaque
 * rectangles. Keep hairline/short white fills (table borders), white glyphs,
 * raster pictures and all non-white drawing operations as ordinary EMF records.
 * No PDF-only rasterization or post-processing is involved.
 */
typedef struct _RWP_PRINT_EMF_CONTEXT
{
    int minWidth, minHeight;
} RWP_PRINT_EMF_CONTEXT;

static BOOL rwp_print_solid_white_brush(HBRUSH brush)
{
    LOGBRUSH brushInfo;
    if (!brush || GetObjectW(brush, sizeof(brushInfo), &brushInfo) != sizeof(brushInfo))
        return FALSE;
    return brushInfo.lbStyle == BS_SOLID && brushInfo.lbColor == RGB(255, 255, 255);
}

static BOOL rwp_print_large_rect(const RECTL *rectangle, const RWP_PRINT_EMF_CONTEXT *ctx)
{
    return rectangle->right - rectangle->left >= ctx->minWidth &&
           rectangle->bottom - rectangle->top >= ctx->minHeight;
}

static int CALLBACK rwp_print_filter_emf(HDC hdc, HANDLETABLE *handles,
                                         const ENHMETARECORD *record,
                                         int handleCount, LPARAM param)
{
    const RWP_PRINT_EMF_CONTEXT *ctx = (const RWP_PRINT_EMF_CONTEXT *)param;

    if (record->iType == EMR_RECTANGLE)
    {
        const EMRRECTANGLE *rectangle = (const EMRRECTANGLE *)record;
        if (rwp_print_large_rect(&rectangle->rclBox, ctx) &&
            rwp_print_solid_white_brush((HBRUSH)GetCurrentObject(hdc, OBJ_BRUSH)))
            return 1;
    }
    else if (record->iType == EMR_BITBLT)
    {
        /* Some versions of GDI record FillRect/PatBlt as a PATCOPY BitBlt,
         * with no source image. Never suppress a real bitmap blit. */
        const EMRBITBLT *blit = (const EMRBITBLT *)record;
        if (blit->dwRop == PATCOPY && blit->cbBitsSrc == 0 &&
            rwp_print_large_rect(&blit->rclBounds, ctx) &&
            rwp_print_solid_white_brush((HBRUSH)GetCurrentObject(hdc, OBJ_BRUSH)))
            return 1;
    }
    else if (record->iType == EMR_FILLRGN)
    {
        const EMRFILLRGN *region = (const EMRFILLRGN *)record;
        HBRUSH brush = NULL;
        if (region->ihBrush & 0x80000000u)
            brush = (HBRUSH)GetStockObject(region->ihBrush & 0x7fffffffu);
        else if (region->ihBrush < (DWORD)handleCount)
            brush = (HBRUSH)handles->objectHandle[region->ihBrush];
        if (rwp_print_large_rect(&region->rclBounds, ctx) &&
            rwp_print_solid_white_brush(brush))
            return 1;
    }
    else if (record->iType == EMR_SETBKMODE &&
             ((const EMRSETBKMODE *)record)->iMode == OPAQUE)
    {
        /* A default white text background is never wanted on a dark page. */
        SetBkMode(hdc, TRANSPARENT);
        return 1;
    }
    else if (record->iType == EMR_EXTTEXTOUTW)
    {
        const EMREXTTEXTOUTW *textRecord = (const EMREXTTEXTOUTW *)record;
        if (textRecord->emrtext.fOptions & ETO_OPAQUE)
        {
            ENHMETARECORD *copy = HeapAlloc(GetProcessHeap(), 0, record->nSize);
            if (copy)
            {
                memcpy(copy, record, record->nSize);
                ((EMREXTTEXTOUTW *)copy)->emrtext.fOptions &= ~ETO_OPAQUE;
                PlayEnhMetaFileRecord(hdc, handles, copy, handleCount);
                HeapFree(GetProcessHeap(), 0, copy);
                return 1;
            }
        }
    }

    return PlayEnhMetaFileRecord(hdc, handles, record, handleCount);
}

static LONG rwp_print_format_range(HWND editor, FORMATRANGE *format,
                                   COLORREF background, LONG scaleNumerator,
                                   LONG scaleDenominator)
{
    HDC printer = format->hdc;
    HDC recordDC;
    HENHMETAFILE metafile;
    LONG nextChar;
    RECT pageBounds;
    RECT emfFrame;
    int widthMM, heightMM;
    RWP_PRINT_EMF_CONTEXT ctx;

    /* Only documents with dark backgrounds or oversized layouts need EMF. */
    if (background == RGB(255, 255, 255) &&
        scaleNumerator >= scaleDenominator)
        return SendMessageW(editor, EM_FORMATRANGE, TRUE, (LPARAM)format);

    /* An EMF frame uses 0.01 mm units.  In the usual case it matches the
     * physical printer page (the V3 proportional-rendering fix). For a wide
     * table, create a proportionally larger *recording frame* and play it into
     * the ordinary page-size rectangle. This preserves the off-page table
     * drawing instead of allowing it to be clipped by the small EMF frame.
     * Expanding both dimensions by the SAME factor keeps images undistorted.
     */
    widthMM = GetDeviceCaps(printer, HORZSIZE);
    heightMM = GetDeviceCaps(printer, VERTSIZE);
    if (widthMM <= 0 || heightMM <= 0)
        return SendMessageW(editor, EM_FORMATRANGE, TRUE, (LPARAM)format);
    if (scaleNumerator < scaleDenominator)
    {
        widthMM = MulDiv(widthMM, scaleDenominator, scaleNumerator);
        heightMM = MulDiv(heightMM, scaleDenominator, scaleNumerator);
    }
    SetRect(&emfFrame, 0, 0, widthMM * 100, heightMM * 100);

    recordDC = CreateEnhMetaFileW(printer, NULL, &emfFrame, NULL);
    if (!recordDC)
        return SendMessageW(editor, EM_FORMATRANGE, TRUE, (LPARAM)format);

    format->hdc = recordDC;
    format->hdcTarget = printer;
    nextChar = SendMessageW(editor, EM_FORMATRANGE, TRUE, (LPARAM)format);
    /* The next page uses a fresh metafile DC; release RichEdit's device cache. */
    SendMessageW(editor, EM_FORMATRANGE, FALSE, 0);
    format->hdc = printer;
    format->hdcTarget = printer;

    metafile = CloseEnhMetaFile(recordDC);
    if (!metafile)
        return nextChar;

    /* The enlarged recording frame maps into an ordinary printable page.
     * Keep the original left/top margins fixed by offsetting the scaled EMF.
     */
    if (scaleNumerator < scaleDenominator)
    {
        int leftMargin = twips_to_pixels(margins.left,
                                         GetDeviceCaps(printer, LOGPIXELSX));
        int topMargin = twips_to_pixels(margins.top,
                                        GetDeviceCaps(printer, LOGPIXELSY));
        pageBounds.left = MulDiv(leftMargin,
                                 scaleDenominator - scaleNumerator,
                                 scaleDenominator);
        pageBounds.top = MulDiv(topMargin,
                                scaleDenominator - scaleNumerator,
                                scaleDenominator);
        pageBounds.right = pageBounds.left + GetDeviceCaps(printer, HORZRES);
        pageBounds.bottom = pageBounds.top + GetDeviceCaps(printer, VERTRES);
    }
    else
        SetRect(&pageBounds, 0, 0, GetDeviceCaps(printer, HORZRES),
                GetDeviceCaps(printer, VERTRES));

    {
        int savedDC = SaveDC(printer);
        if (background == RGB(255, 255, 255))
        {
            /* Normal white documents must not have white fills filtered. */
            PlayEnhMetaFile(printer, metafile, &pageBounds);
        }
        else
        {
            /* Threshold: 3 printer points. Keep 1-pt table borders. */
            ctx.minWidth = max(1, MulDiv(GetDeviceCaps(printer, LOGPIXELSX), 3, 72));
            ctx.minHeight = max(1, MulDiv(GetDeviceCaps(printer, LOGPIXELSY), 3, 72));
            SetBkMode(printer, TRANSPARENT);
            EnumEnhMetaFile(printer, metafile, rwp_print_filter_emf,
                            &ctx, &pageBounds);
        }
        if (savedDC) RestoreDC(printer, savedDC);
    }
    DeleteEnhMetaFile(metafile);
    return nextChar;
}

static void print(LPPRINTDLGW pd, LPWSTR wszFileName)
{
    FORMATRANGE fr;
    DOCINFOW di;
    HWND hEditorWnd = GetDlgItem(pd->hwndOwner, IDC_EDITOR);
    int printedPages = 0;
    COLORREF backgroundColor = RGB(255, 255, 255);
    HBRUSH backgroundBrush = NULL;
    LONG scaleNumerator = 1, scaleDenominator = 1;
    LONG paperWidth, paperHeight, widestCell, contentWidth;
    RECT initialPage;
    RECT printMargins = margins;
    BOOL accepted;

    if (!pd->hDC) return;

    initialPage = get_print_rect(pd->hDC);
    paperWidth = initialPage.right + margins.right;
    paperHeight = initialPage.bottom + margins.bottom;
    contentWidth = initialPage.right - initialPage.left;
    widestCell = rwp_print_widest_table(hEditorWnd);

    /* The document need not be squeezed into the current printer's A4
     * selection.  A table's last \cellx specifies its rightmost coordinate;
     * make the real printer/PDF page wide enough to hold it at 100% scale.
     * For documents without wide tables, retain the user's selected width.
     */
    if (widestCell > contentWidth)
        paperWidth = margins.left + widestCell + 400 + margins.right;
    /* Height is a property of ONE printer page, not of the whole document.
     * Do not measure the total text height here: a long RTF must be split
     * across multiple physical pages by EM_FORMATRANGE below. Otherwise a
     * long document falsely requires a several-metre-high paper size.
     */

    /* First try the exact page, then the closest supported form. */
    accepted = rwp_print_set_custom_paper(pd, paperWidth, paperHeight);
    if (!accepted)
        accepted = rwp_print_set_supported_paper(pd, paperWidth, paperHeight);

    /* In the test document, the wide table is just too wide for A3 landscape
     * when combined with WordPad's old 31-mm print margins. Retry with 10-mm
     * print-only margins: no text or picture is scaled, and the RTF remains
     * untouched. We only do this if the original job did not fit a supported
     * page. */
    if (!accepted && widestCell > contentWidth)
    {
        LONG compact = centmm_to_twips(1000);
        printMargins.left = min(printMargins.left, compact);
        printMargins.right = min(printMargins.right, compact);
        printMargins.top = min(printMargins.top, compact);
        printMargins.bottom = min(printMargins.bottom, compact);
        paperWidth = printMargins.left + widestCell + 400 + printMargins.right;
        /* Keep the selected sheet's normal height for every page. */
        accepted = rwp_print_set_supported_paper(pd, paperWidth, paperHeight);
    }

    if (!accepted)
    {
        MessageBoxW(pd->hwndOwner,
                    L"La stampante selezionata non offre un formato pagina "
                    L"sufficientemente grande per mantenere le proporzioni "
                    L"originali del documento.\n\n"
                    L"Stampa annullata: scegliere una stampante PDF con "
                    L"formati piu' grandi o personalizzati.",
                    L"Dimensioni pagina non supportate",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    /* Use the actual sheet height for per-page layout; the requested width
     * remains the content-alignment width so right-aligned paragraphs do not
     * jump to the far edge of a larger supported paper form. This lets the
     * formatting loop advance from page to page at full, unscaled size. */
    paperHeight = devunits_to_twips(GetDeviceCaps(pd->hDC, PHYSICALHEIGHT),
                                    GetDeviceCaps(pd->hDC, LOGPIXELSY));
    fr.hdc = pd->hDC;
    fr.hdcTarget = pd->hDC;
    fr.rc = get_print_rect(fr.hdc);
    fr.rc.left = printMargins.left;
    fr.rc.top = printMargins.top;
    fr.rc.right = min(fr.rc.right + margins.right - printMargins.right,
                      paperWidth - printMargins.right);
    fr.rc.bottom = min(fr.rc.bottom + margins.bottom - printMargins.bottom,
                       paperHeight - printMargins.bottom);
    SetRect(&fr.rcPage, 0, 0, paperWidth, paperHeight);

    ZeroMemory(&di, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = wszFileName;

    if(pd->Flags & PD_PRINTTOFILE)
    {
        di.lpszOutput = dialog_print_to_file(pd->hwndOwner);
        if(!di.lpszOutput)
            return;
    }

    if(pd->Flags & PD_SELECTION)
    {
        SendMessageW(hEditorWnd, EM_EXGETSEL, 0, (LPARAM)&fr.chrg);
    } else
    {
        GETTEXTLENGTHEX gt;
        gt.flags = GTL_DEFAULT;
        gt.codepage = 1200;
        fr.chrg.cpMin = 0;
        fr.chrg.cpMax = SendMessageW(hEditorWnd, EM_GETTEXTLENGTHEX, (WPARAM)&gt, 0);

        if(pd->Flags & PD_PAGENUMS)
            char_from_pagenum(hEditorWnd, &fr, pd->nFromPage);
    }

    /* A document background is not included in RichEdit's printed text.
     * Paint the printer's entire printable area before rendering each page.
     * Default/automatic backgrounds are intentionally left unchanged. */
    if (wordpad_get_document_background_color(&backgroundColor))
        backgroundBrush = CreateSolidBrush(backgroundColor);

    StartDocW(fr.hdc, &di);
    do
    {
        LONG pageStart = fr.chrg.cpMin;
        if(StartPage(fr.hdc) <= 0)
            break;

        if (backgroundBrush)
        {
            RECT printableArea;
            SetRect(&printableArea, 0, 0,
                    GetDeviceCaps(fr.hdc, HORZRES), GetDeviceCaps(fr.hdc, VERTRES));
            FillRect(fr.hdc, &printableArea, backgroundBrush);
            /* Do not overwrite the newly painted color behind text glyphs. */
            SetBkColor(fr.hdc, backgroundColor);
            SetBkMode(fr.hdc, TRANSPARENT);
        }

        fr.chrg.cpMin = (backgroundBrush || scaleNumerator < scaleDenominator) ?
            rwp_print_format_range(hEditorWnd, &fr, backgroundColor,
                                   scaleNumerator, scaleDenominator) :
            SendMessageW(hEditorWnd, EM_FORMATRANGE, TRUE, (LPARAM)&fr);

        if(EndPage(fr.hdc) <= 0)
            break;

        printedPages++;
        if((pd->Flags & PD_PAGENUMS) && (printedPages > (pd->nToPage - pd->nFromPage)))
            break;
        /* Guard against infinite printing if RichEdit cannot place the next
         * character on a page (e.g. an object taller than the page). */
        if (fr.chrg.cpMin <= pageStart)
            break;
    }
    while(fr.chrg.cpMin && fr.chrg.cpMin < fr.chrg.cpMax);

    EndDoc(fr.hdc);
    SendMessageW(hEditorWnd, EM_FORMATRANGE, FALSE, 0);
    if (backgroundBrush)
        DeleteObject(backgroundBrush);
}

void dialog_printsetup(HWND hMainWnd)
{
    PAGESETUPDLGW ps;

    get_default_printer_opts();

    ZeroMemory(&ps, sizeof(ps));
    ps.lStructSize = sizeof(ps);
    ps.hwndOwner = hMainWnd;
    ps.Flags = PSD_INHUNDREDTHSOFMILLIMETERS | PSD_MARGINS;
    SetRect(&ps.rtMargin, twips_to_centmm(margins.left), twips_to_centmm(margins.top),
            twips_to_centmm(margins.right), twips_to_centmm(margins.bottom));
    ps.hDevMode = devMode;
    ps.hDevNames = devNames;

    if(PageSetupDlgW(&ps))
    {
        SetRect(&margins, centmm_to_twips(ps.rtMargin.left), centmm_to_twips(ps.rtMargin.top),
                centmm_to_twips(ps.rtMargin.right), centmm_to_twips(ps.rtMargin.bottom));
        devMode = ps.hDevMode;
        devNames = ps.hDevNames;
        update_ruler(get_ruler_wnd(hMainWnd));
    }
}

void get_default_printer_opts(void)
{
    PRINTDLGW pd;
    /* It is expensive on machines with network printers or a busy spooler.
     * Do this once, only when printer-dependent features are requested. */
    if (rwpPrinterDefaultsRequested) return;
    rwpPrinterDefaultsRequested = TRUE;
    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDEFAULT;
    /* PD_RETURNDEFAULT requires both input handles to be NULL. It returns
     * DEVMODE/DEVNAMES without creating an unnecessary printer HDC. */
    if (PrintDlgW(&pd))
    {
        devMode = pd.hDevMode;
        devNames = pd.hDevNames;
    }
    else
    {
        /* Do not retain partially returned handles if the lookup fails. */
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
    }
    if (pd.hDC) DeleteDC(pd.hDC);
}

void print_quick(HWND hMainWnd, LPWSTR wszFileName)
{
    PRINTDLGW pd;

    get_default_printer_opts();

    ZeroMemory(&pd, sizeof(pd));
    pd.hwndOwner = hMainWnd;
    pd.hDC = make_dc();
    pd.hDevMode = devMode;
    pd.hDevNames = devNames;

    print(&pd, wszFileName);
    DeleteDC(pd.hDC);
}

void dialog_print(HWND hMainWnd, LPWSTR wszFileName)
{
    PRINTDLGW pd;
    HWND hEditorWnd = GetDlgItem(hMainWnd, IDC_EDITOR);
    int from = 0;
    int to = 0;

    get_default_printer_opts();

    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = hMainWnd;
    pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nMinPage = 1;
    pd.nMaxPage = -1;
    pd.hDevMode = devMode;
    pd.hDevNames = devNames;

    SendMessageW(hEditorWnd, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
    if(from == to)
        pd.Flags |= PD_NOSELECTION;

    if(PrintDlgW(&pd))
    {
        devMode = pd.hDevMode;
        devNames = pd.hDevNames;
        print(&pd, wszFileName);
        update_ruler(get_ruler_wnd(hMainWnd));
    }
}
