#include "archive_reader.h"
#include "miniz.h"
#include <windows.h>
#include <shlobj.h>
#include <dpapi.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

std::wstring Utf8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    
    // 1. Try UTF-8 with strict validation (MB_ERR_INVALID_CHARS)
    int size_needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, &str[0], (int)str.size(), NULL, 0);
    if (size_needed > 0) {
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }
    
    // 2. Check if the string is valid GBK (Simplified Chinese / CP936)
    auto IsValidGBK = [](const std::string& s) -> bool {
        size_t i = 0;
        size_t len = s.length();
        bool hasDoubleByte = false;
        while (i < len) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x80) {
                i++;
            } else {
                if (c >= 0x81 && c <= 0xFE && i + 1 < len) {
                    unsigned char c2 = (unsigned char)s[i + 1];
                    if ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFE)) {
                        hasDoubleByte = true;
                        i += 2;
                        continue;
                    }
                }
                return false; // Invalid GBK byte sequence
            }
        }
        return hasDoubleByte;
    };

    UINT codePage = CP_ACP; // Default fallback: Active system ANSI code page
    if (IsValidGBK(str)) {
        codePage = 936; // Force Simplified Chinese GBK decoding
    }

    size_needed = MultiByteToWideChar(codePage, 0, &str[0], (int)str.size(), NULL, 0);
    if (size_needed > 0) {
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(codePage, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }

    // 3. Ultimate fallback: Map byte-by-byte (latin1 / CP1252 style mapping)
    std::wstring wstrTo;
    wstrTo.reserve(str.size());
    for (char c : str) {
        wstrTo.push_back((wchar_t)(unsigned char)c);
    }
    return wstrTo;
}

std::string WStringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

bool CreateDirectories(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        if (CreateDirectories(path.substr(0, pos))) {
            return CreateDirectoryW(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
        }
    }
    return false;
}

#include <stdarg.h>

static void LogDebug(const char* format, ...) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring logFilePath = std::wstring(tempPath) + L"ArchivePreviewerDebug.txt";
    
    FILE* f = _wfopen(logFilePath.c_str(), L"a");
    if (!f) return;
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[APP %02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    
    fprintf(f, "\n");
    fclose(f);
}

// Helper to run a command hidden and redirect stdout to string
bool RunCommandAndGetOutput(const std::wstring& cmd, std::string& output, int& exitCode, const bool* pCancelFlag = nullptr) {
    LogDebug("RunCommand: %ls", cmd.c_str());
    
    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;
    
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;
    
    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
        LogDebug("RunCommand: CreatePipe failed");
        return false;
    }
    if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        LogDebug("RunCommand: SetHandleInformation failed");
        return false;
    }
    
    STARTUPINFOW siStartInfo;
    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;
    
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    
    std::wstring cmdWritable = cmd;
    if (!CreateProcessW(NULL, &cmdWritable[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &siStartInfo, &piProcInfo)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        LogDebug("RunCommand: CreateProcessW failed. Error: %d", GetLastError());
        return false;
    }
    
    CloseHandle(hChildStd_OUT_Wr); // Close write end in parent
    
    DWORD dwRead;
    CHAR chBuf[4096];
    output.clear();
    
    bool aborted = false;
    while (true) {
        if (pCancelFlag && *pCancelFlag) {
            TerminateProcess(piProcInfo.hProcess, 1);
            aborted = true;
            break;
        }

        DWORD dwReadAvail = 0;
        if (PeekNamedPipe(hChildStd_OUT_Rd, NULL, 0, NULL, &dwReadAvail, NULL) && dwReadAvail > 0) {
            if (ReadFile(hChildStd_OUT_Rd, chBuf, sizeof(chBuf) - 1, &dwRead, NULL) && dwRead > 0) {
                output.append(chBuf, dwRead);
            }
        } else {
            DWORD dwWait = WaitForSingleObject(piProcInfo.hProcess, 50);
            if (dwWait == WAIT_OBJECT_0) {
                // Read remaining data
                while (ReadFile(hChildStd_OUT_Rd, chBuf, sizeof(chBuf) - 1, &dwRead, NULL) && dwRead > 0) {
                    output.append(chBuf, dwRead);
                }
                break;
            }
        }
    }
    
    if (aborted) {
        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
        CloseHandle(hChildStd_OUT_Rd);
        LogDebug("RunCommand: Aborted due to cancellation.");
        return false;
    }
    
    WaitForSingleObject(piProcInfo.hProcess, INFINITE);
    DWORD dwExitCode = 0;
    GetExitCodeProcess(piProcInfo.hProcess, &dwExitCode);
    exitCode = (int)dwExitCode;
    
    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);
    CloseHandle(hChildStd_OUT_Rd);
    
    LogDebug("RunCommand: Finished. ExitCode: %d, Output Size: %u", exitCode, output.size());
    return true;
}

