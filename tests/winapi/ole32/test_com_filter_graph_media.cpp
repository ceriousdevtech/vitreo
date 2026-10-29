// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_filter_graph_media.cpp
/// @brief DirectShow filter graph rendering, seeking and media-control tests.

#include "com_test_common.hpp"
#include "com_test_filter_graph_fixture.hpp"

TEST_F(FilterGraphTest, BasicVideoGetCurrentImageReturnsDib) {
    // Learn IBasicVideo::GetCurrentImage: NULL image pointer returns the
    // required DIB size; a second call fills BITMAPINFOHEADER + pixels.
    char temp_template[] = "/tmp/vitreo_getimage_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.2:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IBASICVIDEO);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto video_qi = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(video_qi), com::S_OK);
    const u32 video = read_u32(QI_OUT_ADDR);

    static constexpr GuestAddr SIZE_ADDR = DATA_BASE + 0x280;
    (void)memory_.write_u32(SIZE_ADDR, 0);
    set_args({video, SIZE_ADDR, 0});
    auto size_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_basic_video(size_marshaler, 29), com::S_OK);
    const u32 need = read_u32(SIZE_ADDR);
    EXPECT_EQ(need, 40u + 16u * 16u * 4u);

    static constexpr GuestAddr DIB_ADDR = DATA_BASE + 0x400;
    set_args({video, SIZE_ADDR, DIB_ADDR});
    auto image_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_basic_video(image_marshaler, 29), com::S_OK);
    EXPECT_EQ(read_u32(DIB_ADDR), 40u);
    EXPECT_EQ(read_u32(DIB_ADDR + 4), 16u);
    EXPECT_EQ(read_u32(DIB_ADDR + 8), 16u);
    EXPECT_EQ(read_u32(DIB_ADDR + 12) & 0xFFFFu, 1u);
    EXPECT_EQ((read_u32(DIB_ADDR + 12) >> 16) & 0xFFFFu, 32u);
    // Bottom-up BGR DIB: last row is the top of the picture (lavfi red).
    // yuv420p round-trip is not exact 255/0/0.
    const u32 top_pixel = read_u32(DIB_ADDR + 40 + 15u * 16u * 4u);
    EXPECT_LT(top_pixel & 0xFFu, 16u);
    EXPECT_LT((top_pixel >> 8) & 0xFFu, 16u);
    EXPECT_GT((top_pixel >> 16) & 0xFFu, 240u);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, MediaSeekingGetDurationAfterRenderFile) {
    char temp_template[] = "/tmp/vitreo_seeking_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IMEDIASEEKING);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(qi_marshaler), com::S_OK);
    const u32 seeking = read_u32(QI_OUT_ADDR);

    static constexpr GuestAddr DUR_ADDR = DATA_BASE + 0x280;
    ASSERT_TRUE(memory_.write_u64(DUR_ADDR, 0).is_ok());
    set_args({seeking, DUR_ADDR});
    auto duration_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_seeking_get_duration(duration_marshaler),
              com::S_OK);
    const u64 duration = memory_.read_u64(DUR_ADDR).value_or(0);
    EXPECT_GE(duration, 3'000'000u);
    EXPECT_LE(duration, 6'000'000u);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, MediaControlGetStateOkWhenVideoRendererRunning) {
    // Video-only files fail the audio decoder. GetState must still be S_OK
    // once IMediaControl::Run has a connected video renderer.
    char temp_template[] = "/tmp/vitreo_runstate_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(qi_marshaler), com::S_OK);
    const u32 media = read_u32(QI_OUT_ADDR);

    set_args({media});
    auto run_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_control_run(run_marshaler), com::S_OK);

    set_args({media, 0, STATE_ADDR});
    auto state_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_control_get_state(state_marshaler),
              com::S_OK);
    EXPECT_EQ(read_u32(STATE_ADDR), 2u);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, MediaControlRunOkForAudioOnly) {
    char temp_template[] = "/tmp/vitreo_audio_run_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "tone.mp3";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i sine=frequency=440:duration=0.4 \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test audio clip";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\tone.mp3").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IMEDIACONTROL);
    set_args({graph, QI_IID_ADDR, QI_OUT_ADDR});
    auto qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_query_interface(qi_marshaler), com::S_OK);
    const u32 media = read_u32(QI_OUT_ADDR);

    set_args({media});
    auto run_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_control_run(run_marshaler), com::S_OK);

    set_args({media, 0, STATE_ADDR});
    auto state_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.filter_graph_media_control_get_state(state_marshaler),
              com::S_OK);
    EXPECT_EQ(read_u32(STATE_ADDR), 2u);

    std::filesystem::remove_all(root);
}

