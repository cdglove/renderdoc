/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2014 Crytek
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/


#pragma once

#include "common/common.h"
#include "os/os_specific.h"
#include "api/replay/basic_types.h"

#include "replay/type_helpers.h"

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>
#include <list>
#include <utility>
#include <set>
using std::set;
using std::string;

// template helpers
template <class T>
struct renderdoc_is_pointer
{
  enum {value = false};
};

template <class T>
struct renderdoc_is_pointer<T *>
{
  enum {value = true};
};

template <class T>
struct renderdoc_is_pointer<const T *>
{
  enum {value = true};
};

template<bool isptr, class T>
struct ToStrHelper
{
	static string Get(const T &el);
};

struct ToStr
{
	template<class T>
	static string Get(const T &el)
	{
		return ToStrHelper<renderdoc_is_pointer<T>::value, T>::Get(el);
	}
};

typedef const char *(*ChunkLookup)(uint32_t chunkType);

class Serialiser;
class ScopedContext;
struct CompressedFileIO;

// holds the memory, length and type for a given chunk, so that it can be
// passed around and moved between owners before being serialised out
class Chunk
{
	public:
		~Chunk();
		
		const char *GetDebugString() { return m_DebugStr.c_str(); }
		byte *GetData() { return m_Data; }
		uint32_t GetLength() { return m_Length; }
		uint32_t GetChunkType() { return m_ChunkType; }

		bool IsAligned() { return m_AlignedData; }
		bool IsTemporary() { return m_Temporary; }
		
#if !defined(RELEASE)
		static uint64_t NumLiveChunks() { return m_LiveChunks; }
		static uint64_t TotalMem() { return m_TotalMem; }
#else
		static uint64_t NumLiveChunks() { return 0; }
		static uint64_t TotalMem() { return 0; }
#endif
		
		// grab current contents of the serialiser into this chunk
		Chunk(Serialiser *ser, uint32_t chunkType, bool temp); 

	private:
		// no copy semantics
		Chunk(const Chunk &);
		Chunk &operator =(const Chunk &);

		friend class ScopedContext;

		bool m_AlignedData;
		bool m_Temporary;

		uint32_t m_ChunkType;

		uint32_t m_Length;
		byte *m_Data;
		string m_DebugStr;
		
#if !defined(RELEASE)
		static int64_t m_LiveChunks, m_MaxChunks, m_TotalMem;
#endif
};

// this class has a few functions. It can be used to serialise chunks - on writing it enforces
// that we only ever write a single chunk, then pull out the data into a Chunk class and erase
// the contents of the serialiser ready to serialise the next (see the RDCASSERT at the start
// of PushContext).
//
// We use this functionality for sending and receiving data across the network as well as saving
// out to the capture logfile format.
//
// It's also used on reading where it will contain the stream of chunks that were written out
// to the logfile on capture.
//
// When reading, the Serialiser allocates a window of memory and scans through the file by reading
// data into that window and moving along through the file. The window will expand to accomodate
// whichever is the biggest single element within a chunk that's read (so that you can always guarantee
// while reading that the element you're interested in is always in memory).
class Serialiser
{
	public:
		enum Mode
		{
			NONE = 0,
			READING,
			WRITING,
		};

		enum SerialiserError
		{
			eSerError_None = 0,
			eSerError_FileIO,
			eSerError_Corrupt,
			eSerError_UnsupportedVersion,
		};

		enum SectionFlags
		{
			eSectionFlag_None          = 0x0,
			eSectionFlag_ASCIIStored   = 0x1,
			eSectionFlag_LZ4Compressed = 0x2,
		};

		enum SectionType
		{
			eSectionType_Unknown = 0,
			eSectionType_FrameCapture,      // renderdoc/internal/framecapture
			eSectionType_ResolveDatabase,   // renderdoc/internal/resolvedb
			eSectionType_FrameBookmarks,    // renderdoc/ui/bookmarks
			eSectionType_Notes,             // renderdoc/ui/notes
			eSectionType_Num,
		};