// Find 7z.exe path on the system
std::wstring Find7Zip() {
    HKEY hKey;
    std::wstring path;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\7-Zip", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(hKey, L"Path", NULL, NULL, (BYTE*)buf, &size) == ERROR_SUCCESS) {
            path = buf;
            if (!path.empty() && path.back() != L'\\') path += L'\\';
            path += L"7z.exe";
        }
        RegCloseKey(hKey);
    }
    
    if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return path;
    }
    
    wchar_t pf[MAX_PATH];
    if (SHGetSpecialFolderPathW(NULL, pf, CSIDL_PROGRAM_FILES, FALSE)) {
        std::wstring fallback = std::wstring(pf) + L"\\7-Zip\\7z.exe";
        if (GetFileAttributesW(fallback.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return fallback;
        }
    }
    return L"7z.exe";
}

// Parse 7z l -slt output
std::vector<ArchiveFileInfo> Parse7ZipOutput(const std::string& output) {
    std::vector<ArchiveFileInfo> files;
    std::string line;
    std::size_t pos = 0;
    
    ArchiveFileInfo currentItem;
    bool hasItem = false;
    bool headerPassed = false;
    
    auto Trim = [](const std::string& s) -> std::string {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };

    while (pos < output.size()) {
        std::size_t nextPos = output.find('\n', pos);
        if (nextPos == std::string::npos) nextPos = output.size();
        line = output.substr(pos, nextPos - pos);
        pos = nextPos + 1;
        
        line = Trim(line);
        if (line.empty()) continue;

        if (line.rfind("----------", 0) == 0) {
            headerPassed = true;
            continue;
        }
        if (line.rfind("--", 0) == 0) {
            continue;
        }

        if (!headerPassed) continue;
        
        std::size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string key = Trim(line.substr(0, eqPos));
            std::string val = Trim(line.substr(eqPos + 1));
            
            if (key == "Path") {
                if (hasItem && !currentItem.internalPath.empty()) {
                    files.push_back(currentItem);
                }
                currentItem = ArchiveFileInfo();
                currentItem.isDirectory = false;
                currentItem.isEncrypted = false;
                currentItem.fileSize = 0;
                currentItem.compressedSize = 0;
                hasItem = true;
                
                currentItem.internalPath = val;
                currentItem.name = Utf8ToWString(val);
            } else if (key == "Folder") {
                currentItem.isDirectory = (val == "+");
            } else if (key == "Size") {
                currentItem.fileSize = _strtoui64(val.c_str(), nullptr, 10);
            } else if (key == "Packed Size") {
                currentItem.compressedSize = _strtoui64(val.c_str(), nullptr, 10);
            } else if (key == "Encrypted") {
                currentItem.isEncrypted = (val == "+");
            }
        }
    }
    
    if (hasItem && !currentItem.internalPath.empty()) {
        files.push_back(currentItem);
    }
    return files;
}

// Password Store logic using Windows DPAPI
std::wstring GetPasswordFilePath() {
    wchar_t path[MAX_PATH];
    if (SHGetSpecialFolderPathW(NULL, path, CSIDL_LOCAL_APPDATA, FALSE)) {
        std::wstring dir = std::wstring(path) + L"\\ArchivePreviewer";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\passwords.dat";
    }
    return L"";
}