TEST_F(FilterGraphTest, AddFilterConnectsVideoToGuestRenderer) {
    // Learn IFilterGraph::AddFilter + RenderFile: a renderer already in the
    // graph is connected via IPin::ReceiveConnection with the file's video
    // type. Width/height live in VIDEOINFOHEADER; apps read them from the
    // filter after RenderFile, not from IBasicVideo.
    char temp_template[] = "/tmp/vitreo_addfilter_test_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.2:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    constexpr GuestAddr FILTER = DATA_BASE + 0x800;
    constexpr GuestAddr FILTER_VT = DATA_BASE + 0x880;
    constexpr GuestAddr ENUM_OBJ = DATA_BASE + 0x900;
    constexpr GuestAddr ENUM_VT = DATA_BASE + 0x980;
    constexpr GuestAddr PIN = DATA_BASE + 0xA00;
    constexpr GuestAddr PIN_VT = DATA_BASE + 0xA80;
    constexpr GuestAddr NAME = DATA_BASE + 0xB00;
    constexpr GuestAddr FILTER_ENUM_PINS = 0x00410010;
    constexpr GuestAddr FILTER_JOIN = 0x00410020;
    constexpr GuestAddr FILTER_ADDREF = 0x00410030;
    constexpr GuestAddr FILTER_RELEASE = 0x00410040;
    constexpr GuestAddr ENUM_NEXT = 0x00410050;
    constexpr GuestAddr ENUM_RELEASE = 0x00410060;
    constexpr GuestAddr PIN_RECEIVE = 0x00410070;
    constexpr GuestAddr PIN_DIRECTION = 0x00410080;
    constexpr GuestAddr PIN_RELEASE = 0x00410090;
    constexpr GuestAddr PIN_QI = 0x004100A0;

    ASSERT_TRUE(memory_.write_u32(FILTER, FILTER_VT).is_ok());
    ASSERT_TRUE(memory_.write_u32(FILTER_VT + 4, FILTER_ADDREF).is_ok());
    ASSERT_TRUE(memory_.write_u32(FILTER_VT + 8, FILTER_RELEASE).is_ok());
    ASSERT_TRUE(memory_.write_u32(FILTER_VT + 40, FILTER_ENUM_PINS).is_ok());
    ASSERT_TRUE(memory_.write_u32(FILTER_VT + 52, FILTER_JOIN).is_ok());
    ASSERT_TRUE(memory_.write_u32(ENUM_OBJ, ENUM_VT).is_ok());
    ASSERT_TRUE(memory_.write_u32(ENUM_VT + 8, ENUM_RELEASE).is_ok());
    ASSERT_TRUE(memory_.write_u32(ENUM_VT + 12, ENUM_NEXT).is_ok());
    ASSERT_TRUE(memory_.write_u32(PIN, PIN_VT).is_ok());
    ASSERT_TRUE(memory_.write_u32(PIN_VT + 0, PIN_QI).is_ok());
    ASSERT_TRUE(memory_.write_u32(PIN_VT + 8, PIN_RELEASE).is_ok());
    ASSERT_TRUE(memory_.write_u32(PIN_VT + 16, PIN_RECEIVE).is_ok());
    ASSERT_TRUE(memory_.write_u32(PIN_VT + 36, PIN_DIRECTION).is_ok());

    bool joined = false;
    bool received = false;
    u32 received_width = 0;
    u32 received_height = 0;
    u32 received_bitcount = 0;
    mgr_.set_guest_call(
        [&](GuestAddr method, const u64* args, usize count, bool is_64bit) -> u64 {
            EXPECT_FALSE(is_64bit);
            if (method == FILTER_ADDREF || method == FILTER_RELEASE ||
                method == ENUM_RELEASE || method == PIN_RELEASE) {
                return 1;
            }
            if (method == FILTER_JOIN) {
                EXPECT_GE(count, 2u);
                joined = args[1] != 0;
                return com::S_OK;
            }
            if (method == FILTER_ENUM_PINS) {
                EXPECT_EQ(count, 2u);
                EXPECT_TRUE(memory_.write_u32(args[1], ENUM_OBJ).is_ok());
                return com::S_OK;
            }
            if (method == ENUM_NEXT) {
                EXPECT_GE(count, 3u);
                EXPECT_TRUE(memory_.write_u32(args[2], PIN).is_ok());
                if (count > 3 && args[3] != 0) {
                    EXPECT_TRUE(memory_.write_u32(args[3], 1).is_ok());
                }
                return com::S_OK;
            }
            if (method == PIN_DIRECTION) {
                EXPECT_EQ(count, 2u);
                EXPECT_TRUE(memory_.write_u32(args[1], 0).is_ok());
                return com::S_OK;
            }
            if (method == PIN_RECEIVE) {
                EXPECT_EQ(count, 3u);
                const GuestAddr mt = static_cast<GuestAddr>(args[2]);
                // A video renderer: MEDIATYPE_Video only (the File Source's
                // MEDIATYPE_Stream is offered first and refused).
                if (memory_.read_u32(mt).value_or(0) != 0x73646976u) return 0x8004022Au;
                auto format = memory_.read_u32(mt + 68);
                EXPECT_TRUE(format.is_ok());
                if (format.is_ok()) {
                    received_width =
                        memory_.read_u32(format.value() + 52).value_or(0);
                    received_height =
                        memory_.read_u32(format.value() + 56).value_or(0);
                    const u32 planes_bits =
                        memory_.read_u32(format.value() + 60).value_or(0);
                    received_bitcount = planes_bits >> 16;
                }
                received = true;
                return com::S_OK;
            }
            if (method == PIN_QI) {
                if (count >= 3) {
                    EXPECT_TRUE(memory_.write_u32(args[2], 0).is_ok());
                }
                return 0x80004002u;
            }
            return com::S_OK;
        });

    write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
    write_guid(IID_ADDR, IID_IGRAPHBUILDER);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 graph = read_u32(OUT_ADDR);

    auto name_marshaler = make_marshaler();
    ASSERT_TRUE(
        name_marshaler.write_wide_string(NAME, "Texture Renderer").is_ok());
    set_args({graph, FILTER, NAME});
    auto add_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_add_filter(add_marshaler), com::S_OK);
    EXPECT_TRUE(joined);

    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    auto path_marshaler = make_marshaler();
    ASSERT_TRUE(
        path_marshaler.write_wide_string(PATH_ADDR, "C:\\clip.mp4").is_ok());
    set_args({graph, PATH_ADDR, 0});
    auto render_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.filter_graph_render_file(render_marshaler), com::S_OK);
    EXPECT_TRUE(received);
    EXPECT_EQ(received_width, 16u);
    EXPECT_EQ(received_height, 16u);
    EXPECT_TRUE(received_bitcount == 12u || received_bitcount == 32u);

    std::filesystem::remove_all(root);
}

