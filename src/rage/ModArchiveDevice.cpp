#include "rage/ModArchiveDevice.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rage/types/FileDeviceTypes.h"
#include "util/Strings.h"

namespace spl::rage
{
namespace
{
using namespace FileDeviceLayout;

constexpr std::size_t kSpareSlots = 16;
constexpr std::size_t kVtableSlots = kKnownSlotCount + kSpareSlots;

constexpr uint32_t kAttributeDirectory = 0x10; // FILE_ATTRIBUTE_DIRECTORY

/// What a file answers to GetFileAttributes: 0, as FiveM's VFS devices answer for any file they
/// can open (vfs-core/src/VFSDevice.cpp:92-106).
constexpr uint32_t kAttributeFile = 0;

constexpr uint32_t kSeekSet = 0; // FILE_BEGIN
constexpr uint32_t kSeekCurrent = 1;
constexpr uint32_t kSeekEnd = 2;

constexpr uint32_t kFailedCount = std::numeric_limits<uint32_t>::max();

[[nodiscard]] ModArchiveDevice& OwnerOf(void* self)
{
    return *static_cast<ModArchiveDeviceObject*>(self)->owner;
}

[[nodiscard]] std::string SourcePathOf(void* self, const char* gamePath)
{
    return gamePath == nullptr ? std::string{} : OwnerOf(self).ToSourcePath(gamePath);
}

/// A slot nobody is known to call: answers "nothing" and is recorded.
template <std::size_t Slot> uint64_t Unexpected(void* self, uint64_t, uint64_t, uint64_t, uint64_t)
{
    (void)OwnerOf(self).NoteUnexpectedSlot(Slot);
    return 0;
}

using UnexpectedFn = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t, uint64_t);

constexpr std::array<UnexpectedFn, kVtableSlots> kUnexpected =
    []<std::size_t... Slots>(std::index_sequence<Slots...>)
{
    return std::array<UnexpectedFn, kVtableSlots>{&Unexpected<Slots>...};
}(std::make_index_sequence<kVtableSlots>{});

// The game never deletes a mounted device, and the device's owner frees it.
void* Destructor(void* self, int /*flags*/)
{
    return self;
}

uint64_t OpenForReading(void* self, const char* fileName)
{
    ModArchiveDevice& owner = OwnerOf(self);
    const std::string path = SourcePathOf(self, fileName);
    const std::optional<DeviceFileInfo> info = owner.GetSource().FindFile(path);
    if (!info)
    {
        return kInvalidFileHandleRaw;
    }
    std::optional<DeviceFileBytes> bytes = owner.GetSource().OpenFile(path);
    if (!bytes)
    {
        return kInvalidFileHandleRaw;
    }
    return owner.AddFile(
        ModArchiveDevice::OpenFile{.bytes = std::move(*bytes), .lengthBytes = info->lengthBytes});
}

uint64_t Open(void* self, const char* fileName, bool readOnly)
{
    return readOnly ? OpenForReading(self, fileName) : kInvalidFileHandleRaw;
}

/// Bulk offsets are relative to the file: the device has no archive the offsets could be into.
uint64_t OpenBulk(void* self, const char* fileName, uint64_t* bulkOffset)
{
    const uint64_t handle = OpenForReading(self, fileName);
    if (handle != kInvalidFileHandleRaw && bulkOffset != nullptr)
    {
        *bulkOffset = 0;
    }
    return handle;
}

uint64_t OpenBulkWrap(void* self, const char* fileName, uint64_t* bulkOffset, void* /*extra*/)
{
    return OpenBulk(self, fileName, bulkOffset); // FiveM's fiCustomDevice does the same
}

uint64_t RefuseHandle(void* /*self*/, const char* /*fileName*/)
{
    return kInvalidFileHandleRaw;
}

/// Copies up to length bytes from offset into buffer.
[[nodiscard]] uint32_t CopyOut(const ModArchiveDevice::OpenFile& file, uint64_t offset,
                               void* buffer, uint32_t length)
{
    const uint64_t available = file.bytes.bytes.size();
    if (offset >= available || buffer == nullptr)
    {
        return 0;
    }
    const auto count = static_cast<uint32_t>(std::min<uint64_t>(length, available - offset));
    std::memcpy(buffer, file.bytes.bytes.data() + offset, count);
    return count;
}

uint32_t Read(void* self, uint64_t handle, void* buffer, uint32_t length)
{
    uint32_t count = kFailedCount;
    OwnerOf(self).WithFile(handle,
                           [&](ModArchiveDevice::OpenFile& file)
                           {
                               count = CopyOut(file, file.cursor, buffer, length);
                               file.cursor += count;
                           });
    return count;
}

uint32_t ReadBulk(void* self, uint64_t handle, uint64_t offset, void* buffer, uint32_t length)
{
    uint32_t count = kFailedCount;
    OwnerOf(self).WithFile(handle, [&](const ModArchiveDevice::OpenFile& file)
                           { count = CopyOut(file, offset, buffer, length); });
    return count;
}

bool ReadFull(void* self, uint64_t handle, void* buffer, uint32_t length)
{
    return Read(self, handle, buffer, length) == length;
}

uint32_t RefuseCount(void* /*self*/)
{
    return 0;
}

uint64_t SeekLong(void* self, uint64_t handle, int64_t distance, uint32_t method)
{
    uint64_t position = std::numeric_limits<uint64_t>::max();
    OwnerOf(self).WithFile(handle,
                           [&](ModArchiveDevice::OpenFile& file)
                           {
                               const auto size = static_cast<int64_t>(file.bytes.bytes.size());
                               int64_t base = 0;
                               switch (method)
                               {
                               case kSeekSet:
                                   break;
                               case kSeekCurrent:
                                   base = static_cast<int64_t>(file.cursor);
                                   break;
                               case kSeekEnd:
                                   base = size;
                                   break;
                               default:
                                   return;
                               }
                               file.cursor = static_cast<uint64_t>(
                                   std::clamp<int64_t>(base + distance, 0, size));
                               position = file.cursor;
                           });
    return position;
}

uint32_t Seek(void* self, uint64_t handle, int32_t distance, uint32_t method)
{
    return static_cast<uint32_t>(SeekLong(self, handle, distance, method));
}

int32_t Close(void* self, uint64_t handle)
{
    return OwnerOf(self).CloseFile(handle) ? 0 : -1;
}

template <std::size_t Slot> uint64_t LengthOfHandle(void* self, uint64_t handle)
{
    uint64_t length = 0;
    OwnerOf(self).WithFile(handle, [&](const ModArchiveDevice::OpenFile& file)
                           { length = file.lengthBytes; });
    return length;
}

uint64_t GetFileLengthLong(void* self, const char* fileName)
{
    const std::optional<DeviceFileInfo> info =
        OwnerOf(self).GetSource().FindFile(SourcePathOf(self, fileName));
    return info ? info->lengthBytes : 0;
}

uint64_t GetFileTime(void* self, const char* fileName)
{
    const std::optional<DeviceFileInfo> info =
        OwnerOf(self).GetSource().FindFile(SourcePathOf(self, fileName));
    return info ? info->fileTime : 0;
}

bool RefuseBool(void* /*self*/)
{
    return false;
}

void FillFindData(FindDataView& findData, const DeviceDirectoryEntry& entry)
{
    const std::size_t length = std::min(entry.name.size(), sizeof(findData.fileName) - 1);
    std::memcpy(findData.fileName, entry.name.data(), length);
    findData.fileName[length] = '\0';
    findData.fileSize = entry.lengthBytes;
    findData.writeTime = entry.fileTime;
    findData.attributes = entry.isDirectory ? kAttributeDirectory : kAttributeFile;
}

uint64_t FindFirst(void* self, const char* path, FindDataView* findData)
{
    ModArchiveDevice& owner = OwnerOf(self);
    const std::string sourcePath = SourcePathOf(self, path);
    std::vector<DeviceDirectoryEntry> entries = owner.GetSource().List(sourcePath);
    if (entries.empty())
    {
        // A file names itself, as a packfile's FindFirst on a file does (VFSRagePackfile7.cpp:562).
        const std::optional<DeviceFileInfo> info = owner.GetSource().FindFile(sourcePath);
        if (!info)
        {
            return kInvalidFileHandleRaw;
        }
        const std::size_t slash = sourcePath.find_last_of('/');
        entries.push_back(DeviceDirectoryEntry{
            .name = slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1),
            .lengthBytes = info->lengthBytes,
            .fileTime = info->fileTime});
    }
    if (findData != nullptr)
    {
        FillFindData(*findData, entries.front());
    }
    return owner.AddSearch(ModArchiveDevice::OpenSearch{.entries = std::move(entries), .next = 1});
}

