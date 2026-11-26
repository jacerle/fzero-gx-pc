#include "assets/avlz.hpp"
#include "assets/gma_model.hpp"
#include "assets/gx_texture.hpp"
#include "assets/u8_arc.hpp"
#include "audio/adx_decoder.hpp"
#include "core/config.hpp"
#include "disc/disc_image.hpp"
#include "disc/rvz_backend.hpp"
#include "disc/sha1.hpp"
#include "fzgx_port/mwcc_shim.h"
#include "fzgx_port/platform_api.h"
#include "game/dol_rel_loader.hpp"
#include "input/input_manager.hpp"
#include "save/gci_card.hpp"
#include "video/gx_state.hpp"

#include <zstd.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void write_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[1] = static_cast<uint8_t>(v & 0xFFU);
}

void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[3] = static_cast<uint8_t>(v & 0xFFU);
}

void write_be24(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>(v & 0xFFU);
}

void test_sha1() {
    const std::string empty = fzgx::Sha1::hash_bytes_hex(nullptr, 0);
    assert(empty == "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    const char* msg = "abc";
    const std::string abc = fzgx::Sha1::hash_bytes_hex(reinterpret_cast<const uint8_t*>(msg), 3);
    assert(abc == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

void test_synthetic_iso_fst_and_mod_override(const std::filesystem::path& tmp) {
    // Build minimal synthetic GameCube ISO:
    // 0x0000: header (GFZE01, magic 0xC2339F3D, title, FST at 0x500)
    // 0x0500: FST with root + dir "sys" + file "sys/boot.txt" + file "hello.bin"
    // 0x0700+: file payloads
    std::vector<uint8_t> iso(0x1000, 0);
    std::memcpy(iso.data() + 0x00, "GFZE01", 6);
    write_be32(iso.data() + 0x1C, fzgx::kGameCubeMagic);
    std::memcpy(iso.data() + 0x20, "F-ZERO GX TEST", 14);
    write_be32(iso.data() + 0x420, 0x0000); // DOL unused here
    write_be32(iso.data() + 0x424, 0x0500); // FST offset

    // FST: 4 entries (root, dir "data", file "data/track.dat", file "readme.txt")
    const uint32_t entry_count = 4;
    const uint32_t fst_off = 0x500;
    const std::string strs = std::string("data\0track.dat\0readme.txt\0", 26);
    const uint32_t fst_size = entry_count * 12 + static_cast<uint32_t>(strs.size());
    write_be32(iso.data() + 0x428, fst_size);

    // Entry 0: root dir (next_idx = 4)
    iso[fst_off + 0] = 1;
    write_be32(iso.data() + fst_off + 8, entry_count);
    // Entry 1: dir "data" (name_off = 0, parent = 0, next_idx = 3)
    iso[fst_off + 12 + 0] = 1;
    write_be24(iso.data() + fst_off + 12 + 1, 0);
    write_be32(iso.data() + fst_off + 12 + 4, 0);
    write_be32(iso.data() + fst_off + 12 + 8, 3);
    // Entry 2: file "track.dat" (name_off = 5, off = 0x800, size = 11)
    iso[fst_off + 24 + 0] = 0;
    write_be24(iso.data() + fst_off + 24 + 1, 5);
    write_be32(iso.data() + fst_off + 24 + 4, 0x800);
    write_be32(iso.data() + fst_off + 24 + 8, 11);
    // Entry 3: file "readme.txt" (name_off = 15, off = 0x900, size = 5)
    iso[fst_off + 36 + 0] = 0;
    write_be24(iso.data() + fst_off + 36 + 1, 15);
    write_be32(iso.data() + fst_off + 36 + 4, 0x900);
    write_be32(iso.data() + fst_off + 36 + 8, 5);

    std::memcpy(iso.data() + fst_off + entry_count * 12, strs.data(), strs.size());
    std::memcpy(iso.data() + 0x800, "MUTE_CITY_1", 11);
    std::memcpy(iso.data() + 0x900, "GFZE0", 5);

    const auto iso_path = tmp / "test_gfze01.iso";
    {
        std::ofstream out(iso_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(iso.data()), static_cast<std::streamsize>(iso.size()));
    }

    const auto mod_dir = tmp / "mods";
    std::filesystem::create_directories(mod_dir / "data");
    {
        std::ofstream mod_out(mod_dir / "data" / "track.dat", std::ios::binary);
        mod_out << "BIG_BLUE_MOD";
    }

    fzgx::DiscImage disc;
    std::string err;
    assert(disc.mount(iso_path, mod_dir, &err));
    assert(disc.header().is_fzero_gx_usa);
    assert(disc.header().game_id == "GFZE01");
    assert(disc.file_count() == 3);

    std::vector<uint8_t> buf;
    bool from_mod = false;
    assert(disc.read_file("readme.txt", buf, &from_mod));
    assert(!from_mod);
    assert(std::string(buf.begin(), buf.end()) == "GFZE0");

    assert(disc.read_file("data/track.dat", buf, &from_mod));
    assert(from_mod);
    assert(std::string(buf.begin(), buf.end()) == "BIG_BLUE_MOD");
}

void write_be64(uint8_t* p, uint64_t v) {
    write_be32(p, static_cast<uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFFu));
}

void write_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void write_f32_be(uint8_t* p, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_be32(p, bits);
}

std::vector<uint8_t> zstd_compress_vec(const std::vector<uint8_t>& src) {
    const size_t bound = ZSTD_compressBound(src.size());
    std::vector<uint8_t> out(bound);
    const size_t n = ZSTD_compress(out.data(), out.size(), src.data(), src.size(), 3);
    assert(!ZSTD_isError(n));
    out.resize(n);
    return out;
}

void test_rvz_zstd_and_prng_packing(const std::filesystem::path& tmp) {
    // Construct a 96 KiB (3 x 32 KiB chunks) logical GameCube ISO:
    // Chunk 0 (0x00000..0x08000): Boot header + FST + readme.txt + cross-chunk boundary start
    // Chunk 1 (0x08000..0x10000): RVZ-packed PRNG junk data (Lagged Fibonacci) + raw tail
    // Chunk 2 (0x10000..0x18000): Raw data ("RVZ_ZSTD_OK" at 0x10000)
    constexpr uint32_t kChunkSize = 0x8000;
    constexpr uint64_t kIsoSize = 0x18000;
    std::vector<uint8_t> iso(kIsoSize, 0);

    std::memcpy(iso.data() + 0x00, "GFZE01", 6);
    write_be32(iso.data() + 0x1C, fzgx::kGameCubeMagic);
    std::memcpy(iso.data() + 0x20, "F-ZERO GX RVZ TEST", 18);
    write_be32(iso.data() + 0x420, 0x0600); // DOL offset
    write_be32(iso.data() + 0x424, 0x0500); // FST offset

    // FST: 2 entries (root + file "bgm/mute.adx" at 0x10000 len 11)
    const uint32_t fst_off = 0x500;
    const std::string strs = std::string("bgm\0mute.adx\0", 13);
    const uint32_t entry_count = 3;
    const uint32_t fst_size = entry_count * 12 + static_cast<uint32_t>(strs.size());
    write_be32(iso.data() + 0x428, fst_size);

    iso[fst_off + 0] = 1;
    write_be32(iso.data() + fst_off + 8, entry_count);
    iso[fst_off + 12 + 0] = 1;
    write_be24(iso.data() + fst_off + 12 + 1, 0);
    write_be32(iso.data() + fst_off + 12 + 8, 3);
    iso[fst_off + 24 + 0] = 0;
    write_be24(iso.data() + fst_off + 24 + 1, 4);
    write_be32(iso.data() + fst_off + 24 + 4, 0x10000);
    write_be32(iso.data() + fst_off + 24 + 8, 11);
    std::memcpy(iso.data() + fst_off + entry_count * 12, strs.data(), strs.size());

    // Minimal DOL at 0x600
    write_be32(iso.data() + 0x600 + 0x00, 0x100);
    write_be32(iso.data() + 0x600 + 0x48, 0x80003100u);
    write_be32(iso.data() + 0x600 + 0x90, 0x20);
    write_be32(iso.data() + 0x600 + 0xE0, 0x80003100u);

    // Build Chunk 1 PRNG seed (68 bytes) and populate iso[0x8000..0x10000)
    uint8_t prng_seed[68]{};
    for (size_t i = 0; i < 68; ++i) {
        prng_seed[i] = static_cast<uint8_t>(0xA5u ^ (i * 13u));
    }
    {
        fzgx::RvzPrng prng;
        prng.seed(prng_seed);
        prng.generate_bytes(iso.data() + 0x8000, kChunkSize);
    }

    // Chunk 2 payload
    std::memcpy(iso.data() + 0x10000, "RVZ_ZSTD_OK", 11);

    const std::string expected_iso_sha1 = fzgx::Sha1::hash_bytes_hex(iso.data(), iso.size());

    // Pack Chunk 0 raw (zstd)
    std::vector<uint8_t> c0(iso.begin(), iso.begin() + kChunkSize);
    std::vector<uint8_t> c0_z = zstd_compress_vec(c0);

    // Pack Chunk 1 with RVZ packing (MSB set + kChunkSize + 68-byte seed), then Zstd
    std::vector<uint8_t> c1_packed(4 + 68, 0);
    write_be32(c1_packed.data(), 0x80000000u | kChunkSize);
    std::memcpy(c1_packed.data() + 4, prng_seed, 68);
    std::vector<uint8_t> c1_z = zstd_compress_vec(c1_packed);

    // Pack Chunk 2 raw (zstd)
    std::vector<uint8_t> c2(iso.begin() + 0x10000, iso.begin() + 0x18000);
    std::vector<uint8_t> c2_z = zstd_compress_vec(c2);

    // wia_raw_data_t: 1 entry starting at 0x80, size (kIsoSize - 0x80) to test 0x8000 alignment rule!
    std::vector<uint8_t> raw_tbl(24, 0);
    write_be64(raw_tbl.data() + 0x00, 0x80);
    write_be64(raw_tbl.data() + 0x08, kIsoSize - 0x80);
    write_be32(raw_tbl.data() + 0x10, 0);
    write_be32(raw_tbl.data() + 0x14, 3);
    std::vector<uint8_t> raw_tbl_z = zstd_compress_vec(raw_tbl);

    // Assemble RVZ file layout with 4-byte aligned group data offsets
    constexpr uint32_t kHeadSize = 0x48;
    constexpr uint32_t kDiscSize = 0xDC;
    std::vector<uint8_t> rvz(kHeadSize + kDiscSize, 0);

    auto append_aligned4 = [&](const std::vector<uint8_t>& blob) -> uint32_t {
        while ((rvz.size() % 4) != 0) rvz.push_back(0);
        const uint32_t off = static_cast<uint32_t>(rvz.size());
        rvz.insert(rvz.end(), blob.begin(), blob.end());
        return off;
    };

    const uint32_t c0_off = append_aligned4(c0_z);
    const uint32_t c1_off = append_aligned4(c1_z);
    const uint32_t c2_off = append_aligned4(c2_z);

    // rvz_group_t table (3 x 12 bytes)
    std::vector<uint8_t> grp_tbl(36, 0);
    write_be32(grp_tbl.data() + 0, c0_off / 4);
    write_be32(grp_tbl.data() + 4, 0x80000000u | static_cast<uint32_t>(c0_z.size()));
    write_be32(grp_tbl.data() + 8, 0);

    write_be32(grp_tbl.data() + 12, c1_off / 4);
    write_be32(grp_tbl.data() + 16, 0x80000000u | static_cast<uint32_t>(c1_z.size()));
    write_be32(grp_tbl.data() + 20, static_cast<uint32_t>(c1_packed.size()));

    write_be32(grp_tbl.data() + 24, c2_off / 4);
    write_be32(grp_tbl.data() + 28, 0x80000000u | static_cast<uint32_t>(c2_z.size()));
    write_be32(grp_tbl.data() + 32, 0);

    std::vector<uint8_t> grp_tbl_z = zstd_compress_vec(grp_tbl);
    const uint32_t raw_tbl_off = append_aligned4(raw_tbl_z);
    const uint32_t grp_tbl_off = append_aligned4(grp_tbl_z);

    // Fill wia_file_head_t
    std::memcpy(rvz.data() + 0x00, "RVZ\x01", 4);
    write_be32(rvz.data() + 0x04, 0x01000000u);
    write_be32(rvz.data() + 0x08, 0x00090000u);
    write_be32(rvz.data() + 0x0C, kDiscSize);
    write_be64(rvz.data() + 0x24, kIsoSize);
    write_be64(rvz.data() + 0x2C, rvz.size());

    // Fill wia_disc_t at 0x48
    uint8_t* wd = rvz.data() + kHeadSize;
    write_be32(wd + 0x00, 1); // GameCube
    write_be32(wd + 0x04, 5); // Zstandard
    write_be32(wd + 0x08, 3);
    write_be32(wd + 0x0C, kChunkSize);
    std::memcpy(wd + 0x10, iso.data(), 0x80); // dhead
    write_be32(wd + 0x90, 0);                 // n_part
    write_be32(wd + 0xB4, 1);                 // n_raw_data
    write_be64(wd + 0xB8, raw_tbl_off);
    write_be32(wd + 0xC0, static_cast<uint32_t>(raw_tbl_z.size()));
    write_be32(wd + 0xC4, 3); // n_groups
    write_be64(wd + 0xC8, grp_tbl_off);
    write_be32(wd + 0xD0, static_cast<uint32_t>(grp_tbl_z.size()));

    const auto rvz_path = tmp / "test_gfze01.rvz";
    {
        std::ofstream out(rvz_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rvz.data()), static_cast<std::streamsize>(rvz.size()));
    }

    fzgx::DiscImage disc;
    std::string err;
    assert(disc.mount(rvz_path, {}, &err));
    assert(disc.header().container_format == "RVZ");
    assert(disc.header().compression_name == "Zstandard");
    assert(disc.header().is_fzero_gx_usa);
    assert(disc.header().is_supported_for_mount);
    assert(disc.header().logical_iso_size == kIsoSize);

    std::vector<uint8_t> fdata;
    assert(disc.read_file("bgm/mute.adx", fdata));
    assert(std::string(fdata.begin(), fdata.end()) == "RVZ_ZSTD_OK");

    std::vector<uint8_t> dol;
    assert(disc.read_dol(dol));
    fzgx::DolInfo dol_info{};
    assert(fzgx::parse_dol_header(dol, dol_info));
    assert(dol_info.entry_point == 0x80003100u);
    assert(dol_info.total_code_bytes == 0x20);

    std::string logical_sha1;
    assert(disc.compute_logical_sha1(logical_sha1));
    assert(logical_sha1 == expected_iso_sha1);
}

void test_adx_decoder() {
    // Build a valid CRI ADX v3 stereo 44100 Hz file with 2 blocks (64 samples) + loop
    constexpr uint16_t kHeaderBytes = 0x34; // 52 bytes -> data_offset = 52
    constexpr uint32_t kTotalSamples = 64;
    std::vector<uint8_t> adx(kHeaderBytes + 2 * 2 * 18, 0);

    write_be16(adx.data() + 0x00, 0x8000u);
    write_be16(adx.data() + 0x02, kHeaderBytes - 4);
    adx[0x04] = 3; // ADX
    adx[0x05] = 18;
    adx[0x06] = 4;
    adx[0x07] = 2; // Stereo
    write_be32(adx.data() + 0x08, 44100);
    write_be32(adx.data() + 0x0C, kTotalSamples);
    write_be16(adx.data() + 0x10, 500);
    adx[0x12] = 3; // Version 3
    write_be32(adx.data() + 0x18, 1);  // Loop enabled
    write_be32(adx.data() + 0x1C, 0);  // Loop start
    write_be32(adx.data() + 0x24, 48); // Loop end
    std::memcpy(adx.data() + kHeaderBytes - 6, "(c)CRI", 6);

    // Populate non-zero scale and nibbles in block 0 & 1
    for (size_t blk = 0; blk < 4; ++blk) {
        uint8_t* p = adx.data() + kHeaderBytes + blk * 18;
        write_be16(p, 0x0100);
        for (size_t i = 2; i < 18; ++i) {
            p[i] = 0x35;
        }
    }

    fzgx::AdxDecoder dec;
    std::string err;
    assert(dec.open(adx, &err));
    assert(dec.info().sample_rate == 44100);
    assert(dec.info().channels == 2);
    assert(dec.info().has_loop);

    // Decode 256 stereo frames at 48 kHz (forces loop wraparound multiple times)
    std::vector<float> pcm(256 * 2, 0.0f);
    const size_t got = dec.read_frames_stereo_f32(pcm.data(), 256, 48000, true);
    assert(got == 256);
    bool has_nonzero = false;
    for (float s : pcm) {
        assert(std::isfinite(s));
        assert(s >= -1.0f && s <= 1.0f);
        if (std::fabs(s) > 1e-5f) has_nonzero = true;
    }
    assert(has_nonzero);
}

void test_dol_and_rel_parsers() {
    std::vector<uint8_t> rel(0x80, 0);
    write_be32(rel.data() + 0x00, 7);    // Module ID
    write_be32(rel.data() + 0x0C, 3);    // 3 sections
    write_be32(rel.data() + 0x10, 0x40); // section_info_offset
    write_be32(rel.data() + 0x1C, 3);    // version
    write_be32(rel.data() + 0x20, 0x1200);
    rel[0x30] = 1;
    write_be32(rel.data() + 0x34, 0x20);
    // Section 1: exec at 0x60 size 0x20
    write_be32(rel.data() + 0x40 + 8, 0x60u | 1u);
    write_be32(rel.data() + 0x40 + 12, 0x20u);

    fzgx::RelInfo rel_info{};
    assert(fzgx::parse_rel_header(rel, rel_info));
    assert(rel_info.id == 7);
    assert(rel_info.num_sections == 3);
    assert(rel_info.bss_size == 0x1200);
    assert(rel_info.sections[1].is_exec);
    assert(rel_info.sections[1].offset == 0x60);
    assert(rel_info.sha1_hex.size() == 40);
}

void test_gci_memory_card(const std::filesystem::path& tmp) {
    const auto slot_a = tmp / "slot_a";
    fzgx::GciCardManager card;
    assert(card.mount_slot(0, slot_a));

    const std::vector<uint8_t> save_payload = {0x46, 0x5A, 0x47, 0x58, 0x01, 0x02, 0x03};
    assert(card.write_gci_file(0, "GFZE", "8P", "fzerogx.dat", save_payload));
    assert(card.files(0).size() == 1);
    assert(card.files(0)[0].game_code == "GFZE");
    assert(card.files(0)[0].company == "8P");
    assert(card.files(0)[0].file_name == "fzerogx.dat");
    assert(card.files(0)[0].block_count == 1);

    std::vector<uint8_t> read_back;
    assert(card.read_gci_payload(0, "fzerogx.dat", read_back));
    assert(read_back.size() == fzgx::GciCardManager::kBlockSize);
    assert(std::memcmp(read_back.data(), save_payload.data(), save_payload.size()) == 0);

    assert(card.delete_gci_file(0, "fzerogx.dat"));
    assert(card.files(0).empty());
}

void test_config_roundtrip(const std::filesystem::path& tmp) {
    const auto cfg_path = tmp / "config.json";
    auto cfg = fzgx::make_default_config();
    cfg.rom_path = "/tmp/gfze01.iso";
    cfg.internal_resolution_scale = 4;
    cfg.aspect_ratio = fzgx::AspectRatioMode::Ratio21_9;
    cfg.hud_mode = fzgx::HudLayoutMode::EdgeAnchored;
    cfg.target_fps = 120;
    cfg.vsync = fzgx::VsyncMode::Mailbox;
    cfg.shader_backend = fzgx::ShaderBackendMode::Librashader;
    cfg.librashader_params["CRT_GAMMA"] = 2.4f;
    cfg.fsr_enabled = true;
    cfg.fsr_sharpness = 0.88f;
    cfg.controllers[0].trigger_digital_threshold = 0.78f;

    assert(fzgx::save_config(cfg_path, cfg));

    fzgx::AppConfig loaded;
    assert(fzgx::load_config(cfg_path, loaded));
    assert(loaded.rom_path == cfg.rom_path);
    assert(loaded.internal_resolution_scale == 4);
    assert(loaded.aspect_ratio == fzgx::AspectRatioMode::Ratio21_9);
    assert(loaded.hud_mode == fzgx::HudLayoutMode::EdgeAnchored);
    assert(loaded.target_fps == 120);
    assert(loaded.vsync == fzgx::VsyncMode::Mailbox);
    assert(loaded.shader_backend == fzgx::ShaderBackendMode::Librashader);
    assert(std::fabs(loaded.librashader_params.at("CRT_GAMMA") - 2.4f) < 1e-4f);
    assert(loaded.fsr_enabled == true);
    assert(std::fabs(loaded.fsr_sharpness - 0.88f) < 1e-4f);
    assert(std::fabs(loaded.controllers[0].trigger_digital_threshold - 0.78f) < 1e-4f);
}

void test_input_calibration_math() {
    float nx = 0.0f, ny = 0.0f;
    int8_t gx = 0, gy = 0;

    // Inside deadzone -> zero
    fzgx::shape_stick(0.05f, -0.05f, 0.15f, 0.95f, nx, ny, gx, gy);
    assert(nx == 0.0f && ny == 0.0f && gx == 0 && gy == 0);

    // At outer gate -> full 127
    fzgx::shape_stick(0.95f, 0.0f, 0.15f, 0.95f, nx, ny, gx, gy);
    assert(gx == 127 && gy == 0);

    // Dual-stage trigger: light press gives analog without digital click
    float nt = 0.0f;
    uint8_t ga = 0;
    bool click = false;
    fzgx::shape_trigger(0.50f, 0.05f, 0.85f, nt, ga, click);
    assert(ga > 100 && !click);

    // Full pull triggers digital click
    fzgx::shape_trigger(0.90f, 0.05f, 0.85f, nt, ga, click);
    assert(ga > 220 && click);
}

void test_mwcc_shims() {
    static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
    static_assert(sizeof(s32) == 4 && sizeof(f32) == 4 && sizeof(f64) == 8);

    const uint8_t be_bytes[4] = {0x12, 0x34, 0x56, 0x78};
    assert(fzgx_be32(be_bytes) == 0x12345678u);
    assert(fzgx_bswap16(0xABCDu) == 0xCDABu);

    const float a[3] = {1.0f, 2.0f, 3.0f};
    const float b[3] = {4.0f, 5.0f, 6.0f};
    float c[3]{};
    fzgx_psvec_add(a, b, c);
    assert(c[0] == 5.0f && c[1] == 7.0f && c[2] == 9.0f);
    assert(fzgx_rlwinm(0x80000001u, 1, 30, 31) == 0x00000003u);
}

void test_avlz_decompress() {
    // Encode "ABCDABCDAAAA" using AV-LZSS:
    // Ring starts at 0xFEE (zeroed).
    // 1. 4 literals: 'A','B','C','D' -> written at 0xFEE..0xFF1 (flag bits 0..3 = 1)
    // 2. Back-ref to 0xFEE len 4 (n-3=1): b0=0xEE, b1=0xF1 (bit 4 = 0) -> outputs "ABCD" at 0xFF2..0xFF5
    // 3. Literal 'A' at 0xFF6 (bit 5 = 1)
    // 4. Overlapping RLE-style back-ref to 0xFF6 len 3 (n-3=0): b0=0xF6, b1=0xF0 (bit 6 = 0) -> outputs "AAA"
    // Flags byte = 0b00101111 = 0x2F
    std::vector<uint8_t> lz = {
        0, 0, 0, 0, // comp_size filled below
        12, 0, 0, 0, // uncomp_size = 12
        0x2F,
        'A', 'B', 'C', 'D',
        0xEE, 0xF1,
        'A',
        0xF6, 0xF0,
    };
    write_le32(lz.data(), static_cast<uint32_t>(lz.size() - 8));

    std::vector<uint8_t> out;
    std::string err;
    assert(fzgx::is_avlz_buffer(lz));
    assert(fzgx::decompress_avlz(lz, out, &err));
    assert(std::string(out.begin(), out.end()) == "ABCDABCDAAAA");

    // Truncated & size mismatch error handling
    std::vector<uint8_t> bad_trunc = lz;
    bad_trunc.pop_back();
    assert(!fzgx::decompress_avlz(bad_trunc, out, &err));

    std::vector<uint8_t> bad_sz = lz;
    write_le32(bad_sz.data() + 4, 99);
    assert(!fzgx::decompress_avlz(bad_sz, out, &err));
}

void test_u8_arc_parser() {
    // U8 archive with root(0), dir "models"(1), file "models/car.gma"(2), file "readme.txt"(3)
    const std::string strs = std::string("\0models\0car.gma\0readme.txt\0", 27);
    constexpr uint32_t kFstOff = 0x20;
    constexpr uint32_t kEntries = 4;
    const uint32_t fst_sz = kEntries * 12 + static_cast<uint32_t>(strs.size());
    const uint32_t data_off = (kFstOff + fst_sz + 31u) & ~31u;

    std::vector<uint8_t> arc(data_off + 64, 0);
    write_be32(arc.data() + 0x00, fzgx::U8Archive::kMagic);
    write_be32(arc.data() + 0x04, kFstOff);
    write_be32(arc.data() + 0x08, fst_sz);
    write_be32(arc.data() + 0x0C, data_off);

    // Entry 0: root
    write_be32(arc.data() + kFstOff + 0x00, 0x01000000u);
    write_be32(arc.data() + kFstOff + 0x08, kEntries);
    // Entry 1: dir "models" (name_off=1, end=3)
    write_be32(arc.data() + kFstOff + 12 + 0x00, 0x01000001u);
    write_be32(arc.data() + kFstOff + 12 + 0x08, 3);
    // Entry 2: file "car.gma" (name_off=8, off=data_off, size=8)
    write_be32(arc.data() + kFstOff + 24 + 0x00, 8);
    write_be32(arc.data() + kFstOff + 24 + 0x04, data_off);
    write_be32(arc.data() + kFstOff + 24 + 0x08, 8);
    // Entry 3: file "readme.txt" (name_off=16, off=data_off+16, size=5)
    write_be32(arc.data() + kFstOff + 36 + 0x00, 16);
    write_be32(arc.data() + kFstOff + 36 + 0x04, data_off + 16);
    write_be32(arc.data() + kFstOff + 36 + 0x08, 5);

    std::memcpy(arc.data() + kFstOff + kEntries * 12, strs.data(), strs.size());
    std::memcpy(arc.data() + data_off, "GCMF_CAR", 8);
    std::memcpy(arc.data() + data_off + 16, "HELLO", 5);

    fzgx::U8Archive u8;
    std::string err;
    assert(u8.load(arc, false, &err));
    assert(u8.file_count() == 2);

    std::vector<uint8_t> fdata;
    assert(u8.read_file("models/car.gma", fdata));
    assert(std::string(fdata.begin(), fdata.end()) == "GCMF_CAR");
    assert(u8.read_file("readme.txt", fdata));
    assert(std::string(fdata.begin(), fdata.end()) == "HELLO");

    // Invalid magic rejected
    arc[0] = 0;
    assert(!u8.load(arc, false, &err));
}

void test_gx_texture_decoders() {
    std::vector<uint8_t> out;

    // 1. I4 (8x8 tile = 32 bytes): nibble 0xF -> 255, nibble 0x0 -> 0
    std::vector<uint8_t> i4(32, 0xF0);
    assert(fzgx::decode_gx_texture(i4, 4, 4, fzgx::GxTexFmt::I4, out));
    assert(out[0] == 255 && out[3] == 255);
    assert(out[4] == 0 && out[7] == 255);

    // 2. I8 (8x4 tile = 32 bytes)
    std::vector<uint8_t> i8(32, 0x7A);
    assert(fzgx::decode_gx_texture(i8, 4, 4, fzgx::GxTexFmt::I8, out));
    assert(out[0] == 0x7A && out[1] == 0x7A && out[3] == 255);

    // 3. IA4 (8x4 tile = 32 bytes): high nibble A=0xA->0xAA, low nibble I=0x3->0x33
    std::vector<uint8_t> ia4(32, 0xA3);
    assert(fzgx::decode_gx_texture(ia4, 4, 4, fzgx::GxTexFmt::IA4, out));
    assert(out[0] == 0x33 && out[3] == 0xAA);

    // 4. IA8 (4x4 tile = 32 bytes): A=0x80, I=0x40
    std::vector<uint8_t> ia8(32, 0);
    ia8[0] = 0x80; ia8[1] = 0x40;
    assert(fzgx::decode_gx_texture(ia8, 4, 4, fzgx::GxTexFmt::IA8, out));
    assert(out[0] == 0x40 && out[3] == 0x80);

    // 5. RGB565 (4x4 tile = 32 bytes): pure red 0xF800
    std::vector<uint8_t> rgb565(32, 0);
    write_be16(rgb565.data(), 0xF800u);
    assert(fzgx::decode_gx_texture(rgb565, 4, 4, fzgx::GxTexFmt::RGB565, out));
    assert(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255);

    // 6. RGB5A3 (4x4 tile = 32 bytes): pixel 0 opaque green (0x83E0), pixel 1 alpha blue (0x400F)
    std::vector<uint8_t> rgb5a3(32, 0);
    write_be16(rgb5a3.data() + 0, 0x83E0u);
    write_be16(rgb5a3.data() + 2, 0x400Fu);
    assert(fzgx::decode_gx_texture(rgb5a3, 4, 4, fzgx::GxTexFmt::RGB5A3, out));
    assert(out[0] == 0 && out[1] == 255 && out[2] == 0 && out[3] == 255);
    assert(out[4] == 0 && out[5] == 0 && out[6] == 255 && out[7] < 255);

    // 7. RGBA8 (4x4 tile = 64 bytes: 32B AR, 32B GB)
    std::vector<uint8_t> rgba8(64, 0);
    rgba8[0] = 0xCC; rgba8[1] = 0x11; // A=0xCC, R=0x11
    rgba8[32] = 0x22; rgba8[33] = 0x33; // G=0x22, B=0x33
    assert(fzgx::decode_gx_texture(rgba8, 4, 4, fzgx::GxTexFmt::RGBA8, out));
    assert(out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33 && out[3] == 0xCC);

    // 8. CMPR (8x8 tile = 32 bytes): sub-block 0 with c0=0xF800 (red) > c1=0x001F (blue), MSB-first row 0 = 0b00_01_10_11 (0x1B)
    std::vector<uint8_t> cmpr(32, 0);
    write_be16(cmpr.data() + 0, 0xF800u);
    write_be16(cmpr.data() + 2, 0x001Fu);
    cmpr[4] = 0x1Bu; // px0=0 (red), px1=1 (blue), px2=2 (2/3 red + 1/3 blue), px3=3
    assert(fzgx::decode_gx_texture(cmpr, 4, 4, fzgx::GxTexFmt::CMPR, out));
    assert(out[0] == 255 && out[2] == 0 && out[3] == 255);
    assert(out[4] == 0 && out[6] == 255 && out[7] == 255);
    assert(out[8] > 150 && out[10] > 70 && out[11] == 255);

    // CMPR transparent mode (c0 <= c1): index 3 must be (0,0,0,0)
    write_be16(cmpr.data() + 0, 0x001Fu);
    write_be16(cmpr.data() + 2, 0xF800u);
    cmpr[4] = 0xFFu; // all index 3
    assert(fzgx::decode_gx_texture(cmpr, 4, 4, fzgx::GxTexFmt::CMPR, out));
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);

    // 9. Amusement Vision .tpl (2 entries: slot 0 empty 0x100, slot 1 4x4 RGBA8)
    std::vector<uint8_t> av_tpl(4 + 32 + 64, 0);
    write_be32(av_tpl.data() + 0, 2);
    write_be32(av_tpl.data() + 4, 0x100u); // slot 0 empty
    write_be32(av_tpl.data() + 20, 0x06u); // slot 1 RGBA8
    write_be32(av_tpl.data() + 24, 36u);
    write_be16(av_tpl.data() + 28, 4);
    write_be16(av_tpl.data() + 30, 4);
    av_tpl[36] = 0xFF; av_tpl[37] = 0xAA;
    fzgx::TplArchive tpl;
    assert(tpl.load(av_tpl, false));
    assert(tpl.textures().size() == 2);
    assert(!tpl.textures()[0].valid);
    assert(tpl.textures()[1].valid);
    assert(tpl.textures()[1].rgba8[0] == 0xAA && tpl.textures()[1].rgba8[3] == 0xFF);

    // 10. Nintendo 0x0020AF30 .tpl
    std::vector<uint8_t> nin_tpl(12 + 8 + 36 + 32, 0);
    write_be32(nin_tpl.data() + 0, fzgx::TplArchive::kNintendoTplMagic);
    write_be32(nin_tpl.data() + 4, 1);
    write_be32(nin_tpl.data() + 8, 12);
    write_be32(nin_tpl.data() + 12, 20); // img_hdr_off
    write_be16(nin_tpl.data() + 20, 4);  // height
    write_be16(nin_tpl.data() + 22, 4);  // width
    write_be32(nin_tpl.data() + 24, 4);  // RGB565
    write_be32(nin_tpl.data() + 28, 56); // data_off
    write_be16(nin_tpl.data() + 56, 0x07E0u); // pure green
    assert(tpl.load(nin_tpl, false));
    assert(tpl.is_nintendo_tpl());
    assert(tpl.valid_texture_count() == 1);
    assert(tpl.textures()[0].rgba8[1] == 255);
}