// Both bitnesses: the AM_MEDIA_TYPE / VIDEOINFOHEADER / PIN_INFO layouts the
// graph writes and the calls it makes into a guest renderer. Offsets and
// sizes are the Windows SDK's, measured with MSVC on winoracle
// (tools/oracle_probe/dshow_layout_probe{32,64}.expected):
//   AM_MEDIA_TYPE  x86 72 bytes, pUnk 60, cbFormat 64, pbFormat 68
//                  x64 88 bytes, pUnk 64, cbFormat 72, pbFormat 80
//   VIDEOINFOHEADER 88 both (AvgTimePerFrame 40, bmiHeader 48)
//   BITMAPINFOHEADER biWidth 4, biHeight 8, biPlanes 12, biBitCount 14,
//                  biCompression 16, biSizeImage 20
//   PIN_INFO       x86 264 bytes (dir 4, achName 8), x64 272 (dir 8, achName 12)
TEST_P(FilterGraphBitnessTest, GuestRendererConnectionUsesSdkLayouts) {
    char temp_template[] = "/tmp/vitreo_addfilter_bitness_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::filesystem::path root = temp_dir;
    const std::filesystem::path host_clip = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.2:r=10 -an -pix_fmt yuv420p \"" +
        host_clip.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr_.set_vfs(&vfs);

    const bool x64 = x64_;
    const u32 mt_size = x64 ? 88u : 72u;
    const u32 mt_punk = x64 ? 64u : 60u;
    const u32 mt_cb = x64 ? 72u : 64u;
    const u32 mt_pb = x64 ? 80u : 68u;

    const GuestAddr FILTER = filter_obj(0);
    const GuestAddr ENUM = filter_obj(1);
    const GuestAddr PIN = filter_obj(2);
    GuestAddr output_pin = 0;
    GuestAddr offered_format = 0;
    u32 got_width = 0, got_height = 0, got_bits = 0, got_planes = 0;
    u32 got_cb = 0, got_sample = 0, got_image = 0, got_compression = 0;
    u64 got_punk = 1;
    Guid got_major{};
    usize run_args = 0;
    i32 pin_refs = 1;
    mgr_.set_guest_call([&](GuestAddr fn, const u64* args, usize count,
                            bool is64) -> u64 {
        EXPECT_EQ(is64, x64);
        const u32 slot = static_cast<u32>((fn - FILTER_FN) / 0x10u);
        const GuestAddr self = static_cast<GuestAddr>(args[0]);
        if (slot == 1) return self == PIN ? static_cast<u64>(++pin_refs) : 2;
        if (slot == 2) return self == PIN ? static_cast<u64>(--pin_refs) : 1;
        if (self == FILTER) {
            if (slot == 13) return 0;  // JoinFilterGraph
            if (slot == 10) {          // EnumPins(IEnumPins**)
                EXPECT_EQ(count, 2u);
                wptr(static_cast<GuestAddr>(args[1]), ENUM);
                return 0;
            }
            if (slot == 6) {  // Run(REFERENCE_TIME)
                run_args = count;
                return 0;
            }
            return 0;
        }
        if (self == ENUM && slot == 3) {  // Next(1, IPin**, ULONG*)
            EXPECT_EQ(count, 4u);
            EXPECT_EQ(args[1], 1u);
            wptr(static_cast<GuestAddr>(args[2]), PIN);
            if (args[3] != 0) (void)memory_.write_u32(static_cast<GuestAddr>(args[3]), 1);
            return 0;
        }
        if (self == PIN) {
            if (slot == 0) {  // QueryInterface(IMemInputPin) -> none
                wptr(static_cast<GuestAddr>(args[2]), 0);
                return 0x80004002u;
            }
            if (slot == 9) {  // QueryDirection(PIN_DIRECTION*) -> PINDIR_INPUT
                (void)memory_.write_u32(static_cast<GuestAddr>(args[1]), 0);
                return 0;
            }
            if (slot == 11) return 0;  // QueryAccept
            if (slot == 4) {           // ReceiveConnection(IPin*, const AM_MEDIA_TYPE*)
                EXPECT_EQ(count, 3u);
                const GuestAddr mt = static_cast<GuestAddr>(args[2]);
                if (memory_.read_u32(mt).value_or(0) != 0x73646976u) return 0x8004022Au;
                output_pin = static_cast<GuestAddr>(args[1]);
                (void)memory_.read_bytes(mt, reinterpret_cast<Byte*>(&got_major), 16);
                got_sample = memory_.read_u32(mt + 40).value_or(0);
                got_punk = ptr(mt + mt_punk);
                got_cb = memory_.read_u32(mt + mt_cb).value_or(0);
                offered_format = static_cast<GuestAddr>(ptr(mt + mt_pb));
                const GuestAddr bmi = offered_format + 48;
                EXPECT_EQ(memory_.read_u32(bmi + 0).value_or(0), 40u);
                got_width = memory_.read_u32(bmi + 4).value_or(0);
                got_height = memory_.read_u32(bmi + 8).value_or(0);
                got_planes = memory_.read_u16(bmi + 12).value_or(0);
                got_bits = memory_.read_u16(bmi + 14).value_or(0);
                got_compression = memory_.read_u32(bmi + 16).value_or(0);
                got_image = memory_.read_u32(bmi + 20).value_or(0);
                return 0;
            }
        }
        return 0;
    });

    const GuestAddr graph = create_graph();
    put_wide(NAME_ADDR, u"Frontier Texture Renderer");
    ASSERT_EQ(invoke(&ComCoreManager::filter_graph_add_filter, {graph, FILTER, NAME_ADDR}),
              com::S_OK);
    static constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
    put_wide(PATH_ADDR, u"C:\\clip.mp4");
    ASSERT_EQ(invoke(&ComCoreManager::filter_graph_render_file, {graph, PATH_ADDR, 0}),
              com::S_OK);

    // MEDIATYPE_Video {73646976-0000-0010-8000-00AA00389B71}.
    EXPECT_EQ(got_major.Data1, 0x73646976u);
    EXPECT_EQ(got_punk, 0u);
    EXPECT_EQ(got_cb, 88u);  // sizeof(VIDEOINFOHEADER)
    ASSERT_NE(offered_format, 0u);
    EXPECT_EQ(got_width, 16u);
    EXPECT_EQ(got_height, 16u);
    EXPECT_EQ(got_planes, 1u);
    ASSERT_TRUE(got_bits == 12u || got_bits == 32u);
    // YV12 FOURCC, or BI_RGB (0) for RGB32.
    EXPECT_EQ(got_compression, got_bits == 12u ? 0x32315659u : 0u);
    EXPECT_EQ(got_image, got_bits == 12u ? 16u * 16u * 3u / 2u : 16u * 16u * 4u);
    EXPECT_EQ(got_sample, got_image);

    // The graph's output pin: {vtable*}{18 pointer-width IPin slots}.
    ASSERT_NE(output_pin, 0u);
    const GuestAddr pin_vt = static_cast<GuestAddr>(ptr(output_pin));
    EXPECT_EQ(pin_vt, output_pin + ps());
    EXPECT_EQ(ptr(pin_vt + 3 * ps()), thunk("Vitreo_FilterGraph_OutputPin_Connect"));
    EXPECT_EQ(ptr(pin_vt + 8 * ps()), thunk("Vitreo_FilterGraph_OutputPin_QueryPinInfo"));
    EXPECT_EQ(ptr(pin_vt + 17 * ps()), thunk("Vitreo_FilterGraph_OutputPin_NewSegment"));

    const auto pin_op = &ComCoreManager::filter_graph_output_pin;
    // ConnectedTo(IPin**): pointer-width, AddRef'd.
    constexpr GuestAddr OUT_PIN = DATA_BASE + 0x3000;
    (void)memory_.write_u64(OUT_PIN, 0xCAFEBABECAFEBABEull);
    const i32 refs_before = pin_refs;
    EXPECT_EQ(invoke_op(pin_op, 3, {output_pin, OUT_PIN}), com::S_OK);
    EXPECT_EQ(ptr(OUT_PIN), PIN);
    EXPECT_EQ(memory_.read_u32(OUT_PIN + 4).value_or(1), x64 ? 0u : 0xCAFEBABEu);
    EXPECT_EQ(pin_refs, refs_before + 1);

    // ConnectionMediaType(AM_MEDIA_TYPE*): the caller's layout, and a
    // pbFormat it owns (a copy, not the graph's block).
    constexpr GuestAddr MT_OUT = DATA_BASE + 0x3100;
    for (u32 i = 0; i < 0x80; i += 4) (void)memory_.write_u32(MT_OUT + i, 0xEEEEEEEEu);
    EXPECT_EQ(invoke_op(pin_op, 4, {output_pin, MT_OUT}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(MT_OUT).value_or(0), 0x73646976u);
    EXPECT_EQ(ptr(MT_OUT + mt_punk), 0u);
    EXPECT_EQ(memory_.read_u32(MT_OUT + mt_cb).value_or(0), 88u);
    const GuestAddr copy = static_cast<GuestAddr>(ptr(MT_OUT + mt_pb));
    EXPECT_NE(copy, 0u);
    EXPECT_NE(copy, offered_format);
    EXPECT_EQ(memory_.read_u32(copy + 48 + 4).value_or(0), 16u);
    EXPECT_EQ(memory_.read_u32(MT_OUT + mt_size).value_or(0), 0xEEEEEEEEu);

    // QueryPinInfo: no IBaseFilter for the source pin -> E_NOTIMPL with the
    // whole PIN_INFO (and nothing past it) cleared.
    constexpr GuestAddr INFO = DATA_BASE + 0x3200;
    const u32 info_size = x64 ? 272u : 264u;
    for (u32 i = 0; i < 0x120; i += 4) (void)memory_.write_u32(INFO + i, 0xEEEEEEEEu);
    EXPECT_EQ(invoke_op(pin_op, 5, {output_pin, INFO}), 0x80004001u);
    EXPECT_EQ(ptr(INFO), 0u);
    EXPECT_EQ(memory_.read_u32(INFO + info_size - 4).value_or(1), 0u);
    EXPECT_EQ(memory_.read_u32(INFO + info_size).value_or(0), 0xEEEEEEEEu);

    // IVideoWindow put_Owner / get_Owner: OAHWND is a LONG_PTR. (op is the
    // index after IDispatch: put_Owner 22, get_Owner 23.)
    const GuestAddr video = qi(graph, IID_IVIDEOWINDOW);
    const u64 owner = x64 ? 0x0000001200345678ull : 0x00345678ull;
    EXPECT_EQ(invoke_op(&ComCoreManager::filter_graph_video_window, 22, {video, owner}),
              com::S_OK);
    constexpr GuestAddr OWNER_OUT = DATA_BASE + 0x3400;
    (void)memory_.write_u64(OWNER_OUT, 0xCAFEBABECAFEBABEull);
    EXPECT_EQ(invoke_op(&ComCoreManager::filter_graph_video_window, 23, {video, OWNER_OUT}),
              com::S_OK);
    EXPECT_EQ(ptr(OWNER_OUT), owner);
    EXPECT_EQ(memory_.read_u32(OWNER_OUT + 4).value_or(1), x64 ? 0x12u : 0xCAFEBABEu);
    (void)invoke_op(&ComCoreManager::filter_graph_video_window, 22, {video, 0});

    // IMediaControl::Run -> IMediaFilter::Run(REFERENCE_TIME): one argument
    // slot on x64, two stack dwords on x86.
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}),
              com::S_OK);
    EXPECT_EQ(run_args, x64 ? 2u : 3u);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}),
              com::S_OK);

    std::filesystem::remove_all(root);
}

