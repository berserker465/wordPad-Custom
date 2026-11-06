# RWordPad Google Drive Upload - one-way proof of concept.
# PowerShell 7 on Windows; launched by File > Upload to Google Drive (Beta).
# No credentials, tokens, or sync metadata are written to the document.
# A refresh token and OAuth client secret are stored locally using Windows DPAPI (CurrentUser).
param(
    [Parameter(Mandatory = $true)]
    [string]$DocumentFile,
    [string]$DocumentFolderID = "",
    [string]$DocumentID = "",
    [string]$DocumentRemoteID = "",
    [string]$ResultFile = ""
)

$ErrorActionPreference = "Stop"

# --------------------- #
#  ---Configuration---  #
# --------------------- #

$OauthScope = "https://www.googleapis.com/auth/drive" # Required for an arbitrary existing folder ID in this beta.
$AuthenticationTimeoutSeconds = 180
$UploadTimeoutMinutes = 10
$ApplicationFolder = Join-Path $env:LOCALAPPDATA "RWordPad\CloudUploadBeta"
$PreferencesFile = Join-Path $ApplicationFolder "preferences.json" # Non-sensitive Client ID and Folder ID only.
$ProtectedCredentialsFile = Join-Path $ApplicationFolder "oauth-credentials.dpapi.json" # Encrypted refresh token and client secret;CurrentUser only.
$RemoteIndexFile = Join-Path $ApplicationFolder "remote-index.json" # Local last-verified remote MD5,for external-change detection.

# --------------------------------- #
#  ---Google Drive Beta Helpers---  #
# --------------------------------- #

# --------------------------------------- #
#  ---Protected Google Authorization---  #
# --------------------------------------- #

function Protect-GoogleCredential {
    param([Parameter(Mandatory=$true)][string]$Value)
    # DPAPI binds this ciphertext to the current Windows account on this machine.
    $raw = [Text.Encoding]::UTF8.GetBytes($Value)
    $entropy = [Text.Encoding]::UTF8.GetBytes("RWordPad-CloudUploadBeta-DPAPI-v1")
    $encrypted = [Security.Cryptography.ProtectedData]::Protect(
        $raw,$entropy,[Security.Cryptography.DataProtectionScope]::CurrentUser)
    try { return [Convert]::ToBase64String($encrypted) }
    finally { [Array]::Clear($raw,0,$raw.Length) }
}

function Unprotect-GoogleCredential {
    param([Parameter(Mandatory=$true)][string]$Encoded)
    $entropy = [Text.Encoding]::UTF8.GetBytes("RWordPad-CloudUploadBeta-DPAPI-v1")
    $raw = [Security.Cryptography.ProtectedData]::Unprotect(
        [Convert]::FromBase64String($Encoded),$entropy,
        [Security.Cryptography.DataProtectionScope]::CurrentUser)
    try { return [Text.Encoding]::UTF8.GetString($raw) }
    finally { [Array]::Clear($raw,0,$raw.Length) }
}

function Get-SavedGoogleAuthorization {
    param([string]$ClientID)

    if (-not (Test-Path -LiteralPath $ProtectedCredentialsFile -PathType Leaf)) { return $null }
    $stored = Get-Content -LiteralPath $ProtectedCredentialsFile -Raw -Encoding UTF8 -ErrorAction Stop |
        ConvertFrom-Json -ErrorAction Stop
    if ($stored.SchemaVersion -ne 1 -or $stored.ClientID -cne $ClientID) { return $null }
    if (-not $stored.ProtectedRefreshToken -or -not $stored.ProtectedClientSecret) {
        throw "The saved authorization file is incomplete.Use Forget saved login to reset it."
    }
    try {
        return [pscustomobject]@{
            ClientSecret = (Unprotect-GoogleCredential -Encoded ([string]$stored.ProtectedClientSecret))
            RefreshToken = (Unprotect-GoogleCredential -Encoded ([string]$stored.ProtectedRefreshToken))
        }
    }
    catch {
        throw "Saved Google authorization could not be decrypted for this Windows account.Use Forget saved login and sign in again."
    }
}