void test_gma_and_gx_display_list() {
    // Build a GMA archive with 2 slots:
    // Slot 0 = 0xFFFFFFFF (empty)
    // Slot 1 = valid GCMF with 1 layer (tpl=2), 1 opaque mesh (VTXFMT0 TRIANGLES+QUADS),
    //          1 transparent mesh (VTXFMT1 TRIANGLESTRIP)
    std::vector<uint8_t> dl0;
    auto push_u8 = [&](std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); };
    auto push_be16 = [&](std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v & 0xFFu));
    };
    auto push_f32 = [&](std::vector<uint8_t>& b, float v) {
        uint8_t tmp[4];
        write_f32_be(tmp, v);
        b.insert(b.end(), tmp, tmp + 4);
    };

    // Opaque DL (VTXFMT0): GX_TRIANGLES (0x90), 3 verts with POS(9)+NRM(10)+TEX0(13)
    push_u8(dl0, 0x90u);
    push_be16(dl0, 3);
    for (int i = 0; i < 3; ++i) {
        push_f32(dl0, static_cast<float>(i)); push_f32(dl0, 0.0f); push_f32(dl0, 0.0f); // pos
        push_f32(dl0, 0.0f); push_f32(dl0, 1.0f); push_f32(dl0, 0.0f); // nrm
        push_f32(dl0, 0.5f); push_f32(dl0, 0.25f); // uv0
    }
    // Also a QUAD (0x80), 4 verts -> 2 triangles (total 3 tris)
    push_u8(dl0, 0x80u);
    push_be16(dl0, 4);
    for (int i = 0; i < 4; ++i) {
        push_f32(dl0, 1.0f); push_f32(dl0, static_cast<float>(i)); push_f32(dl0, 0.0f);
        push_f32(dl0, 0.0f); push_f32(dl0, 0.0f); push_f32(dl0, 1.0f);
        push_f32(dl0, 0.0f); push_f32(dl0, 1.0f);
    }

    // Transparent DL (VTXFMT1): GX_TRIANGLESTRIP (0x99), 4 verts -> 2 triangles
    std::vector<uint8_t> dl1;
    push_u8(dl1, 0x99u);
    push_be16(dl1, 4);
    for (int i = 0; i < 4; ++i) {
        push_be16(dl1, static_cast<uint16_t>(8192)); // x = 1.0
        push_be16(dl1, static_cast<uint16_t>(i * 4096)); // y
        push_be16(dl1, 0); // z
        push_be16(dl1, 0); push_be16(dl1, 16384); push_be16(dl1, 0); // nrm (0,1,0)
        push_be16(dl1, 4096); push_be16(dl1, 4096); // uv (0.5, 0.5)
    }

    // Build GCMF blob
    const uint32_t layer_off = 0x40;
    const uint32_t mesh_off = layer_off + 0x20;
    const uint32_t m0_hdr = mesh_off;
    const uint32_t m0_dl = m0_hdr + 0x60;
    const uint32_t m1_hdr = m0_dl + static_cast<uint32_t>(dl0.size());
    const uint32_t m1_dl = m1_hdr + 0x60;
    const uint32_t gcmf_sz = m1_dl + static_cast<uint32_t>(dl1.size());

    std::vector<uint8_t> gcmf(gcmf_sz, 0);
    write_be32(gcmf.data() + 0x00, fzgx::GmaArchive::kGcmfMagic);
    write_be32(gcmf.data() + 0x04, 0); // standard display-list model
    write_f32_be(gcmf.data() + 0x14, 5.0f);
    write_be16(gcmf.data() + 0x18, 1); // 1 layer
    write_be16(gcmf.data() + 0x1A, 1); // 1 opaque
    write_be16(gcmf.data() + 0x1C, 1); // 1 transparent
    write_be32(gcmf.data() + 0x20, mesh_off);

    // Layer 0: repeat S/T, tpl_index = 2
    write_be32(gcmf.data() + layer_off + 0x00, (1u << 2) | (1u << 4));
    write_be16(gcmf.data() + layer_off + 0x04, 2);

    const uint32_t vtx_mask = (1u << 9) | (1u << 10) | (1u << 13);
    // Mesh 0 (opaque)
    write_be32(gcmf.data() + m0_hdr + 0x00, 0x02u); // double sided
    gcmf[m0_hdr + 0x11] = 255;
    gcmf[m0_hdr + 0x12] = 1; // 1 layer
    gcmf[m0_hdr + 0x13] = 1; // dl0 present
    write_be16(gcmf.data() + m0_hdr + 0x16, 0);
    write_be32(gcmf.data() + m0_hdr + 0x1C, vtx_mask);
    write_be32(gcmf.data() + m0_hdr + 0x28, static_cast<uint32_t>(dl0.size()));
    std::memcpy(gcmf.data() + m0_dl, dl0.data(), dl0.size());

    // Mesh 1 (transparent)
    write_be32(gcmf.data() + m1_hdr + 0x00, 0x80u); // mat rgb
    gcmf[m1_hdr + 0x04] = 255; gcmf[m1_hdr + 0x05] = 128; gcmf[m1_hdr + 0x06] = 0;
    gcmf[m1_hdr + 0x11] = 128;
    gcmf[m1_hdr + 0x12] = 0;
    gcmf[m1_hdr + 0x13] = 1;
    write_be32(gcmf.data() + m1_hdr + 0x1C, vtx_mask);
    write_be32(gcmf.data() + m1_hdr + 0x28, static_cast<uint32_t>(dl1.size()));
    std::memcpy(gcmf.data() + m1_dl, dl1.data(), dl1.size());

    // Wrap into GMA container: 2 entries (slot 0 empty, slot 1 = "BlueFalcon")
    const std::string names = std::string("BlueFalcon\0", 11);
    const uint32_t data_base = 8u + 16u + static_cast<uint32_t>(names.size());
    std::vector<uint8_t> gma_file(data_base + gcmf.size(), 0);
    write_be32(gma_file.data() + 0x00, 2);
    write_be32(gma_file.data() + 0x04, data_base);
    write_be32(gma_file.data() + 0x08, 0xFFFFFFFFu);
    write_be32(gma_file.data() + 0x0C, 0);
    write_be32(gma_file.data() + 0x10, 0);
    write_be32(gma_file.data() + 0x14, 0);
    std::memcpy(gma_file.data() + 24, names.data(), names.size());
    std::memcpy(gma_file.data() + data_base, gcmf.data(), gcmf.size());

    fzgx::GmaArchive gma;
    std::string err;
    assert(gma.load(gma_file, false, &err));
    assert(gma.models().size() == 2);
    assert(!gma.models()[0].valid);
    assert(gma.models()[1].valid);
    assert(gma.models()[1].name == "BlueFalcon");
    assert(gma.models()[1].submeshes.size() == 2);

    const auto& sm0 = gma.models()[1].submeshes[0];
    assert(!sm0.is_transparent);
    assert(sm0.double_sided);
    assert(sm0.primary_tpl_index == 2);
    assert(sm0.wrap_s == fzgx::GxWrapMode::Repeat);
    assert(sm0.indices.size() == 9); // 1 tri + 1 quad (2 tris) = 3 tris = 9 indices
    assert(std::fabs(sm0.vertices[1].pos[0] - 1.0f) < 1e-4f);

    const auto& sm1 = gma.models()[1].submeshes[1];
    assert(sm1.is_transparent);
    assert(sm1.blend_mode == fzgx::GxBlendMode::AlphaBlend);
    assert(sm1.indices.size() == 6); // 4-vert strip = 2 tris = 6 indices
    assert(std::fabs(sm1.vertices[0].pos[0] - 1.0f) < 1e-3f);
    assert(std::fabs(sm1.vertices[0].normal[1] - 1.0f) < 1e-3f);
    assert(std::fabs(sm1.vertices[0].uv0[0] - 0.5f) < 1e-3f);
}

