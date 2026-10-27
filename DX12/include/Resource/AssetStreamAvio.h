#pragma once

// Resource/AssetStreamAvio.h - an FFmpeg AVIOContext over an AssetFS stream, so a demuxer opens a
// video sealed in game.ipak as it opens a loose file. For the FFmpeg modules (Video, Media): include
// it after <libavformat/avformat.h>. The core never includes it.
//
//   m_fmt = avformat_alloc_context();
//   m_io.Open(path);                       // false: no source has the path
//   m_fmt->pb = m_io.Context();
//   m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
//   avformat_open_input(&m_fmt, path.c_str(), nullptr, nullptr);
//   ...
//   avformat_close_input(&m_fmt);          // does not free a custom context:
//   m_io.Close();                          // this does, after it

#include "Resource/AssetFS.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace Resource
{
    class AssetStreamAvio
    {
    public:
        AssetStreamAvio() = default;
        AssetStreamAvio(const AssetStreamAvio&) = delete;
        AssetStreamAvio& operator=(const AssetStreamAvio&) = delete;
        ~AssetStreamAvio() { Close(); }

        // Opens `path` through AssetFS::OpenStream and a context over it. False when no source has it.
        bool Open(const std::string& path)
        {
            Close();
            m_stream = AssetFS::Get().OpenStream(path);
            if (!m_stream) return false;
            auto* buffer = static_cast<unsigned char*>(av_malloc(kBufferBytes));
            if (buffer)
                m_context = avio_alloc_context(buffer, kBufferBytes, 0, m_stream.get(), &ReadPacket, nullptr, &SeekTo);
            if (!m_context)
            {
                av_free(buffer);
                m_stream.reset();
                return false;
            }
            return true;
        }

        AVIOContext* Context() const { return m_context; }

        void Close()
        {
            if (m_context)
            {
                av_freep(&m_context->buffer);   // FFmpeg may have replaced the buffer it was given
                avio_context_free(&m_context);
            }
            m_stream.reset();
        }

    private:
        static constexpr int kBufferBytes = 64 * 1024;

        static int ReadPacket(void* opaque, std::uint8_t* buf, int size)
        {
            const std::size_t n = static_cast<AssetFS::Stream*>(opaque)->Read(buf, static_cast<std::size_t>(size));
            return n > 0 ? static_cast<int>(n) : AVERROR_EOF;
        }

        static std::int64_t SeekTo(void* opaque, std::int64_t offset, int whence)
        {
            auto* stream = static_cast<AssetFS::Stream*>(opaque);
            whence &= ~AVSEEK_FORCE;
            if (whence == AVSEEK_SIZE) return static_cast<std::int64_t>(stream->Size());
            std::int64_t base = 0;
            if (whence == SEEK_CUR)
                base = static_cast<std::int64_t>(stream->Position());
            else if (whence == SEEK_END)
                base = static_cast<std::int64_t>(stream->Size());
            else if (whence != SEEK_SET)
                return AVERROR(EINVAL);
            const std::int64_t to = base + offset;
            if (to < 0 || !stream->Seek(static_cast<std::uint64_t>(to))) return AVERROR(EINVAL);
            return to;
        }

        std::unique_ptr<AssetFS::Stream> m_stream;
        AVIOContext*                     m_context = nullptr;
    };
}