bool FindNext(void* self, uint64_t handle, FindDataView* findData)
{
    bool found = false;
    OwnerOf(self).WithSearch(handle,
                             [&](ModArchiveDevice::OpenSearch& search)
                             {
                                 if (search.next >= search.entries.size())
                                 {
                                     return;
                                 }
                                 if (findData != nullptr)
                                 {
                                     FillFindData(*findData, search.entries[search.next]);
                                 }
                                 ++search.next;
                                 found = true;
                             });
    return found;
}

int32_t FindClose(void* self, uint64_t handle)
{
    return OwnerOf(self).CloseSearch(handle) ? 0 : -1;
}

/// Nothing to resolve: the name is already what this device answers to.
char* ResolvePath(void* /*self*/, char* buffer, int length, const char* path)
{
    if (buffer == nullptr || length <= 0)
    {
        return buffer;
    }
    const std::size_t count =
        path == nullptr ? 0 : std::min(std::strlen(path), static_cast<std::size_t>(length - 1));
    std::memcpy(buffer, path, count);
    buffer[count] = '\0';
    return buffer;
}

uint32_t GetFileAttributes(void* self, const char* path)
{
    const ModArchiveDevice& owner = OwnerOf(self);
    const std::string sourcePath = SourcePathOf(self, path);
    if (owner.GetSource().IsDirectory(sourcePath))
    {
        return kAttributeDirectory;
    }
    return owner.GetSource().FindFile(sourcePath) ? kAttributeFile : kInvalidFileAttributesRaw;
}