void test_gx_hor_plus_projection() {
    const auto p43 = fzgx::gx_make_perspective_mtx(60.0f, 4.0f / 3.0f, 1.0f, 100.0f);
    const auto vk169 = fzgx::gx_build_vulkan_projection(p43.data(), false, 16.0f / 9.0f, true, 4.0f / 3.0f);

    // Column-major: [0] is X scale (widened by (4/3)/(16/9) = 0.75), [5] is negated Y scale
    assert(std::fabs(vk169[0] - p43[0] * 0.75f) < 1e-5f);
    assert(std::fabs(vk169[5] + p43[5]) < 1e-5f);

    // Verify Vulkan depth [0, 1] at z_eye = -near (-1.0) and z_eye = -far (-100.0)
    auto proj_z = [&](const std::array<float, 16>& m, float z_eye) {
        const float z_clip = m[10] * z_eye + m[14];
        const float w_clip = m[11] * z_eye + m[15];
        return z_clip / w_clip;
    };
    assert(std::fabs(proj_z(vk169, -1.0f) - 0.0f) < 1e-5f);
    assert(std::fabs(proj_z(vk169, -100.0f) - 1.0f) < 1e-5f);

    // Ortho projection must NOT be horizontally stretched
    const auto ortho = fzgx::gx_make_ortho_mtx(240.0f, -240.0f, -320.0f, 320.0f, 0.0f, 100.0f);
    const auto vk_ortho = fzgx::gx_build_vulkan_projection(ortho.data(), true, 21.0f / 9.0f, true, 4.0f / 3.0f);
    assert(std::fabs(vk_ortho[0] - ortho[0]) < 1e-6f);
    assert(std::fabs(proj_z(vk_ortho, 0.0f) - 0.0f) < 1e-5f);
    assert(std::fabs(proj_z(vk_ortho, -100.0f) - 1.0f) < 1e-5f);
}

