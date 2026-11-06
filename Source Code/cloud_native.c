/*
 * RWordPad - Native Google Drive transport, Beta 1.5.
 * Uses WinHTTP, Windows CryptoAPI/DPAPI and a WinSock loopback OAuth callback.
 * Document content is never rewritten by this module; a staged copy is used
 * for the first tracked upload so the cloud copy already has its remote ID.
 * Compatibility: reads the Beta 1.4.1 PowerShell DPAPI credential and index.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include "wordpad.h"
#include "cloud_native.h"

#define RWP_DRIVE_MAX_JSON (1024 * 1024)
#define RWP_DRIVE_TIMEOUT 45000
#define RWP_DRIVE_ENTROPY "RWordPad-CloudUploadBeta-DPAPI-v1"
#define RWP_DRIVE_TITLE L"WordPad(Custom) - Upload to Google Drive"
#define RWP_DRIVE_RTF_START "{\\v RWP-CLOUD-V1|"
#define RWP_DRIVE_RTF_END "|/RWP-CLOUD-V1}"

typedef struct RWP_DRIVE_CTX {
    HWND dialog;
    WCHAR document[MAX_PATH];
    WCHAR sourceSnapshot[MAX_PATH]; /* Immutable copy, kept while upload runs. */
    WCHAR docFolder[256];
    WCHAR docID[64];
    WCHAR remoteID[256];
    WCHAR resultID[256];
    WCHAR folder[256];
    char actualFolderID[256];
    char clientID[300];
    char secret[256];
    char refresh[1600];
    char token[3000];
    WCHAR appDir[MAX_PATH];
    WCHAR credentialPath[MAX_PATH];
    WCHAR preferencesPath[MAX_PATH];
    WCHAR indexPath[MAX_PATH];
    char error[1400];
    char retentionWarning[512];
    UINT maxVersions;
    UINT retentionDays;
    BOOL succeeded;
    BOOL busy;
    BOOL automatic;
    RWP_DRIVE_AUTO_COMPLETE autoCallback;
    volatile LONG interrupted;
    volatile LONG refs; /* UI window + upload worker own independent refs. */
    RWP_DRIVE_COMPLETE callback;
    void *userData;
} RWP_DRIVE_CTX;

#define RWP_DRIVE_DONE (WM_APP + 246)
static HWND rwpDriveUploadWnd = NULL;
static HWND rwpDriveAutoWnd = NULL;
BOOL rwp_drive_auto_busy(void)
{
    return rwpDriveAutoWnd && IsWindow(rwpDriveAutoWnd);
}

HWND rwp_drive_upload_window(void)
{
    return rwpDriveUploadWnd;
}

static void rwp_drive_release(RWP_DRIVE_CTX *c)
{
    if (InterlockedDecrement(&c->refs) == 0)
    {
        if (c->sourceSnapshot[0]) DeleteFileW(c->sourceSnapshot);
        SecureZeroMemory(c->token, sizeof(c->token));
        SecureZeroMemory(c->secret, sizeof(c->secret));
        SecureZeroMemory(c->refresh, sizeof(c->refresh));
        free(c);
    }
}


typedef struct RWP_HTTP_RESPONSE {
    DWORD status;
    char *body;
    WCHAR location[2048];
} RWP_HTTP_RESPONSE;

static void rwp_drive_error(RWP_DRIVE_CTX *c, const char *msg)
{
    lstrcpynA(c->error,msg,sizeof(c->error));
}
static void rwp_drive_status(RWP_DRIVE_CTX *c,LPCWSTR message)
{
    if (c->automatic) return;
    SetDlgItemTextW(c->dialog,IDC_CLOUD_NATIVE_STATUS,message);
    UpdateWindow(c->dialog);
}
static void rwp_drive_notice(RWP_DRIVE_CTX *c,LPCWSTR message,UINT flags)
{
    if (c->automatic) return; /* Background upload must never pop up a dialog. */
    MessageBoxW(c->dialog,message,RWP_DRIVE_TITLE,MB_OK|flags);
}
static void rwp_drive_errorbox(RWP_DRIVE_CTX *c)
{
    WCHAR msg[1500];
    if (!c->error[0]) lstrcpynA(c->error,"The Google Drive operation failed.",sizeof(c->error));
    if(!MultiByteToWideChar(CP_UTF8,0,c->error,-1,msg,sizeof(msg)/sizeof(msg[0])))
        lstrcpyW(msg,L"Google Drive operation failed (invalid error response).");
    rwp_drive_notice(c,msg,MB_ICONERROR);
}
static BOOL rwp_drive_readfile(LPCWSTR path,BYTE **out,DWORD *length,DWORD limit)
{
    HANDLE f; LARGE_INTEGER size; DWORD got;
    BYTE *buf;
    *out=NULL; *length=0;
    f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) return FALSE;
    if(!GetFileSizeEx(f,&size)||size.QuadPart<0||size.QuadPart>limit){CloseHandle(f);return FALSE;}
    buf=(BYTE*)malloc((SIZE_T)size.QuadPart+1);
    if(!buf){CloseHandle(f);return FALSE;}
    if(!ReadFile(f,buf,(DWORD)size.QuadPart,&got,NULL)||got!=(DWORD)size.QuadPart){free(buf);CloseHandle(f);return FALSE;}
    CloseHandle(f);buf[got]=0;*length=got;*out=buf;return TRUE;
}
static BOOL rwp_drive_write_atomic(LPCWSTR path,const BYTE *bytes,DWORD length)
{
    WCHAR tmp[MAX_PATH];HANDLE f;DWORD done;BOOL ok;
    if(lstrlenW(path)+35>=MAX_PATH)return FALSE;
    wsprintfW(tmp,L"%s.%lu.%lu.tmp",path,GetCurrentProcessId(),GetTickCount());
    f=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return FALSE;
    ok=WriteFile(f,bytes,length,&done,NULL)&&done==length;
    if(ok)ok=FlushFileBuffers(f);
    CloseHandle(f);
    if(ok)ok=MoveFileExW(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(!ok)DeleteFileW(tmp);
    return ok;
}
static BOOL rwp_drive_ascii(const WCHAR *w,char *out,SIZE_T cap)
{
    SIZE_T i;
    for(i=0;w[i];++i){if(i+1>=cap||w[i]>127)return FALSE;out[i]=(char)w[i];}
    out[i]=0;return TRUE;
}
static BOOL rwp_drive_wide(const char *src,WCHAR *dest,SIZE_T cap)
{
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,src,-1,dest,(int)cap);
    return n>0;
}
static int rwp_drive_hex4(const char *s,unsigned *value)
{
    int i;unsigned u=0;
    for(i=0;i<4;i++){
        char c=s[i];unsigned n;
        if(!c)return 0;
        if(c>='0'&&c<='9')n=(unsigned)(c-'0');
        else if(c>='a'&&c<='f')n=(unsigned)(c-'a'+10);
        else if(c>='A'&&c<='F')n=(unsigned)(c-'A'+10);
        else return 0;
        u=(u<<4)|n;
    }
    *value=u;return 1;
}
static BOOL rwp_drive_json_value(const char *json,const char *key,char *out,SIZE_T cap)
{
    char pattern[130];const char *s,*e;SIZE_T n;
    if(strlen(key)>100)return FALSE;
    sprintf(pattern,"\"%s\"",key);
    s=json;
    while((s=strstr(s,pattern))!=NULL){
        e=s+strlen(pattern);while(*e&&isspace((unsigned char)*e))++e;
        if(*e!=':'){++s;continue;}
        ++e;while(*e&&isspace((unsigned char)*e))++e;
        if(*e!='\"'){++s;continue;}
        ++e;n=0;
        while(*e&&*e!='\"'){
            unsigned ch=(unsigned char)*e++;
            if(ch=='\\'){
                if(!*e)return FALSE;
                ch=(unsigned char)*e++;
                if(ch=='u'){
                    unsigned cp,low;
                    if(!rwp_drive_hex4(e,&cp))return FALSE;e+=4;
                    if(cp>=0xD800&&cp<=0xDBFF){
                        if(e[0]!='\\'||e[1]!='u'||!rwp_drive_hex4(e+2,&low)||
                           low<0xDC00||low>0xDFFF)return FALSE;
                        cp=0x10000+((cp-0xD800)<<10)+(low-0xDC00);e+=6;
                    }else if(cp>=0xDC00&&cp<=0xDFFF)return FALSE;
                    if(cp<0x80){ch=cp;}
                    else if(cp<0x800){
                        if(n+3>cap)return FALSE;
                        out[n++]=(char)(0xC0|(cp>>6));ch=(0x80|(cp&63));
                    }else if(cp<0x10000){
                        if(n+4>cap)return FALSE;
                        out[n++]=(char)(0xE0|(cp>>12));out[n++]=(char)(0x80|((cp>>6)&63));
                        ch=(0x80|(cp&63));
                    }else{
                        if(n+5>cap)return FALSE;
                        out[n++]=(char)(0xF0|(cp>>18));out[n++]=(char)(0x80|((cp>>12)&63));
                        out[n++]=(char)(0x80|((cp>>6)&63));ch=(0x80|(cp&63));
                    }
                }else if(ch=='n')ch='\n';
                else if(ch=='r')ch='\r';
                else if(ch=='t')ch='\t';
                else if(ch=='b')ch='\b';
                else if(ch=='f')ch='\f';
                else if(ch!='\"'&&ch!='\\'&&ch!='/')return FALSE;
            }
            if(ch==0||n+1>=cap)return FALSE;
            out[n++]=(char)ch;
        }
        if(*e!='\"')return FALSE;
        out[n]=0;return TRUE;
    }
    return FALSE;
}
static BOOL rwp_drive_json_true(const char *json,const char *key)
{
    char pattern[130];const char *p;
    if(strlen(key)>100)return FALSE;
    sprintf(pattern,"\"%s\"",key);
    p=json;
    while((p=strstr(p,pattern))!=NULL){
        const char *v=p+strlen(pattern);
        while(*v&&isspace((unsigned char)*v))++v;
        if(*v!=':'){++p;continue;}
        ++v;while(*v&&isspace((unsigned char)*v))++v;
        return !strncmp(v,"true",4) && (v[4]==','||v[4]=='}'||isspace((unsigned char)v[4]));
    }
    return FALSE;
}
static BOOL rwp_drive_base64(const BYTE *src,DWORD len,char **out)
{
    DWORD n=0; char *buf;
    *out=NULL;
    if(!CryptBinaryToStringA(src,len,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,NULL,&n))return FALSE;
    buf=(char*)malloc(n+2);if(!buf)return FALSE;
    if(!CryptBinaryToStringA(src,len,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,buf,&n)){free(buf);return FALSE;}
    *out=buf;return TRUE;
}
static BOOL rwp_drive_unbase64(const char *src,BYTE **out,DWORD *len)
{
    DWORD n=0;BYTE *b;*out=NULL;*len=0;
    if(!CryptStringToBinaryA(src,0,CRYPT_STRING_BASE64,NULL,&n,NULL,NULL))return FALSE;
    b=(BYTE*)malloc(n+1);if(!b)return FALSE;
    if(!CryptStringToBinaryA(src,0,CRYPT_STRING_BASE64,b,&n,NULL,NULL)){free(b);return FALSE;}
    b[n]=0;*out=b;*len=n;return TRUE;
}
static BOOL rwp_drive_dpapi(const char *encoded,char *out,SIZE_T cap)
{
    BYTE *encrypted;DWORD len;DATA_BLOB input,plain,entropy;
    BOOL result=FALSE;
    if(!rwp_drive_unbase64(encoded,&encrypted,&len))return FALSE;
    input.pbData=encrypted;input.cbData=len;
    entropy.pbData=(BYTE*)RWP_DRIVE_ENTROPY;entropy.cbData=(DWORD)strlen(RWP_DRIVE_ENTROPY);
    ZeroMemory(&plain,sizeof(plain));
    if(CryptUnprotectData(&input,NULL,&entropy,NULL,NULL,0,&plain)){
        if(plain.cbData<cap){memcpy(out,plain.pbData,plain.cbData);out[plain.cbData]=0;result=TRUE;}
        SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
    }
    SecureZeroMemory(encrypted,len);free(encrypted);return result;
}
static BOOL rwp_drive_protect(const char *value,char **encoded)
{
    DATA_BLOB input,protectedData,entropy;BOOL result;
    input.pbData=(BYTE*)value;input.cbData=(DWORD)strlen(value);
    entropy.pbData=(BYTE*)RWP_DRIVE_ENTROPY;entropy.cbData=(DWORD)strlen(RWP_DRIVE_ENTROPY);
    ZeroMemory(&protectedData,sizeof(protectedData));
    if(!CryptProtectData(&input,L"RWordPad Cloud Login",&entropy,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&protectedData))return FALSE;
    result=rwp_drive_base64(protectedData.pbData,protectedData.cbData,encoded);
    SecureZeroMemory(protectedData.pbData,protectedData.cbData);LocalFree(protectedData.pbData);
    return result;
}
static BOOL rwp_drive_load_prefs(RWP_DRIVE_CTX *c)
{
    BYTE *bytes;DWORD n;char client[300],folder[256];
    if(!rwp_drive_readfile(c->preferencesPath,&bytes,&n,32768))return FALSE;
    if(rwp_drive_json_value((char*)bytes,"ClientID",client,sizeof(client)))
        lstrcpynA(c->clientID,client,sizeof(c->clientID));
    if(rwp_drive_json_value((char*)bytes,"FolderID",folder,sizeof(folder))&& !c->docFolder[0])
        rwp_drive_wide(folder,c->folder,sizeof(c->folder)/sizeof(c->folder[0]));
    free(bytes);return TRUE;
}
static BOOL rwp_drive_save_prefs(RWP_DRIVE_CTX *c)
{
    char folder[256],buf[1024];
    if(!rwp_drive_ascii(c->folder,folder,sizeof(folder)))return FALSE;
    /* When a per-document folder is selected,keep the original global default. */
    if(c->docFolder[0]){
        BYTE *old;DWORD len;char prev[256];
        folder[0]=0;
        if(rwp_drive_readfile(c->preferencesPath,&old,&len,32768)){
            if(rwp_drive_json_value((char*)old,"FolderID",prev,sizeof(prev)))lstrcpynA(folder,prev,sizeof(folder));
            free(old);
        }
    }
    _snprintf(buf,sizeof(buf)-1,"{\"ClientID\":\"%s\",\"FolderID\":\"%s\"}",c->clientID,folder);
    buf[sizeof(buf)-1]=0;
    return rwp_drive_write_atomic(c->preferencesPath,(BYTE*)buf,(DWORD)strlen(buf));
}
static BOOL rwp_drive_load_saved(RWP_DRIVE_CTX *c)
{
    BYTE *bytes;DWORD len;char client[300],secret[7000],refresh[7000];
    if(!rwp_drive_readfile(c->credentialPath,&bytes,&len,32768))return FALSE;
    if(!rwp_drive_json_value((char*)bytes,"ClientID",client,sizeof(client))||
       strcmp(client,c->clientID)||
       !rwp_drive_json_value((char*)bytes,"ProtectedClientSecret",secret,sizeof(secret))||
       !rwp_drive_json_value((char*)bytes,"ProtectedRefreshToken",refresh,sizeof(refresh))){free(bytes);return FALSE;}
    free(bytes);
    if(!rwp_drive_dpapi(secret,c->secret,sizeof(c->secret))||
       !rwp_drive_dpapi(refresh,c->refresh,sizeof(c->refresh)))return FALSE;
    return TRUE;
}
static BOOL rwp_drive_save_saved(RWP_DRIVE_CTX *c)
{
    char *sec=NULL,*ref=NULL;char *buf=NULL;SIZE_T needed;BOOL ok=FALSE;
    if(!rwp_drive_protect(c->secret,&sec)||!rwp_drive_protect(c->refresh,&ref))goto done;
    needed=strlen(sec)+strlen(ref)+strlen(c->clientID)+256;
    buf=(char*)malloc(needed);if(!buf)goto done;
    _snprintf(buf,needed-1,"{\"SchemaVersion\":1,\"ClientID\":\"%s\",\"ProtectedClientSecret\":\"%s\",\"ProtectedRefreshToken\":\"%s\"}",c->clientID,sec,ref);
    buf[needed-1]=0;
    ok=rwp_drive_write_atomic(c->credentialPath,(BYTE*)buf,(DWORD)strlen(buf));
done:
    if(sec){SecureZeroMemory(sec,strlen(sec));free(sec);}
    if(ref){SecureZeroMemory(ref,strlen(ref));free(ref);}
    if(buf){SecureZeroMemory(buf,needed);free(buf);}
    return ok;
}
static BOOL rwp_drive_urlencode(const char *src,char *dst,SIZE_T cap)
{
    static const char hexdigits[]="0123456789ABCDEF";
    SIZE_T n=0;unsigned char c;
    while((c=(unsigned char)*src++)!=0){
        if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~'){
            if(n+2>cap)return FALSE;dst[n++]=(char)c;
        }else{
            if(n+4>cap)return FALSE;dst[n++]='%';dst[n++]=hexdigits[c>>4];dst[n++]=hexdigits[c&15];
        }
    }
    if(n>=cap)return FALSE;dst[n]=0;return TRUE;
}
static void rwp_drive_http_free(RWP_HTTP_RESPONSE *r)
{
    if(r->body)free(r->body);
    ZeroMemory(r,sizeof(*r));
}
/* Synchronous TLS request. Tokens are passed in headers,not URLs. */
static BOOL rwp_drive_http(const WCHAR *host,const WCHAR *path,const WCHAR *method,
                           const char *token,const char *headers,const BYTE *payload,DWORD payloadLen,
                           LPCWSTR filePath,RWP_HTTP_RESPONSE *response)
{
    HINTERNET session=NULL,connection=NULL,request=NULL;
    WCHAR headerText[5000]={0},*p;DWORD dwSize,statusSize=sizeof(DWORD),read=0,available,used=0;
    DWORD total=payloadLen;BOOL ok=FALSE;
    BYTE *body=NULL;HANDLE file=INVALID_HANDLE_VALUE;
    LARGE_INTEGER fileSize;DWORD wrote;
    ZeroMemory(response,sizeof(*response));
    if(filePath){
        file=CreateFileW(filePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file==INVALID_HANDLE_VALUE||!GetFileSizeEx(file,&fileSize)||fileSize.QuadPart>0xFFFFFFFFULL)goto done;
        total=(DWORD)fileSize.QuadPart;
    }
    session=WinHttpOpen(L"RWordPad/1.5",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session)goto done;
    WinHttpSetTimeouts(session,RWP_DRIVE_TIMEOUT,RWP_DRIVE_TIMEOUT,RWP_DRIVE_TIMEOUT,600000);
    connection=WinHttpConnect(session,host,INTERNET_DEFAULT_HTTPS_PORT,0);
    if(!connection)goto done;
    request=WinHttpOpenRequest(connection,method,path,NULL,WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if(!request)goto done;
    headerText[0]=0;
    if(token&&*token){
        WCHAR wtoken[3000];
        if(!rwp_drive_wide(token,wtoken,sizeof(wtoken)/sizeof(wtoken[0])))goto done;
        if(lstrlenW(wtoken)+40>=sizeof(headerText)/sizeof(headerText[0]))goto done;
        wsprintfW(headerText,L"Authorization: Bearer %s\r\n",wtoken);
        SecureZeroMemory(wtoken,sizeof(wtoken));
    }
    if(headers&&*headers){
        WCHAR wheader[1800];
        if(!rwp_drive_wide(headers,wheader,sizeof(wheader)/sizeof(wheader[0])) ||
           lstrlenW(headerText)+lstrlenW(wheader)+1>=sizeof(headerText)/sizeof(headerText[0]))goto done;
        lstrcatW(headerText,wheader);
    }
    p=headerText;
    if(*p&&!WinHttpAddRequestHeaders(request,p,(DWORD)-1L,WINHTTP_ADDREQ_FLAG_ADD))goto done;
    if(!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,
         filePath?WINHTTP_NO_REQUEST_DATA:(LPVOID)payload,
         filePath?0:payloadLen,total,0))goto done;
    if(filePath){
        BYTE chunk[65536];
        for(;;){
            if(!ReadFile(file,chunk,sizeof(chunk),&read,NULL))goto done;
            if(!read)break;
            if(!WinHttpWriteData(request,chunk,read,&wrote)||wrote!=read)goto done;
        }
    }
    if(!WinHttpReceiveResponse(request,NULL))goto done;
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,&response->status,&statusSize,WINHTTP_NO_HEADER_INDEX))goto done;
    dwSize=sizeof(response->location);
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,
                            response->location,&dwSize,WINHTTP_NO_HEADER_INDEX))response->location[0]=0;
    body=(BYTE*)malloc(1);if(!body)goto done;body[0]=0;
    for(;;){
        if(!WinHttpQueryDataAvailable(request,&available))goto done;
        if(!available)break;
        if(available>RWP_DRIVE_MAX_JSON-used)goto done;
        {
            BYTE *next=(BYTE*)realloc(body,used+available+1);
            if(!next)goto done;body=next;
        }
        if(!WinHttpReadData(request,body+used,available,&read))goto done;
        if(!read)break;used+=read;body[used]=0;
    }
    response->body=(char*)body;body=NULL;ok=TRUE;