		// version number of overall file format or chunk organisation. If the contents/meaning/order of
		// chunks have changed this does not need to be bumped, there are version numbers within each
		// API that interprets the stream that can be bumped.
		static const uint64_t SERIALISE_VERSION = 0x00000032;
		static const uint32_t MAGIC_HEADER;

		//////////////////////////////////////////
		// Init and error handling

		Serialiser(size_t length, const byte *memoryBuf, bool fileheader);
		Serialiser(const char *path, Mode mode, bool debugMode = false);
		~Serialiser();

		bool HasError() { return m_HasError; }
		SerialiserError ErrorCode() { return m_ErrorCode; }

		//////////////////////////////////////////
		// Utility functions

		bool AtEnd()
		{
			return GetOffset() >= m_BufferSize;
		}

		bool HasAlignedData()
		{
			return m_AlignedData;
		}

		bool IsReading() const
		{
			return m_Mode == READING;
		}

		bool IsWriting() const
		{
			return !IsReading();
		}

		uint64_t GetOffset() const
		{
			if(m_HasError)
			{
				RDCERR("Getting offset with error state serialiser");
				return 0;
			}

			RDCASSERT(m_BufferHead && m_Buffer && m_BufferHead >= m_Buffer);
			return m_BufferHead - m_Buffer + m_ReadOffset;
		}

		uint64_t GetSize()
		{
			if(m_Mode == READING)
				return m_BufferSize;
			
			return m_BufferHead - m_Buffer;
		}

		byte *GetRawPtr(size_t offs) const
		{
			return m_Buffer+offs;
		}

		// Set up the base pointer and size. Serialiser will allocate enough for
		// the rest of the file and keep it all in memory (useful to keep everything
		// in actual frame data resident in memory).
		void SetPersistentBlock(uint64_t offs);

		void SetOffset(uint64_t offs);

		void Rewind()
		{
			m_DebugText = "";
			m_Indent = 0;
			m_AlignedData = false;
			SetOffset(0);
		}

		// assumes buffer head is sitting before a chunk (ie. pushcontext will be valid)
		void SkipToChunk(uint32_t chunkIdx, uint32_t *idx = NULL)
		{
			do
			{
				size_t offs = m_BufferHead-m_Buffer + (size_t)m_ReadOffset;

				uint32_t c = PushContext(NULL, NULL, 1, false);

				// found
				if(c == chunkIdx)
				{
					m_Indent--;
					m_BufferHead = (m_Buffer+offs)-(size_t)m_ReadOffset;
					return;
				}
				else
				{
					SkipCurrentChunk();
					PopContext(1);
				}

				if(idx) (*idx)++;

			} while(!AtEnd());
		}
		
		// assumes buffer head is sitting in a chunk (ie. immediately after a pushcontext)
		void SkipCurrentChunk()
		{
			ReadBytes(m_LastChunkLen);
		}

		void InitCallstackResolver();
		bool HasCallstacks() { return m_KnownSections[eSectionType_ResolveDatabase] != NULL; }

		// get callstack resolver, created with the DB in the file
		Callstack::StackResolver *GetCallstackResolver()
		{
			return m_pResolver;
		}

		void SetCallstack(uint64_t *levels, size_t numLevels);

		// get the callstack associated with the last scope
		Callstack::Stackwalk *GetLastCallstack()
		{
			return (m_pCallstack && m_pCallstack->NumLevels() > 0) ? m_pCallstack : NULL;
		}

		//////////////////////////////////////////
		// Public serialisation interface

		int GetContextLevel() { return m_Indent; }
		uint32_t PushContext(const char *name, const char *typeName, uint32_t chunkIdx, bool smallChunk);
		void PopContext(uint32_t chunkIdx);

		// Write a chunk to disk
		void Insert(Chunk *el);