void test_gx_tev_evaluator_and_fifo() {
    fzgx::GxTevConfig cfg = fzgx::make_default_tev_config(true, true);
    cfg.num_stages = 2;
    cfg.konst_regs[0] = {0.2f, 0.1f, 0.0f, 1.0f};
    cfg.swap_table[1] = {2, 1, 0, 3}; // swap R <-> B
    cfg.stages[0].tex_swap = 1;
    cfg.stages[1].kcolor_sel = 12; // K0
    cfg.stages[1].color_in = {
        static_cast<uint8_t>(fzgx::GxTevColorArg::CPrev),
        static_cast<uint8_t>(fzgx::GxTevColorArg::Zero),
        static_cast<uint8_t>(fzgx::GxTevColorArg::Zero),
        static_cast<uint8_t>(fzgx::GxTevColorArg::Konst),
    };
    cfg.stages[1].alpha_in = {
        static_cast<uint8_t>(fzgx::GxTevAlphaArg::Zero),
        static_cast<uint8_t>(fzgx::GxTevAlphaArg::Zero),
        static_cast<uint8_t>(fzgx::GxTevAlphaArg::Zero),
        static_cast<uint8_t>(fzgx::GxTevAlphaArg::APrev),
    };
    cfg.alpha_comp0 = fzgx::GxCompare::GEqual;
    cfg.alpha_ref0 = 0.5f;

    std::array<std::array<float, 4>, 4> tex{};
    tex[0] = {0.0f, 0.5f, 1.0f, 0.8f}; // swapped -> R=1.0, G=0.5, B=0.0, A=0.8
    std::array<float, 4> out{};
    const bool pass = fzgx::evaluate_tev_cpu(cfg, {0.5f, 1.0f, 1.0f, 1.0f}, tex, out);
    assert(pass);
    assert(std::fabs(out[0] - 0.7f) < 1e-4f); // 0.5*1.0 + 0.2
    assert(std::fabs(out[1] - 0.6f) < 1e-4f); // 1.0*0.5 + 0.1
    assert(std::fabs(out[2] - 0.0f) < 1e-4f);
    assert(std::fabs(out[3] - 0.8f) < 1e-4f);

    // Alpha compare rejects below ref0
    tex[0][3] = 0.25f;
    assert(!fzgx::evaluate_tev_cpu(cfg, {1.0f, 1.0f, 1.0f, 1.0f}, tex, out));

    // Test live GX state processor: FIFO draw + EFB copy + primitive draw
    fzgx::GxStateProcessor gx;
    gx.begin_frame(1280, 720, 16.0f / 9.0f, true);
    gx.set_vtx_desc((1u << 9), false); // POS only (F32)

    std::vector<uint8_t> fifo;
    auto push_be32 = [&](uint32_t v) {
        fifo.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
        fifo.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        fifo.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        fifo.push_back(static_cast<uint8_t>(v & 0xFFu));
    };
    auto push_f32 = [&](float f) {
        uint32_t b = 0;
        std::memcpy(&b, &f, sizeof(b));
        push_be32(b);
    };
    // BP Fog Color = (16, 32, 48)
    fifo.push_back(0x61u);
    push_be32(0xF2102030u);
    // GX_TRIANGLES (0x90) x 3 verts
    fifo.push_back(0x90u);
    fifo.push_back(0x00u);
    fifo.push_back(0x03u);
    for (int i = 0; i < 3; ++i) {
        push_f32(static_cast<float>(i));
        push_f32(0.0f);
        push_f32(-5.0f);
    }
    assert(gx.submit_fifo_bytes(fifo));

    fzgx::GxEfbCopyCommand efb{};
    efb.target_efb_id = fzgx::GxStateProcessor::kEfbTexIdBase;
    efb.dst_w = 320;
    efb.dst_h = 180;
    gx.request_efb_copy(efb);

    gx.begin_primitive(0x80u, true, false); // QUAD
    gx.push_vertex({{-1, -1, -2}, {}, {1, 1, 1, 1}, {0, 0}, {}});
    gx.push_vertex({{ 1, -1, -2}, {}, {1, 1, 1, 1}, {1, 0}, {}});
    gx.push_vertex({{ 1,  1, -2}, {}, {1, 1, 1, 1}, {1, 1}, {}});
    gx.push_vertex({{-1,  1, -2}, {}, {1, 1, 1, 1}, {0, 1}, {}});
    gx.end_primitive();
    gx.end_frame();

    const auto& frame = gx.completed_frame();
    assert(frame.efb_copy_count == 1);
    assert(frame.items.size() == 3);
    assert(frame.items[0].type == fzgx::GxCommandItem::Type::Draw);
    assert(frame.items[1].type == fzgx::GxCommandItem::Type::EfbCopy);
    assert(frame.items[2].type == fzgx::GxCommandItem::Type::Draw);
    assert(frame.items[0].draw.index_count == 3);
    assert(frame.items[2].draw.index_count == 6);
    assert(std::fabs(frame.items[0].draw.tev.fog_color[0] - (16.0f / 255.0f)) < 1e-4f);
}