std::vector<std::wstring> PasswordStore::LoadPasswords() {
    std::vector<std::wstring> passwords;
    std::wstring filePath = GetPasswordFilePath();
    if (filePath.empty()) return passwords;

    FILE* f = _wfopen(filePath.c_str(), L"rb");
    if (!f) return passwords;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return passwords;
    }

    std::vector<BYTE> encryptedData(size);
    fread(encryptedData.data(), 1, size, f);
    fclose(f);

    DATA_BLOB dataIn;
    dataIn.pbData = encryptedData.data();
    dataIn.cbData = (DWORD)encryptedData.size();
    DATA_BLOB dataOut;

    if (CryptUnprotectData(&dataIn, NULL, NULL, NULL, NULL, 0, &dataOut)) {
        BYTE* p = dataOut.pbData;
        BYTE* end = p + dataOut.cbData;
        while (p + sizeof(int) <= end) {
            int len = *(int*)p;
            p += sizeof(int);
            if (p + len * sizeof(wchar_t) <= end) {
                std::wstring pwd((wchar_t*)p, len);
                passwords.push_back(pwd);
                p += len * sizeof(wchar_t);
            } else {
                break;
            }
        }
        LocalFree(dataOut.pbData);
    }
    return passwords;
}

bool PasswordStore::SavePasswords(const std::vector<std::wstring>& passwords) {
    std::wstring filePath = GetPasswordFilePath();
    if (filePath.empty()) return false;

    std::vector<BYTE> buffer;
    for (const auto& pwd : passwords) {
        int len = (int)pwd.size();
        BYTE* pLen = (BYTE*)&len;
        buffer.insert(buffer.end(), pLen, pLen + sizeof(int));
        BYTE* pStr = (BYTE*)pwd.c_str();
        buffer.insert(buffer.end(), pStr, pStr + len * sizeof(wchar_t));
    }

    DATA_BLOB dataIn;
    dataIn.pbData = buffer.data();
    dataIn.cbData = (DWORD)buffer.size();
    DATA_BLOB dataOut;

    bool success = false;
    if (CryptProtectData(&dataIn, L"ArchivePreviewerPasswords", NULL, NULL, NULL, 0, &dataOut)) {
        FILE* f = _wfopen(filePath.c_str(), L"wb");
        if (f) {
            fwrite(dataOut.pbData, 1, dataOut.cbData, f);
            fclose(f);
            success = true;
        }
        LocalFree(dataOut.pbData);
    }
    return success;
}

// Archive Reader Implementation
struct ArchiveReader::Impl {
    mz_zip_archive zip;
    FILE* file;
    
    bool use7Zip;
    std::wstring sevenZipPath;
    std::vector<ArchiveFileInfo> cachedFiles;
    
    std::vector<char> memoryBuffer;
    std::wstring tempFilePath;
    
    Impl() : file(nullptr), use7Zip(false) {
        memset(&zip, 0, sizeof(zip));
        sevenZipPath = Find7Zip();
    }
    
    ~Impl() {
        if (file) {
            mz_zip_reader_end(&zip);
            fclose(file);
        } else if (!memoryBuffer.empty()) {
            mz_zip_reader_end(&zip);
        }
        if (!tempFilePath.empty()) {
            DeleteFileW(tempFilePath.c_str());
        }
    }
};

ArchiveReader::ArchiveReader() : m_isOpen(false), m_isEncrypted(false) {}

ArchiveReader::~ArchiveReader() {
    Close();
}

