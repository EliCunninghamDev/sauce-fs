// Windows mount backend built on WinFsp's user-mode file system API.
// NOTE: this file is compiled only on Windows (see CMakeLists.txt).

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "backends/backend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <windows.h>
#include <winfsp/winfsp.h>

#include "core/volume.h"

namespace sauce::fs
{
namespace
{

constexpr const wchar_t* kVolumeLabel = L"sauce-fs";
constexpr UINT32 kVolumeSerial = 0x5A0CE;

FSP_FILE_SYSTEM* g_file_system = nullptr;
volatile LONG g_stopped = 0;

UINT64 file_time_now()
{
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart;
}

std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

// Self-relative descriptor with a NULL DACL: everyone may read the volume.
const std::vector<unsigned char>& security_descriptor()
{
    static const std::vector<unsigned char> descriptor = []
    {
        SECURITY_DESCRIPTOR absolute{};
        if (!InitializeSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&absolute, TRUE, nullptr, FALSE))
        {
            return std::vector<unsigned char>{};
        }

        DWORD size = 0;
        MakeSelfRelativeSD(&absolute, nullptr, &size);
        if (size == 0)
        {
            return std::vector<unsigned char>{};
        }

        std::vector<unsigned char> relative(size);
        if (!MakeSelfRelativeSD(&absolute, relative.data(), &size))
        {
            return std::vector<unsigned char>{};
        }
        relative.resize(size);
        return relative;
    }();
    return descriptor;
}

struct FileContext
{
    bool directory = false;
    std::string data;
};

void fill_file_info(const FileContext& file, FSP_FSCTL_FILE_INFO* info)
{
    const UINT64 now = file_time_now();
    std::memset(info, 0, sizeof *info);
    info->FileAttributes = file.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    info->FileSize = file.data.size();
    info->AllocationSize = file.data.size();
    info->CreationTime = now;
    info->LastAccessTime = now;
    info->LastWriteTime = now;
    info->ChangeTime = now;
    info->IndexNumber = file.directory ? 1 : 2;
}

bool is_root(const wchar_t* file_name)
{
    return file_name != nullptr && file_name[0] == L'\\' && file_name[1] == L'\0';
}

std::wstring name_after_root(const wchar_t* file_name)
{
    std::wstring name = file_name != nullptr ? file_name : L"";
    if (!name.empty() && name.front() == L'\\')
    {
        name.erase(name.begin());
    }
    return name;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }

    std::string narrow_text(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow_text.data(),
                        length, nullptr, nullptr);
    return narrow_text;
}

NTSTATUS get_volume_info(FSP_FILE_SYSTEM*, FSP_FSCTL_VOLUME_INFO* volume_info)
{
    std::memset(volume_info, 0, sizeof *volume_info);
    volume_info->TotalSize = 16ull << 20;
    volume_info->FreeSize = 8ull << 20;
    volume_info->VolumeLabelLength = static_cast<UINT16>(wcslen(kVolumeLabel) * sizeof(WCHAR));
    std::memcpy(volume_info->VolumeLabel, kVolumeLabel, volume_info->VolumeLabelLength);
    return STATUS_SUCCESS;
}