int32_t GetResourceVersion(void* self, const char* fileName, ResourceFlagsView* flags)
{
    const std::optional<DeviceFileInfo> info =
        OwnerOf(self).GetSource().FindFile(SourcePathOf(self, fileName));
    if (!info || !info->resource)
    {
        if (flags != nullptr)
        {
            *flags = ResourceFlagsView{};
        }
        return 0; // FiveM's adapter answers a file with no page flags the same
                  // (RageVFS.cpp:252-265)
    }
    if (flags != nullptr)
    {
        *flags = ResourceFlagsView{.virtualFlags = info->resource->virtualFlags,
                                   .physicalFlags = info->resource->physicalFlags};
    }
    return info->resource->version;
}

uint64_t AnswerTwo(void* /*self*/)
{
    return 2;
}

uint64_t AnswerUnknown40(void* /*self*/, void* /*argument*/)
{
    return 0x40000000;
}

void* GetCollection(void* self)
{
    return self;
}

uint64_t AnswerZero(void* /*self*/)
{
    return 0;
}

const char* GetName(void* /*self*/)
{
    return ModArchiveDevice::kName.data(); // a string literal, so it is terminated
}

[[nodiscard]] std::array<const void*, kVtableSlots> BuildVtable()
{
    std::array<const void*, kVtableSlots> vtable{};
    for (std::size_t slot = 0; slot < kVtableSlots; ++slot)
    {
        vtable[slot] = reinterpret_cast<const void*>(kUnexpected[slot]);
    }
    const auto set = [&vtable](std::size_t slot, auto function)
    { vtable[slot] = reinterpret_cast<const void*>(function); };

    set(kSlotDestructor, &Destructor);
    set(kSlotOpen, &Open);
    set(kSlotOpenBulk, &OpenBulk);
    set(kSlotOpenBulkWrap, &OpenBulkWrap);
    set(kSlotCreateLocal, &RefuseHandle);
    set(kSlotCreate, &RefuseHandle);
    set(kSlotRead, &Read);
    set(kSlotReadBulk, &ReadBulk);
    set(kSlotWriteBulk, &RefuseCount);
    set(kSlotWrite, &RefuseCount);
    set(kSlotSeek, &Seek);
    set(kSlotSeekLong, &SeekLong);
    set(kSlotClose, &Close);
    set(kSlotCloseBulk, &Close);
    set(kSlotGetFileLength, &LengthOfHandle<kSlotGetFileLength>);
    set(kSlotGetFileLengthUInt64, &LengthOfHandle<kSlotGetFileLengthUInt64>);
    set(kSlotRemoveFile, &RefuseBool);
    set(kSlotRenameFile, &RefuseBool);
    set(kSlotCreateDirectory, &RefuseBool);
    set(kSlotRemoveDirectory, &RefuseBool);
    set(kSlotGetFileLengthLong, &GetFileLengthLong);
    set(kSlotGetFileTime, &GetFileTime);
    set(kSlotSetFileTime, &RefuseBool);
    set(kSlotFindFirst, &FindFirst);
    set(kSlotFindNext, &FindNext);
    set(kSlotFindClose, &FindClose);
    set(kSlotResolvePath, &ResolvePath);
    set(kSlotTruncate, &RefuseBool);
    set(kSlotGetFileAttributes, &GetFileAttributes);
    set(kSlotSetFileAttributes, &RefuseBool);
    set(kSlotUnknown34, &AnswerTwo);
    set(kSlotReadFull, &ReadFull);
    set(kSlotWriteFull, &RefuseBool);
    set(kSlotGetResourceVersion, &GetResourceVersion);
    set(kSlotUnknown40, &AnswerUnknown40);
    set(kSlotIsCollection, &RefuseBool);
    set(kSlotGetCollection, &GetCollection);
    set(kSlotGetCollectionId, &AnswerZero);
    set(kSlotGetName, &GetName);
    return vtable;
}