		// serialise a fixed-size array.
		template<int Num, class T>
		void SerialisePODArray(const char *name, T *el)
		{
			uint32_t n = (uint32_t)Num;
			SerialisePODArray(name, el, n);
		}

		// serialise a normal array. Typically this should be a small array,
		// for large byte buffers use SerialiseBuffer which is optimised for that
		//
		// If serialising in, el must either be NULL in which case allocated
		// memory will be returned, or it must be already large enough.
		template<class T>
		void SerialisePODArray(const char *name, T *&el, uint32_t &numElems)
		{
			if(m_Mode == WRITING)
			{
				WriteFrom(numElems);
				WriteBytes((byte *)el, sizeof(T)*numElems);
			}
			else if(m_Mode == READING)
			{
				ReadInto(numElems);

				if(numElems > 0)
				{
					if(el == NULL) el = new T[numElems];

					size_t length = numElems*sizeof(T);

					memcpy(el, ReadBytes(length), length);
				}
			}
			
			if(name != NULL && m_DebugTextWriting)
			{
				if(numElems == 0)
					DebugPrint("%s[]\n", name);

				for(size_t i=0; i < numElems; i++)
					DebugPrint("%s[%d] = %s\n", name, i, ToStr::Get<T>(el[i]).c_str());
			}
		}

		// overload for 64-bit counts
		template<class T>
		void SerialisePODArray(const char *name, T *&el, uint64_t &Num)
		{
			uint32_t n = (uint32_t)Num;
			SerialisePODArray(name, el, n);
			Num = n;
		}

		// serialise a normal array. Typically this should be a small array,
		// for large byte buffers use SerialiseBuffer which is optimised for that
		//
		// If serialising in, el will either be set to NULL or allocated, the
		// existing value will be overwritten.
		template<class T>
		void SerialiseComplexArray(const char *name, T *&el, uint32_t &Num)
		{
			if(m_Mode == WRITING)
			{
				WriteFrom(Num);
				for(uint32_t i=0; i < Num; i++)
					Serialise(m_DebugTextWriting ? StringFormat::Fmt("%s[%i]", name, i).c_str() : "", el[i]);
			}
			else if(m_Mode == READING)
			{
				ReadInto(Num);

				if(Num > 0)
				{
					el = new T[Num];

					for(uint32_t i=0; i < Num; i++)
						Serialise(m_DebugTextWriting ? StringFormat::Fmt("%s[%i]", name, i).c_str() : "", el[i]);
				}
				else
				{
					el = NULL;
				}
			}

			if(name != NULL && m_DebugTextWriting && Num == 0)
				DebugPrint("%s[]\n", name);
		}

		// overload for 64-bit counts
		template<class T>
		void SerialiseComplexArray(const char *name, T *&el, uint64_t &Num)
		{
			uint32_t n = (uint32_t)Num;
			SerialiseComplexArray(name, el, n);
			Num = n;
		}

		// serialise a single element
		template<class T> void Serialise(const char *name, T &el)
		{
			if(m_Mode == WRITING)
			{
				WriteFrom(el);
			}
			else if(m_Mode == READING)
			{
				ReadInto(el);
			}
			
			if(name != NULL && m_DebugTextWriting)
				DebugPrint("%s: %s\n", name, ToStr::Get<T>(el).c_str());
		}

		// object to serialise a single element
		// used to instantiate destructors for structs
		// with allocated members
		template<class T>
		class Deserialise : public T {
		  public:
		    Deserialise(T t) : T(t) {}
		    Deserialise() {}
		    virtual ~Deserialise() {}
		    Mode m_Mode;
		};

		template<typename X>
		void Serialise(const char *name, std::vector<X> &el)
		{
			uint64_t sz = el.size();
			Serialise(name, sz);
			if(m_Mode == WRITING)
			{
				for(size_t i=0; i < sz; i++)
					Serialise("[]", el[i]);
			}
			else
			{
				el.clear();
				el.reserve((size_t)sz);
				for(size_t i=0; i < sz; i++)
				{
					X x = X();
					Serialise("", x);
					el.push_back(x);
				}
			}
		}
		
