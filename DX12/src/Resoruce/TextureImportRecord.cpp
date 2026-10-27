// TextureImportRecord.cpp - the .meta import.* record; the contract is in the header.
#include "Resource/TextureImportRecord.h"

#include "Resource/AssetGuid.h"
#include "Resource/TextureInfo.h"
#include "System/PathUtil.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace Resource
{
namespace
{
    constexpr const char* kPrefix = "import.";

    std::string Trim(std::string s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    }

    // The sidecar's lines, CR stripped; empty when there is no sidecar.
    std::vector<std::string> ReadLines(const std::string& metaPath, bool& exists)
    {
        std::vector<std::string> lines;
        std::string text;
        exists = Sys::ReadFileText(metaPath, text);
        if (!exists) return lines;
        std::size_t pos = 0;
        while (pos < text.size())
        {
            const std::size_t eol = text.find('\n', pos);
            std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        return lines;
    }

    bool IsImportLine(const std::string& line)
    {
        const std::string t = Trim(line);
        return t.rfind(kPrefix, 0) == 0;
    }

    // The preset a .itex's DXGI format names - TextureInfo's `preset`.
    bool FormatFromPreset(const std::string& preset, TextureImportFormat& out)
    {
        return !preset.empty() && TextureImporter::ParseFormatId(preset, out);
    }
}

std::string TextureImportRecordText(const TextureImportRecord& rec)
{
    const TextureImportSettings& s = rec.settings;
    std::string t;
    t += std::string(kPrefix) + "source    = " + rec.source + "\n";
    t += std::string(kPrefix) + "format    = " + TextureImporter::GetFormatId(s.format) + "\n";
    t += std::string(kPrefix) + "maxSize   = " + std::to_string(s.maxSize) + "\n";
    t += std::string(kPrefix) + "mipFilter = " + TextureImporter::GetMipFilterId(s.mipFilter) + "\n";
    t += std::string(kPrefix) + "flipGreen = " + (s.flipGreen ? "1" : "0") + "\n";
    t += std::string(kPrefix) + "mips      = " + (s.generateMips ? "1" : "0") + "\n";
    return t;
}

bool ReadTextureImportRecord(const std::string& itexPath, TextureImportRecord& out)
{
    bool exists = false;
    const std::vector<std::string> lines = ReadLines(MetaPathFor(itexPath), exists);
    TextureImportRecord rec;
    bool any = false, format = false;
    for (const std::string& line : lines)
    {
        if (!IsImportLine(line)) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq)).substr(std::char_traits<char>::length(kPrefix));
        const std::string val = Trim(line.substr(eq + 1));
        any = true;
        if (key == "source")
            rec.source = val;
        else if (key == "format")
            format = TextureImporter::ParseFormatId(val, rec.settings.format);
        else if (key == "maxSize")
            rec.settings.maxSize = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "mipFilter")
            TextureImporter::ParseMipFilterId(val, rec.settings.mipFilter);
        else if (key == "flipGreen")
            rec.settings.flipGreen = val == "1" || val == "true";
        else if (key == "mips")
            rec.settings.generateMips = !(val == "0" || val == "false");
    }
    if (!any) return false;
    rec.recorded = true;
    rec.known    = format;
    out = std::move(rec);
    return true;
}

bool WriteTextureImportRecord(const std::string& itexPath, const TextureImportRecord& rec, std::string* why)
{
    const std::string metaPath = MetaPathFor(itexPath);
    bool exists = false;
    const std::vector<std::string> lines = ReadLines(metaPath, exists);
    std::string text;
    for (const std::string& line : lines)
        if (!IsImportLine(line)) text += line + "\n";
    text += TextureImportRecordText(rec);
    if (!Sys::WriteFileBytesAtomic(metaPath, text.data(), text.size()))
    {
        if (why) *why = "could not write '" + metaPath + "'";
        return false;
    }
    return true;
}

std::string FindTextureImportSource(const std::string& itexPath)
{
    std::error_code ec;
    const std::filesystem::path p = Sys::U8Path(itexPath);
    // Mirrors TextureImporter::GetSourceExtensions (the importer itself is in EditorCore).
    for (const char* ext : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" })
    {
        std::filesystem::path q = p;
        q.replace_extension(ext);
        if (std::filesystem::is_regular_file(q, ec))
        {
            std::string s = Sys::PathToUtf8(q);
            for (char& c : s) if (c == '\\') c = '/';
            return s;
        }
    }
    return {};
}

TextureImportRecord TextureImportRecordFor(const std::string& itexPath)
{
    TextureImportRecord rec;
    if (ReadTextureImportRecord(itexPath, rec)) return rec;
    // No record: the format is the preset the file's DXGI format names; the source is a same-named
    // image beside it.
    const TextureFileInfo info = DescribeTextureFile(itexPath);
    rec.known  = info.ok && FormatFromPreset(info.preset, rec.settings.format);
    rec.source = FindTextureImportSource(itexPath);
    // A one-mip file was not given a chain (TextureImporter always builds one).
    if (info.ok && info.mipLevels == 1 && (info.width > 1 || info.height > 1)) rec.settings.generateMips = false;
    return rec;
}
}
