/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Pictures.h"

#include <Bitmap.h>
#include <ByteOrder.h>
#include <DataIO.h>
#include <File.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>

#include <algorithm>
#include <new>
#include <math.h>
#include <string.h>

#include "Style.h"


/*!	Each pixel of the smaller picture is the average of the pixels of the
	big one it covers: no aliasing, and cheap enough for pictures of a few
	hundred pixels. Both in 32 bits, B_RGBA32 or B_RGB32.
*/
static void
shrink(const BBitmap* from, BBitmap* to)
{
	int32 fromWidth = from->Bounds().IntegerWidth() + 1;
	int32 fromHeight = from->Bounds().IntegerHeight() + 1;
	int32 toWidth = to->Bounds().IntegerWidth() + 1;
	int32 toHeight = to->Bounds().IntegerHeight() + 1;
	const uint8* fromBits = (const uint8*)from->Bits();
	uint8* toBits = (uint8*)to->Bits();
	int32 fromRow = from->BytesPerRow();
	int32 toRow = to->BytesPerRow();

	for (int32 y = 0; y < toHeight; y++) {
		int32 top = (int32)((int64)y * fromHeight / toHeight);
		int32 bottom = std::max(top + 1,
			(int32)((int64)(y + 1) * fromHeight / toHeight));
		uint8* out = toBits + y * toRow;
		for (int32 x = 0; x < toWidth; x++) {
			int32 left = (int32)((int64)x * fromWidth / toWidth);
			int32 right = std::max(left + 1,
				(int32)((int64)(x + 1) * fromWidth / toWidth));
			uint32 sum[4] = { 0, 0, 0, 0 };
			for (int32 sy = top; sy < bottom; sy++) {
				const uint8* in = fromBits + sy * fromRow + left * 4;
				for (int32 sx = left; sx < right; sx++, in += 4) {
					sum[0] += in[0];
					sum[1] += in[1];
					sum[2] += in[2];
					sum[3] += in[3];
				}
			}
			uint32 count = (uint32)(bottom - top) * (right - left);
			for (int32 c = 0; c < 4; c++)
				out[c] = (uint8)((sum[c] + count / 2) / count);
			out += 4;
		}
	}
}


/*!	The picture of the file as a bitmap of its own memory: the translator
	writes the B_TRANSLATOR_BITMAP format to memory, read here, rather than
	BTranslationUtils::GetBitmap(), whose BBitmapStream makes a bitmap of
	app_server.
*/
/*!	Memory for what a translator writes, up to a limit: a small file can
	be a huge picture (a "decompression bomb" from a feed), which would take
	all the memory of a small machine.
*/
class LimitedIO : public BMallocIO {
public:
	static const off_t kLimit = 64 * 1024 * 1024;

	virtual ssize_t WriteAt(off_t position, const void* buffer, size_t size)
	{
		if (position < 0 || position + (off_t)size > kLimit)
			return B_NO_MEMORY;
		return BMallocIO::WriteAt(position, buffer, size);
	}

	virtual status_t SetSize(off_t size)
	{
		if (size > kLimit)
			return B_NO_MEMORY;
		return BMallocIO::SetSize(size);
	}
};


