/* ImageBuffer.cpp
Copyright (c) 2014 by Michael Zahniser

Endless Sky is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

Endless Sky is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "ImageBuffer.h"

#include "../Files.h"
#include "ImageFileData.h"
#include "../Logger.h"

#include <jpeglib.h>
#include <png.h>

#include <cstdio>
#include <set>
#include <stdexcept>
#include <vector>

using namespace std;

namespace {
	const set<string> PNG_EXTENSIONS{".png"};
	const set<string> JPG_EXTENSIONS{".jpg", ".jpeg", ".jpe"};
	const set<string> IMAGE_EXTENSIONS = []()
	{
		set<string> extensions(PNG_EXTENSIONS);
		extensions.insert(JPG_EXTENSIONS.begin(), JPG_EXTENSIONS.end());
		return extensions;
	}();

	bool ReadPNG(const filesystem::path &path, ImageBuffer &buffer, int frame);
	bool ReadJPG(const filesystem::path &path, ImageBuffer &buffer, int frame);
	void Premultiply(ImageBuffer &buffer, int frame, BlendingMode additive);
}



const set<string> &ImageBuffer::ImageExtensions()
{
	return IMAGE_EXTENSIONS;
}



ImageBuffer::ImageBuffer(int frames)
	: width(0), height(0), frames(frames), pixels(nullptr)
{
}



ImageBuffer::~ImageBuffer()
{
	Clear();
}



// Set the number of frames. This must be called before allocating.
void ImageBuffer::Clear(int frames)
{
	delete [] pixels;
	pixels = nullptr;
	this->frames = frames;
}



// Allocate the internal buffer. This must only be called once for each
// image buffer; subsequent calls will be ignored.
void ImageBuffer::Allocate(int width, int height)
{
	// Do nothing if the buffer is already allocated or if any of the dimensions
	// is set to zero.
	if(pixels || !width || !height || !frames)
		return;

	pixels = new uint32_t[width * height * frames];
	this->width = width;
	this->height = height;
}



int ImageBuffer::Width() const
{
	return width;
}



int ImageBuffer::Height() const
{
	return height;
}



int ImageBuffer::Frames() const
{
	return frames;
}



const uint32_t *ImageBuffer::Pixels() const
{
	return pixels;
}



uint32_t *ImageBuffer::Pixels()
{
	return pixels;
}



const uint32_t *ImageBuffer::Begin(int y, int frame) const
{
	return pixels + width * (y + height * frame);
}



uint32_t *ImageBuffer::Begin(int y, int frame)
{
	return pixels + width * (y + height * frame);
}



void ImageBuffer::ShrinkToHalfSize()
{
	ImageBuffer result(frames);
	result.Allocate(width / 2, height / 2);

	unsigned char *begin = reinterpret_cast<unsigned char *>(pixels);
	unsigned char *out = reinterpret_cast<unsigned char *>(result.pixels);
	// Loop through every line of every frame of the buffer.
	for(int y = 0; y < result.height * frames; ++y)
	{
		unsigned char *aIt = begin + (4 * width) * (2 * y);
		unsigned char *aEnd = aIt + 4 * 2 * result.width;
		unsigned char *bIt = begin + (4 * width) * (2 * y + 1);
		for( ; aIt != aEnd; aIt += 4, bIt += 4)
		{
			for(int channel = 0; channel < 4; ++channel, ++aIt, ++bIt, ++out)
				*out = (static_cast<unsigned>(aIt[0]) + static_cast<unsigned>(bIt[0])
					+ static_cast<unsigned>(aIt[4]) + static_cast<unsigned>(bIt[4]) + 2) / 4;
		}
	}
	swap(width, result.width);
	swap(height, result.height);
	swap(pixels, result.pixels);
}



bool ImageBuffer::Read(const ImageFileData &data, int frame)
{
	// First, make sure this is a supported file.
	bool isPNG = PNG_EXTENSIONS.contains(data.extension);
	bool isJPG = JPG_EXTENSIONS.contains(data.extension);

	if(!isPNG && !isJPG)
		return false;

	if(isPNG && !ReadPNG(data.path, *this, frame))
		return false;
	if(isJPG && !ReadJPG(data.path, *this, frame))
		return false;

	if(data.blendingMode != BlendingMode::PREMULTIPLIED_ALPHA)
	{
		if(isPNG || (isJPG && data.blendingMode == BlendingMode::ADDITIVE))
			Premultiply(*this, frame, data.blendingMode);
	}
	return true;
}



namespace {
	void ReadPNGInput(png_structp pngStruct, png_bytep outBytes, png_size_t byteCountToRead)
	{
		static_cast<iostream *>(png_get_io_ptr(pngStruct))->read(reinterpret_cast<char *>(outBytes), byteCountToRead);
	}

	bool ReadPNG(const filesystem::path &path, ImageBuffer &buffer, int frame)
	{
		// Open the file, and make sure it really is a PNG.
		shared_ptr<iostream> file = Files::Open(path.string());
		if(!file)
			return false;

		// Set up libpng.
		png_struct *png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
		if(!png)
			return false;

		png_info *info = png_create_info_struct(png);
		if(!info)
		{
			png_destroy_read_struct(&png, nullptr, nullptr);
			return false;
		}

		if(setjmp(png_jmpbuf(png)))
		{
			png_destroy_read_struct(&png, &info, nullptr);
			return false;
		}

		png_set_read_fn(png, file.get(), ReadPNGInput);
		png_set_sig_bytes(png, 0);

		png_read_info(png, info);
		int width = png_get_image_width(png, info);
		int height = png_get_image_height(png, info);
		// If the buffer is not yet allocated, allocate it.
		try {
			buffer.Allocate(width, height);
		}
		catch(const bad_alloc &)
		{
			png_destroy_read_struct(&png, &info, nullptr);
			const string message = "Failed to allocate contiguous memory for \"" + path.string() + "\"";
			Logger::LogError(message);
			throw runtime_error(message);
		}
		// Make sure this frame's dimensions are valid.
		if(!width || !height || width != buffer.Width() || height != buffer.Height())
		{
			png_destroy_read_struct(&png, &info, nullptr);
			string message = "Skipped processing \"" + path.string() + "\":\n\tAll image frames must have equal ";
			if(width && width != buffer.Width())
				Logger::LogError(message + "width: expected " + to_string(buffer.Width()) + " but was " + to_string(width));
			if(height && height != buffer.Height())
				Logger::LogError(message + "height: expected " + to_string(buffer.Height()) + " but was " + to_string(height));
			return false;
		}

		// Adjust settings to make sure the result will be an RGBA file.
		int colorType = png_get_color_type(png, info);
		int bitDepth = png_get_bit_depth(png, info);

		if(colorType == PNG_COLOR_TYPE_PALETTE)
			png_set_palette_to_rgb(png);
		if(png_get_valid(png, info, PNG_INFO_tRNS))
			png_set_tRNS_to_alpha(png);
		if(colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
			png_set_expand_gray_1_2_4_to_8(png);
		if(bitDepth == 16)
		{
#if PNG_LIBPNG_VER >= 10504
			png_set_scale_16(png);
#else
			png_set_strip_16(png);
#endif
		}
		if(bitDepth < 8)
			png_set_packing(png);
		if(colorType == PNG_COLOR_TYPE_PALETTE || colorType == PNG_COLOR_TYPE_RGB
				|| colorType == PNG_COLOR_TYPE_GRAY)
			png_set_add_alpha(png, 0xFFFF, PNG_FILLER_AFTER);
		if(colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
			png_set_gray_to_rgb(png);
		// Let libpng handle any interlaced image decoding.
		png_set_interlace_handling(png);
		png_read_update_info(png, info);

		// Read the file.
		vector<png_byte *> rows(height, nullptr);
		for(int y = 0; y < height; ++y)
			rows[y] = reinterpret_cast<png_byte *>(buffer.Begin(y, frame));

		png_read_image(png, &rows.front());

		// Clean up. The file will be closed automatically.
		png_destroy_read_struct(&png, &info, nullptr);

		return true;
	}



	bool ReadJPG(const filesystem::path &path, ImageBuffer &buffer, int frame)
	{
		string data = Files::Read(path);
		if(data.empty())
			return false;

		jpeg_decompress_struct cinfo;
		struct jpeg_error_mgr jerr;
		cinfo.err = jpeg_std_error(&jerr);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		jpeg_create_decompress(&cinfo);
#pragma GCC diagnostic pop

		jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char *>(data.data()), data.size());
		jpeg_read_header(&cinfo, true);
		cinfo.out_color_space = JCS_EXT_RGBA;

		// MAYBE: Reading in lots of images in a 32-bit process gets really hairy using the standard approach due to
		// contiguous memory layout requirements. Investigate using an iterative loading scheme for large images.
		jpeg_start_decompress(&cinfo);
		int width = cinfo.image_width;
		int height = cinfo.image_height;
		// If the buffer is not yet allocated, allocate it.
		try {
			buffer.Allocate(width, height);
		}
		catch(const bad_alloc &)
		{
			jpeg_destroy_decompress(&cinfo);
			const string message = "Failed to allocate contiguous memory for \"" + path.string() + "\"";
			Logger::LogError(message);
			throw runtime_error(message);
		}
		// Make sure this frame's dimensions are valid.
		if(!width || !height || width != buffer.Width() || height != buffer.Height())
		{
			jpeg_destroy_decompress(&cinfo);
			string message = "Skipped processing \"" + path.string() + "\":\t\tAll image frames must have equal ";
			if(width && width != buffer.Width())
				Logger::LogError(message + "width: expected " + to_string(buffer.Width()) + " but was " + to_string(width));
			if(height && height != buffer.Height())
				Logger::LogError(message + "height: expected " + to_string(buffer.Height()) + " but was " + to_string(height));
			return false;
		}

		// Read the file.
		vector<JSAMPLE *> rows(height, nullptr);
		for(int y = 0; y < height; ++y)
			rows[y] = reinterpret_cast<JSAMPLE *>(buffer.Begin(y, frame));

		while(height)
			height -= jpeg_read_scanlines(&cinfo, &rows.front() + cinfo.output_scanline, height);

		jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);

		return true;
	}



	void Premultiply(ImageBuffer &buffer, int frame, BlendingMode blend)
	{
		for(int y = 0; y < buffer.Height(); ++y)
		{
			uint32_t *it = buffer.Begin(y, frame);

			for(uint32_t *end = it + buffer.Width(); it != end; ++it)
			{
				uint64_t value = *it;
				uint64_t alpha = (value & 0xFF000000) >> 24;

				uint64_t red = (((value & 0xFF0000) * alpha) / 255) & 0xFF0000;
				uint64_t green = (((value & 0xFF00) * alpha) / 255) & 0xFF00;
				uint64_t blue = (((value & 0xFF) * alpha) / 255) & 0xFF;

				value = red | green | blue;
				if(blend == BlendingMode::HALF_ADDITIVE)
					alpha >>= 2;
				if(blend != BlendingMode::ADDITIVE)
					value |= (alpha << 24);

				*it = static_cast<uint32_t>(value);
			}
		}
	}
}