// ---------------------------------------------------------------------------
// Streaming thread. A CBaseRenderer-like fake: Pause returns S_FALSE until a
// sample arrives; Receive blocks while paused (the preroll) until Run or
// Stop; GetState(ms) waits up to ms for the preroll. Graph behaviour expected
// from Windows (tools/oracle_probe/dshow_state_probe, dshow_complete_probe,
// dshow_sample_probe) and Learn (IMediaControl::Pause / GetState, IMemAllocator,
// IPin::EndOfStream, CBaseOutputPin::Active).
// ---------------------------------------------------------------------------
class FilterGraphStreamingTest : public FilterGraphBitnessTest {
protected:
    static constexpr GuestAddr THREAD_STACKS = 0x00300000;  // 0x1000 each
    static constexpr GuestAddr SAMPLE_BUFFER = DATA_BASE + 0x8000;

    enum Obj : u32 { kFilter = 0, kEnum, kPin, kMemIn, kAlloc, kSample, kMisc };

    void SetUp() override {
        FilterGraphBitnessTest::SetUp();
        ASSERT_TRUE(memory_.map_region(THREAD_STACKS, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        main_thread_ = std::this_thread::get_id();
        stream_entry_ = thunk_internal("Vitreo_FilterGraph_StreamThread");
        run_entry_ = thunk_internal("Vitreo_FilterGraph_RunWorker");
        mgr_.set_audio_activation_thread([this](GuestAddr entry, GuestAddr param) {
            return launch(entry, param);
        });
        mgr_.set_guest_call([this](GuestAddr fn, const u64* args, usize count,
                                   bool is64) -> u64 {
            EXPECT_EQ(is64, x64_);
            return guest(fn, args, count);
        });
    }

    void TearDown() override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            renderer_state_ = 0;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        FilterGraphBitnessTest::TearDown();
    }

    GuestAddr thunk_internal(std::string_view name) {
        auto address = registry_.get_thunk_address("ole32.dll", name, true);
        EXPECT_TRUE(address.is_ok()) << name;
        return address.is_ok() ? address.value() : 0;
    }

    bool launch(GuestAddr entry, GuestAddr param) {
        const bool stream = entry == stream_entry_;
        if (!stream && entry != run_entry_) {
            ADD_FAILURE() << "unexpected thread entry";
            return false;
        }
        const u32 index = thread_index_++;
        if (index >= 16) return false;
        ++live_threads_;
        threads_.emplace_back([this, stream, param, index] {
            cpu::CpuState cpu;
            if (x64_) {
                cpu.init_long_mode();
                cpu.gpr.rcx = param;
                cpu.set_sp(THREAD_STACKS + index * 0x1000u + 0x800u);
            } else {
                cpu.init_protected_mode();
                const GuestAddr sp = THREAD_STACKS + index * 0x1000u + 0x800u;
                cpu.set_sp(sp);
                (void)memory_.write_u32(sp, 0xDEADBEEFu);
                (void)memory_.write_u32(sp + 4, static_cast<u32>(param));
            }
            Marshaler m(memory_);
            m.attach_cpu(cpu);
            if (stream) {
                (void)mgr_.filter_graph_stream_thread(m);
            } else {
                (void)mgr_.filter_graph_run_worker(m);
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                --live_threads_;
            }
            cv_.notify_all();
        });
        return true;
    }

    /// The fake guest objects (one vtable, methods FILTER_FN + slot * 0x10).
    u64 guest(GuestAddr fn, const u64* args, usize count) {
        const u32 slot = static_cast<u32>((fn - FILTER_FN) / 0x10u);
        const GuestAddr self = static_cast<GuestAddr>(args[0]);
        const u32 obj = static_cast<u32>((self - FILTER_OBJS) / 0x100u);
        std::unique_lock<std::mutex> lock(mu_);
        if (slot == 1) return 2;
        if (slot == 2) return 1;
        const bool on_main = std::this_thread::get_id() == main_thread_;
        switch (obj) {
        case kFilter:
            if (slot == 0) {  // QI: IAMFilterMiscFlags
                const Guid iid = read_guid(static_cast<GuestAddr>(args[1]));
                if (iid.Data1 == 0x2DD74950u) {
                    wptr(static_cast<GuestAddr>(args[2]), filter_obj(kMisc));
                    return 0;
                }
                wptr(static_cast<GuestAddr>(args[2]), 0);
                return 0x80004002u;
            }
            if (slot == 13) return 0;  // JoinFilterGraph
            if (slot == 10) {          // EnumPins
                wptr(static_cast<GuestAddr>(args[1]), filter_obj(kEnum));
                return 0;
            }
            if (slot == 4) {  // Stop
                renderer_state_ = 0;
                has_sample_ = false;
                ++stops_;
                cv_.notify_all();
                return 0;
            }
            if (slot == 5) {  // Pause
                renderer_state_ = 1;
                ++pauses_;
                cv_.notify_all();
                return has_sample_ ? 0u : 1u;
            }
            if (slot == 6) {  // Run
                renderer_state_ = 2;
                ++runs_;
                run_on_main_ = on_main;
                run_arg_count_ = count;
                cv_.notify_all();
                return 0;
            }
            if (slot == 7) {  // GetState(ms, FILTER_STATE*)
                ++getstates_;
                const u32 ms = static_cast<u32>(args[1]);
                if (renderer_state_ == 1 && !has_sample_ && ms != 0) {
                    cv_.wait_for(lock, std::chrono::milliseconds(ms),
                                 [&] { return has_sample_; });
                }
                (void)memory_.write_u32(static_cast<GuestAddr>(args[2]), renderer_state_);
                const bool cueing = stall_ || (renderer_state_ == 1 && !has_sample_);
                return cueing ? 0x00040237u : 0u;
            }
            return 0;
        case kEnum:
            if (slot == 3) {
                wptr(static_cast<GuestAddr>(args[2]), filter_obj(kPin));
                if (args[3] != 0) (void)memory_.write_u32(static_cast<GuestAddr>(args[3]), 1);
                return 0;
            }
            return 0;
        case kPin:
            if (slot == 0) {  // QI IMemInputPin
                wptr(static_cast<GuestAddr>(args[2]), filter_obj(kMemIn));
                return 0;
            }
            if (slot == 9) {
                (void)memory_.write_u32(static_cast<GuestAddr>(args[1]), 0);
                return 0;
            }
            if (slot == 15) {  // BeginFlush: release a held sample, refuse new ones
                ++begin_flushes_;
                flushing_ = true;
                has_sample_ = false;
                cv_.notify_all();
                return 0;
            }
            if (slot == 16) {  // EndFlush
                ++end_flushes_;
                flushing_ = false;
                return 0;
            }
            if (slot == 14) {  // EndOfStream
                ++eos_;
                eos_on_main_ = on_main;
                cv_.notify_all();
                return 0;
            }
            if (slot == 4 || slot == 11) {  // ReceiveConnection / QueryAccept: video only
                const GuestAddr mt = static_cast<GuestAddr>(args[slot == 4 ? 2 : 1]);
                const bool video = memory_.read_u32(mt).value_or(0) == 0x73646976u;
                if (slot == 11) return video ? 0u : 1u;
                return video ? 0u : 0x8004022Au;
            }
            return 0;
        case kMemIn:
            if (slot == 3) {  // GetAllocator
                if (no_allocator_) {
                    wptr(static_cast<GuestAddr>(args[1]), 0);
                    return 0x80040211u;
                }
                wptr(static_cast<GuestAddr>(args[1]), filter_obj(kAlloc));
                return 0;
            }
            if (slot == 5) return 0x80004001u;  // GetAllocatorRequirements
            if (slot == 4) {                     // NotifyAllocator
                notified_allocator_ = static_cast<GuestAddr>(args[1]);
                return 0;
            }
            if (slot == 6) {  // Receive(IMediaSample*)
                if (renderer_state_ == 0) return 0x80040227u;  // VFW_E_WRONG_STATE
                if (flushing_) return 1;                        // S_FALSE
                ++received_;
                if (on_main) ++received_on_main_;
                if (renderer_state_ == 1) ++received_paused_;
                times_.push_back(last_time_);
                discontinuities_.push_back(last_discontinuity_);
                if (no_allocator_) inspect_graph_sample(static_cast<GuestAddr>(args[1]));
                has_sample_ = true;
                cv_.notify_all();
                // CBaseRenderer: a paused renderer holds the sample and
                // blocks the sender until Run or Stop.
                const int flush_seen = begin_flushes_;
                cv_.wait(lock, [&] {
                    return renderer_state_ != 1 || (flush_releases_ && begin_flushes_ != flush_seen);
                });
                return 0;
            }
            return 0;
        case kAlloc:
            if (slot == 3) {  // SetProperties(req, actual)
                set_bytes_ = memory_.read_u32(static_cast<GuestAddr>(args[1]) + 4).value_or(0);
                for (u32 i = 0; i < 16; i += 4) {
                    (void)memory_.write_u32(static_cast<GuestAddr>(args[2]) + i,
                                            memory_.read_u32(static_cast<GuestAddr>(args[1]) + i).value_or(0));
                }
                return 0;
            }
            if (slot == 5) { committed_ = true; ++commits_; return 0; }
            if (slot == 6) { committed_ = false; ++decommits_; cv_.notify_all(); return 0; }
            if (slot == 7) {  // GetBuffer
                if (!committed_) return 0x80040211u;
                wptr(static_cast<GuestAddr>(args[1]), filter_obj(kSample));
                return 0;
            }
            return 0;
        case kSample:
            if (slot == 3) {
                wptr(static_cast<GuestAddr>(args[1]), SAMPLE_BUFFER);
                return 0;
            }
            if (slot == 4) return 0x4000;  // GetSize
            if (slot == 6) {               // SetTime(&start, &end)
                last_time_ = args[1] != 0
                    ? static_cast<i64>(memory_.read_u64(static_cast<GuestAddr>(args[1])).value_or(0))
                    : -1;
                return 0;
            }
            if (slot == 16) {
                last_discontinuity_ = args[1] != 0;
                return 0;
            }
            return 0;
        case kMisc:
            if (slot == 3) return 1;  // AM_FILTER_MISC_FLAGS_IS_RENDERER
            return 0;
        default:
            ADD_FAILURE() << "unexpected object " << obj;
            return 0x80004005u;
        }
    }
    /// Call the graph's IMediaSample from inside the fake renderer (the
    /// streaming thread), on a CPU state of its own.
    u32 sample_call(GuestAddr sample, u32 op, std::initializer_list<u64> rest) {
        cpu::CpuState cpu;
        const GuestAddr sp = THREAD_STACKS + 0xF000u;
        std::vector<u64> values{sample};
        values.insert(values.end(), rest.begin(), rest.end());
        if (x64_) {
            cpu.init_long_mode();
            cpu.set_sp(sp);
            const u64 regs[4] = {values.size() > 0 ? values[0] : 0, values.size() > 1 ? values[1] : 0,
                                 values.size() > 2 ? values[2] : 0, values.size() > 3 ? values[3] : 0};
            cpu.gpr.rcx = regs[0];
            cpu.gpr.rdx = regs[1];
            cpu.gpr.r8 = regs[2];
            cpu.gpr.r9 = regs[3];
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(sp);
            for (usize i = 0; i < values.size(); ++i) {
                (void)memory_.write_u32(sp + 4 + static_cast<GuestAddr>(i * 4),
                                        static_cast<u32>(values[i]));
            }
        }
        Marshaler m(memory_);
        m.attach_cpu(cpu);
        return mgr_.filter_graph_media_sample(m, op);
    }
    void inspect_graph_sample(GuestAddr sample) {
        received_sample_ = sample;
        constexpr GuestAddr S = DATA_BASE + 0x7800;
        seen_gettime_hr_ = sample_call(sample, 2, {S, S + 8});
        seen_start_ = static_cast<i64>(memory_.read_u64(S).value_or(0));
        seen_end_ = static_cast<i64>(memory_.read_u64(S + 8).value_or(0));
        seen_sync_hr_ = sample_call(sample, 4, {});
        seen_disc_hr_ = sample_call(sample, 12, {});
        seen_length_ = sample_call(sample, 8, {});
    }
    [[nodiscard]] Guid read_guid(GuestAddr address) {
        Guid guid{};
        (void)memory_.read_bytes(address, reinterpret_cast<Byte*>(&guid), sizeof(guid));
        return guid;
    }
    template <typename Pred>
    bool wait_for(Pred pred, int ms = 5000) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(ms), pred);
    }

    /// AddFilter + RenderFile of a 16x16, 10 fps, `seconds`-long clip.
    GuestAddr build_graph(capsule::Vfs& vfs, std::filesystem::path& root, double seconds,
                          bool with_audio = false) {
        char temp_template[] = "/tmp/vitreo_stream_XXXXXX";
        char* temp_dir = ::mkdtemp(temp_template);
        EXPECT_NE(temp_dir, nullptr);
        root = temp_dir;
        const std::string audio =
            with_audio ? " -f lavfi -i sine=frequency=440:duration=" + std::to_string(seconds) +
                             " -shortest"
                       : std::string(" -an");
        const std::string command =
            "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=red:s=16x16:d=" +
            std::to_string(seconds) + ":r=10" + audio + " -pix_fmt yuv420p \"" +
            (root / "clip.mp4").string() + "\"";
        if (std::system(command.c_str()) != 0) return 0;
        EXPECT_TRUE(vfs.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr_.set_vfs(&vfs);
        const GuestAddr graph = create_graph();
        put_wide(NAME_ADDR, u"Frontier Texture Renderer");
        EXPECT_EQ(invoke(&ComCoreManager::filter_graph_add_filter,
                         {graph, filter_obj(kFilter), NAME_ADDR}),
                  com::S_OK);
        constexpr GuestAddr PATH_ADDR = DATA_BASE + 0x200;
        put_wide(PATH_ADDR, u"C:\\clip.mp4");
        const u32 render = invoke(&ComCoreManager::filter_graph_render_file, {graph, PATH_ADDR, 0});
        // The host decoder gives up when it has no first frame within 2 s
        // (seen on a loaded machine): that is the environment, not the graph.
        if (render == 0x80040218u) return 0;
        EXPECT_EQ(render, com::S_OK);
        return graph;
    }
    u32 get_state(GuestAddr control, u32 timeout, u32& state) {
        constexpr GuestAddr STATE = DATA_BASE + 0x600;
        (void)memory_.write_u32(STATE, 99);
        const u32 hr = invoke(&ComCoreManager::filter_graph_media_control_get_state,
                              {control, timeout, STATE});
        state = memory_.read_u32(STATE).value_or(99);
        return hr;
    }

    std::thread::id main_thread_;
    GuestAddr stream_entry_{0};
    GuestAddr run_entry_{0};
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
    u32 thread_index_{0};
    int live_threads_{0};
    // renderer model (mu_)
    u32 renderer_state_{0};
    bool has_sample_{false};
    bool stall_{false};
    bool committed_{false};
    int commits_{0}, decommits_{0}, pauses_{0}, runs_{0}, stops_{0}, getstates_{0};
    int received_{0}, received_on_main_{0}, received_paused_{0}, eos_{0};
    bool eos_on_main_{true}, run_on_main_{true};
    usize run_arg_count_{0};
    u32 set_bytes_{0};
    GuestAddr notified_allocator_{0};
    i64 last_time_{-1};
    bool last_discontinuity_{false};
    std::vector<i64> times_;
    std::vector<bool> discontinuities_;
    bool no_allocator_{false};
    bool flushing_{false};
    bool flush_releases_{false};
    int begin_flushes_{0}, end_flushes_{0};
    GuestAddr received_sample_{0};
    u32 seen_gettime_hr_{0}, seen_sync_hr_{0}, seen_disc_hr_{0}, seen_length_{0};
    i64 seen_start_{-1}, seen_end_{-1};
};

