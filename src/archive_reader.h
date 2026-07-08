#pragma once
#include <string>
#include <vector>
#include <memory>

struct ArchiveFileInfo {
    std::wstring name;          // Wide string for UI display
    std::string internalPath;   // Original UTF-8/CP437 path inside the archive
    unsigned __int64 fileSize;
    unsigned __int64 compressedSize;
    bool isDirectory;
    bool isEncrypted;           // Flag indicating if this file is password-protected
};

class PasswordStore {
public:
    static std::vector<std::wstring> LoadPasswords();
    static bool SavePasswords(const std::vector<std::wstring>& passwords);
};

class ArchiveReader {
public:
    ArchiveReader();
    ~ArchiveReader();

    // Opens an archive. If password is required and provided, decrypts using it.
    bool Open(const std::wstring& archivePath, const std::wstring& password = L"");
    bool OpenFromStream(struct IStream* pStream, const std::wstring& extension, const std::wstring& password = L"");
    void Close();

    std::vector<ArchiveFileInfo> ListFiles() const;
    bool ExtractFileToMemory(const std::string& internalPath, std::vector<char>& outBuffer) const;
    bool ExtractFileToDisk(const std::string& internalPath, const std::wstring& destDiskPath) const;
    bool ExtractAll(const std::wstring& destDirectoryPath) const;

    bool IsOpen() const { return m_isOpen; }
    std::wstring GetArchivePath() const { return m_archivePath; }
    bool IsEncrypted() const { return m_isEncrypted; }
    std::wstring GetPassword() const { return m_password; }

private:
    std::wstring m_archivePath;
    std::wstring m_password;
    bool m_isOpen;
    bool m_isEncrypted;
    
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

bool IsNonFirstMultiPartVolume(const std::wstring& filePath);