bool ArchiveReader::Open(const std::wstring& archivePath, const std::wstring& password) {
    Close();
    m_archivePath = archivePath;
    m_password = password;
    m_isEncrypted = false;
    m_isOpen = false;

    m_impl = std::make_unique<Impl>();

    DWORD attribs = GetFileAttributesW(archivePath.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES || (attribs & FILE_ATTRIBUTE_DIRECTORY)) {
        m_impl.reset();
        return false;
    }

    std::wstring ext;
    size_t dotPos = archivePath.find_last_of(L'.');
    if (dotPos != std::wstring::npos) {
        ext = archivePath.substr(dotPos);
    }
    for (auto& ch : ext) ch = towupper(ch);

    // Try miniz if it's an unencrypted zip archive
    if (ext == L".ZIP" && password.empty()) {
        m_impl->file = _wfopen(archivePath.c_str(), L"rb");
        if (m_impl->file) {
            fseek(m_impl->file, 0, SEEK_END);
            mz_uint64 size = _ftelli64(m_impl->file);
            fseek(m_impl->file, 0, SEEK_SET);

            if (mz_zip_reader_init_cfile(&m_impl->zip, m_impl->file, size, 0)) {
                mz_uint numFiles = mz_zip_reader_get_num_files(&m_impl->zip);
                bool hasEncrypted = false;
                for (mz_uint i = 0; i < numFiles; ++i) {
                    mz_zip_archive_file_stat file_stat;
                    if (mz_zip_reader_file_stat(&m_impl->zip, i, &file_stat)) {
                        if (file_stat.m_is_encrypted) {
                            hasEncrypted = true;
                            break;
                        }
                    }
                }

                if (!hasEncrypted) {
                    m_isOpen = true;
                    return true;
                } else {
                    mz_zip_reader_end(&m_impl->zip);
                    fclose(m_impl->file);
                    m_impl->file = nullptr;
                }
            } else {
                fclose(m_impl->file);
                m_impl->file = nullptr;
            }
        }
    }

    // Otherwise, use 7-Zip CLI!
    m_impl->use7Zip = true;
    
    std::wstring cmd = L"\"" + m_impl->sevenZipPath + L"\" l -sccUTF-8 -slt \"" + archivePath + L"\"";
    if (!password.empty()) {
        cmd += L" -p\"" + password + L"\"";
    } else {
        cmd += L" -p-";
    }

    std::string output;
    int exitCode = 0;
    if (!RunCommandAndGetOutput(cmd, output, exitCode, m_pCancelFlag)) {
        m_impl.reset();
        return false;
    }

    if (exitCode != 0) {
        m_isEncrypted = true;
        m_impl.reset();
        return false;
    }

    m_impl->cachedFiles = Parse7ZipOutput(output);
    
    bool hasEncrypted = false;
    std::string firstEncryptedFile;
    for (const auto& f : m_impl->cachedFiles) {
        if (f.isEncrypted) {
            hasEncrypted = true;
            if (firstEncryptedFile.empty() && !f.isDirectory) {
                firstEncryptedFile = f.internalPath;
            }
        }
    }

    if (hasEncrypted) {
        m_isEncrypted = true;
        
        // If password was empty, test if it is actually required
        if (!firstEncryptedFile.empty()) {
            std::wstring testCmd = L"\"" + m_impl->sevenZipPath + L"\" t -sccUTF-8 \"" + archivePath + L"\"";
            if (!password.empty()) {
                testCmd += L" -p\"" + password + L"\"";
            } else {
                testCmd += L" -p-";
            }
            testCmd += L" \"" + Utf8ToWString(firstEncryptedFile) + L"\"";
            
            std::string testOut;
            int testCode = 0;
            if (RunCommandAndGetOutput(testCmd, testOut, testCode, m_pCancelFlag) && testCode != 0) {
                m_impl.reset();
                return false;
            }
        }
    }

    m_isOpen = true;
    return true;
}

static size_t mz_zip_stream_read_func(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n) {
    IStream* pStream = static_cast<IStream*>(pOpaque);
    if (!pStream) return 0;
    
    LARGE_INTEGER liOffset;
    liOffset.QuadPart = file_ofs;
    if (FAILED(pStream->Seek(liOffset, STREAM_SEEK_SET, NULL))) {
        return 0;
    }
    
    ULONG bytesRead = 0;
    if (FAILED(pStream->Read(pBuf, (ULONG)n, &bytesRead))) {
        return 0;
    }
    return bytesRead;
}