function Save-GoogleAuthorization {
    param([string]$ClientID,[string]$ClientSecret,[string]$RefreshToken)

    if ([string]::IsNullOrWhiteSpace($RefreshToken) -or [string]::IsNullOrWhiteSpace($ClientSecret)) {
        throw "Google authorization is missing a refresh token or client secret."
    }
    [IO.Directory]::CreateDirectory($ApplicationFolder) | Out-Null
    $document = [ordered]@{
        SchemaVersion = 1
        ClientID = $ClientID
        ProtectedClientSecret = (Protect-GoogleCredential -Value $ClientSecret)
        ProtectedRefreshToken = (Protect-GoogleCredential -Value $RefreshToken)
    } | ConvertTo-Json -Depth 3
    $temporary = Join-Path $ApplicationFolder ("oauth-credentials."+[guid]::NewGuid().ToString("N")+".tmp")
    try {
        [IO.File]::WriteAllText($temporary,$document,[Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary,$ProtectedCredentialsFile,$true)
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-GoogleRefreshToken {
    param([string]$ClientID,[string]$ClientSecret,[string]$RefreshToken)

    $parameters = @{
        client_id = $ClientID
        client_secret = $ClientSecret
        refresh_token = $RefreshToken
        grant_type = "refresh_token"
    }
    try {
        $result = Invoke-RestMethod -Uri "https://oauth2.googleapis.com/token" -Method Post `
            -Body $parameters -ContentType "application/x-www-form-urlencoded" -TimeoutSec 45
    }
    catch {
        $detail = Get-GoogleErrorDetail $_
        if ($detail -match '(^|[^a-z_])invalid_grant([^a-z_]|$)') {
            # Expired or revoked refresh token;browser authorization is required again.
            return $null
        }
        throw "Google OAuth refresh failed:$detail"
    }
    if (-not $result.access_token) { throw "Google OAuth refresh returned no access token." }
    return $result
}

function ConvertTo-Base64Url {
    param([byte[]]$Bytes)
    return [Convert]::ToBase64String($Bytes).TrimEnd("=").Replace("+", "-").Replace("/", "_")
}

function New-RandomBase64Url {
    param([int]$ByteCount = 32)
    $bytes = [byte[]]::new($ByteCount)
    [Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return ConvertTo-Base64Url $bytes
}

function ConvertFrom-QueryString {
    param([string]$Query)
    $results = @{}
    foreach ($item in $Query.TrimStart("?").Split("&")) {
        if (-not $item) { continue }
        $parts = $item.Split("=", 2)
        $key = [Uri]::UnescapeDataString($parts[0].Replace("+", " "))
        $value = if ($parts.Count -eq 2) {
            [Uri]::UnescapeDataString($parts[1].Replace("+", " "))
        } else { "" }
        $results[$key] = $value
    }
    return $results
}

function Get-GoogleErrorDetail {
    param([System.Management.Automation.ErrorRecord]$Failure)

    $detail = [string]$Failure.ErrorDetails.Message
    if (-not [string]::IsNullOrWhiteSpace($detail)) {
        try {
            $parsed = $detail | ConvertFrom-Json -ErrorAction Stop
            $parts = @()
            if ($parsed.error) {
                if ($parsed.error -is [string]) { $parts += [string]$parsed.error }
                elseif ($parsed.error.message) { $parts += [string]$parsed.error.message }
            }
            if ($parsed.error_description) { $parts += [string]$parsed.error_description }
            if ($parts.Count -gt 0) { return ($parts -join ":") }
        }
        catch { }
        return $detail.Substring(0,[Math]::Min(700,$detail.Length))
    }
    return [string]$Failure.Exception.Message
}

function Invoke-GoogleDesktopLogin {
    param([string]$ClientID,[string]$ClientSecret,[System.Windows.Forms.Form]$Owner)

    # Binding directly to 127.0.0.1 avoids HTTP.sys URL ACL requirements.
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
        $redirect = "http://127.0.0.1:$port/"
        $verifier = New-RandomBase64Url 48
        $digest = [Security.Cryptography.SHA256]::HashData([Text.Encoding]::ASCII.GetBytes($verifier))
        $challenge = ConvertTo-Base64Url $digest
        $state = New-RandomBase64Url 32
        $url = "https://accounts.google.com/o/oauth2/v2/auth?" +
            "client_id=$([Uri]::EscapeDataString($ClientID))" +
            "&response_type=code" +
            "&redirect_uri=$([Uri]::EscapeDataString($redirect))" +
            "&scope=$([Uri]::EscapeDataString($OauthScope))" +
            "&access_type=offline" +
            "&prompt=consent" +
            "&state=$([Uri]::EscapeDataString($state))" +
            "&code_challenge=$([Uri]::EscapeDataString($challenge))" +
            "&code_challenge_method=S256"

        Start-Process -FilePath $url -ErrorAction Stop
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $code = $null
        while ($watch.Elapsed.TotalSeconds -lt $AuthenticationTimeoutSeconds) {
            [System.Windows.Forms.Application]::DoEvents()
            if ($Owner.IsDisposed -or $Owner.Disposing) {
                throw "Google sign-in was cancelled."
            }
            if (-not $listener.Pending()) {
                Start-Sleep -Milliseconds 100
                continue
            }
            $connection = $listener.AcceptTcpClient()
            try {
                $connection.ReceiveTimeout = 5000
                $connection.SendTimeout = 5000
                $stream = $connection.GetStream()
                $reader = [IO.StreamReader]::new($stream,[Text.Encoding]::ASCII,$false,1024,$true)
                $firstLine = $reader.ReadLine()
                if (-not $firstLine -or $firstLine.Length -gt 8192) { continue }
                if ($firstLine -match '^GET (/[^\r\n ]*) HTTP/1\.[01]$') {
                    $path = $Matches[1]
                } else { continue }
                # Consume the remaining browser headers before replying.
                for ($headerIndex = 0; $headerIndex -lt 100; $headerIndex++) {
                    $headerLine = $reader.ReadLine()
                    if ($null -eq $headerLine -or $headerLine -eq "") { break }
                }
                $uri = [Uri]::new("http://127.0.0.1:$port$path")
                if ($uri.AbsolutePath -ne "/") { continue }
                $query = ConvertFrom-QueryString $uri.Query
                $validState = $query.ContainsKey("state") -and ($query["state"] -ceq $state)
                $validCode = $validState -and $query.ContainsKey("code") -and $query["code"]
                $body = if ($validCode) {
                    "<html><body><h3>Authorization received.</h3><p>Return to RWordPad. You can close this tab.</p></body></html>"
                } else {
                    "<html><body><h3>Authorization was not completed.</h3><p>Return to RWordPad.</p></body></html>"
                }
                $bodyBytes = [Text.Encoding]::UTF8.GetBytes($body)
                $header = "HTTP/1.1 200 OK`r`nContent-Type: text/html; charset=utf-8`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`nCache-Control: no-store`r`n`r`n"
                $headerBytes = [Text.Encoding]::ASCII.GetBytes($header)
                $stream.Write($headerBytes,0,$headerBytes.Length)
                $stream.Write($bodyBytes,0,$bodyBytes.Length)
                $stream.Flush()
                if (-not $validState) { continue }
                if ($query.ContainsKey("error")) {
                    throw "Google authorization was declined:$($query['error'])"
                }
                if ($validCode) {
                    $code = [string]$query["code"]
                    break
                }
            }
            finally {
                $connection.Dispose()
            }
        }
        if (-not $code) { throw "Google sign-in timed out or no authorization code was returned." }
        $tokenParameters = @{
            client_id = $ClientID
            code = $code
            code_verifier = $verifier
            redirect_uri = $redirect
            grant_type = "authorization_code"
        }
        if (-not [string]::IsNullOrWhiteSpace($ClientSecret)) {
            $tokenParameters.client_secret = $ClientSecret
        }
        try {
            $token = Invoke-RestMethod -Uri "https://oauth2.googleapis.com/token" -Method Post `
                -Body $tokenParameters -ContentType "application/x-www-form-urlencoded" -TimeoutSec 45
        }
        catch {
            throw "Google OAuth token exchange failed:$(Get-GoogleErrorDetail $_)"
        }
        if (-not $token.access_token) {
            throw "Google did not return an access token."
        }
        return $token
    }
    finally {
        $listener.Stop()
    }
}

function Invoke-GoogleGetFolder {
    param([string]$AccessToken,[string]$FolderID)

    $escaped = [Uri]::EscapeDataString($FolderID)
    $request = "https://www.googleapis.com/drive/v3/files/$escaped" +
        "?fields=id,name,mimeType,trashed&supportsAllDrives=true"
    try {
        $folder = Invoke-RestMethod -Method Get -Uri $request `
            -Headers @{Authorization = "Bearer $AccessToken"} -TimeoutSec 45
    }
    catch {
        throw "Google Drive folder lookup failed:$(Get-GoogleErrorDetail $_)"
    }
    if ($folder.trashed) { throw "The selected Google Drive folder is in the trash." }
    if ($folder.mimeType -ne "application/vnd.google-apps.folder") {
        throw "The specified ID does not identify a Google Drive folder."
    }
    return $folder
}

# ---------------------------------------- #
#  ---Cloud Remote Identity and Safety---  #
# ---------------------------------------- #

function Get-GoogleRemoteFile {
    param([string]$AccessToken,[string]$FileID)
    $id = [Uri]::EscapeDataString($FileID)
    $uri = "https://www.googleapis.com/drive/v3/files/$id" +
        "?fields=id,name,mimeType,trashed,parents,size,md5Checksum&supportsAllDrives=true"
    try {
        return Invoke-RestMethod -Uri $uri -Method Get -TimeoutSec 45 -Headers @{
            Authorization = "Bearer $AccessToken"
        }
    }
    catch {
        $detail = Get-GoogleErrorDetail $_
        if ($_.Exception.Response.StatusCode -eq [Net.HttpStatusCode]::NotFound) {
            throw "The linked Google Drive file ID could not be found:$FileID. It might have been permanently deleted or access is unavailable. No replacement was created."
        }
        throw "Google Drive remote file lookup failed:$detail"
    }
}

function Get-GoogleNewFileID {
    param([string]$AccessToken)
    try {
        $result = Invoke-RestMethod -Uri "https://www.googleapis.com/drive/v3/files/generateIds?count=1&space=drive&type=files" `
            -Method Get -Headers @{Authorization="Bearer $AccessToken"} -TimeoutSec 45
    }
    catch { throw "Google Drive could not reserve a file ID:$(Get-GoogleErrorDetail $_)" }
    $id = [string]$result.ids[0]
    if ($id -notmatch '^[A-Za-z0-9_-]{10,255}$') { throw "Google Drive returned an invalid reserved file ID." }
    return $id
}

# The upload copy gets its reserved ID before creation. The source RTF is NEVER
# rewritten by PowerShell: RWordPad updates its in-memory settings when the
# helper reports success,then saves the currently open document itself.
function Assert-CloudRtfIdentity {
    param([string]$Path,[string]$ExpectedDocumentID,[string]$ExpectedRemoteID)
    $rtf = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($Path))
    $pattern = '(?s)\{\\v RWP-CLOUD-V1\|([A-Za-z0-9+/=]+)\|/RWP-CLOUD-V1\}'
    $found = [regex]::Matches($rtf,$pattern)
    if ($found.Count -ne 1) {
        throw "Expected exactly one embedded cloud settings group in this RTF. No upload was performed."
    }
    try {
        $metadata = [Text.Encoding]::UTF8.GetString(
            [Convert]::FromBase64String($found[0].Groups[1].Value)) |
            ConvertFrom-Json -AsHashtable -ErrorAction Stop
    }
    catch { throw "Embedded cloud metadata is unreadable. No upload was performed." }
    if ([string]$metadata.DocumentID -cne $ExpectedDocumentID -or
        [string]$metadata.RemoteFileID -cne $ExpectedRemoteID) {
        throw "The document's embedded identity no longer matches the active upload request. No upload was performed."
    }
}

function New-CloudRtfUploadCopy {
    param([string]$Path,[string]$FileID,[string]$ExpectedDocumentID)
    $raw = [IO.File]::ReadAllBytes($Path)
    $rtf = [Text.Encoding]::Latin1.GetString($raw)
    $pattern = '(?s)\{\\v RWP-CLOUD-V1\|([A-Za-z0-9+/=]+)\|/RWP-CLOUD-V1\}'
    $matchesFound = [regex]::Matches($rtf,$pattern)
    if ($matchesFound.Count -ne 1) {
        throw "The RTF does not contain exactly one recognizable cloud metadata group. No upload was performed."
    }
    $match = $matchesFound[0]
    try {
        $metadataJson = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($match.Groups[1].Value))
        $metadata = $metadataJson | ConvertFrom-Json -AsHashtable -ErrorAction Stop
    }
    catch { throw "Unable to decode embedded RTF cloud metadata. No upload was performed." }
    if ([string]$metadata.DocumentID -cne $ExpectedDocumentID -or
        -not [string]::IsNullOrEmpty([string]$metadata.RemoteFileID)) {
        throw "Embedded cloud identity changed after the upload was requested. No upload was performed."
    }
    $metadata.RemoteFileID = $FileID
    $json = $metadata | ConvertTo-Json -Compress -Depth 5
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json))
    $replacement = '{\v RWP-CLOUD-V1|' + $encoded + '|/RWP-CLOUD-V1}'
    $updated = $rtf.Substring(0,$match.Index) + $replacement + $rtf.Substring($match.Index+$match.Length)
    $temporary = Join-Path ([IO.Path]::GetTempPath()) ("RWP-Cloud-"+[guid]::NewGuid().ToString("N")+".rtf")
    [IO.File]::WriteAllBytes($temporary,[Text.Encoding]::Latin1.GetBytes($updated))
    return $temporary
}