NTSTATUS get_security_by_name(FSP_FILE_SYSTEM*, PWSTR file_name, PUINT32 file_attributes,
                              PSECURITY_DESCRIPTOR descriptor, SIZE_T* descriptor_size)
{
    const bool root = is_root(file_name);
    if (!root)
    {
        const std::vector<Entry> entries = snapshot();
        if (find_entry(entries, narrow(name_after_root(file_name))) == nullptr)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    if (file_attributes != nullptr)
    {
        *file_attributes = root ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    }

    if (descriptor_size != nullptr)
    {
        const std::vector<unsigned char>& security = security_descriptor();
        if (security.size() > *descriptor_size)
        {
            *descriptor_size = security.size();
            return STATUS_BUFFER_OVERFLOW;
        }
        *descriptor_size = security.size();
        if (descriptor != nullptr && !security.empty())
        {
            std::memcpy(descriptor, security.data(), security.size());
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS open_file(FSP_FILE_SYSTEM*, PWSTR file_name, UINT32, UINT32 granted_access,
                   PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info)
{
    constexpr UINT32 write_access = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                                    FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;
    if ((granted_access & write_access) != 0)
    {
        return STATUS_ACCESS_DENIED;
    }

    auto context = std::make_unique<FileContext>();
    if (is_root(file_name))
    {
        context->directory = true;
    }
    else
    {
        const std::wstring name = name_after_root(file_name);
        const std::vector<Entry> entries = snapshot();
        bool found = false;
        for (const Entry& entry : entries)
        {
            if (widen(entry.name) == name)
            {
                context->data = entry.data;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    fill_file_info(*context, file_info);
    *file_context = context.release();
    return STATUS_SUCCESS;
}

NTSTATUS read_file(FSP_FILE_SYSTEM*, PVOID file_context, PVOID buffer, UINT64 offset, ULONG length,
                   PULONG bytes_transferred)
{
    const auto* context = static_cast<const FileContext*>(file_context);
    *bytes_transferred = 0;
    if (context->directory)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (offset >= context->data.size())
    {
        return STATUS_SUCCESS;
    }

    const ULONG available = static_cast<ULONG>(context->data.size() - offset);
    const ULONG count = std::min(length, available);
    std::memcpy(buffer, context->data.data() + offset, count);
    *bytes_transferred = count;
    return STATUS_SUCCESS;
}

NTSTATUS read_directory(FSP_FILE_SYSTEM*, PVOID file_context, PWSTR, PWSTR marker, PVOID buffer,
                        ULONG length, PULONG bytes_transferred)
{
    const auto* context = static_cast<const FileContext*>(file_context);
    if (!context->directory)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const std::vector<Entry> entries = snapshot();
    bool past_marker = marker == nullptr;
    for (const Entry& entry : entries)
    {
        const std::wstring name = widen(entry.name);
        if (!past_marker)
        {
            if (name == marker)
            {
                past_marker = true;
            }
            continue;
        }

        const size_t name_bytes = name.size() * sizeof(WCHAR);
        std::vector<unsigned char> storage(sizeof(FSP_FSCTL_DIR_INFO) + name_bytes, 0);
        auto* dir_info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(storage.data());
        dir_info->Size = static_cast<UINT16>(storage.size());
        dir_info->FileInfo.FileAttributes = FILE_ATTRIBUTE_READONLY;
        dir_info->FileInfo.FileSize = entry.data.size();
        dir_info->FileInfo.AllocationSize = entry.data.size();
        std::memcpy(dir_info->FileNameBuf, name.data(), name_bytes);

        if (!FspFileSystemAddDirInfo(dir_info, buffer, length, bytes_transferred))
        {
            return STATUS_SUCCESS;
        }
    }

    // NULL marks the end of the directory; without it WinFsp expects more entries.
    FspFileSystemAddDirInfo(nullptr, buffer, length, bytes_transferred);
    return STATUS_SUCCESS;
}

NTSTATUS get_file_info(FSP_FILE_SYSTEM*, PVOID file_context, FSP_FSCTL_FILE_INFO* file_info)
{
    fill_file_info(*static_cast<const FileContext*>(file_context), file_info);
    return STATUS_SUCCESS;
}

VOID close_file(FSP_FILE_SYSTEM*, PVOID file_context)
{
    delete static_cast<FileContext*>(file_context);
}

VOID cleanup_file(FSP_FILE_SYSTEM*, PVOID, PWSTR, ULONG) {}

FSP_FILE_SYSTEM_INTERFACE file_system_interface()
{
    FSP_FILE_SYSTEM_INTERFACE ops{};
    ops.GetVolumeInfo = get_volume_info;
    ops.GetSecurityByName = get_security_by_name;
    ops.Open = open_file;
    ops.Read = read_file;
    ops.ReadDirectory = read_directory;
    ops.GetFileInfo = get_file_info;
    ops.Cleanup = cleanup_file;
    ops.Close = close_file;
    return ops;
}

BOOL WINAPI console_handler(DWORD)
{
    InterlockedExchange(&g_stopped, 1);
    return TRUE;
}

} // namespace

int mount(int argc, char** argv)
{
    if (argc < 1)
    {
        std::fprintf(stderr, "sauce-fs: mount point required (drive letter or directory)\n");
        return 2;
    }

    FSP_FSCTL_VOLUME_PARAMS parameters{};
    parameters.SectorSize = 512;
    parameters.SectorsPerAllocationUnit = 1;
    parameters.MaxComponentLength = 255;
    parameters.VolumeCreationTime = file_time_now();
    parameters.VolumeSerialNumber = kVolumeSerial;
    parameters.FileInfoTimeout = 1000;
    parameters.CaseSensitiveSearch = 1;
    parameters.CasePreservedNames = 1;
    parameters.UnicodeOnDisk = 1;
    parameters.PersistentAcls = 1;
    parameters.ReadOnlyVolume = 1;

    const FSP_FILE_SYSTEM_INTERFACE ops = file_system_interface();
    NTSTATUS status = FspFileSystemCreate(const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME),
                                          &parameters, &ops, &g_file_system);
    if (status != STATUS_SUCCESS)
    {
        std::fprintf(stderr, "sauce-fs: FspFileSystemCreate failed (0x%08lx)\n",
                     static_cast<unsigned long>(status));
        return 1;
    }

    std::wstring mount_point = widen(argv[0]);
    status = FspFileSystemSetMountPoint(g_file_system, mount_point.data());
    if (status != STATUS_SUCCESS)
    {
        std::fprintf(stderr, "sauce-fs: cannot mount at %s (0x%08lx)\n", argv[0],
                     static_cast<unsigned long>(status));
        FspFileSystemDelete(g_file_system);
        return 1;
    }

    status = FspFileSystemStartDispatcher(g_file_system, 0);
    if (status != STATUS_SUCCESS)
    {
        std::fprintf(stderr, "sauce-fs: dispatcher failed (0x%08lx)\n",
                     static_cast<unsigned long>(status));
        FspFileSystemDelete(g_file_system);
        return 1;
    }

    SetConsoleCtrlHandler(console_handler, TRUE);
    std::printf("sauce-fs mounted at %s (Ctrl+C to unmount)\n", argv[0]);

    while (InterlockedCompareExchange(&g_stopped, 0, 0) == 0)
    {
        Sleep(200);
    }

    FspFileSystemStopDispatcher(g_file_system);
    FspFileSystemDelete(g_file_system);
    g_file_system = nullptr;
    return 0;
}

Check backend_check()
{
    UINT32 version = 0;
    const NTSTATUS status = FspVersion(&version);
    const std::string detail =
        "winfsp " + std::to_string(version >> 16) + "." + std::to_string(version & 0xFFFF);
    return {"winfsp", status == STATUS_SUCCESS, detail};
}

} // namespace sauce::fs