TEST_P(FilterGraphStreamingTest, PausePrerollsOnTheStreamingThreadAndRunStreamsToEnd) {
    capsule::Vfs vfs;
    std::filesystem::path root;
    const GuestAddr graph = build_graph(vfs, root, 0.5);
    if (graph == 0) GTEST_SKIP() << "no test video (ffmpeg missing or host decoder timed out)";
    // DecideAllocator: one YV12 16x16 buffer, NotifyAllocator(that allocator).
    EXPECT_EQ(set_bytes_, 16u * 16u * 3u / 2u);
    EXPECT_EQ(notified_allocator_, filter_obj(kAlloc));
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);

    // Pause returns without delivering on this thread, although the
    // renderer's Receive blocks while paused.
    const auto t0 = std::chrono::steady_clock::now();
    const u32 pause_hr = invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
    // The renderer had no sample when paused: S_FALSE unless the preroll
    // already landed by the FGM's GetState(0).
    EXPECT_TRUE(pause_hr == com::S_FALSE || pause_hr == com::S_OK) << std::hex << pause_hr;
    EXPECT_EQ(commits_, 1);  // CBaseOutputPin::Active
    u32 state = 0;
    EXPECT_EQ(get_state(control, 3000, state), com::S_OK);
    EXPECT_EQ(state, 1u);
    {
        std::lock_guard<std::mutex> lock(mu_);
        EXPECT_EQ(received_, 1);
        EXPECT_EQ(received_on_main_, 0);
        ASSERT_EQ(discontinuities_.size(), 1u);
        EXPECT_TRUE(discontinuities_[0]);
        EXPECT_EQ(times_[0], 0);
    }
    // Still exactly one preroll sample while paused; Pause again is
    // GetState(0) only and now S_OK.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_pause, {control}), com::S_OK);
    {
        std::lock_guard<std::mutex> lock(mu_);
        EXPECT_EQ(received_, 1);
        EXPECT_EQ(pauses_, 1);
    }

    // Run (prerolled: GetState(0) then Run, S_OK) streams frames and ends
    // with EndOfStream, all on the streaming thread.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}), com::S_OK);
    EXPECT_EQ(run_arg_count_, x64_ ? 2u : 3u);
    EXPECT_TRUE(wait_for([&] { return eos_ == 1; }));
    {
        std::lock_guard<std::mutex> lock(mu_);
        EXPECT_GE(received_, 2);
        EXPECT_EQ(received_on_main_, 0);
        EXPECT_FALSE(eos_on_main_);
        for (usize i = 1; i < times_.size(); ++i) {
            EXPECT_GE(times_[i], times_[i - 1]);
            EXPECT_FALSE(discontinuities_[i]);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(eos_, 1);

    // The renderer's EC_COMPLETE (counted: one IS_RENDERER filter) reaches
    // the application with lParam2 = 0.
    const GuestAddr sink = qi(graph, IID_IMEDIAEVENTSINK);
    const GuestAddr events = qi(graph, IID_IMEDIAEVENTEX);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_sink_notify,
                     {sink, 1, 0, filter_obj(kFilter)}),
              com::S_OK);
    constexpr GuestAddr CODE = DATA_BASE + 0x640;
    constexpr GuestAddr P1 = DATA_BASE + 0x648;
    constexpr GuestAddr P2 = DATA_BASE + 0x650;
    (void)memory_.write_u64(P2, 0xCAFEBABECAFEBABEull);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event,
                     {events, CODE, P1, P2, 0}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u32(CODE).value_or(0), 1u);
    EXPECT_EQ(ptr(P2), 0u);
    // A second report until the next Run is dropped (measured).
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_sink_notify,
                     {sink, 1, 0, filter_obj(kFilter)}),
              com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event,
                     {events, CODE, P1, P2, 0}),
              0x80004004u);

    // Stop: renderer Pause then Stop (measured order from Running), the
    // allocator is decommitted and the streaming thread exits.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_TRUE(wait_for([&] { return live_threads_ == 0; }));
    {
        std::lock_guard<std::mutex> lock(mu_);
        EXPECT_EQ(stops_, 1);
        EXPECT_EQ(decommits_, 1);
        EXPECT_FALSE(committed_);
    }
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
    std::filesystem::remove_all(root);
}