function Get-RemoteUploadHistory {
    if (-not (Test-Path -LiteralPath $RemoteIndexFile -PathType Leaf)) { return @{} }
    try {
        $index = Get-Content -LiteralPath $RemoteIndexFile -Raw -Encoding UTF8 |
            ConvertFrom-Json -AsHashtable -ErrorAction Stop
        if ($index -is [hashtable]) { return $index }
    }
    catch { }
    return @{}
}

function Save-RemoteUploadHistory {
    param([hashtable]$History)
    [IO.Directory]::CreateDirectory($ApplicationFolder) | Out-Null
    $content = $History | ConvertTo-Json -Depth 4 -Compress
    $temporary = Join-Path $ApplicationFolder ("remote-index-"+[guid]::NewGuid().ToString("N")+".tmp")
    try {
        [IO.File]::WriteAllText($temporary,$content,[Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary,$RemoteIndexFile,$true)
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
    }
}

function Write-CloudUploadResult {
    param([string]$FileID,[string]$DocumentID,[string]$Operation)
    if ([string]::IsNullOrEmpty($ResultFile)) { return }
    if ($FileID -notmatch '^[A-Za-z0-9_-]{10,255}$') { throw "Unable to report an invalid file ID." }
    $data = [ordered]@{RemoteFileID=$FileID;DocumentID=$DocumentID;Operation=$Operation} |
        ConvertTo-Json -Compress
    # The file path is a unique random path passed by the native process.
    [IO.File]::WriteAllText($ResultFile,$data,[Text.UTF8Encoding]::new($false))
}

function Invoke-GoogleUploadDocument {
    param([string]$AccessToken,[string]$FolderID,[string]$Path,
          [string]$OriginalFileName,[string]$ExistingID="",[string]$ReservedID="")

    # Two-step resumable upload; contents stream from disk in both modes.
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    $filename = if ($OriginalFileName) { $OriginalFileName } else { $file.Name }
    $extension = [IO.Path]::GetExtension($filename)
    if ($extension -ieq ".rtf") { $mimeType = "application/rtf" }
    elseif ($extension -ieq ".txt") { $mimeType = "text/plain" }
    else { $mimeType = "application/octet-stream" }
    if ($ExistingID) {
        $method = [Net.Http.HttpMethod]::Patch
        $uri = "https://www.googleapis.com/upload/drive/v3/files/$([Uri]::EscapeDataString($ExistingID))?uploadType=resumable" +
            "&fields=id,name,parents,size,md5Checksum,webViewLink&supportsAllDrives=true"
        $metadata = "{}" # Update ONLY the media; do not move/rename the existing file.
    }
    else {
        $method = [Net.Http.HttpMethod]::Post
        $uri = "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable" +
            "&fields=id,name,parents,size,md5Checksum,webViewLink&supportsAllDrives=true"
        $document = @{name=$filename;parents=@($FolderID)}
        if ($ReservedID) { $document.id = $ReservedID }
        $metadata = $document | ConvertTo-Json -Compress
    }

    $client = [Net.Http.HttpClient]::new()
    $client.Timeout = [TimeSpan]::FromMinutes($UploadTimeoutMinutes)
    try {
        $startRequest = [Net.Http.HttpRequestMessage]::new($method,$uri)
        try {
            $startRequest.Headers.Authorization = [Net.Http.Headers.AuthenticationHeaderValue]::new("Bearer",$AccessToken)
            $startRequest.Headers.TryAddWithoutValidation("X-Upload-Content-Type",$mimeType) | Out-Null
            $startRequest.Headers.TryAddWithoutValidation("X-Upload-Content-Length",$file.Length.ToString([Globalization.CultureInfo]::InvariantCulture)) | Out-Null
            $startRequest.Content = [Net.Http.StringContent]::new($metadata,[Text.Encoding]::UTF8,"application/json")
            $startResponse = $client.SendAsync($startRequest).GetAwaiter().GetResult()
            try {
                $startBody = $startResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
                if (-not $startResponse.IsSuccessStatusCode) {
                    throw "Drive refused the upload session (HTTP $([int]$startResponse.StatusCode)):$($startBody.Substring(0,[Math]::Min(500,$startBody.Length)))"
                }
                $sessionUri = $startResponse.Headers.Location
                if (-not $sessionUri -or $sessionUri.Scheme -ne "https" -or
                    $sessionUri.Host -notin @("www.googleapis.com","content.googleapis.com")) {
                    throw "Google did not return a valid HTTPS upload session URL."
                }
            }
            finally { $startResponse.Dispose() }
        }
        finally { $startRequest.Dispose() }
        $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try {
            $uploadRequest = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::Put,$sessionUri)
            try {
                $uploadRequest.Content = [Net.Http.StreamContent]::new($stream)
                $uploadRequest.Content.Headers.ContentType = [Net.Http.Headers.MediaTypeHeaderValue]::Parse($mimeType)
                $uploadResponse = $client.SendAsync($uploadRequest).GetAwaiter().GetResult()
                try {
                    $responseText = $uploadResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
                    if (-not $uploadResponse.IsSuccessStatusCode) {
                        throw "Upload failed (HTTP $([int]$uploadResponse.StatusCode)):$($responseText.Substring(0,[Math]::Min(500,$responseText.Length)))"
                    }
                    $uploaded = $responseText | ConvertFrom-Json
                    $expectedID = if ($ExistingID) { $ExistingID } else { $ReservedID }
                    if (-not $uploaded.id -or ($expectedID -and $uploaded.id -cne $expectedID)) {
                        throw "Upload returned an unexpected remote file ID. Check Google Drive before retrying."
                    }
                    if ($null -ne $uploaded.size -and [int64]$uploaded.size -ne [int64]$file.Length) {
                        throw "Drive reported a different file size for uploaded ID:$($uploaded.id)."
                    }
                    if (-not $ExistingID -and $FolderID -ne "root" -and
                        $uploaded.parents -and $FolderID -notin @($uploaded.parents)) {
                        throw "Drive reported a different destination folder for uploaded ID:$($uploaded.id)."
                    }
                    if (-not [string]::IsNullOrEmpty([string]$uploaded.md5Checksum)) {
                        $localMd5 = (Get-FileHash -LiteralPath $Path -Algorithm MD5).Hash.ToLowerInvariant()
                        if ($localMd5 -cne ([string]$uploaded.md5Checksum).ToLowerInvariant()) {
                            throw "Remote checksum differs from the uploaded document for ID:$($uploaded.id)."
                        }
                    }
                    return $uploaded
                }
                finally { $uploadResponse.Dispose() }
            }
            finally { $uploadRequest.Dispose() }
        }
        finally { $stream.Dispose() }
    }
    finally { $client.Dispose() }
}