bool ArchiveReader::OpenFromStream(IStream* pStream, const std::wstring& extension, const std::wstring& password) {
    Close();
    if (!pStream) return false;
    
    std::wstring ext = extension;
    for (auto& ch : ext) ch = towupper(ch);
    
    if (ext == L".ZIP" && password.empty()) {
        m_impl = std::make_unique<Impl>();
        m_impl->use7Zip = false;
        
        STATSTG stat = { 0 };
        if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_NONAME))) {
            mz_uint64 fileSize = stat.cbSize.QuadPart;
            m_impl->zip.m_pRead = mz_zip_stream_read_func;
            m_impl->zip.m_pIO_opaque = pStream;
            if (mz_zip_reader_init(&m_impl->zip, fileSize, 0)) {
                mz_uint numFiles = mz_zip_reader_get_num_files(&m_impl->zip);
                bool hasEncrypted = false;
                for (mz_uint i = 0; i < numFiles; ++i) {
                    mz_zip_archive_file_stat file_stat;
                    if (mz_zip_reader_file_stat(&m_impl->zip, i, &file_stat)) {
                        if (file_stat.m_is_encrypted) {
                            hasEncrypted = true;
                            break;
                        }
                    }
                }

                if (!hasEncrypted) {
                    m_isOpen = true;
                    return true;
                } else {
                    mz_zip_reader_end(&m_impl->zip);
                }
            }
        }
        m_impl.reset();
    }
    
    STATSTG stat = { 0 };
    if (FAILED(pStream->Stat(&stat, STATFLAG_NONAME))) return false;
    
    mz_uint64 fileSize = stat.cbSize.QuadPart;
    if (fileSize > 1000ULL * 1024ULL * 1024ULL) {
        // Skip files larger than 1 GB (1000 MB) to prevent Windows Explorer hang
        return false;
    }
    
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    
    wchar_t tempFile[MAX_PATH];
    GetTempFileNameW(tempPath, L"AP_", 0, tempFile);
    
    std::wstring finalTempPath = tempFile;
    size_t dotPos = finalTempPath.find_last_of(L'.');
    if (dotPos != std::wstring::npos) {
        finalTempPath = finalTempPath.substr(0, dotPos) + extension;
    }
    
    FILE* f = _wfopen(finalTempPath.c_str(), L"wb");
    if (!f) return false;
    
    LARGE_INTEGER liZero = { 0 };
    pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
    
    char chunk[65536];
    ULONG bytesRead = 0;
    HRESULT hr;
    do {
        bytesRead = 0;
        hr = pStream->Read(chunk, sizeof(chunk), &bytesRead);
        if (FAILED(hr)) break;
        if (bytesRead > 0) {
            fwrite(chunk, 1, bytesRead, f);
        }
    } while (hr == S_OK && bytesRead > 0);
    fclose(f);
    
    if (FAILED(hr)) {
        DeleteFileW(finalTempPath.c_str());
        return false;
    }
    
    bool success = Open(finalTempPath, password);
    if (!success) {
        DeleteFileW(finalTempPath.c_str());
        return false;
    }
    
    if (m_impl) {
        m_impl->tempFilePath = finalTempPath;
    }
    return true;
}

void ArchiveReader::Close() {
    m_impl.reset();
    m_isOpen = false;
    m_archivePath.clear();
    m_password.clear();
    m_isEncrypted = false;
}

std::vector<ArchiveFileInfo> ArchiveReader::ListFiles() const {
    std::vector<ArchiveFileInfo> files;
    if (!m_isOpen || !m_impl) return files;

    if (!m_impl->use7Zip) {
        mz_uint numFiles = mz_zip_reader_get_num_files(&m_impl->zip);
        for (mz_uint i = 0; i < numFiles; ++i) {
            mz_zip_archive_file_stat file_stat;
            if (!mz_zip_reader_file_stat(&m_impl->zip, i, &file_stat)) {
                continue;
            }

            ArchiveFileInfo info;
            info.internalPath = file_stat.m_filename;
            info.name = Utf8ToWString(file_stat.m_filename);
            info.fileSize = file_stat.m_uncomp_size;
            info.compressedSize = file_stat.m_comp_size;
            info.isDirectory = mz_zip_reader_is_file_a_directory(&m_impl->zip, i) ? true : false;
            info.isEncrypted = file_stat.m_is_encrypted ? true : false;
            files.push_back(info);
        }
    } else {
        files = m_impl->cachedFiles;
    }
    return files;
}