// With an audio stream the graph also renders audio (on Windows RenderFile
// gives it a DirectSound Renderer, an IS_RENDERER filter): the application's
// EC_COMPLETE waits for both the guest video renderer and the audio side.
// Planet Coaster's intro movie waits for exactly this event.
TEST_P(FilterGraphStreamingTest, AudioStreamCountsAsARendererForEcComplete) {
    capsule::Vfs vfs;
    std::filesystem::path root;
    const GuestAddr graph = build_graph(vfs, root, 0.5, true);
    if (graph == 0) GTEST_SKIP() << "no test video (ffmpeg missing or host decoder timed out)";
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    (void)invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    u32 state = 0;
    EXPECT_EQ(get_state(control, 3000, state), com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}), com::S_OK);
    EXPECT_TRUE(wait_for([&] { return eos_ == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const GuestAddr sink = qi(graph, IID_IMEDIAEVENTSINK);
    const GuestAddr events = qi(graph, IID_IMEDIAEVENTEX);
    constexpr GuestAddr CODE = DATA_BASE + 0x640;
    constexpr GuestAddr P1 = DATA_BASE + 0x648;
    constexpr GuestAddr P2 = DATA_BASE + 0x650;
    // The audio side has reported; one of two renderers is not enough.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event,
                     {events, CODE, P1, P2, 0}),
              0x80004004u);
    // The video renderer's EC_COMPLETE completes the count.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_sink_notify,
                     {sink, 1, 0, filter_obj(kFilter)}),
              com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event,
                     {events, CODE, P1, P2, 0}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u32(CODE).value_or(0), 1u);
    EXPECT_EQ(ptr(P2), 0u);

    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_TRUE(wait_for([&] { return live_threads_ == 0; }));
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
    std::filesystem::remove_all(root);
}