const std::array<const void*, kVtableSlots> g_modArchiveDeviceVtable = BuildVtable();

/// "\\a//b/" -> "a/b", lower-cased.
[[nodiscard]] std::string NormalizeKey(std::string_view path)
{
    std::string normalized;
    normalized.reserve(path.size());
    for (const char c : path)
    {
        const char character = c == '\\' ? '/' : c;
        if (character == '/' && (normalized.empty() || normalized.back() == '/'))
        {
            continue;
        }
        normalized.push_back(character);
    }
    if (!normalized.empty() && normalized.back() == '/')
    {
        normalized.pop_back();
    }
    return util::ToLower(normalized);
}
} // namespace

ModArchiveDevice::ModArchiveDevice(const IDeviceFileSource& source, std::string folder)
    : m_object{.vtable = g_modArchiveDeviceVtable.data(), .owner = this}, m_source(source),
      m_folder(NormalizeKey(folder))
{
}

std::string ModArchiveDevice::ToSourcePath(std::string_view gamePath) const
{
    const std::size_t mount = gamePath.find(":/");
    const std::string relative =
        NormalizeKey(mount == std::string_view::npos ? gamePath : gamePath.substr(mount + 2));
    if (m_folder.empty() || relative.empty())
    {
        return m_folder.empty() ? relative : m_folder;
    }
    return m_folder + "/" + relative;
}

uint64_t ModArchiveDevice::AddFile(OpenFile file)
{
    const std::lock_guard lock{m_mutex};
    const uint64_t handle = m_nextHandle++;
    m_files.emplace(handle, std::move(file));
    return handle;
}

bool ModArchiveDevice::CloseFile(uint64_t handle)
{
    const std::lock_guard lock{m_mutex};
    return m_files.erase(handle) != 0;
}

uint64_t ModArchiveDevice::AddSearch(OpenSearch search)
{
    const std::lock_guard lock{m_mutex};
    const uint64_t handle = m_nextHandle++;
    m_searches.emplace(handle, std::move(search));
    return handle;
}

bool ModArchiveDevice::CloseSearch(uint64_t handle)
{
    const std::lock_guard lock{m_mutex};
    return m_searches.erase(handle) != 0;
}

std::size_t ModArchiveDevice::CountOpenHandles() const
{
    const std::lock_guard lock{m_mutex};
    return m_files.size() + m_searches.size();
}

bool ModArchiveDevice::NoteUnexpectedSlot(std::size_t slot)
{
    const std::lock_guard lock{m_mutex};
    if (std::ranges::find(m_unexpectedSlots, slot) != m_unexpectedSlots.end())
    {
        return false;
    }
    m_unexpectedSlots.push_back(slot);
    return true;
}

std::vector<std::size_t> ModArchiveDevice::GetUnexpectedSlots() const
{
    const std::lock_guard lock{m_mutex};
    return m_unexpectedSlots;
}
} // namespace spl::rage