bool ArchiveReader::ExtractFileToMemory(const std::string& internalPath, std::vector<char>& outBuffer) const {
    if (!m_isOpen || !m_impl) return false;

    if (!m_impl->use7Zip) {
        int fileIndex = mz_zip_reader_locate_file(&m_impl->zip, internalPath.c_str(), nullptr, 0);
        if (fileIndex < 0) return false;

        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(&m_impl->zip, fileIndex, &file_stat)) {
            return false;
        }

        outBuffer.resize(file_stat.m_uncomp_size);
        if (file_stat.m_uncomp_size == 0) return true;

        mz_bool success = mz_zip_reader_extract_to_mem(&m_impl->zip, fileIndex, outBuffer.data(), outBuffer.size(), 0);
        return success ? true : false;
    } else {
        std::wstring cmd = L"\"" + m_impl->sevenZipPath + L"\" e -sccUTF-8 \"" + m_archivePath + L"\" -so";
        if (!m_password.empty()) {
            cmd += L" -p\"" + m_password + L"\"";
        } else {
            cmd += L" -p-";
        }
        cmd += L" \"" + Utf8ToWString(internalPath) + L"\"";

        std::string output;
        int exitCode = 0;
        if (!RunCommandAndGetOutput(cmd, output, exitCode, m_pCancelFlag) || exitCode != 0) {
            return false;
        }

        outBuffer.assign(output.begin(), output.end());
        return true;
    }
}

bool ArchiveReader::ExtractFileToDisk(const std::string& internalPath, const std::wstring& destDiskPath) const {
    if (!m_isOpen || !m_impl) return false;

    size_t pos = destDiskPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        CreateDirectories(destDiskPath.substr(0, pos));
    }

    std::vector<char> buffer;
    if (!ExtractFileToMemory(internalPath, buffer)) {
        return false;
    }

    FILE* out = _wfopen(destDiskPath.c_str(), L"wb");
    if (!out) return false;

    if (!buffer.empty()) {
        fwrite(buffer.data(), 1, buffer.size(), out);
    }
    fclose(out);
    return true;
}

bool ArchiveReader::ExtractAll(const std::wstring& destDirectoryPath) const {
    if (!m_isOpen || !m_impl) return false;

    if (m_impl->use7Zip) {
        CreateDirectories(destDirectoryPath);
        
        std::wstring cmd = L"\"" + m_impl->sevenZipPath + L"\" e -y -sccUTF-8 -o\"" + destDirectoryPath + L"\" \"" + m_archivePath + L"\"";
        if (!m_password.empty()) {
            cmd += L" -p\"" + m_password + L"\"";
        } else {
            cmd += L" -p-";
        }

        std::string output;
        int exitCode = 0;
        if (!RunCommandAndGetOutput(cmd, output, exitCode, m_pCancelFlag) || exitCode != 0) {
            return false;
        }
        return true;
    }
    
    std::vector<ArchiveFileInfo> files = ListFiles();
    for (const auto& file : files) {
        if (file.isDirectory) continue;

        std::wstring destPath = destDirectoryPath + L"\\" + fs::path(file.name).filename().wstring();
        for (auto& ch : destPath) {
            if (ch == L'/') ch = L'\\';
        }

        if (!ExtractFileToDisk(file.internalPath, destPath)) {
            return false;
        }
    }
    return true;
}

bool IsNonFirstMultiPartVolume(const std::wstring& filePath) {
    size_t dotPos = filePath.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = filePath.substr(dotPos);
    for (auto& ch : ext) ch = towupper(ch);
    
    size_t slashPos = filePath.find_last_of(L"\\/");
    size_t startPos = (slashPos == std::wstring::npos) ? 0 : slashPos + 1;
    std::wstring stem = filePath.substr(startPos, dotPos - startPos);
    for (auto& ch : stem) ch = towupper(ch);
    
    if (ext == L".RAR") {
        size_t partPos = stem.rfind(L".PART");
        if (partPos != std::wstring::npos && partPos + 5 < stem.size()) {
            std::wstring numStr = stem.substr(partPos + 5);
            bool isNumeric = !numStr.empty();
            for (wchar_t c : numStr) {
                if (!iswdigit(c)) {
                    isNumeric = false;
                    break;
                }
            }
            if (isNumeric) {
                int partNum = _wtoi(numStr.c_str());
                if (partNum > 1) {
                    return true;
                }
            }
        }
    }
    return false;
}