TEST_P(FilterGraphStreamingTest, StopAndFinalReleaseReleaseABlockedPreroll) {
    capsule::Vfs vfs;
    std::filesystem::path root;
    const GuestAddr graph = build_graph(vfs, root, 2.0);
    if (graph == 0) GTEST_SKIP() << "no test video (ffmpeg missing or host decoder timed out)";
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    (void)invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    // The streaming thread is now blocked inside Receive (paused renderer).
    EXPECT_TRUE(wait_for([&] { return received_ == 1; }));
    EXPECT_EQ(live_threads_, 1);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_TRUE(wait_for([&] { return live_threads_ == 0; }));

    // Paused again, then the last Release: the graph stops the renderer and
    // its streaming thread leaves Receive and exits.
    (void)invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    EXPECT_TRUE(wait_for([&] { return received_ == 2; }));
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_release, {control}), 1u);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_release, {graph}), 0u);
    EXPECT_TRUE(wait_for([&] { return live_threads_ == 0; }));
    EXPECT_EQ(stops_, 2);
    EXPECT_EQ(decommits_, 2);
    std::filesystem::remove_all(root);
}

TEST_P(FilterGraphStreamingTest, GetStateTimesOutAndRunCompletesLaterWhileCueing) {
    capsule::Vfs vfs;
    std::filesystem::path root;
    const GuestAddr graph = build_graph(vfs, root, 2.0);
    if (graph == 0) GTEST_SKIP() << "no test video (ffmpeg missing or host decoder timed out)";
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    {
        std::lock_guard<std::mutex> lock(mu_);
        stall_ = true;  // GetState stays VFW_S_STATE_INTERMEDIATE
    }
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_pause, {control}),
              com::S_FALSE);
    u32 state = 0;
    EXPECT_EQ(get_state(control, 0, state), 0x00040237u);
    EXPECT_EQ(state, 1u);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(get_state(control, 200, state), 0x00040237u);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, std::chrono::milliseconds(190));
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
    EXPECT_EQ(state, 1u);

    // Run while cueing: S_FALSE now, the renderer's Run later from another
    // thread once GetState stops reporting intermediate.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}),
              com::S_FALSE);
    EXPECT_EQ(get_state(control, 0, state), 0x00040237u);
    EXPECT_EQ(state, 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(runs_, 0);
    {
        std::lock_guard<std::mutex> lock(mu_);
        stall_ = false;
    }
    EXPECT_TRUE(wait_for([&] { return runs_ == 1; }));
    EXPECT_FALSE(run_on_main_);
    EXPECT_EQ(get_state(control, 0, state), com::S_OK);
    EXPECT_EQ(state, 2u);
    EXPECT_TRUE(wait_for([&] { return received_ >= 2; }));

    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_TRUE(wait_for([&] { return live_threads_ == 0; }));
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
    std::filesystem::remove_all(root);
}


