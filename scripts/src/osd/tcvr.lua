-- TCVR native-core OSD: deliberately excludes SDL, Qt and MAME's frontend.
function maintargetosdoptions(_target, _subtarget)
end

local function tcvr_includes()
    includedirs {
        MAME_DIR .. "src/osd",
        MAME_DIR .. "src/emu",
        MAME_DIR .. "src/lib",
        MAME_DIR .. "src/lib/util",
        MAME_DIR .. "src/osd/modules/file",
        MAME_DIR .. "3rdparty",
        ext_includedir("asio"),
    }
end

project ("qtdbg_tcvr")
uuid (os.uuid("qtdbg_tcvr"))
kind (LIBTYPE)
tcvr_includes()
files { MAME_DIR .. "src/osd/modules/debugger/none.cpp" }

project ("osd_tcvr")
uuid (os.uuid("osd_tcvr"))
kind (LIBTYPE)
tcvr_includes()
files {
    MAME_DIR .. "src/osd/modules/output/none.cpp",
    MAME_DIR .. "src/osd/tcvr/tcvr_osd.cpp",
}

project ("ocore_tcvr")
uuid (os.uuid("ocore_tcvr"))
kind (LIBTYPE)
tcvr_includes()
files {
    MAME_DIR .. "src/osd/asio.cpp",
    MAME_DIR .. "src/osd/osdcore.cpp",
    MAME_DIR .. "src/osd/osdsync.cpp",
    MAME_DIR .. "src/osd/strconv.cpp",
    MAME_DIR .. "src/osd/modules/file/posixdir.cpp",
    MAME_DIR .. "src/osd/modules/file/posixfile.cpp",
    MAME_DIR .. "src/osd/modules/lib/osdlib_unix.cpp",
    MAME_DIR .. "src/osd/modules/lib/osdobj_common.cpp",
}
