#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <miniz.h>
#include <string>
#include <string_view>
#include <vector>

namespace spl::tests
{
/// Builds a minimal valid RPF7 archive in memory: stored or deflated files, directories
/// implied by paths, children in byte order. Keeps binary fixtures out of the repository
/// the way StreamTree::RscHeaderBytes does for RSC headers.
class RpfBuilder
{
public:
    /// A stored file at a forward-slash path, e.g. "content/car.ytd".
    void AddFile(std::string_view path, std::string_view contents)
    {
        m_files.emplace_back(File{std::string{path}, std::string{contents}, false});
    }

    /// A raw-deflated file, like the real archives hold.
    void AddCompressedFile(std::string_view path, std::string_view contents)
    {
        m_files.emplace_back(File{std::string{path}, std::string{contents}, true});
    }

    [[nodiscard]] std::string Build() const
    {
        struct Node
        {
            bool isDirectory = true;
            std::string contents;
            bool compressed = false;
            std::map<std::string, Node> children; // byte order, like the on-disk table
        };
        Node root;
        for (const File& file : m_files)
        {
            Node* node = &root;
            std::size_t begin = 0;
            while (true)
            {
                const std::size_t slash = file.path.find('/', begin);
                const std::string part = (slash == std::string::npos)
                                             ? file.path.substr(begin)
                                             : file.path.substr(begin, slash - begin);
                Node& child = node->children[part];
                if (slash == std::string::npos)
                {
                    child.isDirectory = false;
                    child.contents = file.contents;
                    child.compressed = file.compressed;
                    break;
                }
                node = &child;
                begin = slash + 1;
            }
        }

        // Index order: the root, then each directory's children as one contiguous block.
        // Directories are processed in increasing index order, so blocks never overlap.
        struct Placed
        {
            const Node* node = nullptr;
            std::string name;
        };
        std::vector<Placed> placed{{&root, ""}};
        std::vector<uint32_t> firstChild(1, 0);
        for (std::size_t dir = 0; dir < placed.size(); ++dir)
        {
            if (!placed[dir].node->isDirectory)
            {
                continue;
            }
            firstChild[dir] = static_cast<uint32_t>(placed.size());
            for (const auto& [name, child] : placed[dir].node->children)
            {
                placed.push_back(Placed{&child, name});
                firstChild.push_back(0);
            }
        }

        std::string names{"\0", 1};
        std::vector<uint32_t> nameOffsets{0};
        for (std::size_t index = 1; index < placed.size(); ++index)
        {
            nameOffsets.push_back(static_cast<uint32_t>(names.size()));
            names += placed[index].name;
            names.push_back('\0');
        }

        constexpr uint32_t kSectorBytes = 512;
        const uint32_t tableEnd = static_cast<uint32_t>(16 + placed.size() * 16 + names.size());
        const uint32_t dataStart = (tableEnd + kSectorBytes - 1) / kSectorBytes * kSectorBytes;

        const auto appendU32 = [](std::string& out, uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                out.push_back(static_cast<char>((value >> shift) & 0xFF));
            }
        };

        std::string out;
        appendU32(out, 0x52504637); // "RPF7"
        appendU32(out, static_cast<uint32_t>(placed.size()));
        appendU32(out, static_cast<uint32_t>(names.size()));
        appendU32(out, 0x4E45504F); // "OPEN"
        std::string data;
        for (std::size_t index = 0; index < placed.size(); ++index)
        {
            const Placed& entry = placed[index];
            if (entry.node->isDirectory)
            {
                const uint64_t packed =
                    static_cast<uint64_t>(nameOffsets[index]) | (0x7FFFFFull << 40);
                for (int shift = 0; shift < 64; shift += 8)
                {
                    out.push_back(static_cast<char>((packed >> shift) & 0xFF));
                }
                appendU32(out, firstChild[index]);
                appendU32(out, static_cast<uint32_t>(entry.node->children.size()));
                continue;
            }
            const std::string payload =
                entry.node->compressed ? Deflate(entry.node->contents) : entry.node->contents;
            const uint32_t sector = static_cast<uint32_t>((dataStart + data.size()) / kSectorBytes);
            // OpenIV shape, verified against real packages: stored files carry their
            // on-disk size with the stored bit set; deflated files carry the compressed
            // size with it clear and the decompressed size in virtFlags.
            uint64_t packed = static_cast<uint64_t>(nameOffsets[index]) |
                              (static_cast<uint64_t>(payload.size()) << 16) |
                              (static_cast<uint64_t>(sector) << 40);
            if (!entry.node->compressed)
            {
                packed |= (0x80ULL << 56); // bit 23 of the offset field: stored
            }
            for (int shift = 0; shift < 64; shift += 8)
            {
                out.push_back(static_cast<char>((packed >> shift) & 0xFF));
            }
            appendU32(out, static_cast<uint32_t>(entry.node->contents.size()));
            appendU32(out, 0);
            data += payload;
            data.append((kSectorBytes - data.size() % kSectorBytes) % kSectorBytes, '\0');
        }

        out += names;
        out.append(dataStart - static_cast<uint32_t>(out.size()), '\0');
        out += data;
        return out;
    }

private:
    struct File
    {
        std::string path;
        std::string contents;
        bool compressed = false;
    };

    /// Raw deflate, matching what the reader inflates with -15.
    [[nodiscard]] static std::string Deflate(const std::string& contents)
    {
        std::string out(contents.size() + 1024, '\0');
        const size_t written =
            tdefl_compress_mem_to_mem(out.data(), out.size(), contents.data(), contents.size(), 0);
        out.resize(written);
        return out;
    }

    std::vector<File> m_files;
};
} // namespace spl::tests