# ---------------------------- #
#  ---Cloud Upload Beta UI---  #
# ---------------------------- #

if (-not $IsWindows) { throw "Cloud Upload Beta requires Windows and PowerShell 7." }
if (-not (Test-Path -LiteralPath $DocumentFile -PathType Leaf)) {
    throw "Saved document was not found:$DocumentFile"
}
Add-Type -AssemblyName System.Security.Cryptography.ProtectedData
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$preferences = @{}
if (Test-Path -LiteralPath $PreferencesFile -PathType Leaf) {
    try {
        $preferences = Get-Content -LiteralPath $PreferencesFile -Raw -ErrorAction Stop | ConvertFrom-Json -AsHashtable
    }
    catch { $preferences = @{} }
}

$form = [System.Windows.Forms.Form]::new()
$form.Text = "RWordPad - Google Drive Upload (Beta)"
$form.StartPosition = "CenterScreen"
$form.Size = [System.Drawing.Size]::new(700,448)
$form.MinimumSize = [System.Drawing.Size]::new(630,448)
$form.Font = [System.Drawing.Font]::new("Segoe UI",10)
$form.MaximizeBox = $false

$docLabel = [System.Windows.Forms.Label]::new()
$docLabel.SetBounds(16,15,650,41)
$docLabel.AutoEllipsis = $true
$docLabel.Text = "Document: $DocumentFile"
$form.Controls.Add($docLabel)