static BBitmap*
decode(const char* path)
{
	BFile file(path, B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return NULL;
	LimitedIO data;
	if (BTranslatorRoster::Default()->Translate(&file, NULL, NULL, &data,
			B_TRANSLATOR_BITMAP) != B_OK
		|| data.BufferLength() < sizeof(TranslatorBitmap))
		return NULL;

	TranslatorBitmap header;
	memcpy((void*)&header, data.Buffer(), sizeof(header));
	if (swap_data(B_UINT32_TYPE, &header, sizeof(header),
			B_SWAP_BENDIAN_TO_HOST) != B_OK
		|| header.magic != B_TRANSLATOR_BITMAP)
		return NULL;
	BRect bounds = header.bounds;
	// sizes a picture can have, rows as long as their pixels
	if (!bounds.IsValid() || bounds.left != 0 || bounds.top != 0
		|| bounds.Width() >= 16384 || bounds.Height() >= 16384)
		return NULL;
	int32 rows = bounds.IntegerHeight() + 1;
	int32 columns = bounds.IntegerWidth() + 1;
	int32 bytesPerPixel = header.colors == B_RGB32 || header.colors == B_RGBA32
		|| header.colors == B_RGB32_BIG || header.colors == B_RGBA32_BIG ? 4 : 1;
	if (header.rowBytes == 0
		|| header.rowBytes < (uint32)columns * bytesPerPixel
		|| (size_t)header.rowBytes * rows
			> data.BufferLength() - sizeof(header))
		return NULL;

	BBitmap* picture = new(std::nothrow) BBitmap(bounds,
		B_BITMAP_NO_SERVER_LINK, header.colors, header.rowBytes);
	if (picture == NULL)
		return NULL;
	if (picture->InitCheck() != B_OK) {
		delete picture;
		return NULL;
	}
	const uint8* pixels = (const uint8*)data.Buffer() + sizeof(header);
	int32 row = std::min((int32)header.rowBytes, picture->BytesPerRow());
	for (int32 y = 0; y < rows; y++) {
		memcpy((uint8*)picture->Bits() + y * picture->BytesPerRow(),
			pixels + y * header.rowBytes, row);
	}
	return picture;
}


BBitmap*
Pictures::Read(const char* path, float largest)
{
	if (path == NULL || path[0] == '\0')
		return NULL;
	BBitmap* picture = decode(path);
	if (picture == NULL)
		return NULL;

	// in 32 bits, should the translator have given something else
	color_space space = picture->ColorSpace();
	if (space != B_RGBA32 && space != B_RGB32) {
		BBitmap* converted = new(std::nothrow) BBitmap(picture->Bounds(),
			B_BITMAP_NO_SERVER_LINK, B_RGBA32);
		if (converted == NULL || converted->InitCheck() != B_OK
			|| converted->ImportBits(picture) != B_OK) {
			delete converted;
			delete picture;
			return NULL;
		}
		delete picture;
		picture = converted;
		space = B_RGBA32;
	}

	float limit = Style::S(largest);
	float width = picture->Bounds().Width() + 1;
	float height = picture->Bounds().Height() + 1;
	if (width <= limit && height <= limit)
		return picture;

	float scale = std::min(limit / width, limit / height);
	BRect bounds(0, 0, std::max(1.0f, floorf(width * scale + 0.5f)) - 1,
		std::max(1.0f, floorf(height * scale + 0.5f)) - 1);
	BBitmap* small = new(std::nothrow) BBitmap(bounds,
		B_BITMAP_NO_SERVER_LINK, space);
	if (small == NULL)
		return picture;
	if (small->InitCheck() != B_OK) {
		delete small;
		return picture;
	}
	shrink(picture, small);
	delete picture;
	return small;
}


BBitmap*
Pictures::ForScreen(BBitmap* read)
{
	if (read == NULL)
		return NULL;
	BBitmap* shown = new(std::nothrow) BBitmap(read->Bounds(),
		read->ColorSpace());
	if (shown == NULL) {
		delete read;
		return NULL;
	}
	if (shown->InitCheck() != B_OK) {
		delete shown;
		delete read;
		return NULL;
	}
	int32 rows = read->Bounds().IntegerHeight() + 1;
	int32 row = std::min(read->BytesPerRow(), shown->BytesPerRow());
	for (int32 y = 0; y < rows; y++) {
		memcpy((uint8*)shown->Bits() + y * shown->BytesPerRow(),
			(const uint8*)read->Bits() + y * read->BytesPerRow(), row);
	}
	delete read;
	return shown;
}


BBitmap*
Pictures::Load(const char* path, float largest)
{
	return ForScreen(Read(path, largest));
}
