// license:BSD-3-Clause
// Standalone regression for the M20 raw image handler. No historical media required.
#include "formats/m20_dsk.h"
#include "formats/mfi_dsk.h"
#include "util/ioprocs.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

struct format_probe : m20_format
{
	using floppy_image_format_t::generate_bitstream_from_track;
	using floppy_image_format_t::extract_sectors_from_bitstream_fm_pc;
	using floppy_image_format_t::extract_sectors_from_bitstream_mfm_pc;
};

int main(int argc, char **argv)
{
	format_probe fmt;
	if (argc == 4 && std::strcmp(argv[1], "convert") == 0)
	{
		auto in = util::stdio_read(std::fopen(argv[2], "rb"));
		assert(in);
		floppy_image image(84, 2, floppy_image::FF_525);
		bool mfi = FLOPPY_MFI_FORMAT.identify(*in, floppy_image::FF_525, {});
		assert((mfi ? FLOPPY_MFI_FORMAT.load(*in, floppy_image::FF_525, {}, image) : fmt.load(*in, floppy_image::FF_525, {}, image)));
		int tracks, heads; image.get_actual_geometry(tracks, heads);
		std::printf("geometry: %d cylinders, %d heads\n", tracks, heads);
		auto out = util::stdio_read_write(std::fopen(argv[3], "w+b"));
		assert(out);
		assert(std::strstr(argv[3], ".mfi") ? FLOPPY_MFI_FORMAT.save(*out, {}, image) : fmt.save(*out, {}, image));
		return 0;
	}
	for (int test_case : {0, 1, 2})
	{
		int tracks = test_case == 2 ? 80 : 35;
		int step = test_case == 1 ? 2 : 1;
		std::vector<uint32_t> variants = test_case ? std::vector<uint32_t>{floppy_image::DSDD, floppy_image::DSQD} : std::vector<uint32_t>{floppy_image::DSDD};
		std::vector<uint8_t> raw(tracks * 2 * 16 * 256);
		for (size_t i = 0; i < raw.size(); i++) raw[i] = uint8_t((i * 17) ^ (i >> 8) ^ (i >> 16));
		for (int s = 0; s < 16; s++) std::fill_n(raw.begin() + s * 256 + 128, 128, 0);
		auto in = util::ram_read(raw.data(), raw.size());
		assert(fmt.identify(*in, floppy_image::FF_525, {}) != 0);
		floppy_image image(84, 2, floppy_image::FF_525);
		assert(fmt.load(*in, floppy_image::FF_525, variants, image));
		int actual_tracks, heads; image.get_actual_geometry(actual_tracks, heads);
		assert(actual_tracks == (tracks - 1) * step + 1 && heads == 2);
		assert(image.get_variant() == (tracks == 80 ? floppy_image::DSQD : floppy_image::DSDD));
		// Independently decode all tracks, including the final cylinder and C0/H1.
		for (int c = 0; c < tracks; c++) for (int h = 0; h < 2; h++)
		{
			bool fm = !c && !h;
			auto bits = fmt.generate_bitstream_from_track(c * step, h, fm ? 4000 : 2000, image);
			auto sectors = fm ? fmt.extract_sectors_from_bitstream_fm_pc(bits) : fmt.extract_sectors_from_bitstream_mfm_pc(bits);
			assert(sectors.size() == 17);
			for (int s = 1; s <= 16; s++)
			{
				assert(sectors[s].size() == (fm ? 128 : 256));
				assert(std::equal(sectors[s].begin(), sectors[s].end(), raw.begin() + ((c * 2 + h) * 16 + s - 1) * 256));
			}
		}
		auto out = util::stdio_read_write(std::tmpfile());
		assert(out && fmt.save(*out, {}, image));
		uint64_t length; assert(!out->length(length) && length == raw.size());
		std::vector<uint8_t> saved(raw.size());
		auto [err, count] = read_at(*out, 0, saved.data(), saved.size());
		assert(!err && count == saved.size() && saved == raw);
		floppy_image reopened(84, 2, floppy_image::FF_525);
		assert(fmt.load(*out, floppy_image::FF_525, variants, reopened));
		auto out2 = util::stdio_read_write(std::tmpfile());
		assert(fmt.save(*out2, {}, reopened));
		auto [err2, count2] = read_at(*out2, 0, saved.data(), saved.size());
		assert(!err2 && count2 == saved.size() && saved == raw);
		if (tracks == 80)
		{
			floppy_image dd(42, 2, floppy_image::FF_525);
			assert(!fmt.load(*in, floppy_image::FF_525, {}, dd));
		}
		else
		{
			// PCOS VFORMAT writes five additional cylinders on 320 KB media.
			// The established raw format still exports only the first 35.
			for (int c = 35; c < 40; c++) for (int h = 0; h < 2; h++)
				image.get_buffer(c * step, h) = image.get_buffer(34 * step, h);
			auto spare = util::stdio_read_write(std::tmpfile());
			assert(spare && fmt.save(*spare, {}, image));
			assert(!spare->length(length) && length == raw.size());
			auto [spare_err, spare_count] = read_at(*spare, 0, saved.data(), saved.size());
			assert(!spare_err && spare_count == saved.size() && saved == raw);
			if (step == 2)
			{
				image.get_buffer(1, 0) = image.get_buffer(2, 0);
				auto odd = util::stdio_read_write(std::tmpfile());
				assert(odd && !fmt.save(*odd, {}, image));
				assert(!odd->length(length) && length == 0);
				image.get_buffer(1, 0).clear();
			}
		}
		// A missing late track must fail without producing a partial raw export.
		image.get_buffer((tracks - 1) * step, 0).clear();
		auto bad = util::stdio_read_write(std::tmpfile());
		assert(!fmt.save(*bad, {}, image));
		assert(!bad->length(length) && length == 0);
		std::printf("PASS: %d cylinders (step %d), FM/MFM, full byte round trip, reopen, missing-track rejection%s\n",
			tracks, step, tracks == 35 ? ", spare cylinders" : "");
	}
	std::vector<uint8_t> truncated(655359);
	auto bad = util::ram_read(truncated.data(), truncated.size());
	assert(!fmt.identify(*bad, floppy_image::FF_525, {}));
	floppy_image image(84, 2, floppy_image::FF_525);
	assert(!fmt.load(*bad, floppy_image::FF_525, {}, image));
	std::puts("PASS: invalid image length and 80-cylinder image in 40-cylinder drive rejected");
}