done:
    if(body)free(body);
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    if(request)WinHttpCloseHandle(request);
    if(connection)WinHttpCloseHandle(connection);
    if(session)WinHttpCloseHandle(session);
    SecureZeroMemory(headerText,sizeof(headerText));
    if(!ok)rwp_drive_http_free(response);
    return ok;
}
static BOOL rwp_drive_request(RWP_DRIVE_CTX *c,const WCHAR *host,const WCHAR *path,
                              const WCHAR *method,const char *token,const char *headers,
                              const BYTE *payload,DWORD length,LPCWSTR file,RWP_HTTP_RESPONSE *r)
{
    if(!rwp_drive_http(host,path,method,token,headers,payload,length,file,r)){
        rwp_drive_error(c,"Windows HTTPS request failed. Check network access, TLS and system proxy settings.");return FALSE;
    }
    if(r->status<200||r->status>299){
        char message[1400];
        _snprintf(message,sizeof(message)-1,"Google request returned HTTP %lu: %.900s",r->status,r->body?r->body:"");
        message[sizeof(message)-1]=0;rwp_drive_error(c,message);return FALSE;
    }
    return TRUE;
}
static BOOL rwp_drive_crypto_hash_file(LPCWSTR path,ALG_ID type,char *result,SIZE_T cap)
{
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;BYTE digest[64],buf[65536];
    DWORD n,dlen=sizeof(digest);HANDLE f=INVALID_HANDLE_VALUE;BOOL ok=FALSE;
    static const char hex[]="0123456789abcdef";DWORD i;
    if(!CryptAcquireContextW(&provider,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))goto done;
    if(!CryptCreateHash(provider,type,0,0,&hash))goto done;
    f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)goto done;
    for(;;){if(!ReadFile(f,buf,sizeof(buf),&n,NULL))goto done;if(!n)break;if(!CryptHashData(hash,buf,n,0))goto done;}
    if(!CryptGetHashParam(hash,HP_HASHVAL,digest,&dlen,0)||cap<dlen*2+1)goto done;
    for(i=0;i<dlen;++i){result[2*i]=hex[digest[i]>>4];result[2*i+1]=hex[digest[i]&15];}
    result[dlen*2]=0;ok=TRUE;