void test_gma_multilayer_tev_generation() {
    fzgx::GmaMaterialLayer l0{};
    l0.tpl_index = 0;
    fzgx::GmaMaterialLayer l1{};
    l1.tpl_index = 1;
    l1.is_env_map = true;
    fzgx::GmaSubMesh sm{};
    sm.primary_tpl_index = 0;
    sm.secondary_tpl_index = 1;
    sm.blend_mode = fzgx::GxBlendMode::Cutout;

    const auto tev = fzgx::build_gma_submesh_tev(sm, {l0, l1});
    assert(tev.num_stages == 2);
    assert(tev.alpha_comp0 == fzgx::GxCompare::GEqual);
    assert(tev.stages[1].tex_map == 1);
    assert(tev.stages[1].tex_coord == 1);
}

} // namespace

int main() {
    const auto tmp = std::filesystem::temp_directory_path() / "fzgx_pc_unit_tests";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    test_sha1();
    test_synthetic_iso_fst_and_mod_override(tmp);
    test_rvz_zstd_and_prng_packing(tmp);
    test_adx_decoder();
    test_dol_and_rel_parsers();
    test_gci_memory_card(tmp);
    test_config_roundtrip(tmp);
    test_input_calibration_math();
    test_mwcc_shims();
    test_avlz_decompress();
    test_u8_arc_parser();
    test_gx_texture_decoders();
    test_gma_and_gx_display_list();
    test_gx_hor_plus_projection();
    test_gx_tev_evaluator_and_fifo();
    test_gma_multilayer_tev_generation();

    std::filesystem::remove_all(tmp);
    std::cout << "All fzgx_tests passed.\n";
    return 0;
}