		template<typename X>
		void Serialise(const char *name, rdctype::array<X> &el)
		{
			int32_t sz = el.count;
			Serialise(name, sz);
			if(m_Mode == WRITING)
			{
				for(int32_t i=0; i < sz; i++)
					Serialise("[]", el.elems[i]);
			}
			else
			{
				create_array_uninit(el, sz);
				for(int32_t i=0; i < sz; i++)
					Serialise("", el.elems[i]);
			}
		}
		
		void Serialise(const char *name, rdctype::str &el)
		{
			int32_t sz = el.count;
			Serialise(name, sz);
			if(m_Mode == WRITING)
			{
				for(int32_t i=0; i < sz; i++)
					Serialise("[]", el.elems[i]);
			}
			else
			{
				create_array_uninit(el, sz);
				for(int32_t i=0; i < sz; i++)
					Serialise("", el.elems[i]);
			}
		}

		template<typename X, typename Y>
		void Serialise(const char *name, std::pair<X, Y> &el)
		{
			Serialise(name, el.first);
			Serialise(name, el.second);
		}
		
		template<typename X, typename Y>
		void Serialise(const char *name, rdctype::pair<X, Y> &el)
		{
			Serialise(name, el.first);
			Serialise(name, el.second);
		}

		template<typename X>
		void Serialise(const char *name, std::list<X> &el)
		{
			uint64_t sz = el.size();
			Serialise(name, sz);
			if(m_Mode == WRITING)
			{
				for(auto it=el.begin(); it != el.end(); ++it)
					Serialise("[]", *it);
			}
			else
			{
				el.clear();
				for(uint64_t i=0; i < sz; i++)
				{
					X x = X();
					Serialise("", x);
					el.push_back(x);
				}
			}
		}

		// not sure if I still neeed these specialisations anymore.
		void SerialiseString(const char *name, string &el);

		// serialise a buffer. 
		//
		// If serialising in, buf must either be NULL in which case allocated
		// memory will be returned, or it must be already large enough.
		void SerialiseBuffer(const char *name, byte *&buf, size_t &len);
		void AlignNextBuffer(const size_t alignment);

		// NOT recommended interface. Useful for specific situations if e.g. you have
		// a buffer of data that is not arbitrary in size and can be determined by a 'type' or
		// similar elsewhere in the stream, so you want to skip the type-safety of the above
		// and write directly into the stream. Must be matched by a RawReadBytes.
		void RawWriteBytes(const void *data, size_t bytes)
		{
			WriteBytes((const byte *)data, bytes);
		}

		const void *RawReadBytes(size_t bytes)
		{
			return ReadBytes(bytes);
		}

		// prints to the debug output log
		void DebugPrint(const char *fmt, ...);

		static byte *AllocAlignedBuffer(size_t size, size_t align = 64);
		static void FreeAlignedBuffer(byte *buf);

		void FlushToDisk();

		// set a function used when serialising a text representation
		// of the chunks
		void SetChunkNameLookup(ChunkLookup lookup)
		{
			m_ChunkLookup = lookup;
		}

		void SetDebugText(bool enabled)
		{
			m_DebugTextWriting = enabled;
		}

		bool GetDebugText()
		{
			return m_DebugTextWriting;
		}

		string GetDebugStr()
		{
			return m_DebugText;
		}

		// debug-only output must be locked since it's global across all serialisers
		// essentially, which might not be thread safe in the normal flow
		void DebugLock()
		{
			m_DebugLock.Lock();
		}

		void DebugUnlock()
		{
			m_DebugLock.Unlock();
		}
		
	private:
		//////////////////////////////////////////
		// Raw memory buffer read/write

		void WriteBytes(const byte *buf, size_t nBytes);
		void *ReadBytes(size_t nBytes);

