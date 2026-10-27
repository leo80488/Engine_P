#pragma once
#include <vector>
#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
namespace Allocator
{
	template<typename T, size_t blockSize = 256 >
	struct BlockAllocator
	{
		struct Block
		{
			struct alignas(alignof(T)) RawData
			{
				uint8_t data[sizeof(T)];
			};
			std::vector<RawData> memory;
		};
		std::vector<Block> blocks;
		std::vector<T*> freeList;

		template<typename... Args>
		inline T* Allocate(Args&&... args)
		{
			if (freeList.empty())
			{
				freeList.reserve(blockSize);
				Block& newBlock = blocks.emplace_back();
				newBlock.memory.resize(blockSize);
				T* ptr = reinterpret_cast<T*>(newBlock.memory.data());
				for (size_t i = 0; i < blockSize; ++i)
				{
					freeList.push_back(ptr + i);
				}
			}
			T* obj = freeList.back();
			freeList.pop_back();
			return new (obj) T(std::forward<Args>(args)...);
		}
		inline void Free(T* obj)
		{
			obj->~T();
			freeList.push_back(obj);
		}
		inline bool IsEmpty() const
		{
			return freeList.size() == blocks.size() * blockSize;
		}
	};

	struct LinearAllocator
	{
		uint8_t* data = nullptr;
		size_t capacity = 0;
		size_t offset = 0;

		constexpr void init(void* mem, size_t size)
		{
			data = (uint8_t*)mem;
			capacity = size;
			reset();
		}
		constexpr uint8_t* allocate(size_t size)
		{
			if (offset + size >= capacity)
				return nullptr;
			uint8_t* ptr = data + offset;
			offset += size;
			return ptr;
		}
		constexpr void free(size_t size)
		{
			size = (std::min)(size, offset);
			offset -= size;
		}
		constexpr void reset()
		{
			offset = 0;
		}

	};
}