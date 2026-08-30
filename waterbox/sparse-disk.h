// sparse-disk.h - a writable hard disk that does not live in guest memory.
//
// DOSBox-X's drive C: has to be WRITABLE and it has to be part of the machine,
// because a movie replays the disk as much as the RAM. The obvious way to get
// both inside a sandbox is to copy the whole image into guest memory, which is
// what this core did - and it puts a hard ceiling on the disk: miniBox fits the
// entire guest inside one aligned 4 GiB region, so a 2 GB image was already
// most of the machine and a 4 GB one was impossible at any setting. It also
// cost that size in HOST memory whether or not a single sector was ever
// written, because the sealed baseline a savestate is diffed against covers
// everything the guest has mapped.
//
// So the disk is split in two:
//
//   THE BASE is read-only and stays where it was. For a mounted image that is
//   a file on the host, read through the sandbox's own file system a chunk at a
//   time; for one of the package's formatted disks it is a small decompressed
//   head followed by zeros, which needs no storage at all. Neither is guest
//   memory, so neither is in a savestate, and neither bounds the disk's size.
//
//   THE OVERLAY is every chunk that has been WRITTEN, and only those. It is
//   ordinary guest memory, so a savestate carries it automatically - and
//   carries nothing else, which is the point: a state after five minutes of DOS
//   is the few hundred kilobytes that changed rather than two gigabytes that
//   mostly did not.
//
// Determinism is unaffected. The base is a file the project pins by hash and
// this only ever reads it; two runs of the same movie read the same bytes in
// the same order and write the same overlay.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class SparseDisk
{
public:
	// 64 KiB: big enough that the map stays small on a disk a game has been
	// installed onto, small enough that touching one byte does not fault in a
	// megabyte. DOS writes in clusters far below this.
	static constexpr uint64_t CHUNK = 64 * 1024;

	~SparseDisk() { if (_base != nullptr) fclose(_base); }

	// A disk backed by a file on the host. The file is opened once and kept
	// open: it is read from, never written to, and the writes go to the
	// overlay instead.
	bool openFile(const std::string &path, uint64_t size)
	{
		_base = fopen(path.c_str(), "rb");
		if (_base == nullptr) return false;
		_size = size;
		return true;
	}

	// A disk whose base is a head of real bytes and then nothing: the package's
	// formatted images, which are a boot sector and a FAT and then two
	// gigabytes of zeros. The head is small enough to hold; the zeros are not
	// held at all.
	void openHead(std::vector<uint8_t> head, uint64_t size)
	{
		_head = std::move(head);
		_size = size;
	}

	uint64_t size() const { return _size; }
	bool isOpen() const { return _size != 0; }

	// How much has been written, for the save-data channel and for anyone
	// wondering what a state is carrying.
	uint64_t overlayBytes() const { return (uint64_t)_overlay.size() * CHUNK; }
	const std::map<uint64_t, std::vector<uint8_t>> &overlay() const { return _overlay; }

	bool read(uint64_t offset, void *dst, size_t len)
	{
		if (offset + len > _size) return false;
		uint8_t *out = (uint8_t *)dst;
		while (len > 0) {
			const uint64_t chunk = offset / CHUNK;
			const uint64_t within = offset % CHUNK;
			const size_t take = (size_t)std::min<uint64_t>(len, CHUNK - within);
			auto it = _overlay.find(chunk);
			if (it != _overlay.end()) {
				memcpy(out, it->second.data() + within, take);
			} else if (!readBase(offset, out, take)) {
				return false;
			}
			offset += take; out += take; len -= take;
		}
		return true;
	}

	bool write(uint64_t offset, const void *src, size_t len)
	{
		if (offset + len > _size) return false;
		const uint8_t *in = (const uint8_t *)src;
		while (len > 0) {
			const uint64_t chunk = offset / CHUNK;
			const uint64_t within = offset % CHUNK;
			const size_t take = (size_t)std::min<uint64_t>(len, CHUNK - within);
			std::vector<uint8_t> *block = materialise(chunk);
			if (block == nullptr) return false;
			memcpy(block->data() + within, in, take);
			offset += take; in += take; len -= take;
		}
		return true;
	}

private:
	// A chunk about to be written has to start as what the base says it is:
	// DOS writes 512 bytes into a 64 KiB chunk and the other 65024 must still
	// read back as they did. This is the only place the base is copied.
	std::vector<uint8_t> *materialise(uint64_t chunk)
	{
		auto it = _overlay.find(chunk);
		if (it != _overlay.end()) return &it->second;

		const uint64_t start = chunk * CHUNK;
		const size_t span = (size_t)std::min<uint64_t>(CHUNK, _size - start);
		std::vector<uint8_t> block(CHUNK, 0);
		if (!readBase(start, block.data(), span)) return nullptr;
		auto ins = _overlay.emplace(chunk, std::move(block));
		return &ins.first->second;
	}

	// What the disk looked like before anything was written to it.
	bool readBase(uint64_t offset, uint8_t *dst, size_t len)
	{
		if (_base != nullptr) {
			// A short read is the end of a file smaller than the disk, which
			// is legal: the rest of the disk is zeros.
			if (fseeko(_base, (off_t)offset, SEEK_SET) != 0) return false;
			const size_t got = fread(dst, 1, len, _base);
			if (got < len) memset(dst + got, 0, len - got);
			return true;
		}
		// the head-and-zeros form
		size_t fromHead = 0;
		if (offset < _head.size()) {
			fromHead = (size_t)std::min<uint64_t>(len, _head.size() - offset);
			memcpy(dst, _head.data() + offset, fromHead);
		}
		if (fromHead < len) memset(dst + fromHead, 0, len - fromHead);
		return true;
	}

	FILE *_base = nullptr;
	std::vector<uint8_t> _head;
	uint64_t _size = 0;
	// chunk index -> its bytes. std::map rather than a hash: the save-data
	// channel writes the overlay out and a movie has to get the same file
	// twice, so the order has to be the keys' and not a hash seed's.
	std::map<uint64_t, std::vector<uint8_t>> _overlay;
};

// The one hard disk this machine has, if it has one. Named the way the memory
// file directory beside it is, so drive_fat can ask "is this the disk?" before
// it goes looking for a file.
extern SparseDisk _sparseHardDisk;
extern std::string _sparseHardDiskName;