$clientLabel = [System.Windows.Forms.Label]::new()
$clientLabel.SetBounds(16,63,640,22)
$clientLabel.Text = "Google OAuth Desktop Client ID:"
$form.Controls.Add($clientLabel)

$loadCredentials = [System.Windows.Forms.Button]::new()
$loadCredentials.SetBounds(478,60,186,27)
$loadCredentials.Anchor = "Top,Right"
$loadCredentials.Text = "Load OAuth JSON..."
$form.Controls.Add($loadCredentials)

$clientBox = [System.Windows.Forms.TextBox]::new()
$clientBox.SetBounds(16,88,648,26)
$clientBox.Anchor = "Top,Left,Right"
$clientBox.Text = [string]$preferences.ClientID
$form.Controls.Add($clientBox)

$secretLabel = [System.Windows.Forms.Label]::new()
$secretLabel.SetBounds(16,126,640,22)
$secretLabel.Text = "Google OAuth Desktop Client Secret (or use saved authorization):"
$form.Controls.Add($secretLabel)

$secretBox = [System.Windows.Forms.TextBox]::new()
$secretBox.SetBounds(16,151,648,26)
$secretBox.Anchor = "Top,Left,Right"
$secretBox.UseSystemPasswordChar = $true
$form.Controls.Add($secretBox)

