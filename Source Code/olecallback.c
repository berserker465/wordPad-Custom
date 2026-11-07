/*
 * Wordpad implementation - Richedit OLE callback implementation
 *
 * Copyright 2010 by Dylan Smith <dylan.ah.smith@gmail.com>
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

#define COBJMACROS

#define WIN32_NO_STATUS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <richedit.h>
#include <richole.h>
#include <wine/debug.h>

#include "wordpad.h"

#ifndef RECO_DROP
#define RECO_DROP 1
#endif

WINE_DEFAULT_DEBUG_CHANNEL(wordpad);

struct IRichEditOleCallbackImpl {
    const IRichEditOleCallbackVtbl *vtbl;
    IStorage *stg;
    int item_num;
};

struct IRichEditOleCallbackImpl olecallback;
/* Stored at EM_SETOLECALLBACK time: the OLE callback does not provide an
 * editor HWND when it asks WordPad to build the context menu. */
static HWND rwpCallbackEditor;

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_QueryInterface(
    IRichEditOleCallback* This,
    REFIID riid,
    void **ppvObject)
{
    WINE_TRACE("(%p, %s, %p)\n", This, wine_dbgstr_guid(riid), ppvObject);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRichEditOleCallback))
    {
        *ppvObject = This;
        return S_OK;
    }
    WINE_FIXME("Unknown interface: %s\n", wine_dbgstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE RichEditOleCallback_AddRef(
    IRichEditOleCallback* This)
{
    WINE_TRACE("(%p)\n", This);
    /* singleton */
    return 1;
}

static ULONG STDMETHODCALLTYPE RichEditOleCallback_Release(
    IRichEditOleCallback* This)
{
    WINE_TRACE("(%p)\n", This);
    return 1;
}

/*** IRichEditOleCallback methods ***/
static HRESULT STDMETHODCALLTYPE RichEditOleCallback_GetNewStorage(
    IRichEditOleCallback* This,
    LPSTORAGE *lplpstg)
{
    WCHAR name[32];
    static const WCHAR template[] = {'R','E','O','L','E','_','%','u','\0'};

    WINE_TRACE("(%p, %p)\n", This, lplpstg);
    wsprintfW(name, template, olecallback.item_num++);
    return IStorage_CreateStorage(olecallback.stg, name,
                      STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE,
                      0, 0, lplpstg);
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_GetInPlaceContext(
    IRichEditOleCallback* This,
    LPOLEINPLACEFRAME *lplpFrame,
    LPOLEINPLACEUIWINDOW *lplpDoc,
    LPOLEINPLACEFRAMEINFO lpFrameInfo)
{
    WINE_FIXME("(%p, %p, %p, %p) stub\n", This, lplpFrame, lplpDoc, lpFrameInfo);
    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_ShowContainerUI(
    IRichEditOleCallback* This,
    BOOL fShow)
{
    WINE_TRACE("(%p, %d)\n", This, fShow);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_QueryInsertObject(
    IRichEditOleCallback* This,
    LPCLSID lpclsid,
    LPSTORAGE lpstg,
    LONG cp)
{
    WINE_TRACE("(%p, %p, %p, %d)\n", This, lpclsid, lpstg, cp);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_DeleteObject(
    IRichEditOleCallback* This,
    LPOLEOBJECT lpoleobj)
{
    WINE_TRACE("(%p, %p)\n", This, lpoleobj);
    return S_OK;
}

/* Rich Edit also accepts rich text via drag/drop, outside WM_PASTE.
 * Inspect the OLE data object, then ask Rich Edit for text rather than a
 * nested row definition when the destination is an existing cell. */
static BOOL rwp_dataobject_has_table_markup(IDataObject *object)
{
    UINT format = RegisterClipboardFormatW(L"Rich Text Format");
    FORMATETC request;
    STGMEDIUM medium;
    const BYTE *bytes;
    BOOL found = FALSE;
    if (!object || !format) return FALSE;
    ZeroMemory(&request, sizeof(request));
    ZeroMemory(&medium, sizeof(medium));
    request.cfFormat = (CLIPFORMAT)format;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = -1;
    request.tymed = TYMED_HGLOBAL;
    if (FAILED(IDataObject_GetData(object, &request, &medium))) return FALSE;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
    {
        bytes = GlobalLock(medium.hGlobal);
        if (bytes)
        {
            found = rwp_rtf_data_has_table_markup(bytes, GlobalSize(medium.hGlobal));
            GlobalUnlock(medium.hGlobal);
        }
    }
    ReleaseStgMedium(&medium);
    return found;
}

static BOOL rwp_dataobject_has_text_format(IDataObject *object, CLIPFORMAT format)
{
    FORMATETC request;
    ZeroMemory(&request, sizeof(request));
    request.cfFormat = format;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = -1;
    request.tymed = TYMED_HGLOBAL;
    return SUCCEEDED(IDataObject_QueryGetData(object, &request));
}

static LONG rwp_ole_target_cp(DWORD reco)
{
    CHARRANGE selection;
    if (!rwpCallbackEditor) return -1;
    if (reco == RECO_DROP)
    {
        POINT point;
        POINTL local;
        RECT client;
        LRESULT cp;
        if (!GetCursorPos(&point)) return -1;
        ScreenToClient(rwpCallbackEditor, &point);
        GetClientRect(rwpCallbackEditor, &client);
        if (!PtInRect(&client, point)) return -1;
        local.x = point.x;
        local.y = point.y;
        cp = SendMessageW(rwpCallbackEditor, EM_CHARFROMPOS, 0, (LPARAM)&local);
        return cp >= 0 ? (LONG)cp : -1;
    }
    SendMessageW(rwpCallbackEditor, EM_EXGETSEL, 0, (LPARAM)&selection);
    return selection.cpMin;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_QueryAcceptData(
    IRichEditOleCallback* This,
    LPDATAOBJECT lpdataobj,
    CLIPFORMAT *lpcfFormat,
    DWORD reco,
    BOOL fReally,
    HGLOBAL hMetaPict)
{
    WINE_TRACE("(%p, %p, %p, %x, %d, %p)\n",
               This, lpdataobj, lpcfFormat, reco, fReally, hMetaPict);

    if (lpdataobj && lpcfFormat && rwpCallbackEditor &&
        rwp_dataobject_has_table_markup(lpdataobj) &&
        rwp_editor_position_inside_table(rwpCallbackEditor, rwp_ole_target_cp(reco)))
    {
        /* Force a non-tabular format for drops and native OLE pastes.
         * When no text alternative exists, reject instead of inserting
         * unsupported nested tables into the user's document. */
        if (rwp_dataobject_has_text_format(lpdataobj, CF_UNICODETEXT))
            *lpcfFormat = CF_UNICODETEXT;
        else if (rwp_dataobject_has_text_format(lpdataobj, CF_TEXT))
            *lpcfFormat = CF_TEXT;
        else return E_FAIL;
        return S_OK;
    }

    if (fReally && reco == RECO_DROP && lpdataobj && rwpCallbackEditor)
    {
        FORMATETC requested;
        STGMEDIUM received;
        ZeroMemory(&requested, sizeof(requested));
        ZeroMemory(&received, sizeof(received));
        requested.cfFormat = CF_HDROP;
        requested.dwAspect = DVASPECT_CONTENT;
        requested.lindex = -1;
        requested.tymed = TYMED_HGLOBAL;
        if (SUCCEEDED(IDataObject_GetData(lpdataobj, &requested, &received)))
        {
            if (received.tymed == TYMED_HGLOBAL && received.hGlobal &&
                DragQueryFileW((HDROP)received.hGlobal, 0xFFFFFFFF, NULL, 0) == 1)
            {
                WCHAR path[MAX_PATH];
                UINT chars = DragQueryFileW((HDROP)received.hGlobal, 0,
                                            path, MAX_PATH);
                if (chars > 0 && chars < MAX_PATH)
                    rwp_prepare_explorer_image_drop(rwpCallbackEditor, path);
            }
            ReleaseStgMedium(&received);
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_ContextSensitiveHelp(
    IRichEditOleCallback* This,
    BOOL fEnterMode)
{
    WINE_TRACE("(%p, %d)\n", This, fEnterMode);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_GetClipboardData(
    IRichEditOleCallback* This,
    CHARRANGE *lpchrg,
    DWORD reco,
    LPDATAOBJECT *lplpdataobj)
{
    WINE_TRACE("(%p, %p, %x, %p)\n", This, lpchrg, reco, lplpdataobj);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_GetDragDropEffect(
    IRichEditOleCallback* This,
    BOOL fDrag,
    DWORD grfKeyState,
    LPDWORD pdwEffect)
{
    WINE_TRACE("(%p, %d, %x, %p)\n", This, fDrag, grfKeyState, pdwEffect);
    if (pdwEffect)
          *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}

/* Convert the current right-click location into a Rich Edit character index.
 * The OLE callback supplies the current selection, not the clicked position.
 * When the mouse is outside the edit control (keyboard context menu), use the
 * supplied selection instead. */
static LONG rwp_ole_context_char_position(const CHARRANGE *selection)
{
    POINT mouse;
    POINTL local;
    RECT client;
    LRESULT cp;

    if (rwpCallbackEditor && IsWindow(rwpCallbackEditor) &&
        GetCursorPos(&mouse))
    {
        ScreenToClient(rwpCallbackEditor, &mouse);
        GetClientRect(rwpCallbackEditor, &client);
        if (PtInRect(&client, mouse))
        {
            local.x = mouse.x;
            local.y = mouse.y;
            cp = SendMessageW(rwpCallbackEditor, EM_CHARFROMPOS,
                              0, (LPARAM)&local);
            if (cp >= 0 && cp != (LRESULT)-1)
                return (LONG)cp;
        }
    }
    return selection ? selection->cpMin : -1;
}

static HRESULT STDMETHODCALLTYPE RichEditOleCallback_GetContextMenu(
    IRichEditOleCallback* This,
    WORD seltype,
    LPOLEOBJECT lpoleobj,
    CHARRANGE *lpchrg,
    HMENU *lphmenu)
{
    HINSTANCE hInstance = GetModuleHandleW(0);
    HMENU hPopupMenu = LoadMenuW(hInstance, MAKEINTRESOURCEW(IDM_POPUP));
    LONG clickedCp = rwp_ole_context_char_position(lpchrg);
    BOOL inTable = rwp_set_table_context_for_popup(rwpCallbackEditor, clickedCp);
    BOOL inObject = rwp_prepare_attachment_export_context(rwpCallbackEditor,
                                                       clickedCp, lpoleobj);

    WINE_TRACE("(%p, %x, %p, %p, %p)\n",
               This, seltype, lpoleobj, lpchrg, lphmenu);

    *lphmenu = hPopupMenu ? GetSubMenu(hPopupMenu, 0) : NULL;
    if (*lphmenu)
    {
        /* Rich Edit normally uses this OLE popup, not the fallback popup
         * built by wordpad.c. Add the shared plain-text paste command here. */
        rwp_insert_plain_paste_menu_item(*lphmenu, FALSE);
        rwp_insert_capitalization_menu(*lphmenu,
                                        GetMenuItemCount(*lphmenu),
                                        rwpCallbackEditor);
        /* Rich Edit normally requests its visible popup via this OLE
         * callback; the fallback WM_CONTEXTMENU path is not sufficient. */
        rwp_append_remove_link_to_popup(*lphmenu, rwpCallbackEditor, clickedCp);
    }

    /* Show the sizing command only for inline PNG/JPEG pictures, not OLE icons. */
    if (*lphmenu && inObject && rwp_has_image_display_context())
    {
        const BOOL italian =
            PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN;
        const UINT flags = MF_STRING |
            ((SendMessageW(rwpCallbackEditor, EM_GETOPTIONS, 0, 0) &
              ECO_READONLY) ? MF_GRAYED : 0);
        HMENU imageOptions = CreatePopupMenu();
        if (imageOptions)
        {
            AppendMenuW(imageOptions, flags, ID_IMAGE_DISPLAY_SIZE,
                        italian ? L"Dimensioni di visualizzazione..." :
                                  L"Image display size...");
            AppendMenuW(imageOptions, flags, ID_IMAGE_COMPRESS_SINGLE,
                        italian ? L"Comprimi in JPG..." :
                                  L"Compress to JPEG...");
            AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(*lphmenu, MF_POPUP, (UINT_PTR)imageOptions,
                        italian ? L"Opzioni immagine" : L"Image options");
        }
    }

    if (*lphmenu && inObject)
    {
        const BOOL italian =
            PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN;
        AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(*lphmenu, MF_STRING, ID_EXPORT_ATTACHMENT,
                    italian ? L"Esporta allegato..." :
                              L"Export attachment...");
    }

    if (*lphmenu)
    {
        const BOOL italian =
            PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN;
        const UINT flags = MF_STRING |
            ((SendMessageW(rwpCallbackEditor, EM_GETOPTIONS, 0, 0) &
              ECO_READONLY) ? MF_GRAYED : 0);
        rwp_set_insert_context_for_popup(rwpCallbackEditor, clickedCp);
        AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(*lphmenu, flags, ID_INSERT_ATTACHMENT,
                    italian ? L"Aggiungi immagine o file..." :
                              L"Add image or file...");
    }

    /* This callback owns the visible context menu (not WM_CONTEXTMENU in
     * wordpad.c). Display table commands only when the clicked character
     * belongs to a table. The table target remains recorded for WM_COMMAND,
     * without moving the caret or modifying the document selection. */
    if (*lphmenu && inTable)
    {
        const BOOL italian =
            PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN;
        const UINT itemFlags = MF_STRING |
            ((SendMessageW(rwpCallbackEditor, EM_GETOPTIONS, 0, 0) &
              ECO_READONLY) ? MF_GRAYED : 0);

        /* Copying is valid even when the document is read-only. */
        AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(*lphmenu, MF_STRING, ID_TABLE_COPY_COLUMN,
                    italian ? L"Copia colonna" : L"Copy column");
        AppendMenuW(*lphmenu, MF_STRING, ID_TABLE_SELECT_COLUMN,
                    italian ? L"Seleziona colonna" : L"Select column");
        AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(*lphmenu, MF_STRING, ID_TABLE_COPY_BORDER_FORMAT,
                    italian ? L"Copia formato bordi cella" :
                              L"Copy cell border format");
        if (rwp_has_copied_cell_border_format())
            AppendMenuW(*lphmenu, itemFlags, ID_TABLE_PASTE_BORDER_FORMAT,
                        italian ? L"Incolla formato bordi cella" :
                                  L"Paste cell border format");
        AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(*lphmenu, itemFlags, ID_TABLE_ADD_ROW,
                    italian ? L"Aggiungi riga..." : L"Add row...");
        AppendMenuW(*lphmenu, itemFlags, ID_TABLE_ADD_COLUMN,
                    italian ? L"Aggiungi colonna..." : L"Add column...");
        AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(*lphmenu, itemFlags, ID_TABLE_DELETE_ROW,
                    italian ? L"Rimuovi riga" : L"Remove row");
        AppendMenuW(*lphmenu, itemFlags, ID_TABLE_DELETE_COLUMN,
                    italian ? L"Rimuovi colonna" : L"Remove column");
        AppendMenuW(*lphmenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(*lphmenu, itemFlags, ID_TABLE_MANAGE,
                    italian ? L"Gestione tabella..." : L"Table management...");
    }
    return S_OK;
}

struct IRichEditOleCallbackVtbl olecallbackVtbl = {
    RichEditOleCallback_QueryInterface,
    RichEditOleCallback_AddRef,
    RichEditOleCallback_Release,
    RichEditOleCallback_GetNewStorage,
    RichEditOleCallback_GetInPlaceContext,
    RichEditOleCallback_ShowContainerUI,
    RichEditOleCallback_QueryInsertObject,
    RichEditOleCallback_DeleteObject,
    RichEditOleCallback_QueryAcceptData,
    RichEditOleCallback_ContextSensitiveHelp,
    RichEditOleCallback_GetClipboardData,
    RichEditOleCallback_GetDragDropEffect,
    RichEditOleCallback_GetContextMenu
};

struct IRichEditOleCallbackImpl olecallback = {
    &olecallbackVtbl, NULL, 0
};

HRESULT setup_richedit_olecallback(HWND hEditorWnd)
{
    HRESULT hr = StgCreateDocfile(NULL,
          STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_DELETEONRELEASE,
          0, &olecallback.stg);

    rwpCallbackEditor = hEditorWnd;
    SendMessageW(hEditorWnd, EM_SETOLECALLBACK, 0, (LPARAM)&olecallback);
    return hr;
}
