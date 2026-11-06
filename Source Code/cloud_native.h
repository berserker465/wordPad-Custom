/* Native Google Drive upload interface for RWordPad. No PowerShell dependency. */
#ifndef RWP_CLOUD_NATIVE_H
#define RWP_CLOUD_NATIVE_H
#include <windows.h>
/* Called on the WordPad UI thread on success/cancellation. */
typedef void (*RWP_DRIVE_COMPLETE)(BOOL succeeded, LPCWSTR remoteID, void *userData);
/* Invoked on the UI thread for silent automatic uploads. */
typedef void (*RWP_DRIVE_AUTO_COMPLETE)(BOOL succeeded, LPCWSTR remoteID,
                                        LPCWSTR error, void *userData);
BOOL rwp_drive_native_upload_async(HWND owner, LPCWSTR document, LPCWSTR folderOverride,
                                   LPCWSTR documentID, LPCWSTR remoteID,
                                   UINT maxVersions, UINT retentionDays,
                                   RWP_DRIVE_COMPLETE callback, void *userData);
HWND rwp_drive_upload_window(void);
BOOL rwp_drive_auto_busy(void);
BOOL rwp_drive_native_auto_upload_async(HWND owner, LPCWSTR document,
                                        LPCWSTR savedSnapshot, LPCWSTR folder,
                                        LPCWSTR documentID, LPCWSTR remoteID,
                                        UINT maxVersions, UINT retentionDays,
                                        RWP_DRIVE_AUTO_COMPLETE callback,
                                        void *userData);
void rwp_cloud_apply_theme(HWND dialog);
/* Read-only folder metadata lookup; call from a background thread only.
 * Returns a status, writes the UTF-16 display name on success.
 * No browser login is launched; the stored OAuth refresh token is reused. */
#define RWP_DRIVE_FOLDER_OK 0
#define RWP_DRIVE_FOLDER_NO_LOGIN 1
#define RWP_DRIVE_FOLDER_NOT_FOUND 2
#define RWP_DRIVE_FOLDER_NOT_FOLDER 3
#define RWP_DRIVE_FOLDER_ERROR 4
int rwp_drive_native_folder_name(LPCWSTR folderID, WCHAR *name, UINT capacity);
/* Read-only revision history and separate-file recovery. Does not change the
 * current document or prune remote history directly. Returns FALSE if already open. */
BOOL rwp_drive_native_history_open(HWND owner, LPCWSTR document,
                                   LPCWSTR folder, LPCWSTR remoteID,
                                   UINT maxVersions);
#endif