$loadCredentials.Add_Click({
    $dialog = [System.Windows.Forms.OpenFileDialog]::new()
    try {
        $dialog.Title = "Select Google OAuth Desktop Client JSON"
        $dialog.Filter = "JSON files (*.json)|*.json|All files (*.*)|*.*"
        $dialog.CheckFileExists = $true
        if ($dialog.ShowDialog($form) -ne [System.Windows.Forms.DialogResult]::OK) { return }
        try {
            $credentials = Get-Content -LiteralPath $dialog.FileName -Raw -Encoding UTF8 -ErrorAction Stop |
                ConvertFrom-Json -ErrorAction Stop
            if (-not $credentials.installed -or
                [string]$credentials.installed.client_id -notmatch '^[A-Za-z0-9._-]+\.apps\.googleusercontent\.com$') {
                throw "The selected file is not a valid Google OAuth Desktop client JSON."
            }
            $clientBox.Text = [string]$credentials.installed.client_id
            $secretBox.Text = [string]$credentials.installed.client_secret
            $statusLabel.Text = "OAuth client credentials loaded for this session."
        }
        catch {
            [System.Windows.Forms.MessageBox]::Show($form,$_.Exception.Message,"Cloud Upload - Credentials","OK","Error") | Out-Null
        }
    }
    finally { $dialog.Dispose() }
})

$folderLabel = [System.Windows.Forms.Label]::new()
$folderLabel.SetBounds(16,186,640,22)
$folderLabel.Text = "Destination Google Drive Folder ID (or 'root'):"
$form.Controls.Add($folderLabel)

$folderBox = [System.Windows.Forms.TextBox]::new()
$folderBox.SetBounds(16,211,648,26)
$folderBox.Anchor = "Top,Left,Right"
$folderBox.Text = if ($DocumentFolderID) { $DocumentFolderID } else { [string]$preferences.FolderID }
$form.Controls.Add($folderBox)

$notice = [System.Windows.Forms.Label]::new()
$notice.SetBounds(16,248,650,55)
$notice.Text = "Beta 1.4: First manual upload creates a file. Subsequent uploads update the same Drive ID. Remote changes require explicit approval. No automatic sync."
$form.Controls.Add($notice)

$statusLabel = [System.Windows.Forms.Label]::new()
$statusLabel.SetBounds(16,307,650,31)
$statusLabel.Text = "Ready.Upload will reuse saved Google authorization when available."
$form.Controls.Add($statusLabel)
if (Test-Path -LiteralPath $ProtectedCredentialsFile -PathType Leaf) {
    $statusLabel.Text = "Saved login found.Upload will try Google authorization without opening the browser."
}

$submit = [System.Windows.Forms.Button]::new()
$submit.SetBounds(330,352,188,34)
$submit.Anchor = "Bottom,Right"
$submit.Text = "Upload document"
$form.Controls.Add($submit)

$forget = [System.Windows.Forms.Button]::new()
$forget.SetBounds(16,352,180,34)
$forget.Anchor = "Bottom,Left"
$forget.Text = "Forget saved login"
$forget.Add_Click({
    if (-not (Test-Path -LiteralPath $ProtectedCredentialsFile -PathType Leaf)) {
        $statusLabel.Text = "No saved Google authorization was found."
        return
    }
    $answer = [System.Windows.Forms.MessageBox]::Show($form,
        "Remove the saved Google authorization from this Windows account? You will need to sign in again on the next upload.",
        "Forget Google Login","YesNo","Question")
    if ($answer -ne [System.Windows.Forms.DialogResult]::Yes) { return }
    try {
        Remove-Item -LiteralPath $ProtectedCredentialsFile -ErrorAction Stop
        $secretBox.Clear()
        $statusLabel.Text = "Saved authorization removed.Next upload will require Google sign-in."
    }
    catch {
        [System.Windows.Forms.MessageBox]::Show($form,$_.Exception.Message,"Cloud Upload - Error","OK","Error") | Out-Null
    }
})
$form.Controls.Add($forget)

$close = [System.Windows.Forms.Button]::new()
$close.SetBounds(532,352,132,34)
$close.Anchor = "Bottom,Right"
$close.Text = "Close"
$close.Add_Click({ $form.Close() })
$form.Controls.Add($close)

$submit.Add_Click({
    $clientID = $clientBox.Text.Trim()
    $clientSecret = $secretBox.Text.Trim()
    $folderID = $folderBox.Text.Trim()
    if ($clientID -notmatch '^[A-Za-z0-9._-]+\.apps\.googleusercontent\.com$') {
        [System.Windows.Forms.MessageBox]::Show($form,"Enter a valid OAuth Desktop Client ID ending in .apps.googleusercontent.com.","Cloud Upload", "OK", "Warning") | Out-Null
        return
    }
    if ($folderID -ne "root" -and $folderID -notmatch '^[A-Za-z0-9_-]{10,}$') {
        [System.Windows.Forms.MessageBox]::Show($form,"Enter the folder ID copied from a Google Drive folder URL,or 'root'.","Cloud Upload", "OK", "Warning") | Out-Null
        return
    }
    $submit.Enabled = $false
    $tokenResponse = $null
    $token = $null
    $savedAuthorization = $null
    $stagedUpload = $null
    try {
        # Keep non-sensitive settings separate from DPAPI-protected OAuth credentials.
        [IO.Directory]::CreateDirectory($ApplicationFolder) | Out-Null
        # A document override is not a change to the default folder of other documents.
        $savedDefaultFolder = if ($DocumentFolderID) { [string]$preferences.FolderID } else { $folderID }
        @{ClientID=$clientID;FolderID=$savedDefaultFolder} | ConvertTo-Json | `
            Set-Content -LiteralPath $PreferencesFile -Encoding UTF8 -ErrorAction Stop

        $savedAuthorization = Get-SavedGoogleAuthorization -ClientID $clientID
        if ($savedAuthorization) {
            if (-not $clientSecret) { $clientSecret = $savedAuthorization.ClientSecret }
            # If an explicitly supplied secret differs,consider this a fresh authorization.
            if ($clientSecret -ceq $savedAuthorization.ClientSecret) {
                $statusLabel.Text = "Reusing saved Google authorization..."
                $form.Refresh()
                $tokenResponse = Invoke-GoogleRefreshToken -ClientID $clientID `
                    -ClientSecret $clientSecret -RefreshToken $savedAuthorization.RefreshToken
            }
        }

        if (-not $tokenResponse) {
            if (-not $clientSecret) {
                throw "Your Google OAuth client secret is required.Click Load OAuth JSON and select the Desktop credentials file."
            }
            $statusLabel.Text = "Opening Google sign-in in your browser..."
            $form.Refresh()
            $tokenResponse = Invoke-GoogleDesktopLogin -ClientID $clientID -ClientSecret $clientSecret -Owner $form
            if ($tokenResponse.refresh_token) {
                try {
                    Save-GoogleAuthorization -ClientID $clientID -ClientSecret $clientSecret `
                        -RefreshToken ([string]$tokenResponse.refresh_token)
                }
                catch {
                    [System.Windows.Forms.MessageBox]::Show($form,
                        "Google authorization succeeded,but encrypted credential storage failed.The upload can continue;you will need to sign in again next time.`n$($_.Exception.Message)",
                        "Cloud Upload - Credential Storage","OK","Warning") | Out-Null
                }
            }
            else {
                [System.Windows.Forms.MessageBox]::Show($form,
                    "Google returned an access token but no refresh token.The upload can continue, but sign-in will be required again next time.",
                    "Cloud Upload - Authorization","OK","Warning") | Out-Null
            }
        }
        $token = [string]$tokenResponse.access_token
        if (-not $token) { throw "Google did not provide an access token." }
        $statusLabel.Text = "Checking destination folder..."
        $form.Refresh()
        $remoteFolder = Invoke-GoogleGetFolder -AccessToken $token -FolderID $folderID
        $fileName = [IO.Path]::GetFileName($DocumentFile)
        $tracked = -not [string]::IsNullOrWhiteSpace($DocumentID)
        $record = $null
        $history = Get-RemoteUploadHistory
        $operation = "Created"
        $fileID = $DocumentRemoteID
        if ($tracked -and $DocumentID -notmatch '^[A-Za-z0-9_-]{10,63}$') {
            throw "Invalid embedded document identity."
        }
        if ($fileID -and $fileID -notmatch '^[A-Za-z0-9_-]{10,255}$') {
            throw "Invalid embedded Google Drive file ID."
        }
        if ($tracked -and $fileID) {
            Assert-CloudRtfIdentity -Path $DocumentFile `
                -ExpectedDocumentID $DocumentID -ExpectedRemoteID $fileID
            $statusLabel.Text = "Checking existing remote document..."
            $form.Refresh()
            $record = Get-GoogleRemoteFile -AccessToken $token -FileID $fileID
            if ($record.trashed) {
                throw "This document's remote file is in the Google Drive trash (ID:$fileID). Restore it in Drive or detach the cloud identity in WordPad. No new copy was created."
            }
            if (-not $record.md5Checksum) {
                throw "The remote ID does not identify a normal binary file with an MD5 checksum. Upload cancelled."
            }
            if ($record.name -cne $fileName) {
                throw "The linked remote file is named '$($record.name)',not '$fileName'. To avoid overwriting a different document, upload was cancelled. Use New Cloud Identity in the WordPad document settings if this is a separate copy."
            }
            if ($remoteFolder.id -notin @($record.parents)) {
                throw "The linked remote file is not inside the selected Google Drive folder. No upload was performed."
            }
            $localMd5 = (Get-FileHash -LiteralPath $DocumentFile -Algorithm MD5).Hash.ToLowerInvariant()
            if ($localMd5 -ceq ([string]$record.md5Checksum).ToLowerInvariant()) {
                $operation = "Unchanged"
                $statusLabel.Text = "Already up to date. Remote file ID:$fileID"
                Write-CloudUploadResult -FileID $fileID -DocumentID $DocumentID -Operation $operation
                [System.Windows.Forms.MessageBox]::Show($form,
                    "The cloud file is already identical to the local document.`nGoogle Drive File ID:$fileID",
                    "Cloud Upload - Unchanged", "OK", "Information") | Out-Null
                if ($ResultFile) { $form.Close() }
                return
            }
            $knownMd5 = [string]$history[$fileID]
            if (-not $knownMd5 -or $knownMd5 -cne ([string]$record.md5Checksum).ToLowerInvariant()) {
                $confirmation = [System.Windows.Forms.MessageBox]::Show($form,
                    "Google Drive's existing file differs from the last verified upload (or no previous checksum is recorded).`n`nRemote file:$($record.name)`nRemote ID:$fileID`n`nReplace the remote content with this local file?",
                    "Cloud Upload - Remote May Have Changed","YesNo","Warning")
                if ($confirmation -ne [System.Windows.Forms.DialogResult]::Yes) {
                    $statusLabel.Text = "Upload cancelled to protect the remote version."
                    return
                }
            }
            $operation = "Updated"
        }
        elseif ($tracked) {
            $statusLabel.Text = "Reserving a unique Google Drive file ID..."
            $form.Refresh()
            $fileID = Get-GoogleNewFileID -AccessToken $token
            $stagedUpload = New-CloudRtfUploadCopy -Path $DocumentFile `
                -FileID $fileID -ExpectedDocumentID $DocumentID
        }
        $uploadPath = if ($stagedUpload) { $stagedUpload } else { $DocumentFile }
        $statusLabel.Text = "Uploading document to '$($remoteFolder.name)'..."
        $form.Refresh()
        $uploaded = Invoke-GoogleUploadDocument -AccessToken $token -FolderID $folderID `
            -Path $uploadPath -OriginalFileName $fileName `
            -ExistingID $(if ($record) { $fileID } else { "" }) `
            -ReservedID $(if ($tracked -and -not $record) { $fileID } else { "" })
        $token = $null
        $tokenResponse = $null
        $fileID = [string]$uploaded.id
        try {
            $remoteMd5 = [string]$uploaded.md5Checksum
            if ($remoteMd5) {
                $history[$fileID] = $remoteMd5.ToLowerInvariant()
                Save-RemoteUploadHistory -History $history
            }
        }
        catch {
            [System.Windows.Forms.MessageBox]::Show($form,
                "Upload succeeded, but the local remote-checksum history could not be saved. Future overwrites may require confirmation.`n$($_.Exception.Message)",
                "Cloud Upload - Warning", "OK", "Warning") | Out-Null
        }
        $statusLabel.Text = "$operation. Remote file ID:$fileID"
        Write-CloudUploadResult -FileID $fileID -DocumentID $DocumentID -Operation $operation
        [System.Windows.Forms.MessageBox]::Show($form,
            "$operation:$($uploaded.name)`nFolder:$($remoteFolder.name)`nGoogle Drive File ID:$fileID",
            "Cloud Upload - Success", "OK", "Information") | Out-Null
        if ($ResultFile) { $form.Close() }
    }
    catch {
        if ($form.IsDisposed) { return }
        $statusLabel.Text = "Upload not confirmed. See the error dialog."
        $message = $_.Exception.Message
        if ($message.Length -gt 1400) { $message = $message.Substring(0,1400) }
        [System.Windows.Forms.MessageBox]::Show($form,$message,"Cloud Upload - Error","OK","Error") | Out-Null
    }
    finally {
        if ($stagedUpload -and (Test-Path -LiteralPath $stagedUpload -PathType Leaf)) {
            Remove-Item -LiteralPath $stagedUpload -Force -ErrorAction SilentlyContinue
        }
        $token = $null
        $tokenResponse = $null
        $savedAuthorization = $null
        $clientSecret = $null
        if (-not $form.IsDisposed) { $submit.Enabled = $true }
    }
})

[void]$form.ShowDialog()
$form.Dispose()