done:
    if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);
    if(hash)CryptDestroyHash(hash);
    if(provider)CryptReleaseContext(provider,0);
    return ok;
}
static BOOL rwp_drive_sha256(const BYTE *bytes,DWORD len,BYTE out[32])
{
    HCRYPTPROV prov=0;HCRYPTHASH hash=0;DWORD n=32;BOOL ok=FALSE;
    if(!CryptAcquireContextW(&prov,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))return FALSE;
    if(CryptCreateHash(prov,CALG_SHA_256,0,0,&hash)&&CryptHashData(hash,bytes,len,0))
        ok=CryptGetHashParam(hash,HP_HASHVAL,out,&n,0)&&n==32;
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(prov,0);return ok;
}
static BOOL rwp_drive_random(BYTE *data,DWORD len)
{
    HCRYPTPROV p=0;BOOL ok=FALSE;
    if(CryptAcquireContextW(&p,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)){
        ok=CryptGenRandom(p,len,data);CryptReleaseContext(p,0);
    }
    return ok;
}
static void rwp_drive_base64url(char *str)
{
    char *p=str,*out=str;while(*p){if(*p=='+')*out='-';else if(*p=='/')*out='_';else if(*p!='=')*out=*p;++p;if(p[-1]!='=')++out;}*out=0;
}
static BOOL rwp_drive_token_exchange(RWP_DRIVE_CTX *c,const char *code,
                                     const char *verifier,const char *redirect,BOOL refreshing)
{
    char escapedID[1000],escapedSecret[1000],escapedCode[3000],escapedVerifier[500],escapedRedirect[500];
    char body[6500],refreshCopy[1600];RWP_HTTP_RESPONSE r;BOOL ok=FALSE;
    if(!rwp_drive_urlencode(c->clientID,escapedID,sizeof(escapedID))||
       !rwp_drive_urlencode(c->secret,escapedSecret,sizeof(escapedSecret)))return FALSE;
    if(refreshing){
        if(!rwp_drive_urlencode(c->refresh,refreshCopy,sizeof(refreshCopy)))return FALSE;
        _snprintf(body,sizeof(body)-1,"client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",escapedID,escapedSecret,refreshCopy);
    }else{
        if(!rwp_drive_urlencode(code,escapedCode,sizeof(escapedCode))||
           !rwp_drive_urlencode(verifier,escapedVerifier,sizeof(escapedVerifier))||
           !rwp_drive_urlencode(redirect,escapedRedirect,sizeof(escapedRedirect)))return FALSE;
        _snprintf(body,sizeof(body)-1,"client_id=%s&client_secret=%s&code=%s&code_verifier=%s&redirect_uri=%s&grant_type=authorization_code",escapedID,escapedSecret,escapedCode,escapedVerifier,escapedRedirect);
    }
    body[sizeof(body)-1]=0;
    if(!rwp_drive_http(L"oauth2.googleapis.com",L"/token",L"POST",NULL,
            "Content-Type: application/x-www-form-urlencoded\r\n",
            (BYTE*)body,(DWORD)strlen(body),NULL,&r)){
        rwp_drive_error(c,"OAuth HTTPS token request could not be completed.");goto done;
    }
    if(r.status!=200){
        _snprintf(c->error,sizeof(c->error)-1,"Google OAuth token exchange returned HTTP %lu: %.900s",r.status,r.body?r.body:"");
        c->error[sizeof(c->error)-1]=0;
        rwp_drive_http_free(&r);goto done;
    }
    if(!rwp_drive_json_value(r.body,"access_token",c->token,sizeof(c->token))){
        rwp_drive_error(c,"Google returned a token response without an access token.");rwp_drive_http_free(&r);goto done;
    }
    if(!refreshing){
        char token[1600];
        if(rwp_drive_json_value(r.body,"refresh_token",token,sizeof(token))){
            lstrcpynA(c->refresh,token,sizeof(c->refresh));
            if(!rwp_drive_save_saved(c))
                rwp_drive_notice(c,L"Google login succeeded,but encrypted credential storage failed. You may need to sign in next time.",MB_ICONWARNING);
        }else rwp_drive_notice(c,L"Google did not issue a refresh token. This upload can continue, but a future upload may require signing in again.",MB_ICONWARNING);
    }
    rwp_drive_http_free(&r);ok=TRUE;
done:
    SecureZeroMemory(body,sizeof(body));SecureZeroMemory(refreshCopy,sizeof(refreshCopy));
    return ok;
}
static int rwp_drive_hex(char c)
{
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;return -1;
}
static BOOL rwp_drive_query_param(const char *path,const char *key,char *value,SIZE_T cap)
{
    const char *q=strchr(path,'?'),*end;SIZE_T n=0,keylen=strlen(key);
    if(!q)return FALSE;
    ++q;
    while(*q){
        end=strchr(q,'&');if(!end)end=q+strlen(q);
        if((SIZE_T)(end-q)>keylen&&memcmp(q,key,keylen)==0&&q[keylen]=='='){
            q+=keylen+1;
            while(q<end){
                if(n+1>=cap)return FALSE;
                if(*q=='%'&&q+2<end){int a=rwp_drive_hex(q[1]),b=rwp_drive_hex(q[2]);if(a<0||b<0)return FALSE;value[n++]=(char)((a<<4)|b);q+=3;}
                else {value[n++]=(*q=='+')?' ':*q;++q;}
            }
            value[n]=0;return TRUE;
        }
        if(!*end)break;q=end+1;
    }
    return FALSE;
}
static BOOL rwp_drive_login_browser(RWP_DRIVE_CTX *c)
{
    WSADATA wsa;SOCKET server=INVALID_SOCKET;SOCKET peer=INVALID_SOCKET;
    struct sockaddr_in address;int len=sizeof(address),sel;fd_set fds;
    struct timeval wait;DWORD start=GetTickCount();
    BYTE random1[48],random2[32],sha[32];char *encoded=NULL;
    char verifier[160],state[160],challenge[160],url[2400],redirect[130];
    char clientEnc[1000],scopeEnc[300],redirectEnc[300],code[2000],stateGot[200];
    char request[9000];WCHAR urlW[2600];BOOL ok=FALSE;
    if(WSAStartup(MAKEWORD(2,2),&wsa)!=0){rwp_drive_error(c,"WinSock initialization failed.");return FALSE;}
    server=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(server==INVALID_SOCKET){rwp_drive_error(c,"Cannot open OAuth loopback socket.");goto done;}
    ZeroMemory(&address,sizeof(address));address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
    if(bind(server,(struct sockaddr*)&address,sizeof(address))!=0||listen(server,1)!=0||
       getsockname(server,(struct sockaddr*)&address,&len)!=0){
        rwp_drive_error(c,"Cannot listen on the local OAuth callback port.");goto done;
    }
    if(!rwp_drive_random(random1,sizeof(random1))||!rwp_drive_random(random2,sizeof(random2))||
       !rwp_drive_sha256(random1,sizeof(random1),sha)){
        rwp_drive_error(c,"Unable to generate secure OAuth PKCE random data.");goto done;
    }
    if(!rwp_drive_base64(random1,sizeof(random1),&encoded))goto done;
    lstrcpynA(verifier,encoded,sizeof(verifier));free(encoded);encoded=NULL;rwp_drive_base64url(verifier);
    if(!rwp_drive_sha256((BYTE*)verifier,(DWORD)strlen(verifier),sha))goto done;
    if(!rwp_drive_base64(sha,sizeof(sha),&encoded))goto done;
    lstrcpynA(challenge,encoded,sizeof(challenge));free(encoded);encoded=NULL;rwp_drive_base64url(challenge);
    if(!rwp_drive_base64(random2,sizeof(random2),&encoded))goto done;
    lstrcpynA(state,encoded,sizeof(state));free(encoded);encoded=NULL;rwp_drive_base64url(state);
    sprintf(redirect,"http://127.0.0.1:%u/",(unsigned)ntohs(address.sin_port));
    if(!rwp_drive_urlencode(c->clientID,clientEnc,sizeof(clientEnc))||
       !rwp_drive_urlencode("https://www.googleapis.com/auth/drive",scopeEnc,sizeof(scopeEnc))||
       !rwp_drive_urlencode(redirect,redirectEnc,sizeof(redirectEnc)))goto done;
    _snprintf(url,sizeof(url)-1,
       "https://accounts.google.com/o/oauth2/v2/auth?client_id=%s&response_type=code&redirect_uri=%s&scope=%s&access_type=offline&prompt=consent&state=%s&code_challenge=%s&code_challenge_method=S256",
       clientEnc,redirectEnc,scopeEnc,state,challenge);
    url[sizeof(url)-1]=0;
    if(!rwp_drive_wide(url,urlW,sizeof(urlW)/sizeof(urlW[0])))goto done;
    if((INT_PTR)ShellExecuteW(c->dialog,L"open",urlW,NULL,NULL,SW_SHOWNORMAL)<=32){
        rwp_drive_error(c,"Unable to open the default browser for Google sign-in.");goto done;
    }
    rwp_drive_status(c,L"Complete the Google sign-in in your browser (up to 3 minutes)...");
    while(GetTickCount()-start<180000){
        char *firstEnd,*path,reply[800],got[8192];int received;
        if(InterlockedCompareExchange(&c->interrupted,0,0)){
            rwp_drive_error(c,"Login cancelled.");goto done;
        }
        FD_ZERO(&fds);FD_SET(server,&fds);wait.tv_sec=0;wait.tv_usec=100000;
        sel=select(0,&fds,NULL,NULL,&wait);
        if(sel==SOCKET_ERROR){rwp_drive_error(c,"OAuth callback socket failed.");goto done;}
        if(sel==0)continue;
        peer=accept(server,NULL,NULL);if(peer==INVALID_SOCKET)continue;
        {
            DWORD timeout=5000;
            setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,(const char*)&timeout,sizeof(timeout));
            received=recv(peer,got,sizeof(got)-1,0);
        }
        if(received<=0){closesocket(peer);peer=INVALID_SOCKET;continue;}
        got[received]=0;firstEnd=strstr(got," HTTP/1.");
        if(strncmp(got,"GET /",5)!=0||!firstEnd){closesocket(peer);peer=INVALID_SOCKET;continue;}
        *firstEnd=0;path=got+4;
        code[0]=0;stateGot[0]=0;
        if(rwp_drive_query_param(path,"state",stateGot,sizeof(stateGot)) &&
           !strcmp(stateGot,state) &&rwp_drive_query_param(path,"code",code,sizeof(code)) &&code[0]){
            lstrcpyA(reply,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n<html><body><h3>Authorization received.</h3><p>Return to WordPad(Custom). You may close this tab.</p></body></html>");
            send(peer,reply,(int)strlen(reply),0);closesocket(peer);peer=INVALID_SOCKET;
            rwp_drive_status(c,L"Exchanging Google authorization code...");
            ok=rwp_drive_token_exchange(c,code,verifier,redirect,FALSE);
            goto done;
        }
        lstrcpyA(reply,"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html><body><h3>Authorization not completed.</h3></body></html>");
        send(peer,reply,(int)strlen(reply),0);closesocket(peer);peer=INVALID_SOCKET;
    }
    rwp_drive_error(c,"Google login timed out after 180 seconds.");
done:
    if(peer!=INVALID_SOCKET)closesocket(peer);
    if(server!=INVALID_SOCKET)closesocket(server);
    WSACleanup();
    if(encoded)free(encoded);
    SecureZeroMemory(verifier,sizeof(verifier));SecureZeroMemory(code,sizeof(code));
    return ok;
}
static BOOL rwp_drive_authorize(RWP_DRIVE_CTX *c)
{
    char previousError[1400];
    /* No implicit browser login during automatic backups. */
    if (c->automatic && (!c->refresh[0] || !c->secret[0])) {
        rwp_drive_error(c,"Saved Google authorization is missing. Use manual Upload to sign in.");
        return FALSE;
    }
    if(c->refresh[0]){
        rwp_drive_status(c,L"Reusing saved Google authorization...");
        if(rwp_drive_token_exchange(c,NULL,NULL,NULL,TRUE))return TRUE;
        lstrcpynA(previousError,c->error,sizeof(previousError));
        /* Only an invalid_grant token failure should cause reauthorization. */
        if(strstr(previousError,"invalid_grant")){
            c->refresh[0]=0;c->token[0]=0;
            if (c->automatic) {
                rwp_drive_error(c,"Google authorization expired. Sign in through manual Upload.");
                return FALSE;
            }
        }else return FALSE;
    }
    if(!c->secret[0]){
        rwp_drive_error(c,"A Google OAuth Desktop Client Secret is needed. Load the OAuth JSON file before signing in.");return FALSE;
    }
    if (c->automatic) {
        rwp_drive_error(c,"Google authorization requires a manual sign-in.");
        return FALSE;
    }
    return rwp_drive_login_browser(c);
}
static BOOL rwp_drive_json_name(const char *utf8,char *out,SIZE_T cap)
{
    const unsigned char *p=(const unsigned char*)utf8;SIZE_T n=0;
    while(*p){
        unsigned char v=*p++;
        if(n+8>=cap)return FALSE;
        if(v=='\"'||v=='\\'){out[n++]='\\';out[n++]=v;}
        else if(v<32){_snprintf(out+n,7,"\\u%04x",v);n+=6;}
        else out[n++]=v;
    }
    out[n]=0;return TRUE;
}
static BOOL rwp_drive_last_filename(LPCWSTR path,char *name,SIZE_T capacity)
{
    const WCHAR *p=wcsrchr(path,L'\\');int n;
    p=p?p+1:path;
    n=WideCharToMultiByte(CP_UTF8,0,p,-1,name,(int)capacity,NULL,NULL);
    return n>0;
}
static BOOL rwp_drive_valid_id(const char *str,int minlen,int maxlen)
{
    int n=0;char v;
    if(!str)return FALSE;
    while((v=str[n])!=0){if(!isalnum((unsigned char)v)&&v!='_'&&v!='-')return FALSE;if(++n>maxlen)return FALSE;}
    return n>=minlen;
}
static BOOL rwp_drive_prepare_path(RWP_DRIVE_CTX *c)
{
    WCHAR base[MAX_PATH],next[MAX_PATH];DWORD len;
    len=GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
    if(!len||len>=MAX_PATH)return FALSE;
    if(lstrlenW(base)+35>=MAX_PATH)return FALSE;
    wsprintfW(next,L"%s\\RWordPad",base);CreateDirectoryW(next,NULL);
    wsprintfW(c->appDir,L"%s\\CloudUploadBeta",next);CreateDirectoryW(c->appDir,NULL);
    wsprintfW(c->credentialPath,L"%s\\oauth-credentials.dpapi.json",c->appDir);
    wsprintfW(c->preferencesPath,L"%s\\preferences.json",c->appDir);
    wsprintfW(c->indexPath,L"%s\\remote-index.json",c->appDir);
    return TRUE;
}
/* Preserve the legacy remote-index.json format: {"GoogleFileID":"md5hash"}. */
static BOOL rwp_drive_index_get(RWP_DRIVE_CTX *c,const char *remote,char md5[40])
{
    BYTE *data;DWORD length;BOOL ok=FALSE;
    md5[0]=0;
    if(!rwp_drive_readfile(c->indexPath,&data,&length,RWP_DRIVE_MAX_JSON))return FALSE;
    if(rwp_drive_json_value((char*)data,remote,md5,40)&&strlen(md5)==32)ok=TRUE;
    free(data);return ok;
}
static BOOL rwp_drive_index_set(RWP_DRIVE_CTX *c,const char *remote,const char *md5)
{
    BYTE *data=NULL,*result;DWORD length=0;SIZE_T cap,n=0;char pattern[310],*old,*value,*end;
    BOOL ok=FALSE;
    if(!rwp_drive_valid_id(remote,10,255)||strlen(md5)!=32)return FALSE;
    if(!rwp_drive_readfile(c->indexPath,&data,&length,RWP_DRIVE_MAX_JSON)){
        /* An unreadable existing history is not an empty history. */
        if(GetFileAttributesW(c->indexPath)!=INVALID_FILE_ATTRIBUTES)return FALSE;
        {
            const char empty[]="{}";
            length=2;data=(BYTE*)malloc(3);if(!data)return FALSE;memcpy(data,empty,3);
        }
    }
    cap=(SIZE_T)length+strlen(remote)+strlen(md5)+100;
    result=(BYTE*)malloc(cap);if(!result){free(data);return FALSE;}
    sprintf(pattern,"\"%s\"",remote);
    old=strstr((char*)data,pattern);
    if(old){
        value=old+strlen(pattern);
        while(*value&&isspace((unsigned char)*value))++value;
        if(*value!=':')goto done;
        ++value;while(*value&&isspace((unsigned char)*value))++value;
        if(*value!='\"')goto done;
        ++value;end=strchr(value,'\"');if(!end)goto done;
        n=(SIZE_T)(value-(char*)data);
        memcpy(result,data,n);memcpy(result+n,md5,32);n+=32;
        memcpy(result+n,end,strlen(end)+1);n+=strlen(end);
    }else{
        end=strrchr((char*)data,'}');
        if(!end)goto done;
        n=(SIZE_T)(end-(char*)data);
        while(n>0&&isspace(data[n-1]))--n;
        if(n==0||data[0]!='{')goto done;
        memcpy(result,data,n);
        if(n>1&&result[n-1]!='{')result[n++]=',';
        n+=(SIZE_T)sprintf((char*)result+n,"\"%s\":\"%s\"}",remote,md5);
    }
    ok=rwp_drive_write_atomic(c->indexPath,result,(DWORD)n);
done:
    free(result);free(data);return ok;
}
static BYTE *rwp_drive_find(const BYTE *data,SIZE_T length,const char *pattern)
{
    SIZE_T i,n=strlen(pattern);
    if(length<n)return NULL;
    for(i=0;i<=length-n;++i)if(!memcmp(data+i,pattern,n))return (BYTE*)(data+i);
    return NULL;
}
/* Validate the same hidden metadata identity as the 1.4.1 PowerShell helper. */
static BOOL rwp_drive_rtf_metadata(RWP_DRIVE_CTX *c,BYTE *data,DWORD len,
                                   BYTE **groupStart,BYTE **groupEnd,char **json)
{
    BYTE *start,*stop,*duplicate;DWORD size;BYTE *decoded;
    char *base64;SIZE_T n;
    char expected[64],actual[256];
    *groupStart=NULL;*groupEnd=NULL;*json=NULL;
    start=rwp_drive_find(data,len,RWP_DRIVE_RTF_START);
    if(!start)return FALSE;
    duplicate=rwp_drive_find(start+strlen(RWP_DRIVE_RTF_START),
                  len-(SIZE_T)(start-data)-strlen(RWP_DRIVE_RTF_START),RWP_DRIVE_RTF_START);
    if(duplicate)return FALSE;
    stop=rwp_drive_find(start+strlen(RWP_DRIVE_RTF_START),
                  len-(SIZE_T)(start-data)-strlen(RWP_DRIVE_RTF_START),RWP_DRIVE_RTF_END);
    if(!stop)return FALSE;
    n=(SIZE_T)(stop-start)-strlen(RWP_DRIVE_RTF_START);
    if(n==0||n>12000)return FALSE;
    base64=(char*)malloc(n+1);if(!base64)return FALSE;
    memcpy(base64,start+strlen(RWP_DRIVE_RTF_START),n);base64[n]=0;
    if(!rwp_drive_unbase64(base64,&decoded,&size)){free(base64);return FALSE;}
    free(base64);
    {
        char docID[64],remoteID[256];
        if(!rwp_drive_ascii(c->docID,docID,sizeof(docID)) ||
           !rwp_drive_ascii(c->remoteID,remoteID,sizeof(remoteID)) ||
           !rwp_drive_json_value((char*)decoded,"DocumentID",expected,sizeof(expected))||
           !rwp_drive_json_value((char*)decoded,"RemoteFileID",actual,sizeof(actual))||
           strcmp(docID,expected)||strcmp(remoteID,actual)){
            free(decoded);return FALSE;
        }
    }
    *groupStart=start;*groupEnd=stop+strlen(RWP_DRIVE_RTF_END);*json=(char*)decoded;
    return TRUE;
}
/* A temporary RTF is uploaded with the reserved ID already embedded. */
static BOOL rwp_drive_stage(RWP_DRIVE_CTX *c,const char *remoteID,WCHAR staged[MAX_PATH])
{
    BYTE *data=NULL,*begin,*end,*newData;DWORD len;char *json=NULL;
    char *newJson=NULL,*base64=NULL;char *key,*value,*closing;
    SIZE_T offset,newLen,position,tailLen,bodyLen;BOOL ok=FALSE;
    WCHAR tempDir[MAX_PATH];
    staged[0]=0;
    if(!rwp_drive_readfile(c->sourceSnapshot,&data,&len,64*1024*1024)){
        rwp_drive_error(c,"Unable to read source RTF for first-upload staging.");return FALSE;
    }
    if(!rwp_drive_rtf_metadata(c,data,len,&begin,&end,&json)){
        rwp_drive_error(c,"The RTF cloud metadata is missing, duplicated or its identity has changed. No upload was performed.");goto done;
    }
    key=strstr(json,"\"RemoteFileID\"");
    if(!key)goto done;
    value=strchr(key,':');if(!value)goto done;
    ++value;while(*value&&isspace((unsigned char)*value))++value;
    if(*value!='\"'||value[1]!='\"')goto done;
    closing=value+2;
    newLen=strlen(json)+strlen(remoteID)+1;
    newJson=(char*)malloc(newLen);if(!newJson)goto done;
    position=(SIZE_T)(value-json)+1;
    memcpy(newJson,json,position);
    memcpy(newJson+position,remoteID,strlen(remoteID));
    strcpy(newJson+position+strlen(remoteID),closing-1);
    if(!rwp_drive_base64((BYTE*)newJson,(DWORD)strlen(newJson),&base64))goto done;
    offset=(SIZE_T)(begin-data);tailLen=(SIZE_T)len-(SIZE_T)(end-data);
    bodyLen=strlen(RWP_DRIVE_RTF_START)+strlen(base64)+strlen(RWP_DRIVE_RTF_END);
    newData=(BYTE*)malloc(offset+bodyLen+tailLen);if(!newData)goto done;
    memcpy(newData,data,offset);
    memcpy(newData+offset,RWP_DRIVE_RTF_START,strlen(RWP_DRIVE_RTF_START));
    memcpy(newData+offset+strlen(RWP_DRIVE_RTF_START),base64,strlen(base64));
    memcpy(newData+offset+strlen(RWP_DRIVE_RTF_START)+strlen(base64),RWP_DRIVE_RTF_END,strlen(RWP_DRIVE_RTF_END));
    memcpy(newData+offset+bodyLen,end,tailLen);
    if(GetTempPathW(MAX_PATH,tempDir)==0||!GetTempFileNameW(tempDir,L"RWD",0,staged)){
        rwp_drive_error(c,"Could not allocate a temporary upload file.");free(newData);goto done;
    }
    {
        HANDLE f=CreateFileW(staged,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        DWORD doneBytes=0;
        if(f!=INVALID_HANDLE_VALUE){ok=WriteFile(f,newData,(DWORD)(offset+bodyLen+tailLen),&doneBytes,NULL)&&
                                doneBytes==(DWORD)(offset+bodyLen+tailLen);CloseHandle(f);}
    }
    free(newData);
    if(!ok){DeleteFileW(staged);staged[0]=0;rwp_drive_error(c,"Unable to write the temporary RTF upload copy.");}
done:
    if(base64)free(base64);
    if(newJson)free(newJson);
    if(json)free(json);
    if(data)free(data);
    return ok;
}
static BOOL rwp_drive_get_folder(RWP_DRIVE_CTX *c,char *folderName,SIZE_T cap)
{
    char id[256];WCHAR path[700];RWP_HTTP_RESPONSE r;char mime[128];
    if(!rwp_drive_ascii(c->folder,id,sizeof(id)))return FALSE;
    if(!rwp_drive_valid_id(id,10,255)&&strcmp(id,"root")){rwp_drive_error(c,"Invalid destination Google Drive folder ID.");return FALSE;}
    wsprintfW(path,L"/drive/v3/files/%s?fields=id,name,mimeType,trashed&supportsAllDrives=true",c->folder);
    if(!rwp_drive_request(c,L"www.googleapis.com",path,L"GET",c->token,NULL,NULL,0,NULL,&r)){
        rwp_drive_http_free(&r);return FALSE;
    }
    if(!rwp_drive_json_value(r.body,"mimeType",mime,sizeof(mime))||
       strcmp(mime,"application/vnd.google-apps.folder")||
       rwp_drive_json_true(r.body,"trashed")){
        rwp_drive_error(c,"Selected destination is not an active Google Drive folder.");rwp_drive_http_free(&r);return FALSE;
    }
    if(!rwp_drive_json_value(r.body,"id",c->actualFolderID,sizeof(c->actualFolderID)) ||
       !rwp_drive_valid_id(c->actualFolderID,10,255)){
        rwp_drive_error(c,"Google did not return the destination folder ID.");
        rwp_drive_http_free(&r);return FALSE;
    }
    if(!rwp_drive_json_value(r.body,"name",folderName,cap))lstrcpynA(folderName,id,(int)cap);
    rwp_drive_http_free(&r);return TRUE;
}
static BOOL rwp_drive_get_remote(RWP_DRIVE_CTX *c,const char *id,char **body)
{
    WCHAR path[800];RWP_HTTP_RESPONSE r;WCHAR wID[256];
    *body=NULL;
    if(!rwp_drive_wide(id,wID,sizeof(wID)/sizeof(wID[0])))return FALSE;
    wsprintfW(path,L"/drive/v3/files/%s?fields=id,name,mimeType,trashed,parents,size,md5Checksum,headRevisionId&supportsAllDrives=true",wID);
    if(!rwp_drive_request(c,L"www.googleapis.com",path,L"GET",c->token,NULL,NULL,0,NULL,&r)){
        if(r.status==404)rwp_drive_error(c,"The linked Google Drive file was not found (deleted or inaccessible). No replacement was created.");
        rwp_drive_http_free(&r);return FALSE;
    }
    *body=r.body;r.body=NULL;rwp_drive_http_free(&r);return TRUE;
}
static BOOL rwp_drive_reserve(RWP_DRIVE_CTX *c,char remoteID[256])
{
    RWP_HTTP_RESPONSE r;const char *s,*end;SIZE_T n;
    if(!rwp_drive_request(c,L"www.googleapis.com",L"/drive/v3/files/generateIds?count=1&space=drive&type=files",L"GET",c->token,NULL,NULL,0,NULL,&r)){
        rwp_drive_http_free(&r);return FALSE;
    }
    s=strstr(r.body,"\"ids\"");if(!s){rwp_drive_http_free(&r);return FALSE;}
    s=strchr(s,'[');if(!s){rwp_drive_http_free(&r);return FALSE;}
    s=strchr(s,'\"');if(!s){rwp_drive_http_free(&r);return FALSE;}
    ++s;end=strchr(s,'\"');if(!end){rwp_drive_http_free(&r);return FALSE;}
    n=(SIZE_T)(end-s);if(n>=256){rwp_drive_http_free(&r);return FALSE;}
    memcpy(remoteID,s,n);remoteID[n]=0;
    rwp_drive_http_free(&r);return rwp_drive_valid_id(remoteID,10,255);
}
static BOOL rwp_drive_get_upload_location(RWP_DRIVE_CTX *c,const char *name,const char *remoteID,
                                          BOOL existing,LPCWSTR source,WCHAR location[2048])
{
    WCHAR path[1024],wID[256];RWP_HTTP_RESPONSE r;char fileNameJson[2048],metadata[2900],headers[600];
    HANDLE f;LARGE_INTEGER size;DWORD n;
    f=CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return FALSE;
    if(!GetFileSizeEx(f,&size)){CloseHandle(f);return FALSE;}CloseHandle(f);
    if(size.QuadPart>0xFFFFFFFFULL){rwp_drive_error(c,"Files of 4 GiB or larger are not supported by this uploader.");return FALSE;}
    if(existing){
        if(!rwp_drive_wide(remoteID,wID,sizeof(wID)/sizeof(wID[0])))return FALSE;
        wsprintfW(path,L"/upload/drive/v3/files/%s?uploadType=resumable&fields=id,name,parents,size,md5Checksum,webViewLink&supportsAllDrives=true",wID);
        lstrcpyA(metadata,"{}");
    }else{
        char encodedName[2050],folder[256];
        if(!rwp_drive_json_name(name,encodedName,sizeof(encodedName))||
           !rwp_drive_ascii(c->folder,folder,sizeof(folder)))return FALSE;
        if(remoteID&&*remoteID)_snprintf(metadata,sizeof(metadata)-1,"{\"name\":\"%s\",\"parents\":[\"%s\"],\"id\":\"%s\"}",encodedName,folder,remoteID);
        else _snprintf(metadata,sizeof(metadata)-1,"{\"name\":\"%s\",\"parents\":[\"%s\"]}",encodedName,folder);
        metadata[sizeof(metadata)-1]=0;
        lstrcpyW(path,L"/upload/drive/v3/files?uploadType=resumable&fields=id,name,parents,size,md5Checksum,webViewLink&supportsAllDrives=true");
    }
    {
        const WCHAR *extension=wcsrchr(c->document,L'.');
        const char *mime=(extension&&!lstrcmpiW(extension,L".rtf"))?"application/rtf":
              ((extension&&!lstrcmpiW(extension,L".txt"))?"text/plain":"application/octet-stream");
        _snprintf(headers,sizeof(headers)-1,
            "Content-Type: application/json; charset=UTF-8\r\nX-Upload-Content-Type: %s\r\nX-Upload-Content-Length: %lu\r\n",
            mime,(unsigned long)size.QuadPart);
    }
    headers[sizeof(headers)-1]=0;
    if(!rwp_drive_request(c,L"www.googleapis.com",path,existing?L"PATCH":L"POST",c->token,headers,(BYTE*)metadata,(DWORD)strlen(metadata),NULL,&r)){
        rwp_drive_http_free(&r);return FALSE;
    }
    if(!r.location[0]){
        rwp_drive_error(c,"Google didn't provide a resumable upload URL.");rwp_drive_http_free(&r);return FALSE;
    }
    lstrcpynW(location,r.location,2048);rwp_drive_http_free(&r);return TRUE;
}
static BOOL rwp_drive_upload_media(RWP_DRIVE_CTX *c,LPCWSTR location,LPCWSTR source,
                                   const char *expectedID,char returnedID[256],char remoteMd5[40])
{
    URL_COMPONENTSW u;WCHAR host[300],path[2048],extra[1000];RWP_HTTP_RESPONSE r;
    char localMd5[40],remoteSize[50];
    const char *mime="application/octet-stream";
    LARGE_INTEGER size;HANDLE file;
    ZeroMemory(&u,sizeof(u));u.dwStructSize=sizeof(u);
    u.lpszHostName=host;u.dwHostNameLength=sizeof(host)/sizeof(host[0]);
    u.lpszUrlPath=path;u.dwUrlPathLength=sizeof(path)/sizeof(path[0]);
    u.lpszExtraInfo=extra;u.dwExtraInfoLength=sizeof(extra)/sizeof(extra[0]);
    if(!WinHttpCrackUrl(location,0,0,&u)||u.nScheme!=INTERNET_SCHEME_HTTPS||
       u.nPort!=INTERNET_DEFAULT_HTTPS_PORT||
       (lstrcmpiW(host,L"www.googleapis.com")&&lstrcmpiW(host,L"content.googleapis.com"))){
        rwp_drive_error(c,"Google returned an unrecognized upload destination.");return FALSE;
    }
    if(u.dwExtraInfoLength){
        if(u.dwUrlPathLength+u.dwExtraInfoLength+1>=sizeof(path)/sizeof(path[0]))return FALSE;
        wcsncat(path,extra,u.dwExtraInfoLength);
    }
    file=CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return FALSE;
    if(!GetFileSizeEx(file,&size)){CloseHandle(file);return FALSE;}CloseHandle(file);
    {
        const WCHAR *extension=wcsrchr(c->document,L'.');
        if(extension&&!lstrcmpiW(extension,L".rtf"))mime="application/rtf";
        else if(extension&&!lstrcmpiW(extension,L".txt"))mime="text/plain";
    }
    {
        char header[130];
        _snprintf(header,sizeof(header)-1,"Content-Type: %s\r\n",mime);
        header[sizeof(header)-1]=0;
        if(!rwp_drive_request(c,host,path,L"PUT",NULL,header,NULL,0,source,&r)){
            rwp_drive_http_free(&r);return FALSE;
        }
    }
    if(!rwp_drive_json_value(r.body,"id",returnedID,256)||
       (expectedID&&*expectedID&&strcmp(returnedID,expectedID))){
        rwp_drive_error(c,"Upload response contains an incorrect Google Drive file ID.");rwp_drive_http_free(&r);return FALSE;
    }
    if(rwp_drive_json_value(r.body,"size",remoteSize,sizeof(remoteSize))){
        if(_atoi64(remoteSize)!=size.QuadPart){rwp_drive_error(c,"Remote file size differs from uploaded file.");rwp_drive_http_free(&r);return FALSE;}
    }
    if(!rwp_drive_json_value(r.body,"md5Checksum",remoteMd5,40)||!rwp_drive_crypto_hash_file(source,CALG_MD5,localMd5,sizeof(localMd5))||
       _stricmp(localMd5,remoteMd5)){
        rwp_drive_error(c,"The uploaded file's checksum could not be verified.");rwp_drive_http_free(&r);return FALSE;
    }
    rwp_drive_http_free(&r);return TRUE;
}
static BOOL rwp_drive_parent_match(const char *metadata,const char *folder)
{
    const char *s=strstr(metadata,"\"parents\"");const char *end;char quoted[270];
    if(!s)return FALSE;
    s=strchr(s,'[');if(!s)return FALSE;
    end=strchr(s,']');if(!end)return FALSE;
    _snprintf(quoted,sizeof(quoted)-1,"\"%s\"",folder);quoted[sizeof(quoted)-1]=0;
    s=strstr(s,quoted);return s!=NULL&&s<end;
}
static BOOL rwp_retention_enforce(RWP_DRIVE_CTX *c,const char *remoteID);
static BOOL rwp_drive_upload_action(RWP_DRIVE_CTX *c)
{
    char folderName[500],fileName[1100],fileID[256],expected[256],remoteMD5[40];
    char verified[40],localMD5[40],name[1100],trashed[20],mime[128],folder[256];
    char *record=NULL;WCHAR session[2048],uploadFile[MAX_PATH],staged[MAX_PATH];
    BOOL tracked,existing=FALSE,success=FALSE;WCHAR prompt[1400];
    staged[0]=0;
    if(!rwp_drive_ascii(c->folder,folder,sizeof(folder))||
       (!rwp_drive_valid_id(folder,10,255)&&strcmp(folder,"root"))){
        rwp_drive_error(c,"Enter a valid Google Drive folder ID (or root).");return FALSE;
    }
    if(!rwp_drive_last_filename(c->document,fileName,sizeof(fileName))){
        rwp_drive_error(c,"Cannot encode the source filename.");return FALSE;
    }
    tracked=c->docID[0]!=0;
    if(tracked){
        if(!rwp_drive_ascii(c->docID,expected,sizeof(expected))||!rwp_drive_valid_id(expected,10,63)){
            rwp_drive_error(c,"The embedded document ID is invalid.");return FALSE;
        }
        if(!rwp_drive_ascii(c->remoteID,fileID,sizeof(fileID))){rwp_drive_error(c,"Invalid remote ID.");return FALSE;}
        if(fileID[0]&&!rwp_drive_valid_id(fileID,10,255)){
            rwp_drive_error(c,"Invalid embedded remote file ID.");return FALSE;
        }
    }else fileID[0]=0;
    rwp_drive_status(c,L"Checking Google Drive destination folder...");
    if(!rwp_drive_get_folder(c,folderName,sizeof(folderName)))goto done;
    if(tracked&&fileID[0]){
        BYTE *sourceBytes=NULL,*start,*end;DWORD length;char *cloudJson=NULL;
        if(!rwp_drive_readfile(c->sourceSnapshot,&sourceBytes,&length,64*1024*1024)||
           !rwp_drive_rtf_metadata(c,sourceBytes,length,&start,&end,&cloudJson)){
            if(sourceBytes)free(sourceBytes);
            rwp_drive_error(c,"The saved RTF metadata no longer matches its selected document identity. No upload performed.");goto done;
        }
        free(cloudJson);free(sourceBytes);
        rwp_drive_status(c,L"Checking existing Google Drive file...");
        if(!rwp_drive_get_remote(c,fileID,&record))goto done;
        if(rwp_drive_json_true(record,"trashed")){
            rwp_drive_error(c,"The linked Google Drive file is in the trash. Restore it or choose New Cloud Identity. Nothing was uploaded.");goto done;
        }
        if(!rwp_drive_json_value(record,"name",name,sizeof(name))||strcmp(name,fileName)){
            rwp_drive_error(c,"Linked Google Drive file has been renamed. Upload cancelled to protect the remote file.");goto done;
        }
        if(!rwp_drive_parent_match(record,c->actualFolderID)){
            rwp_drive_error(c,"Linked file is no longer inside the configured folder. Upload cancelled.");goto done;
        }
        if(!rwp_drive_json_value(record,"md5Checksum",remoteMD5,sizeof(remoteMD5))||strlen(remoteMD5)!=32){
            rwp_drive_error(c,"Remote file has no usable MD5 checksum. Upload cancelled.");goto done;
        }
        if(!rwp_drive_crypto_hash_file(c->sourceSnapshot,CALG_MD5,localMD5,sizeof(localMD5))){
            rwp_drive_error(c,"Unable to hash the local document.");goto done;
        }
        if(!_stricmp(localMD5,remoteMD5)){
            if(c->maxVersions && !rwp_retention_enforce(c,fileID)) {
                lstrcpynA(c->retentionWarning,c->error,sizeof(c->retentionWarning));
                c->error[0]=0;
            }
            lstrcpynA(c->error,"",sizeof(c->error));
            lstrcpynW(c->resultID,c->remoteID,sizeof(c->resultID)/sizeof(c->resultID[0]));
            rwp_drive_notice(c,L"The Google Drive file is already identical to this document. Upload skipped.",MB_ICONINFORMATION);
            success=TRUE;goto done;
        }
        if(!rwp_drive_index_get(c,fileID,verified)||_stricmp(verified,remoteMD5)){
            _snwprintf(prompt,sizeof(prompt)/sizeof(prompt[0])-1,
                L"The linked Google Drive file differs from the last verified upload, or no trusted checksum history exists.\n\nRemote file ID: %s\n\nOverwrite the remote version with this local file?",c->remoteID);
            prompt[sizeof(prompt)/sizeof(prompt[0])-1]=0;
            if (c->automatic) {
                rwp_drive_error(c,"Remote Google Drive file changed or trusted checksum history is missing. Automatic overwrite refused; use manual Upload to review.");
                goto done;
            }
            if(MessageBoxW(c->dialog,prompt,RWP_DRIVE_TITLE,MB_YESNO|MB_ICONWARNING)!=IDYES){
                rwp_drive_status(c,L"Upload cancelled; remote version preserved.");goto done;
            }
        }
        existing=TRUE;
    }else if(tracked){
        rwp_drive_status(c,L"Reserving Google Drive file identity...");
        if(!rwp_drive_reserve(c,fileID)){
            if(!c->error[0])rwp_drive_error(c,"Failed to reserve a Drive file ID.");goto done;
        }
        if(!rwp_drive_stage(c,fileID,staged))goto done;
    }
    lstrcpynW(uploadFile,staged[0]?staged:c->sourceSnapshot,MAX_PATH);
    rwp_drive_status(c,L"Starting Google Drive upload...");
    if(!rwp_drive_get_upload_location(c,fileName,existing?fileID:(tracked?fileID:NULL),
                                      existing,uploadFile,session))goto done;
    rwp_drive_status(c,L"Uploading document. Please keep WordPad open...");
    if(!rwp_drive_upload_media(c,session,uploadFile,
                               tracked?fileID:NULL,expected,remoteMD5))goto done;
    if(tracked){
        if(!rwp_drive_index_set(c,expected,remoteMD5))
            rwp_drive_notice(c,L"Upload was verified, but the local checksum-history database could not be updated. The next overwrite may ask for confirmation.",MB_ICONWARNING);
        if(!rwp_drive_wide(expected,c->resultID,sizeof(c->resultID)/sizeof(c->resultID[0])))goto done;
    }
    if(tracked && c->maxVersions && !rwp_retention_enforce(c,expected)) {
        lstrcpynA(c->retentionWarning,c->error,sizeof(c->retentionWarning));
        c->error[0]=0; /* The upload itself succeeded. */
    }
    rwp_drive_notice(c,existing?L"Google Drive successfully updated the existing file and verified its checksum.":
                           L"Google Drive successfully created the file and verified its checksum.",MB_ICONINFORMATION);
    if(c->retentionWarning[0])
        rwp_drive_notice(c,L"The backup succeeded, but revision retention needs attention.\n"
            L"Some older revisions may not have been cleaned up or protected.\n"
            L"Check Google Drive access and revision history.",MB_ICONWARNING);
    success=TRUE;
done:
    if(record)free(record);
    if(staged[0])DeleteFileW(staged);
    SecureZeroMemory(c->token,sizeof(c->token));
    return success;
}
/* Upload happens on a background worker so the main editor remains usable.
 * The completion notification is marshalled back to the dialog's UI thread. */
static DWORD WINAPI rwp_drive_worker(LPVOID data)
{
    RWP_DRIVE_CTX *c = (RWP_DRIVE_CTX *)data;
    /* ShellExecute can require COM initialization on this new thread. */
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL success = rwp_drive_authorize(c) && rwp_drive_upload_action(c);
    c->succeeded = success;
    PostMessageW(c->dialog, RWP_DRIVE_DONE, success ? 1 : 0, 0);
    if (SUCCEEDED(com)) CoUninitialize();
    rwp_drive_release(c);
    return 0;
}

static INT_PTR CALLBACK rwp_drive_dialog(HWND dialog,UINT message,WPARAM wp,LPARAM lp)
{
    RWP_DRIVE_CTX *c=(RWP_DRIVE_CTX*)GetWindowLongPtrW(dialog,DWLP_USER);
    if(message==WM_INITDIALOG){
        c=(RWP_DRIVE_CTX*)lp;c->dialog=dialog;SetWindowLongPtrW(dialog,DWLP_USER,(LONG_PTR)c);
        rwp_cloud_apply_theme(dialog);
        SetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_DOCUMENT,c->document);
        rwp_drive_load_prefs(c);
        {
            WCHAR text[320];
            rwp_drive_wide(c->clientID,text,sizeof(text)/sizeof(text[0]));
            SetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_CLIENT,text);
        }
        if(c->docFolder[0])lstrcpynW(c->folder,c->docFolder,256);
        if(!c->folder[0])lstrcpyW(c->folder,L"root");
        SetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_FOLDER,c->folder);
        if (c->automatic) {
            /* A hidden dialog is only a UI-thread message sink for the worker. */
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK,0), 0);
            return TRUE;
        }
        rwp_drive_status(c,GetFileAttributesW(c->credentialPath)!=INVALID_FILE_ATTRIBUTES?
                         L"Saved Google login found; will attempt automatic token renewal.":
                         L"Ready. Load your Desktop OAuth JSON if a new login is needed.");
        return TRUE;
    }
    if(!c)return FALSE;
    if(message==RWP_DRIVE_DONE)
    {
        c->busy=FALSE;
        if (c->automatic) {
            RWP_DRIVE_AUTO_COMPLETE cb=c->autoCallback;
            void *userData=c->userData;
            WCHAR fileID[256],failure[512];
            lstrcpynW(fileID,c->resultID,256);
            failure[0]=0;
            if(wp && c->retentionWarning[0]) {
                if(!MultiByteToWideChar(CP_UTF8,0,c->retentionWarning,-1,failure,512))
                    lstrcpyW(failure,L"Cloud revision retention could not be completed.");
            }
            if(!wp) {
                if(!c->error[0])lstrcpynA(c->error,"Google Drive upload failed.",sizeof(c->error));
                if(!MultiByteToWideChar(CP_UTF8,0,c->error,-1,failure,512))
                    lstrcpyW(failure,L"Google Drive upload failed.");
            }
            c->autoCallback=NULL;
            DestroyWindow(dialog);
            if(cb)cb(wp?TRUE:FALSE,fileID,failure,userData);
            return TRUE;
        }
        if(wp)
        {
            RWP_DRIVE_COMPLETE callback=c->callback;
            void *userData=c->userData;
            WCHAR fileID[256];
            lstrcpynW(fileID,c->resultID,256);
            c->callback=NULL;
            DestroyWindow(dialog);
            if(callback)callback(TRUE,fileID,userData);
        }
        else
        {
            if(c->error[0])rwp_drive_errorbox(c);
            rwp_drive_status(c,L"Upload did not complete. Correct the problem and retry.");
            EnableWindow(GetDlgItem(dialog,IDOK),TRUE);
            EnableWindow(GetDlgItem(dialog,IDCANCEL),TRUE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_LOADJSON),TRUE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_FORGET),TRUE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_FOLDER),TRUE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_CLIENT),TRUE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_SECRET),TRUE);
        }
        return TRUE;
    }
    if(message==WM_CLOSE)
    {
        if(c->busy)
        {
            /* A running resumable upload cannot safely be assumed cancelled.
             * Keep its window/context alive until Google reports the result. */
            rwp_drive_status(c,L"An upload is in progress. WordPad remains usable; this window closes when it finishes.");
        }
        else DestroyWindow(dialog);
        return TRUE;
    }
    if(message==WM_NCDESTROY)
    {
        /* The UI ref and the worker ref are independent. If the editor
         * exits during an upload the worker still has a valid context. */
        RWP_DRIVE_COMPLETE callback=c->callback;
        void *userData=c->userData;
        c->callback=NULL;
        if(rwpDriveUploadWnd==dialog)rwpDriveUploadWnd=NULL;
        if(rwpDriveAutoWnd==dialog)rwpDriveAutoWnd=NULL;
        c->dialog=NULL;
        if(callback)callback(FALSE,L"",userData);
        if(c->autoCallback) {
            RWP_DRIVE_AUTO_COMPLETE autoCallback=c->autoCallback;
            c->autoCallback=NULL;
            autoCallback(FALSE,L"",L"Automatic upload window closed before completion.",userData);
        }
        rwp_drive_release(c);
        return TRUE;
    }
    if(message==WM_COMMAND){
        switch(LOWORD(wp)){
        case IDC_CLOUD_NATIVE_LOADJSON:
        {
            OPENFILENAMEW ofn;WCHAR filename[MAX_PATH];BYTE *json;DWORD size;
            ZeroMemory(&ofn,sizeof(ofn));ZeroMemory(filename,sizeof(filename));
            ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=dialog;ofn.lpstrFile=filename;ofn.nMaxFile=MAX_PATH;
            ofn.lpstrFilter=L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
            ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
            if(!GetOpenFileNameW(&ofn))return TRUE;
            if(!rwp_drive_readfile(filename,&json,&size,32768)){
                rwp_drive_notice(c,L"Unable to read the OAuth Desktop credentials JSON.",MB_ICONERROR);return TRUE;
            }
            if(!strstr((char*)json,"\"installed\"")||
               !rwp_drive_json_value((char*)json,"client_id",c->clientID,sizeof(c->clientID))||
               !rwp_drive_json_value((char*)json,"client_secret",c->secret,sizeof(c->secret))){
                rwp_drive_notice(c,L"The JSON file does not contain Desktop OAuth credentials.",MB_ICONERROR);
            }else{
                WCHAR client[320];
                rwp_drive_wide(c->clientID,client,sizeof(client)/sizeof(client[0]));
                SetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_CLIENT,client);
                SetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_SECRET,L"");
                rwp_drive_status(c,L"OAuth Desktop client credentials loaded for this session.");
            }
            free(json);return TRUE;
        }
        case IDC_CLOUD_NATIVE_FORGET:
            if(MessageBoxW(dialog,L"Remove the saved Google authorization for this Windows account?\nYou will need to sign in again.",RWP_DRIVE_TITLE,MB_YESNO|MB_ICONQUESTION)==IDYES){
                if(DeleteFileW(c->credentialPath)||GetLastError()==ERROR_FILE_NOT_FOUND){
                    SecureZeroMemory(c->refresh,sizeof(c->refresh));SecureZeroMemory(c->secret,sizeof(c->secret));
                    rwp_drive_status(c,L"Saved authorization removed.");
                }else rwp_drive_notice(c,L"Unable to delete the encrypted login file.",MB_ICONERROR);
            }
            return TRUE;
        case IDOK:
        {
            WCHAR typedClient[320],typedSecret[300];char newClient[300],secret[256];
            if(c->busy)return TRUE;
            if (c->automatic) {
                HANDLE thread;
                /* Never modify preferences and never request OAuth interactively. */
                if(!c->clientID[0] || !rwp_drive_load_saved(c) ||
                   !c->refresh[0] || !c->secret[0]) {
                    rwp_drive_error(c,"No saved Google authorization. Use File > Upload to Google Drive once.");
                    PostMessageW(dialog,RWP_DRIVE_DONE,0,0);
                    return TRUE;
                }
                c->busy=TRUE;
                InterlockedIncrement(&c->refs);
                thread=CreateThread(NULL,0,rwp_drive_worker,c,0,NULL);
                if(!thread) {
                    rwp_drive_release(c);
                    c->busy=FALSE;
                    rwp_drive_error(c,"Unable to start automatic backup worker thread.");
                    PostMessageW(dialog,RWP_DRIVE_DONE,0,0);
                } else CloseHandle(thread);
                return TRUE;
            }
            GetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_CLIENT,typedClient,320);
            GetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_SECRET,typedSecret,300);
            GetDlgItemTextW(dialog,IDC_CLOUD_NATIVE_FOLDER,c->folder,256);
            if(!rwp_drive_ascii(typedClient,newClient,sizeof(newClient))||
               strlen(newClient)<strlen(".apps.googleusercontent.com")||
               strcmp(newClient+strlen(newClient)-strlen(".apps.googleusercontent.com"),
                      ".apps.googleusercontent.com")){
                rwp_drive_notice(c,L"Enter a valid Google OAuth Desktop Client ID.",MB_ICONWARNING);return TRUE;
            }
            if(!rwp_drive_ascii(typedSecret,secret,sizeof(secret))){
                rwp_drive_notice(c,L"Invalid OAuth secret.",MB_ICONWARNING);return TRUE;
            }
            if(strcmp(c->clientID,newClient)){
                SecureZeroMemory(c->refresh,sizeof(c->refresh));
                SecureZeroMemory(c->secret,sizeof(c->secret));
                lstrcpynA(c->clientID,newClient,sizeof(c->clientID));
            }
            if(!c->refresh[0]){
                char originalSecret[256];lstrcpynA(originalSecret,c->secret,sizeof(originalSecret));
                if(!rwp_drive_load_saved(c)){
                    lstrcpynA(c->secret,originalSecret,sizeof(c->secret));
                }
            }
            if(secret[0]){
                if(c->secret[0]&&strcmp(c->secret,secret))c->refresh[0]=0;
                lstrcpynA(c->secret,secret,sizeof(c->secret));
            }
            c->error[0]=0;
            if(!rwp_drive_save_prefs(c))
            {
                rwp_drive_error(c,"Unable to store the client and folder preferences.");
                rwp_drive_errorbox(c);
                return TRUE;
            }
            c->busy=TRUE;
            InterlockedExchange(&c->interrupted,0);
            EnableWindow(GetDlgItem(dialog,IDOK),FALSE);
            EnableWindow(GetDlgItem(dialog,IDCANCEL),FALSE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_LOADJSON),FALSE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_FORGET),FALSE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_FOLDER),FALSE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_CLIENT),FALSE);
            EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_SECRET),FALSE);
            {
                HANDLE thread;
                InterlockedIncrement(&c->refs);
                thread=CreateThread(NULL,0,rwp_drive_worker,c,0,NULL);
                if(!thread)
                {
                    rwp_drive_release(c);
                    c->busy=FALSE;
                    EnableWindow(GetDlgItem(dialog,IDOK),TRUE);
                    EnableWindow(GetDlgItem(dialog,IDCANCEL),TRUE);
                    EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_LOADJSON),TRUE);
                    EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_FORGET),TRUE);
                    EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_FOLDER),TRUE);
                    EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_CLIENT),TRUE);
                    EnableWindow(GetDlgItem(dialog,IDC_CLOUD_NATIVE_SECRET),TRUE);
                    rwp_drive_notice(c,L"Could not start the upload worker thread.",MB_ICONERROR);
                    return TRUE;
                }
                CloseHandle(thread);
            }
            rwp_drive_status(c,L"Uploading in the background. You can continue editing WordPad.");
            return TRUE;
        }
        case IDCANCEL:
            SendMessageW(dialog,WM_CLOSE,0,0);return TRUE;
        }
    }
    return FALSE;
}
BOOL rwp_drive_native_upload_async(HWND owner,LPCWSTR document,LPCWSTR folderOverride,
                                   LPCWSTR documentID,LPCWSTR remoteID,
                                   UINT maxVersions,UINT retentionDays,
                                   RWP_DRIVE_COMPLETE callback,void *userData)
{
    RWP_DRIVE_CTX *c;
    WCHAR directory[MAX_PATH];
    HWND dialog;
    if(rwp_drive_auto_busy()) return FALSE;
    if(rwpDriveUploadWnd&&IsWindow(rwpDriveUploadWnd))
    {
        ShowWindow(rwpDriveUploadWnd,SW_RESTORE);
        SetForegroundWindow(rwpDriveUploadWnd);
        return FALSE;
    }
    if(!document||!document[0]||!callback)return FALSE;
    c=(RWP_DRIVE_CTX*)calloc(1,sizeof(*c));
    if(!c)return FALSE;
    c->refs=1;
    c->callback=callback;
    c->userData=userData;
    c->maxVersions=maxVersions;
    c->retentionDays=retentionDays;
    lstrcpynW(c->document,document,MAX_PATH);
    lstrcpynW(c->docFolder,folderOverride?folderOverride:L"",256);
    lstrcpynW(c->docID,documentID?documentID:L"",64);
    lstrcpynW(c->remoteID,remoteID?remoteID:L"",256);
    if(!rwp_drive_prepare_path(c))
    {
        MessageBoxW(owner,L"Could not locate the local Google Drive preference folder.",
                    RWP_DRIVE_TITLE,MB_OK|MB_ICONERROR);
        rwp_drive_release(c);
        return FALSE;
    }
    /* Snap the saved bytes once: editing/saving in WordPad while the window
     * is open or the network is busy cannot alter the upload's contents. */
    if(!GetTempPathW(MAX_PATH,directory) ||
       !GetTempFileNameW(directory,L"RWC",0,c->sourceSnapshot) ||
       !CopyFileW(document,c->sourceSnapshot,FALSE))
    {
        MessageBoxW(owner,L"Could not make a temporary snapshot of the document.",
                    RWP_DRIVE_TITLE,MB_OK|MB_ICONERROR);
        rwp_drive_release(c);
        return FALSE;
    }
    dialog=CreateDialogParamW(GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDD_CLOUD_NATIVE_UPLOAD),owner,rwp_drive_dialog,(LPARAM)c);
    if(!dialog)
    {
        rwp_drive_release(c);
        return FALSE;
    }
    rwpDriveUploadWnd=dialog;
    ShowWindow(dialog,SW_SHOWNORMAL);
    SetForegroundWindow(dialog);
    return TRUE;
}