TEST_P(FilterGraphStreamingTest, GraphSampleWithoutAllocatorFollowsCMediaSample) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        no_allocator_ = true;  // GetAllocator fails: the graph's own sample
    }
    capsule::Vfs vfs;
    std::filesystem::path root;
    const GuestAddr graph = build_graph(vfs, root, 2.0);
    if (graph == 0) GTEST_SKIP() << "no test video (ffmpeg missing or host decoder timed out)";
    EXPECT_EQ(notified_allocator_, 0u);
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    (void)invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    EXPECT_TRUE(wait_for([&] { return received_ == 1; }));
    GuestAddr sample = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        sample = received_sample_;
        // What the renderer saw inside Receive: start time 0 with no stop
        // time (VFW_S_NO_STOP_TIME, end = start + 1), a sync point, a
        // discontinuity (first sample), 16*16*3/2 bytes of YV12.
        EXPECT_EQ(seen_gettime_hr_, 0x00040270u);
        EXPECT_EQ(seen_start_, 0);
        EXPECT_EQ(seen_end_, 1);
        EXPECT_EQ(seen_sync_hr_, com::S_OK);
        EXPECT_EQ(seen_disc_hr_, com::S_OK);
        EXPECT_EQ(seen_length_, 16u * 16u * 3u / 2u);
    }
    ASSERT_NE(sample, 0u);
    // CMediaSample semantics on the sample itself (dshow_sample_probe).
    constexpr GuestAddr T0 = DATA_BASE + 0x700;
    constexpr GuestAddr T1 = DATA_BASE + 0x708;
    (void)memory_.write_u64(T0, 1000);
    (void)memory_.write_u64(T1, 2000);
    const auto sop = &ComCoreManager::filter_graph_media_sample;
    EXPECT_EQ(invoke_op(sop, 3, {sample, T0, T1}), com::S_OK);  // SetTime
    EXPECT_EQ(invoke_op(sop, 2, {sample, T0 + 0x20, T1 + 0x20}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(T0 + 0x20).value_or(0), 1000u);
    EXPECT_EQ(memory_.read_u64(T1 + 0x20).value_or(0), 2000u);
    EXPECT_EQ(invoke_op(sop, 3, {sample, 0, T1}), com::S_OK);  // NULL start clears
    (void)memory_.write_u64(T0 + 0x20, 111);
    EXPECT_EQ(invoke_op(sop, 2, {sample, T0 + 0x20, T1 + 0x20}), 0x80040249u);
    EXPECT_EQ(memory_.read_u64(T0 + 0x20).value_or(0), 111u);  // untouched
    EXPECT_EQ(invoke_op(sop, 14, {sample, T0 + 0x20, T1 + 0x20}), 0x80040251u);
    EXPECT_EQ(invoke_op(sop, 9, {sample, 0x100000}), 0x8004020Du);  // > GetSize
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_TRUE(wait_for([&] { return live_threads_ == 0; }));
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
    std::filesystem::remove_all(root);
}

TEST_P(FilterGraphStreamingTest, SeekWhilePausedFlushesAndPrerollsAgain) {
    capsule::Vfs vfs;
    std::filesystem::path root;
    const GuestAddr graph = build_graph(vfs, root, 2.0);
    if (graph == 0) GTEST_SKIP() << "no test video (ffmpeg missing or host decoder timed out)";
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    (void)invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    EXPECT_TRUE(wait_for([&] { return received_ == 1; }));
    // IMediaSeeking::SetPositions(&pos, AM_SEEKING_AbsolutePositioning,
    // NULL, 0): BeginFlush / EndFlush on the renderer's pin and a fresh
    // preroll flagged as a discontinuity.
    const GuestAddr seeking = qi(graph, IID_IMEDIASEEKING);
    constexpr GuestAddr POS = DATA_BASE + 0x780;
    (void)memory_.write_u64(POS, 5'000'000);
    {
        std::lock_guard<std::mutex> lock(mu_);
        // A flushing renderer lets go of the held sample.
        flush_releases_ = true;
    }
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_seeking_set_positions,
                     {seeking, POS, 1, 0, 0}),
              com::S_OK);
    EXPECT_TRUE(wait_for([&] { return received_ == 2; }));
    {
        std::lock_guard<std::mutex> lock(mu_);
        EXPECT_EQ(begin_flushes_, 1);
        EXPECT_EQ(end_flushes_, 1);
        ASSERT_EQ(discontinuities_.size(), 2u);
        EXPECT_TRUE(discontinuities_[1]);
        EXPECT_EQ(received_on_main_, 0);
    }
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_TRUE(wait_for([&] { return live_threads_ == 0; }));
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
    std::filesystem::remove_all(root);
}

INSTANTIATE_TEST_SUITE_P(Bitness, FilterGraphStreamingTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });
