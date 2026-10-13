// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
/*********************************************************************

    formats/m20_dsk.cpp

    Olivetti M20 floppy-disk images

    Track 0/head 0 is FM, 128 byte sectors. The rest is MFM,
    256 byte sectors.
    Images contain 35 (320 KB drives) or 80 (640 KB drives) cylinders,
    two heads and sixteen sectors per track.
    In image files the sectors of track 0/head 0 are 256 bytes
    long to simplify access. Only the first half of these sectors
    contain image data.

*********************************************************************/

#include "m20_dsk.h"

#include "ioprocs.h"

namespace {

int image_tracks(util::random_read &io)
{
	uint64_t size;
	if (io.length(size))
		return 0;

	switch (size)
	{
	case 35 * 2 * 16 * 256: return 35;
	case 80 * 2 * 16 * 256: return 80;
	default: return 0;
	}
}

} // anonymous namespace


m20_format::m20_format()
{
}

const char *m20_format::name() const noexcept
{
	return "m20";
}

const char *m20_format::description() const noexcept
{
	return "M20 disk image";
}

const char *m20_format::extensions() const noexcept
{
	return "img";
}

bool m20_format::supports_save() const noexcept
{
	return true;
}

int m20_format::identify(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants) const
{
	return image_tracks(io) ? FIFID_SIZE : 0;
}

bool m20_format::load(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants, floppy_image &image) const
{
	int const track_count = image_tracks(io);
	// BIOS 2.0 double-steps when reading 48 TPI media in a 96 TPI drive.
	int const track_step = track_count == 35 && has_variant(variants, floppy_image::DSQD) ? 2 : 1;
	int max_tracks, max_heads;
	image.get_maximal_geometry(max_tracks, max_heads);
	if (!track_count || max_tracks < (track_count - 1) * track_step + 1 || max_heads < 2)
		return false;

	for (int track = 0; track < track_count; track++)
		for (int head = 0; head < 2; head ++) {
			bool mfm = track || head;
			desc_pc_sector sects[16];
			uint8_t sectdata[16*256];
			auto const [err, actual] = read_at(io, 16*256*(track*2+head), sectdata, sizeof(sectdata));
			if (err || actual != sizeof(sectdata))
				return false;
			for (int i = 0; i < 16; i++) {
				int j = i/2 + (i & 1 ? 0 : 8);
				sects[i].track = track;
				sects[i].head = head;
				sects[i].sector = j+1;
				sects[i].size = mfm ? 1 : 0;
				sects[i].actual_size = mfm ? 256 : 128;
				sects[i].data = sectdata + 256*j;
				sects[i].deleted = false;
				sects[i].bad_data_crc = false;
				sects[i].bad_addr_crc = false;
				sects[i].weak = false;
			}

			if(mfm)
				build_wd_track_mfm(track * track_step, head, image, 100000, 16, sects, 50, 32, 22);
			else
				build_wd_track_fm(track * track_step, head, image, 50000, 16, sects, 24, 16, 11);
		}

	image.set_form_variant(floppy_image::FF_525, track_count == 80 ? floppy_image::DSQD : floppy_image::DSDD);
	return true;
}

bool m20_format::save(util::random_read_write &io, const std::vector<uint32_t> &variants, const floppy_image &image) const
{
	int track_count, head_count;
	image.get_actual_geometry(track_count, head_count);
	int const track_step = (track_count >= 69 && track_count <= 79 && image.get_variant() == floppy_image::DSDD) ? 2 : 1;
	if (track_step == 2)
	{
		// Do not discard data written between the tracks of a 48 TPI disk.
		for (int track = 1; track < 69; track += 2)
			for (int head = 0; head < head_count; head++)
				if (image.track_is_formatted(track, head))
					return false;
		track_count = 35;
	}
	// Native 320 KB formatting also writes the spare/protection cylinders
	// 35-39.  The traditional raw format stores only the first 35 cylinders.
	else if (track_count >= 35 && track_count <= 40)
		track_count = 35;
	if ((track_count != 35 && track_count != 80) || head_count != 2)
		return false;

	// Validate every sector before writing.  Raw images cannot represent
	// missing sectors or different sector sizes.  Pad the FM sectors to 256 bytes.
	std::vector<uint8_t> data(track_count * head_count * 16 * 256, 0);
	for (int track = 0; track < track_count; track++) {
		for (int head = 0; head < head_count; head++) {
			bool const mfm = track || head;
			auto const bitstream = generate_bitstream_from_track(track * track_step, head, mfm ? 2000 : 4000, image);
			auto const sectors = mfm ? extract_sectors_from_bitstream_mfm_pc(bitstream) : extract_sectors_from_bitstream_fm_pc(bitstream);
			unsigned const sector_size = mfm ? 256 : 128;

			for (int i = 0; i < 16; i++) {
				if (sectors.size() <= i + 1 || sectors[i + 1].size() != sector_size)
					return false;
				std::copy(sectors[i + 1].begin(), sectors[i + 1].end(), data.begin() + 256 * (16 * (track * head_count + head) + i));
			}
		}
	}

	auto const [err, actual] = write_at(io, 0, data.data(), data.size());
	return !err && actual == data.size();
}

const m20_format FLOPPY_M20_FORMAT;