/* Modeless but invisible transport for opt-in saves. A copy of the exact
 * saved RTF is captured by the caller and copied once more into this context;
 * the caller may then release its own queue snapshot. */
BOOL rwp_drive_native_auto_upload_async(HWND owner, LPCWSTR document,
                                        LPCWSTR savedSnapshot, LPCWSTR folder,
                                        LPCWSTR documentID, LPCWSTR remoteID,
                                        UINT maxVersions, UINT retentionDays,
                                        RWP_DRIVE_AUTO_COMPLETE callback,
                                        void *userData)
{
    RWP_DRIVE_CTX *c;
    WCHAR directory[MAX_PATH];
    HWND dialog;
    if (rwp_drive_auto_busy() || rwp_drive_upload_window() ||
        !document || !*document || !savedSnapshot || !*savedSnapshot ||
        !folder || !*folder || !documentID || !*documentID ||
        !remoteID || !*remoteID || !callback) return FALSE;
    c=(RWP_DRIVE_CTX*)calloc(1,sizeof(*c));
    if(!c) return FALSE;
    c->refs=1;
    c->automatic=TRUE;
    c->autoCallback=callback;
    c->userData=userData;
    c->maxVersions=maxVersions;
    c->retentionDays=retentionDays;
    lstrcpynW(c->document,document,MAX_PATH);
    lstrcpynW(c->docFolder,folder,256);
    lstrcpynW(c->docID,documentID,64);
    lstrcpynW(c->remoteID,remoteID,256);
    if(!rwp_drive_prepare_path(c) ||
       !GetTempPathW(MAX_PATH,directory) ||
       !GetTempFileNameW(directory,L"RWA",0,c->sourceSnapshot) ||
       !CopyFileW(savedSnapshot,c->sourceSnapshot,FALSE)) {
        rwp_drive_release(c);
        return FALSE;
    }
    dialog=CreateDialogParamW(GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDD_CLOUD_NATIVE_UPLOAD),owner,rwp_drive_dialog,(LPARAM)c);
    if(!dialog) {
        rwp_drive_release(c);
        return FALSE;
    }
    rwpDriveAutoWnd=dialog;
    /* Do not ShowWindow: the editor remains fully usable. */
    return TRUE;
}