		void ReadFromFile(uint64_t bufferOffs, size_t length);

		template<class T> void WriteFrom(const T &f)
		{
			WriteBytes((byte *)&f, sizeof(T));
		}

		template<class T> void ReadInto(T &f)
		{
			if(m_HasError)
			{
				RDCERR("Reading into with error state serialiser");
				return;
			}

			char *data = (char *)ReadBytes(sizeof(T));
			f = *((T *)data);
		}

		// no copies
		Serialiser(const Serialiser &other);
		
		static void CreateResolver(void *ths);

		// clean out for before constructor and after destructor (and other times probably)
		void Reset();

		string GetIndent()
		{
			if(m_Mode == READING)
				return string(m_Indent > 0 ? 4 : 0, ' ');

			return string((size_t)m_Indent*4, ' ');
		}

		//////////////////////////////////////////
		
		static const uint64_t BufferAlignment;
		
		//////////////////////////////////////////

		uint64_t m_SerVer;
		
		Mode m_Mode;

		SerialiserError m_ErrorCode;
		bool m_HasError;
		bool m_DebugEnabled;

		int m_Indent;

		Callstack::Stackwalk *m_pCallstack;
		Callstack::StackResolver *m_pResolver;
		Threading::ThreadHandle m_ResolverThread;
		volatile bool m_ResolverThreadKillSignal;

		string m_Filename;

		// raw binary buffer
		uint64_t m_BufferSize;
		byte *m_Buffer;
		byte *m_BufferHead;
		size_t m_LastChunkLen;
		bool m_AlignedData;
		vector<uint64_t> m_ChunkFixups;

		// reading from file:

		struct Section
		{
			Section() : type(eSectionType_Unknown), flags(eSectionFlag_None), fileoffset(0), compressedReader(NULL) {}
			string name;
			SectionType type;
			SectionFlags flags;

			uint64_t fileoffset;
			uint64_t size;
			vector<byte> data;    // some sections can be loaded entirely into memory
			CompressedFileIO *compressedReader;
		};

		// this lists all sections in file order
		vector<Section*> m_Sections;

		// this lists known sections, some may be NULL
		Section *m_KnownSections[eSectionType_Num];

		// where does our in-memory window point to in the data stream. ie. m_pBuffer[0] is
		// m_ReadOffset into the frame capture section
		uint64_t m_ReadOffset;

		// how big is the current in-memory window
		size_t m_CurrentBufferSize;

		// the file pointer to read from
		FILE *m_ReadFileHandle;

		// writing to file
		vector<Chunk *> m_Chunks;

		// a database of strings read from the file, useful when serialised structures
		// expect a char* to return and point to static memory
		set<string> m_StringDB;

		// debug buffer
		bool m_DebugTextWriting;
		string m_DebugText;
		ChunkLookup m_ChunkLookup;
		
		Threading::CriticalSection m_DebugLock;
};

template<> void Serialiser::Serialise(const char *name, string &el);

// floats need aligned reads
template<> void Serialiser::ReadInto(float &f);

class ScopedContext
{
	public:
		ScopedContext(Serialiser *s, const char *n, const char *t, uint32_t i, bool smallChunk)
			: m_Idx(i), m_Ser(s), m_Ended(false)
		{
			m_Ser->PushContext(n, t, m_Idx, smallChunk);
		}
		ScopedContext(Serialiser *s, const char *n, uint32_t i, bool smallChunk)
			: m_Idx(i), m_Ser(s), m_Ended(false)
		{
			m_Ser->PushContext(n, NULL, m_Idx, smallChunk);
		}
		~ScopedContext()
		{
			if(!m_Ended)
				End();
		}

		Chunk *Get(bool temporary = false)
		{
			End();
			return new Chunk(m_Ser, m_Idx, temporary);
		}
	private:
		uint32_t m_Idx;
		Serialiser *m_Ser;

		bool m_Ended;