/* -------------------------------------------------------------------- */
/* ---Google Drive revision history (Beta 1.7 / non-destructive restore)--- */
/* All network I/O runs on a worker. The current editor and remote head are
 * never modified by a restore; older revisions may be pinned only after
 * explicit consent, since Google requires keepForever for downloading them. */
#define RWP_HISTORY_DONE (WM_APP + 247)
#define RWP_HISTORY_LIMIT 500
#define RWP_HISTORY_LIST 1
#define RWP_HISTORY_RESTORE 2

typedef struct RWP_HISTORY_REVISION {
    char id[256];
    char modified[64];
    char size[32];
    char checksum[40];
    BOOL protectedRevision;
    BOOL head;
} RWP_HISTORY_REVISION;

typedef struct RWP_HISTORY_CONTEXT {
    RWP_DRIVE_CTX drive;
    volatile LONG refs;
    HWND dialog;
    WCHAR document[MAX_PATH];
    WCHAR destination[MAX_PATH];
    WCHAR remote[256];
    WCHAR folder[256];
    char head[256];
    RWP_HISTORY_REVISION revisions[RWP_HISTORY_LIMIT];
    RWP_HISTORY_REVISION selected;
    unsigned count;
    unsigned limit;
    BOOL more;
    BOOL working;
    BOOL closeWhenDone;
    int operation;
} RWP_HISTORY_CONTEXT;

static HWND rwpHistoryWnd = NULL;

static void rwp_history_release(RWP_HISTORY_CONTEXT *h)
{
    if (InterlockedDecrement(&h->refs) == 0) {
        SecureZeroMemory(h->drive.token,sizeof(h->drive.token));
        SecureZeroMemory(h->drive.secret,sizeof(h->drive.secret));
        SecureZeroMemory(h->drive.refresh,sizeof(h->drive.refresh));
        free(h);
    }
}

/* Extract individual JSON objects, respecting quoted braces and escapes.
 * The existing field reader is then applied to each isolated object. */
static char *rwp_history_object_end(char *begin)
{
    char *p = begin;
    unsigned depth = 0;
    BOOL inString = FALSE, escape = FALSE;
    for (; *p; ++p) {
        if (inString) {
            if (escape) escape = FALSE;
            else if (*p == '\\') escape = TRUE;
            else if (*p == '"') inString = FALSE;
        } else if (*p == '"') inString = TRUE;
        else if (*p == '{') ++depth;
        else if (*p == '}') {
            if (!depth) return NULL;
            if (!--depth) return p;
        }
    }
    return NULL;
}

static BOOL rwp_history_append_page(RWP_HISTORY_CONTEXT *h, char *body)
{
    char *p = strstr(body,"\"revisions\"");
    if (!p || !(p = strchr(p,'['))) {
        rwp_drive_error(&h->drive,"Google Drive did not return a revisions array.");
        return FALSE;
    }
    ++p;
    while (*p && *p != ']') {
        char *end, saved;
        RWP_HISTORY_REVISION item;
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == ',' || *p == '\t') ++p;
        if (*p == ']') break;
        if (*p != '{' || !(end = rwp_history_object_end(p))) {
            rwp_drive_error(&h->drive,"Malformed Google Drive revision list.");
            return FALSE;
        }
        saved = end[1];end[1] = 0;
        ZeroMemory(&item,sizeof(item));
        if (!rwp_drive_json_value(p,"id",item.id,sizeof(item.id)) ||
            !rwp_drive_valid_id(item.id,1,255)) {
            end[1] = saved;
            rwp_drive_error(&h->drive,"Google Drive returned an invalid revision ID.");
            return FALSE;
        }
        rwp_drive_json_value(p,"modifiedTime",item.modified,sizeof(item.modified));
        rwp_drive_json_value(p,"size",item.size,sizeof(item.size));
        rwp_drive_json_value(p,"md5Checksum",item.checksum,sizeof(item.checksum));
        item.protectedRevision = rwp_drive_json_true(p,"keepForever");
        item.head = h->head[0] && !strcmp(item.id,h->head);
        end[1] = saved;
        if (h->count < RWP_HISTORY_LIMIT) h->revisions[h->count++] = item;
        else h->more = TRUE;
        p = end + 1;
    }
    if (*p != ']') {
        rwp_drive_error(&h->drive,"Incomplete Google Drive revision list.");
        return FALSE;
    }
    return TRUE;
}

static int rwp_history_sort(const void *a,const void *b)
{
    const RWP_HISTORY_REVISION *x = (const RWP_HISTORY_REVISION*)a;
    const RWP_HISTORY_REVISION *y = (const RWP_HISTORY_REVISION*)b;
    if (x->head != y->head) return x->head ? -1 : 1;
    return strcmp(y->modified,x->modified);
}

static BOOL rwp_history_list(RWP_HISTORY_CONTEXT *h)
{
    RWP_DRIVE_CTX *c = &h->drive;
    char *remote = NULL;
    char folderName[600],name[1100],expectedName[1100];
    char pageToken[2000] = "", encoded[6000];
    WCHAR encodedW[6000];
    unsigned pages = 0;
    WCHAR path[7000];
    RWP_HTTP_RESPONSE response;
    h->count = 0;h->more = FALSE;h->head[0] = 0;
    if (!rwp_drive_get_folder(c,folderName,sizeof(folderName))) return FALSE;
    {
        char remoteID[256];
        if (!rwp_drive_ascii(h->remote,remoteID,sizeof(remoteID)) ||
            !rwp_drive_valid_id(remoteID,10,255)) {
            rwp_drive_error(c,"Invalid document Google Drive file ID.");return FALSE;
        }
        if (!rwp_drive_get_remote(c,remoteID,&remote)) return FALSE;
    }
    if (rwp_drive_json_true(remote,"trashed")) {
        rwp_drive_error(c,"The linked file is in Google Drive trash. Restore it before opening its history.");
        goto failure;
    }
    if (!rwp_drive_last_filename(h->document,expectedName,sizeof(expectedName)) ||
        !rwp_drive_json_value(remote,"name",name,sizeof(name)) ||
        strcmp(name,expectedName)) {
        rwp_drive_error(c,"The linked Google Drive file was renamed. History lookup cancelled.");
        goto failure;
    }
    if (!rwp_drive_parent_match(remote,c->actualFolderID)) {
        rwp_drive_error(c,"The linked Google Drive file moved out of its configured folder.");
        goto failure;
    }
    rwp_drive_json_value(remote,"headRevisionId",h->head,sizeof(h->head));
    free(remote);remote=NULL;
    do {
        if (++pages > 12) { h->more = TRUE;break; }
        if (pageToken[0]) {
            if (!rwp_drive_urlencode(pageToken,encoded,sizeof(encoded)) ||
                !rwp_drive_wide(encoded,encodedW,sizeof(encodedW)/sizeof(encodedW[0]))) {
                rwp_drive_error(c,"Google Drive pagination token is invalid.");return FALSE;
            }
            _snwprintf(path,sizeof(path)/sizeof(path[0])-1,
                L"/drive/v3/files/%s/revisions?pageSize=100&fields=nextPageToken,revisions(id,modifiedTime,size,md5Checksum,keepForever)&pageToken=%s",
                h->remote,encodedW);
        } else {
            _snwprintf(path,sizeof(path)/sizeof(path[0])-1,
                L"/drive/v3/files/%s/revisions?pageSize=100&fields=nextPageToken,revisions(id,modifiedTime,size,md5Checksum,keepForever)",
                h->remote);
        }
        path[sizeof(path)/sizeof(path[0])-1]=0;
        if (!rwp_drive_request(c,L"www.googleapis.com",path,L"GET",c->token,
                               NULL,NULL,0,NULL,&response)) {
            rwp_drive_http_free(&response);return FALSE;
        }
        if (!rwp_history_append_page(h,response.body)) {
            rwp_drive_http_free(&response);return FALSE;
        }
        pageToken[0]=0;
        rwp_drive_json_value(response.body,"nextPageToken",pageToken,sizeof(pageToken));
        rwp_drive_http_free(&response);
        if (h->count>=RWP_HISTORY_LIMIT) { if(pageToken[0])h->more=TRUE;break; }
    } while (pageToken[0]);
    qsort(h->revisions,h->count,sizeof(h->revisions[0]),rwp_history_sort);
    /* A current head is not purgeable even when the API omits its ID. */
    if(h->count && !h->head[0])h->revisions[0].head=TRUE;
    return TRUE;
failure:
    free(remote);
    return FALSE;
}

/* Shared by all editor instances in the current Windows session. This
 * serializes retention and explicit revision recovery, including their
 * local ownership-ledger changes and remote delete/pin operations. */
static HANDLE rwp_retention_lock(void)
{
    HANDLE mutex=CreateMutexW(NULL,FALSE,L"Local\\WordPadCustom-Drive-Retention-v1");
    DWORD result;
    if(!mutex)return NULL;
    result=WaitForSingleObject(mutex,120000);
    if(result==WAIT_OBJECT_0||result==WAIT_ABANDONED)return mutex;
    CloseHandle(mutex);return NULL;
}
static void rwp_retention_unlock(HANDLE mutex)
{
    if(mutex){ReleaseMutex(mutex);CloseHandle(mutex);}
}

/* -------------------------------------------------------------------- */
/* Hybrid cloud retention. The local ledger records only revisions whose
 * Keep Forever flag was set automatically by WordPad(Custom). Revisions
 * protected by the user or another client are never deleted by the sweeper.
 * Google allows 200 Keep Forever revisions per binary file, but the head
 * revision is safe without pinning, so do not waste a slot on the head. */
#define RWP_RETENTION_PIN_LIMIT 200
#define RWP_RETENTION_LIST_LIMIT 500

typedef struct RWP_RETENTION_LEDGER {
    WCHAR path[MAX_PATH];
    char remoteID[256];
    char pins[RWP_RETENTION_PIN_LIMIT][256];
    unsigned count;
} RWP_RETENTION_LEDGER;

static BOOL rwp_retention_path(RWP_DRIVE_CTX *c,const char *remoteID,
                               RWP_RETENTION_LEDGER *m)
{
    ULONGLONG hash=14695981039346656037ULL;
    const unsigned char *p=(const unsigned char*)remoteID;
    if (!rwp_drive_valid_id(remoteID,10,255))return FALSE;
    while(*p){hash^=*p++;hash*=1099511628211ULL;}
    if(lstrlenW(c->appDir)>MAX_PATH-58)return FALSE;
    _snwprintf(m->path,MAX_PATH-1,L"%s\\retention-%08lX%08lX.txt",
              c->appDir,(DWORD)(hash>>32),(DWORD)hash);
    m->path[MAX_PATH-1]=0;
    lstrcpynA(m->remoteID,remoteID,sizeof(m->remoteID));
    return TRUE;
}

static int rwp_retention_owned(const RWP_RETENTION_LEDGER *m,const char *id)
{
    unsigned i;
    for(i=0;i<m->count;i++)if(!strcmp(m->pins[i],id))return (int)i;
    return -1;
}

static void rwp_retention_remove(RWP_RETENTION_LEDGER *m,unsigned i)
{
    if(i>=m->count)return;
    if(i+1<m->count)memmove(m->pins+i,m->pins+i+1,
                           (m->count-i-1)*sizeof(m->pins[0]));
    --m->count;
}

static BOOL rwp_retention_read(RWP_RETENTION_LEDGER *m)
{
    BYTE *raw=NULL;DWORD length=0;
    char *line,*next;
    SIZE_T len,rem;
    DWORD flags=GetFileAttributesW(m->path);
    if(flags==INVALID_FILE_ATTRIBUTES)
        return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND;
    if(!rwp_drive_readfile(m->path,&raw,&length,65536))return FALSE;
    line=(char*)raw;
    next=strchr(line,'\n');
    if(!next)goto bad;
    *next++=0;
    len=strlen(line);
    if(len && line[len-1]=='\r')line[--len]=0;
    if(strncmp(line,"RWP-RETENTION-1 ",16)||strcmp(line+16,m->remoteID))goto bad;
    while(*next){
        char *id=next;
        line=strchr(next,'\n');
        if(line){*line=0;next=line+1;}else next+=strlen(next);
        rem=strlen(id);if(rem && id[rem-1]=='\r')id[--rem]=0;
        if(!rem)continue;
        if(m->count>=RWP_RETENTION_PIN_LIMIT ||
           !rwp_drive_valid_id(id,1,255) || rwp_retention_owned(m,id)>=0)goto bad;
        lstrcpynA(m->pins[m->count++],id,sizeof(m->pins[0]));
    }
    free(raw);return TRUE;
bad:
    m->count=0;
    free(raw);return FALSE;
}

static BOOL rwp_retention_save(const RWP_RETENTION_LEDGER *m)
{
    char *buf=(char*)malloc(65536),*cursor;
    unsigned i;BOOL ok=FALSE;
    if(!buf)return FALSE;
    cursor=buf;
    cursor+=sprintf(cursor,"RWP-RETENTION-1 %s\n",m->remoteID);
    for(i=0;i<m->count;i++){
        SIZE_T len=strlen(m->pins[i]);
        if((SIZE_T)(cursor-buf)+len+2>=65536)goto done;
        memcpy(cursor,m->pins[i],len);cursor+=len;*cursor++='\n';
    }
    ok=rwp_drive_write_atomic(m->path,(BYTE*)buf,(DWORD)(cursor-buf));
done:
    free(buf);return ok;
}

static BOOL rwp_retention_time(const char *iso, ULONGLONG *ticks)
{
    int y,mo,d,h,mi,s;
    SYSTEMTIME st;FILETIME ft;ULARGE_INTEGER val;
    const char *suffix;
    if(strlen(iso)<20 || sscanf(iso,"%4d-%2d-%2dT%2d:%2d:%2d",
        &y,&mo,&d,&h,&mi,&s)!=6)return FALSE;
    /* Drive returns UTC RFC3339. Refuse to delete using ambiguous offsets. */
    suffix=iso+19;
    if(*suffix=='.'){
        ++suffix;
        if(*suffix<'0'||*suffix>'9')return FALSE;
        while(*suffix>='0'&&*suffix<='9')++suffix;
    }
    if(strcmp(suffix,"Z"))return FALSE;
    ZeroMemory(&st,sizeof(st));
    st.wYear=(WORD)y;st.wMonth=(WORD)mo;st.wDay=(WORD)d;
    st.wHour=(WORD)h;st.wMinute=(WORD)mi;st.wSecond=(WORD)s;
    if(!SystemTimeToFileTime(&st,&ft))return FALSE;
    val.LowPart=ft.dwLowDateTime;val.HighPart=ft.dwHighDateTime;
    *ticks=val.QuadPart;return TRUE;
}

static BOOL rwp_retention_request(RWP_DRIVE_CTX *c,const char *remoteID,
                                  const char *revisionID,BOOL keep)
{
    WCHAR path[800],wideRemote[256],wideRevision[256];
    RWP_HTTP_RESPONSE response;BOOL ok;
    static const char payload[]="{\"keepForever\":true}";
    if(!rwp_drive_valid_id(remoteID,10,255) ||
       !rwp_drive_valid_id(revisionID,1,255) ||
       !rwp_drive_wide(remoteID,wideRemote,256) ||
       !rwp_drive_wide(revisionID,wideRevision,256))return FALSE;
    _snwprintf(path,799,L"/drive/v3/files/%s/revisions/%s%s",
        wideRemote,wideRevision,keep?L"?fields=id,keepForever":L"");
    path[799]=0;
    ok=rwp_drive_request(c,L"www.googleapis.com",path,keep?L"PATCH":L"DELETE",
        c->token,keep?"Content-Type: application/json; charset=UTF-8\r\n":NULL,
        keep?(const BYTE*)payload:NULL,keep?(DWORD)strlen(payload):0,NULL,&response);
    if(ok && keep && (!response.body ||
        !rwp_drive_json_true(response.body,"keepForever"))){
        rwp_drive_error(c,"Drive did not confirm the Keep Forever flag.");ok=FALSE;
    }
    rwp_drive_http_free(&response);return ok;
}

static BOOL rwp_retention_enforce(RWP_DRIVE_CTX *c,const char *remoteID)
{
    RWP_HISTORY_CONTEXT *h=NULL;
    RWP_RETENTION_LEDGER *ledger=NULL;
    FILETIME currentFT;
    ULARGE_INTEGER now;
    BOOL retain[RWP_RETENTION_LIST_LIMIT];
    unsigned i,j,protectedCount=0;
    unsigned keepMinimum=c->maxVersions;
    BOOL ok=FALSE,changed=FALSE;
    HANDLE mutex;
    if(!keepMinimum || keepMinimum>200 || c->retentionDays>3650)return TRUE;
    mutex=rwp_retention_lock();
    if(!mutex){rwp_drive_error(c,"Could not acquire the revision-retention lock.");return FALSE;}
    ledger=(RWP_RETENTION_LEDGER*)calloc(1,sizeof(*ledger));
    h=(RWP_HISTORY_CONTEXT*)calloc(1,sizeof(*h));
    if(!ledger||!h){rwp_drive_error(c,"No memory for revision retention.");goto done;}
    if(!rwp_retention_path(c,remoteID,ledger) || !rwp_retention_read(ledger)){
        rwp_drive_error(c,"Local revision ownership ledger is unreadable; automatic cleanup skipped.");goto done;
    }
    /* The same authenticated worker handles the verified upload and pruning. */
    h->drive=*c;
    lstrcpynW(h->document,c->document,MAX_PATH);
    if(!rwp_drive_wide(remoteID,h->remote,256))goto done;
    if(!rwp_history_list(h)){
        lstrcpynA(c->error,h->drive.error,sizeof(c->error));goto done;
    }
    if(h->more || h->count>RWP_RETENTION_LIST_LIMIT){
        rwp_drive_error(c,"Revision history is incomplete; refusing automatic cleanup.");goto done;
    }
    GetSystemTimeAsFileTime(&currentFT);
    now.LowPart=currentFT.dwLowDateTime;now.HighPart=currentFT.dwHighDateTime;
    for(i=0;i<h->count;i++){
        ULONGLONG ticks=0;
        BOOL fresh=FALSE;
        if(!rwp_retention_time(h->revisions[i].modified,&ticks)){
            rwp_drive_error(c,"Revision date cannot be verified; no revisions were deleted.");goto done;
        }
        if(c->retentionDays){
            ULONGLONG window=(ULONGLONG)c->retentionDays*864000000000ULL;
            fresh=ticks>=now.QuadPart || now.QuadPart-ticks<=window;
        }
        retain[i]=i<200 && (i<keepMinimum || fresh);
        if(h->revisions[i].protectedRevision)++protectedCount;
    }
    /* Reconcile revisions already pruned by Google or other clients. */
    for(i=0;i<ledger->count;){
        BOOL found=FALSE;
        for(j=0;j<h->count;j++)if(!strcmp(ledger->pins[i],h->revisions[j].id)){
            found=h->revisions[j].protectedRevision;break;
        }
        if(!found){rwp_retention_remove(ledger,i);changed=TRUE;}
        else ++i;
    }
    if(changed && !rwp_retention_save(ledger)){
        rwp_drive_error(c,"Failed to save revision ownership ledger; cleanup disabled.");goto done;
    }
    /* Delete only OLD revisions known to have been pinned by this feature.
     * Never touch versions the user explicitly protected for recovery. */
    for(i=h->count;i>0;){
        RWP_HISTORY_REVISION *rev=&h->revisions[--i];
        int owned=rwp_retention_owned(ledger,rev->id);
        if(!retain[i] && !rev->head && rev->protectedRevision && owned>=0){
            if(!rwp_retention_request(c,remoteID,rev->id,FALSE))goto done;
            --protectedCount;
            rwp_retention_remove(ledger,(unsigned)owned);
            if(!rwp_retention_save(ledger)){
                rwp_drive_error(c,"Old revision was deleted, but the ownership ledger update failed.");goto done;
            }
            rev->protectedRevision=FALSE; /* Deleted: not eligible for pinning. */
        }
    }
    /* Pin selected non-head revisions newest first. The current head is
     * always safe without a pin and will be protected on the next upload. */
    for(i=0;i<h->count;i++){
        RWP_HISTORY_REVISION *rev=&h->revisions[i];
        if(!retain[i] || rev->head || rev->protectedRevision)continue;
        if(protectedCount>=200 || ledger->count>=RWP_RETENTION_PIN_LIMIT){
            rwp_drive_error(c,"Drive has no free Keep Forever slots. User-protected revisions were preserved.");goto done;
        }
        if(!rwp_retention_request(c,remoteID,rev->id,TRUE))goto done;
        ++protectedCount;
        lstrcpynA(ledger->pins[ledger->count++],rev->id,256);
        if(!rwp_retention_save(ledger)){
            rwp_drive_error(c,"Revision was protected, but the ownership ledger could not be saved.");goto done;
        }
    }
    ok=TRUE;
done:
    if(h)free(h);
    if(ledger)free(ledger);
    rwp_retention_unlock(mutex);
    return ok;
}

/* A revision explicitly downloaded by the user must not later be deleted by
 * automatic retention even if it was originally auto-pinned. */
static void rwp_retention_mark_manual(RWP_DRIVE_CTX *c,const char *remoteID,
                                      const char *revisionID)
{
    RWP_RETENTION_LEDGER *ledger;
    int owned;
    ledger=(RWP_RETENTION_LEDGER*)calloc(1,sizeof(*ledger));
    if(!ledger)return;
    if(rwp_retention_path(c,remoteID,ledger) && rwp_retention_read(ledger)){
        owned=rwp_retention_owned(ledger,revisionID);
        if(owned>=0){
            rwp_retention_remove(ledger,(unsigned)owned);
            rwp_retention_save(ledger);
        }
    }
    free(ledger);
}

/* Stream download into a temporary file in the selected directory, verify
 * against the revision's MD5, and only then move it into the Save As path. */
static BOOL rwp_history_download(RWP_HISTORY_CONTEXT *h)
{
    RWP_DRIVE_CTX *c = &h->drive;
    HINTERNET session=NULL,connection=NULL,request=NULL;
    HANDLE file=INVALID_HANDLE_VALUE;
    WCHAR path[1024],auth[3400],tmp[MAX_PATH]={0},host[] = L"www.googleapis.com";
    WCHAR tokenW[3000],revisionW[256];
    DWORD status=0,statusSize=sizeof(status),available=0,read=0,written=0;
    ULONGLONG total=0;
    BYTE buffer[65536];
    char checksum[40];
    BOOL success=FALSE, tempCreated=FALSE;
    if (lstrlenW(h->destination)+38>=MAX_PATH) {
        rwp_drive_error(c,"The recovery path is too long.");return FALSE;
    }
    if (!rwp_drive_wide(h->selected.id,revisionW,256)) {
        rwp_drive_error(c,"Invalid revision ID encoding.");return FALSE;
    }
    if (h->selected.head) {
        _snwprintf(path,1023,L"/drive/v3/files/%s?alt=media",h->remote);
    } else {
        _snwprintf(path,1023,L"/drive/v3/files/%s/revisions/%s?alt=media",
                   h->remote,revisionW);
    }
    path[1023]=0;
    if(!rwp_drive_wide(c->token,tokenW,3000)){
        rwp_drive_error(c,"Invalid Google access token.");return FALSE;
    }
    wsprintfW(auth,L"Authorization: Bearer %s\r\n",tokenW);
    SecureZeroMemory(tokenW,sizeof(tokenW));
    session=WinHttpOpen(L"RWordPad/1.7",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session)goto failed;
    WinHttpSetTimeouts(session,RWP_DRIVE_TIMEOUT,RWP_DRIVE_TIMEOUT,RWP_DRIVE_TIMEOUT,600000);
    connection=WinHttpConnect(session,host,INTERNET_DEFAULT_HTTPS_PORT,0);
    if(!connection)goto failed;
    request=WinHttpOpenRequest(connection,L"GET",path,NULL,WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if(!request)goto failed;
    if(!WinHttpAddRequestHeaders(request,auth,(DWORD)-1L,WINHTTP_ADDREQ_FLAG_ADD))goto failed;
    SecureZeroMemory(auth,sizeof(auth));
    if(!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,
                           WINHTTP_NO_REQUEST_DATA,0,0,0) ||
       !WinHttpReceiveResponse(request,NULL) ||
       !WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,&status,&statusSize,
                            WINHTTP_NO_HEADER_INDEX))goto failed;
    if(status!=200) {
        _snprintf(c->error,sizeof(c->error)-1,
            "Google Drive refused to download this revision (HTTP %lu).",status);
        c->error[sizeof(c->error)-1]=0;
        goto done;
    }
    wsprintfW(tmp,L"%s.%lu.%lu.part",h->destination,GetCurrentProcessId(),GetTickCount());
    file=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) {
        rwp_drive_error(c,"Unable to create the temporary recovery file.");goto done;
    }
    tempCreated=TRUE;
    for(;;) {
        if(!WinHttpQueryDataAvailable(request,&available))goto failed;
        if(!available)break;
        while(available) {
            DWORD take=available>sizeof(buffer)?(DWORD)sizeof(buffer):available;
            if(total+take>512ULL*1024*1024) {
                rwp_drive_error(c,"Recovery download exceeds the 512 MB safety limit.");goto done;
            }
            if(!WinHttpReadData(request,buffer,take,&read) || !read ||
               !WriteFile(file,buffer,read,&written,NULL) || written!=read)goto failed;
            total+=read;
            available-=read;
        }
    }
    if(!FlushFileBuffers(file))goto failed;
    CloseHandle(file);file=INVALID_HANDLE_VALUE;
    if(!h->selected.checksum[0] || strlen(h->selected.checksum)!=32 ||
       !rwp_drive_crypto_hash_file(tmp,CALG_MD5,checksum,sizeof(checksum)) ||
       _stricmp(checksum,h->selected.checksum)) {
        rwp_drive_error(c,"Recovery checksum mismatch, or revision has no verifiable MD5. Local file not replaced.");
        goto done;
    }
    if(h->selected.size[0] && _atoi64(h->selected.size)!=(LONGLONG)total) {
        rwp_drive_error(c,"Downloaded revision size differs from Google Drive metadata.");goto done;
    }
    if(!MoveFileExW(tmp,h->destination,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        rwp_drive_error(c,"Could not finalize the recovered file. Check folder permissions.");goto done;
    }
    tmp[0]=0;
    tempCreated=FALSE;
    success=TRUE;
    goto done;
failed:
    rwp_drive_error(c,"Network or disk error during revision download; existing local files were not changed.");
done:
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    if(!success && tempCreated && tmp[0])DeleteFileW(tmp);
    if(request)WinHttpCloseHandle(request);
    if(connection)WinHttpCloseHandle(connection);
    if(session)WinHttpCloseHandle(session);
    SecureZeroMemory(auth,sizeof(auth));
    return success;
}

static BOOL rwp_history_restore(RWP_HISTORY_CONTEXT *h)
{
    RWP_DRIVE_CTX *c=&h->drive;
    RWP_HTTP_RESPONSE response;
    WCHAR path[1024],revisionW[256];
    if(!rwp_drive_wide(h->selected.id,revisionW,256)) {
        rwp_drive_error(c,"Invalid revision ID encoding.");return FALSE;
    }
    if(!h->selected.head && !h->selected.protectedRevision) {
        static const char payload[]="{\"keepForever\":true}";
        _snwprintf(path,1023,
            L"/drive/v3/files/%s/revisions/%s?fields=id,keepForever",
            h->remote,revisionW);path[1023]=0;
        if(!rwp_drive_request(c,L"www.googleapis.com",path,L"PATCH",c->token,
                              "Content-Type: application/json\r\n",
                              (const BYTE*)payload,(DWORD)strlen(payload),NULL,&response)) {
            rwp_drive_http_free(&response);return FALSE;
        }
        if (!rwp_drive_json_true(response.body,"keepForever")) {
            rwp_drive_http_free(&response);
            rwp_drive_error(c,"Google Drive did not confirm permanent retention of the revision.");
            return FALSE;
        }
        h->selected.protectedRevision=TRUE;
        rwp_drive_http_free(&response);
    }
    {
        BOOL restored=rwp_history_download(h);
        if(restored && h->selected.protectedRevision) {
            char remoteID[256];
            if(rwp_drive_ascii(h->remote,remoteID,sizeof(remoteID)))
                rwp_retention_mark_manual(c,remoteID,h->selected.id);
        }
        return restored;
    }
}

static DWORD WINAPI rwp_history_worker(LPVOID parameter)
{
    RWP_HISTORY_CONTEXT *h=(RWP_HISTORY_CONTEXT*)parameter;
    RWP_DRIVE_CTX *c=&h->drive;
    HRESULT com=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    BOOL ok=FALSE;
    c->error[0]=0;
    c->automatic=TRUE; /* Reuse encrypted OAuth; no browser dialogs in worker. */
    if(!rwp_drive_prepare_path(c))
        rwp_drive_error(c,"Cannot access WordPad Google Drive configuration folder.");
    else if(!rwp_drive_load_prefs(c) || !c->clientID[0] ||
            !rwp_drive_load_saved(c))
        rwp_drive_error(c,"Google login is not saved. Use File > Upload to Google Drive to sign in.");
    else if(rwp_drive_authorize(c)) {
        if(h->operation==RWP_HISTORY_LIST)ok=rwp_history_list(h);
        else if(h->operation==RWP_HISTORY_RESTORE){
            HANDLE mutex=rwp_retention_lock();
            if(!mutex)rwp_drive_error(c,"Could not lock revision recovery while retention runs.");
            else {ok=rwp_history_restore(h);rwp_retention_unlock(mutex);}
        }
    }
    SecureZeroMemory(c->token,sizeof(c->token));
    if(h->dialog)PostMessageW(h->dialog,RWP_HISTORY_DONE,ok,h->operation);
    if(SUCCEEDED(com))CoUninitialize();
    rwp_history_release(h);
    return 0;
}

static BOOL rwp_history_start(RWP_HISTORY_CONTEXT *h,int operation)
{
    HANDLE thread;
    if(h->working)return FALSE;
    h->operation=operation;h->working=TRUE;
    EnableWindow(GetDlgItem(h->dialog,IDC_CLOUD_HISTORY_REFRESH),FALSE);
    EnableWindow(GetDlgItem(h->dialog,IDC_CLOUD_HISTORY_DOWNLOAD),FALSE);
    EnableWindow(GetDlgItem(h->dialog,IDC_CLOUD_HISTORY_LIST),FALSE);
    SetDlgItemTextW(h->dialog,IDC_CLOUD_HISTORY_STATUS,
        operation==RWP_HISTORY_LIST ? L"Loading revision history..." :
                                     L"Downloading and verifying the selected revision...");
    InterlockedIncrement(&h->refs);
    thread=CreateThread(NULL,0,rwp_history_worker,h,0,NULL);
    if(!thread) {
        rwp_history_release(h);h->working=FALSE;
        EnableWindow(GetDlgItem(h->dialog,IDC_CLOUD_HISTORY_REFRESH),TRUE);
        EnableWindow(GetDlgItem(h->dialog,IDC_CLOUD_HISTORY_DOWNLOAD),TRUE);
        EnableWindow(GetDlgItem(h->dialog,IDC_CLOUD_HISTORY_LIST),TRUE);
        SetDlgItemTextW(h->dialog,IDC_CLOUD_HISTORY_STATUS,L"Unable to start revision worker.");
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

static void rwp_history_render(RWP_HISTORY_CONTEXT *h)
{
    unsigned i;
    HWND list=GetDlgItem(h->dialog,IDC_CLOUD_HISTORY_LIST);
    WCHAR line[550],stamp[40],kind[48],size[48],total[160];
    SendMessageW(list,LB_RESETCONTENT,0,0);
    for(i=0;i<h->count;i++) {
        const RWP_HISTORY_REVISION *rev=&h->revisions[i];
        unsigned j;
        LONGLONG bytes=_atoi64(rev->size);
        for(j=0;j<19 && rev->modified[j];++j)
            stamp[j]=(rev->modified[j]=='T' ? L' ' : (WCHAR)(unsigned char)rev->modified[j]);
        stamp[j]=0;
        if(!j)lstrcpyW(stamp,L"Unknown date");
        if(rev->head)lstrcpyW(kind,L"Current");
        else if(rev->protectedRevision)lstrcpyW(kind,L"Protected");
        else lstrcpyW(kind,L"Temporary");
        if(rev->size[0])
            _snwprintf(size,47,L"%I64d KB",(bytes+1023)/1024);
        else lstrcpyW(size,L"Size unknown");
        size[47]=0;
        _snwprintf(line,549,L"%s UTC    |    %s    |    %s",stamp,size,kind);
        line[549]=0;
        SendMessageW(list,LB_ADDSTRING,0,(LPARAM)line);
    }
    if(h->count) SendMessageW(list,LB_SETCURSEL,0,0);
    _snwprintf(total,159,L"%u revision(s)%s. Double-click one to download it.",
               h->count,h->more?L" (first 500 shown)":L"");
    total[159]=0;
    SetDlgItemTextW(h->dialog,IDC_CLOUD_HISTORY_STATUS,total);
}

static INT_PTR CALLBACK rwp_history_dialog(HWND dialog,UINT message,WPARAM wp,LPARAM lp)
{
    RWP_HISTORY_CONTEXT *h=(RWP_HISTORY_CONTEXT*)GetWindowLongPtrW(dialog,DWLP_USER);
    if(message==WM_INITDIALOG) {
        BOOL italian=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_ITALIAN;
        h=(RWP_HISTORY_CONTEXT*)lp;
        h->dialog=dialog;
        SetWindowLongPtrW(dialog,DWLP_USER,(LONG_PTR)h);
        rwp_cloud_apply_theme(dialog);
        if(italian) {
            SetWindowTextW(dialog,L"WordPad(Custom) - Cronologia versioni Google Drive");
            SetDlgItemTextW(dialog,IDC_CLOUD_HISTORY_REFRESH,L"Aggiorna");
            SetDlgItemTextW(dialog,IDC_CLOUD_HISTORY_DOWNLOAD,L"Scarica selezionata...");
            SetDlgItemTextW(dialog,IDCANCEL,L"Chiudi");
            SetDlgItemTextW(dialog,IDC_CLOUD_HISTORY_NOTICE,
                L"Il recupero crea un file separato. La conservazione automatica protegge le versioni recenti.");
        }
        if(!italian)SetDlgItemTextW(dialog,IDC_CLOUD_HISTORY_NOTICE,
            L"Recovery creates a separate file. Recent revisions are retained automatically.");
        PostMessageW(dialog,WM_COMMAND,MAKEWPARAM(IDC_CLOUD_HISTORY_REFRESH,BN_CLICKED),0);
        return TRUE;
    }
    if(!h)return FALSE;
    if(message==RWP_HISTORY_DONE) {
        WCHAR failure[1500];
        h->working=FALSE;
        if(h->closeWhenDone){DestroyWindow(dialog);return TRUE;}
        EnableWindow(GetDlgItem(dialog,IDC_CLOUD_HISTORY_REFRESH),TRUE);
        EnableWindow(GetDlgItem(dialog,IDC_CLOUD_HISTORY_LIST),TRUE);
        EnableWindow(GetDlgItem(dialog,IDC_CLOUD_HISTORY_DOWNLOAD),h->count!=0);
        if(wp) {
            if(lp==RWP_HISTORY_LIST)rwp_history_render(h);
            else {
                unsigned i;
                for(i=0;i<h->count;i++)
                    if(!strcmp(h->revisions[i].id,h->selected.id))
                        h->revisions[i].protectedRevision=h->selected.protectedRevision;
                rwp_history_render(h);
                SetDlgItemTextW(dialog,IDC_CLOUD_HISTORY_STATUS,
                    L"Recovery downloaded and checksum verified. Original document unchanged.");
                MessageBoxW(dialog,
                    L"The selected revision was downloaded as a separate file and its MD5 checksum verified.\n"
                    L"The open document and remote file were not changed.\n\n"
                    L"IMPORTANT: The recovered RTF retains the original cloud identity and settings.\n"
                    L"Before editing and saving that copy in WordPad(Custom), use Cloud Backup Settings > New Cloud Identity\n"
                    L"(or disable automatic backup), so it does not update the existing remote document.",
                    L"Cloud Version History",MB_OK|MB_ICONINFORMATION);
            }
        }else {
            if(!MultiByteToWideChar(CP_UTF8,0,h->drive.error,-1,failure,1500))
                lstrcpyW(failure,L"Google Drive operation failed.");
            SetDlgItemTextW(dialog,IDC_CLOUD_HISTORY_STATUS,L"Google Drive operation failed.");
            MessageBoxW(dialog,failure,L"Cloud Version History",MB_OK|MB_ICONERROR);
        }
        return TRUE;
    }
    if(message==WM_COMMAND) {
        int id=LOWORD(wp);
        if(id==IDC_CLOUD_HISTORY_REFRESH) {
            if(!h->working)rwp_history_start(h,RWP_HISTORY_LIST);
            return TRUE;
        }
        if(id==IDC_CLOUD_HISTORY_DOWNLOAD ||
           (id==IDC_CLOUD_HISTORY_LIST && HIWORD(wp)==LBN_DBLCLK)) {
            int index;
            WCHAR dest[MAX_PATH],initialDir[MAX_PATH];
            OPENFILENAMEW picker;
            if(h->working)return TRUE;
            index=(int)SendDlgItemMessageW(dialog,IDC_CLOUD_HISTORY_LIST,LB_GETCURSEL,0,0);
            if(index<0 || (unsigned)index>=h->count) {
                MessageBoxW(dialog,L"Select a revision first.",L"Cloud Version History",
                            MB_OK|MB_ICONINFORMATION);return TRUE;
            }
            h->selected=h->revisions[index];
            if(!h->selected.head && !h->selected.protectedRevision &&
               MessageBoxW(dialog,
                   L"Google Drive requires an older revision to be marked Keep Forever before it can be downloaded.\n\n"
                   L"This permanently protects that revision from automatic purging and uses Google Drive storage (maximum 200 protected revisions per file).\n\n"
                   L"Protect the selected revision and download it?",
                   L"Cloud Version History",MB_YESNO|MB_ICONQUESTION)!=IDYES)
                return TRUE;
            ZeroMemory(&picker,sizeof(picker));
            lstrcpyW(dest,L"Recovered-Version.rtf");
            lstrcpynW(initialDir,h->document,MAX_PATH);
            {
                WCHAR *slash=wcsrchr(initialDir,L'\\');
                if(slash) *slash=0;
            }
            picker.lStructSize=sizeof(picker);
            picker.hwndOwner=dialog;
            picker.lpstrFile=dest;
            picker.nMaxFile=MAX_PATH;
            picker.lpstrFilter=L"RTF Documents (*.rtf)\0*.rtf\0All files (*.*)\0*.*\0\0";
            picker.lpstrDefExt=L"rtf";
            picker.lpstrInitialDir=initialDir;
            picker.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_EXPLORER;
            if(!GetSaveFileNameW(&picker))return TRUE;
            if(!lstrcmpiW(dest,h->document)) {
                MessageBoxW(dialog,
                    L"Recovery must be saved separately. The current document cannot be overwritten.",
                    L"Cloud Version History",MB_OK|MB_ICONWARNING);return TRUE;
            }
            lstrcpynW(h->destination,dest,MAX_PATH);
            rwp_history_start(h,RWP_HISTORY_RESTORE);
            return TRUE;
        }
        if(id==IDCANCEL){SendMessageW(dialog,WM_CLOSE,0,0);return TRUE;}
    }
    if(message==WM_CLOSE) {
        if(h->working) {
            h->closeWhenDone=TRUE;
            SetDlgItemTextW(dialog,IDC_CLOUD_HISTORY_STATUS,
                L"Waiting for Google Drive operation to finish before closing...");
            ShowWindow(dialog,SW_HIDE);
        } else DestroyWindow(dialog);
        return TRUE;
    }
    if(message==WM_NCDESTROY) {
        if(rwpHistoryWnd==dialog)rwpHistoryWnd=NULL;
        h->dialog=NULL;
        rwp_history_release(h);
        return TRUE;
    }
    return FALSE;
}

BOOL rwp_drive_native_history_open(HWND owner,LPCWSTR document,
                                   LPCWSTR folder,LPCWSTR remoteID,
                                   UINT maxVersions)
{
    RWP_HISTORY_CONTEXT *h;
    HWND dialog;
    if(rwpHistoryWnd && IsWindow(rwpHistoryWnd)) {
        ShowWindow(rwpHistoryWnd,SW_RESTORE);
        SetForegroundWindow(rwpHistoryWnd);
        return FALSE;
    }
    if(!document || !*document || !folder || !*folder ||
       !remoteID || !*remoteID)return FALSE;
    h=(RWP_HISTORY_CONTEXT*)calloc(1,sizeof(*h));
    if(!h)return FALSE;
    h->refs=1;
    h->limit=maxVersions;
    lstrcpynW(h->document,document,MAX_PATH);
    lstrcpynW(h->folder,folder,256);
    lstrcpynW(h->remote,remoteID,256);
    lstrcpynW(h->drive.folder,folder,256);
    lstrcpynW(h->drive.docFolder,folder,256);
    lstrcpynW(h->drive.remoteID,remoteID,256);
    dialog=CreateDialogParamW(GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDD_CLOUD_HISTORY),owner,rwp_history_dialog,(LPARAM)h);
    if(!dialog){rwp_history_release(h);return FALSE;}
    rwpHistoryWnd=dialog;
    ShowWindow(dialog,SW_SHOWNORMAL);
    SetForegroundWindow(dialog);
    return TRUE;
}

/* Read-only folder-name lookup for the per-document settings dialog.
 * Called only by its short-lived worker: never block the UI thread or launch
 * OAuth/browser authorization. Google Drive's root alias works as a file ID. */
int rwp_drive_native_folder_name(LPCWSTR folderID, WCHAR *name, UINT capacity)
{
    RWP_DRIVE_CTX *c;
    RWP_HTTP_RESPONSE response;
    WCHAR path[520];
    char asciiID[256], utf8Name[2048], mime[150];
    int result = RWP_DRIVE_FOLDER_ERROR;
    if (!name || !capacity) return result;
    name[0] = 0;
    if (!folderID || !rwp_drive_ascii(folderID, asciiID, sizeof(asciiID)) ||
        !rwp_drive_valid_id(asciiID, 1, 255)) return result;
    c = (RWP_DRIVE_CTX *)calloc(1, sizeof(*c));
    if (!c) return result;
    c->automatic = TRUE;
    if (!rwp_drive_prepare_path(c) || !rwp_drive_load_prefs(c) ||
        !c->clientID[0] || !rwp_drive_load_saved(c)) {
        result = RWP_DRIVE_FOLDER_NO_LOGIN;
        goto done;
    }
    if (!rwp_drive_authorize(c)) {
        result = RWP_DRIVE_FOLDER_NO_LOGIN;
        goto done;
    }
    _snwprintf(path, sizeof(path)/sizeof(path[0])-1,
        L"/drive/v3/files/%s?fields=name,mimeType,trashed&supportsAllDrives=true", folderID);
    path[sizeof(path)/sizeof(path[0])-1] = 0;
    ZeroMemory(&response, sizeof(response));
    if (!rwp_drive_request(c, L"www.googleapis.com", path, L"GET",
                           c->token, NULL, NULL, 0, NULL, &response)) {
        if (response.status == 404 || response.status == 403)
            result = RWP_DRIVE_FOLDER_NOT_FOUND;
        rwp_drive_http_free(&response);
        goto done;
    }
    if (rwp_drive_json_true(response.body, "trashed"))
        result = RWP_DRIVE_FOLDER_NOT_FOUND;
    else if (!rwp_drive_json_value(response.body, "mimeType", mime, sizeof(mime)))
        result = RWP_DRIVE_FOLDER_ERROR;
    else if (strcmp(mime, "application/vnd.google-apps.folder"))
        result = RWP_DRIVE_FOLDER_NOT_FOLDER;
    else if (rwp_drive_json_value(response.body, "name", utf8Name, sizeof(utf8Name)) &&
             rwp_drive_wide(utf8Name, name, capacity) && name[0])
        result = RWP_DRIVE_FOLDER_OK;
    rwp_drive_http_free(&response);
done:
    SecureZeroMemory(c->token, sizeof(c->token));
    SecureZeroMemory(c->secret, sizeof(c->secret));
    SecureZeroMemory(c->refresh, sizeof(c->refresh));
    free(c);
    return result;
}