		void End()
		{
			RDCASSERT(!m_Ended);

			m_Ser->PopContext(m_Idx);

			m_Ended = true;
		}
};

// can be overridden to locally cache the serialiser pointer (e.g. for TLS lookup)
#define GET_SERIALISER GetSerialiser()

#define SCOPED_SERIALISE_CONTEXT(n) ScopedContext scope(GET_SERIALISER, GetChunkName(n), n, false);
#define SCOPED_SERIALISE_CONTEXT(n) ScopedContext scope(GET_SERIALISER, GetChunkName(n), n, false);
#define SCOPED_SERIALISE_SMALL_CONTEXT(n) ScopedContext scope(GET_SERIALISER, GetChunkName(n), n, true);

#define SERIALISE_ELEMENT(type, name, inValue) type name; if(m_State >= WRITING) name = (inValue); GET_SERIALISER->Serialise(#name, name);
#define SERIALISE_ELEMENT_CLASS(type, name, inValue) Serialiser::Deserialise<type> name; if(m_State >= WRITING) { static_cast<type&>(name) = (inValue); name.m_Mode = Serialiser::WRITING; } else name.m_Mode = Serialiser::READING; GET_SERIALISER->Serialise(#name, static_cast<type&>(name)); 
#define SERIALISE_ELEMENT_OPT(type, name, inValue, Condition) type name = type(); if(Condition) { if(m_State >= WRITING) name = (inValue); GET_SERIALISER->Serialise(#name, name); }
#define SERIALISE_ELEMENT_ARR(type, name, inValues, count) type *name = new type[count]; for(size_t serialiseIdx=0; serialiseIdx < count; serialiseIdx++) { if(m_State >= WRITING) name[serialiseIdx] = (inValues)[serialiseIdx]; GET_SERIALISER->Serialise(#name, name[serialiseIdx]); }
#define SERIALISE_ELEMENT_ARR_OPT(type, name, inValues, count, Condition) type *name = NULL; if(Condition) { name = new type[count]; for(size_t serialiseIdx=0; serialiseIdx < count; serialiseIdx++) { if(m_State >= WRITING) name[serialiseIdx] = (inValues)[serialiseIdx]; GET_SERIALISER->Serialise(#name, name[serialiseIdx]); } }
#define SERIALISE_ELEMENT_PTR(type, name, inValue) type name; if(inValue && m_State >= WRITING) name = *(inValue); GET_SERIALISER->Serialise(#name, name);
#define SERIALISE_ELEMENT_PTR_OPT(type, name, inValue, Condition) type name; if(Condition) { if(inValue && m_State >= WRITING) name = *(inValue); GET_SERIALISER->Serialise(#name, name); }
#define SERIALISE_ELEMENT_BUF(type, name, inBuf, Len) type name = (type)NULL; if(m_State >= WRITING) name = (type)(inBuf); size_t CONCAT(buflen, __LINE__) = Len; GET_SERIALISER->SerialiseBuffer(#name, name, CONCAT(buflen, __LINE__));
#define SERIALISE_ELEMENT_BUF_OPT(type, name, inBuf, Len, Condition) type name = (type)NULL; if(Condition) { if(m_State >= WRITING) name = (type)(inBuf); size_t CONCAT(buflen, __LINE__) = Len; GET_SERIALISER->SerialiseBuffer(#name, name, CONCAT(buflen, __LINE__)); }

// forward declare generic pointer version to void*
template<class T>
struct ToStrHelper<true, T>
{
	static string Get(const T &el)
	{
		void *ptr = (void *)el;
		return ToStrHelper<false, void*>::Get(ptr);
	}
};

#define TOSTR_CASE_STRINGIZE(a) case a: return #a;
#define TOSTR_CASE_STRINGIZE_CONCAT(a, b) case CONCAT(a, b): return #b;
#define TOSTR_CASE_STRINGIZE_NAMESPACE(a, b) case a::b: return #b;

