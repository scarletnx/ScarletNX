# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The macOS app and the iOS one (docs/APPLE.md), made by `cmake --install` and packed by CPack. Run on a Mac with Xcode.
#
#   cpack --config build/macos/CPackConfig.cmake   ScarletNX-macos-arm64.zip of ScarletNX.app, signed ad hoc
#   cpack --config build/ios/CPackConfig.cmake     ScarletNX-ios-arm64.ipa, unsigned, for a sideloading tool to sign
#
# `-G DragNDrop` makes the macOS app a disk image instead, and `-D CPACK_PACKAGE_FILE_NAME=...` names the package.
# On macOS the install prefix is the app's Contents (CMakeLists.txt puts the programs in MacOS/ and share/ beside it,
# where they look: ../share/scarletnx), so `cmake --install build/macos --prefix ScarletNX.app/Contents` makes the app
# too. On iOS the app is the bundle the build makes, with share/ in it.
#
# SNX_MOLTENVK is a MoltenVK release's MoltenVK-macos.tar or MoltenVK-ios.tar, extracted: Ryujinx's (V380-Ori/
# Ryujinx.MoltenVK, which .github/workflows/build.yml pins), whose patches emulators' pipelines need. Its dynamic
# library goes in the app's Frameworks, where the runtime has SDL load it (backend/sdl3.c). Without it the macOS app
# uses the system's Vulkan loader (Homebrew's molten-vk), and the iOS app has none.
set(SNX_MOLTENVK "" CACHE PATH "An extracted MoltenVK release whose library the Apple app carries (empty: none)")
# SNX_KOSMICKRISP is a LunarG Vulkan SDK's macOS folder: lib/libvulkan_kosmickrisp.dylib, lib/libvulkan.1.dylib and
# share/vulkan/icd.d/'s KosmicKrisp JSON. The macOS app carries the driver, its loader and an ICD JSON whose path is
# relative, so a game's runtime copy of them works outside the bundle too (docs/APPLE.md). The system's KosmicKrisp
# serves a build without it, from Homebrew's mesa or the SDK on the Mac. LunarG's build is self-contained, unlike
# Homebrew's, which links the prefix's libraries.
set(SNX_KOSMICKRISP "" CACHE PATH "A LunarG Vulkan SDK's macOS folder whose KosmicKrisp the app carries (empty: none)")

# the file leaf of the KosmicKrisp install at SNX_KOSMICKRISP, its macOS/, or their lib/, into var; a missing one stops
# the configure
function(snx_kosmickrisp_part leaf var)
    foreach(dir "${SNX_KOSMICKRISP}" "${SNX_KOSMICKRISP}/macOS" "${SNX_KOSMICKRISP}/lib"
            "${SNX_KOSMICKRISP}/macOS/lib")
        if(EXISTS "${dir}/${leaf}")
            set(${var} "${dir}/${leaf}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "SNX_KOSMICKRISP=${SNX_KOSMICKRISP} has no ${leaf} in it, its macOS/ or a lib/")
endfunction()

# the file or folder of the MoltenVK release under dynamic/ whose path ends as regex says, into var; and its license,
# installed beside the app's other ones
function(snx_moltenvk_part regex var)
    file(GLOB_RECURSE found LIST_DIRECTORIES true "${SNX_MOLTENVK}/*")
    list(FILTER found INCLUDE REGEX "/dynamic/(.*/)?${regex}$")
    if(NOT found)
        message(FATAL_ERROR "SNX_MOLTENVK=${SNX_MOLTENVK} has no ${regex} under dynamic/")
    endif()
    list(GET found 0 part)
    set(${var} "${part}" PARENT_SCOPE)
    file(GLOB_RECURSE license "${SNX_MOLTENVK}/LICENSE")
    if(license)
        list(GET license 0 license)
        install(FILES "${license}" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/MoltenVK)
    endif()
endfunction()

set(CPACK_PACKAGE_NAME ScarletNX)
set(CPACK_PACKAGE_VENDOR "the ScarletNX contributors")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
if(SNX_KOSMICKRISP AND IOS)
    message(WARNING "SNX_KOSMICKRISP is a macOS driver: the iOS app carries MoltenVK alone")
endif()
if(IOS)
    install(FILES dist/icons/hicolor/256x256/apps/wimsy.png DESTINATION scarletnx.app RENAME AppIcon.png)
    if(SNX_MOLTENVK)
        snx_moltenvk_part("ios-arm64/MoltenVK\\.framework" framework)
        install(DIRECTORY "${framework}" DESTINATION scarletnx.app/Frameworks USE_SOURCE_PERMISSIONS)
    endif()
    # an IPA is a zip of Payload/<name>.app
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGING_INSTALL_PREFIX /Payload)
    set(CPACK_ARCHIVE_FILE_EXTENSION ipa)
    set(CPACK_PACKAGE_FILE_NAME ScarletNX-ios-arm64)
else()
    # the icon, drawn from the PNG with macOS's own tools
    set(png "${PROJECT_SOURCE_DIR}/dist/icons/hicolor/256x256/apps/wimsy.png")
    set(iconset "${PROJECT_BINARY_DIR}/ScarletNX.iconset")
    set(icns "${PROJECT_BINARY_DIR}/ScarletNX.icns")
    set(sizes)
    foreach(size 16 32 128 256)
        list(APPEND sizes COMMAND sips -z ${size} ${size} "${png}" --out "${iconset}/icon_${size}x${size}.png")
    endforeach()
    add_custom_command(OUTPUT "${icns}" COMMAND "${CMAKE_COMMAND}" -E make_directory "${iconset}" ${sizes}
                       COMMAND iconutil -c icns "${iconset}" -o "${icns}" DEPENDS "${png}" VERBATIM)
    add_custom_target(snx_macos_icon ALL DEPENDS "${icns}")
    install(FILES dist/apple/Info-macOS.plist DESTINATION . RENAME Info.plist)
    install(FILES "${icns}" DESTINATION Resources)
    if(SNX_MOLTENVK)
        snx_moltenvk_part("dylib/macOS/libMoltenVK\\.dylib" dylib)
        install(FILES "${dylib}" DESTINATION Frameworks)
    endif()
    if(SNX_KOSMICKRISP)
        snx_kosmickrisp_part("libvulkan_kosmickrisp.dylib" kk_driver)
        # the loader as the runtime names it (snx_vulkan_driver_choose), whether the SDK has that name or a versioned one
        set(kk_loader "")
        foreach(dir "${SNX_KOSMICKRISP}" "${SNX_KOSMICKRISP}/lib")
            if(EXISTS "${dir}/libvulkan.1.dylib")
                set(kk_loader "${dir}/libvulkan.1.dylib")
            elseif(NOT kk_loader)
                file(GLOB kk_versioned "${dir}/libvulkan.1.*.dylib")
                if(kk_versioned)
                    list(GET kk_versioned 0 kk_loader)
                endif()
            endif()
        endforeach()
        if(NOT kk_loader)
            message(FATAL_ERROR "SNX_KOSMICKRISP=${SNX_KOSMICKRISP} has no Vulkan loader (libvulkan.1.dylib)")
        endif()
        # the API version its own ICD JSON names, so the loader takes the driver for what it is
        set(kk_api "1.3.0")
        file(GLOB kk_jsons "${SNX_KOSMICKRISP}/share/vulkan/icd.d/*.json"
             "${SNX_KOSMICKRISP}/../share/vulkan/icd.d/*.json")
        foreach(kk_json IN LISTS kk_jsons)
            if(kk_json MATCHES "[Kk]osmic[Kk]risp")
                file(READ "${kk_json}" kk_text)
                if(kk_text MATCHES "\"api_version\"[ \t\r\n]*:[ \t\r\n]*\"([0-9.]+)\"")
                    set(kk_api "${CMAKE_MATCH_1}")
                endif()
                break()
            endif()
        endforeach()
        # a path relative to the JSON: the copy a game's runtime has in its own folder finds its driver there
        set(kk_icd "${PROJECT_BINARY_DIR}/libkosmickrisp_icd.json")
        file(WRITE "${kk_icd}" "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"libvulkan_kosmickrisp.dylib\",\n        \"api_version\": \"${kk_api}\"\n    }\n}\n")
        # the library each name stands for: install() keeps a symbolic link as it is, and a link into the SDK's or
        # Homebrew's own folder would break in the app, or in a game's folder that has no such folder
        get_filename_component(kk_driver "${kk_driver}" REALPATH)
        get_filename_component(kk_loader "${kk_loader}" REALPATH)
        install(FILES "${kk_driver}" DESTINATION Frameworks RENAME libvulkan_kosmickrisp.dylib)
        install(FILES "${kk_loader}" DESTINATION Frameworks RENAME libvulkan.1.dylib)
        install(FILES "${kk_icd}" DESTINATION Frameworks)
        # the licenses the SDK carries with the driver and the loader: its Licenses folder, and a LICENSE file
        if(IS_DIRECTORY "${SNX_KOSMICKRISP}/../Licenses")
            install(DIRECTORY "${SNX_KOSMICKRISP}/../Licenses/"
                    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/KosmicKrisp/Licenses)
        endif()
        file(GLOB kk_licenses "${SNX_KOSMICKRISP}/LICENSE*" "${SNX_KOSMICKRISP}/../LICENSE*")
        set(kk_license_files)
        foreach(kk_license IN LISTS kk_licenses)
            if(NOT IS_DIRECTORY "${kk_license}")
                list(APPEND kk_license_files "${kk_license}")
            endif()
        endforeach()
        if(kk_license_files)
            install(FILES ${kk_license_files} DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/KosmicKrisp)
        endif()
    endif()
    # Apple silicon runs no unsigned code: the app signed ad hoc, for the Mac it is copied to, after everything else is
    # in it (the rules run in the order they are declared, CMP0082, and this is the last). Only when the prefix is an
    # app's Contents
    install(CODE [[
        set(contents "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
        if(contents MATCHES "\\.app/Contents/?$")
            get_filename_component(app "${contents}" DIRECTORY)
            execute_process(COMMAND codesign --force --deep --sign - "${app}" COMMAND_ERROR_IS_FATAL ANY)
        endif()
    ]])
    # the zip the releases carry (scripts/publish-builds.py); its symbolic links (scarletnx-switch) stay links
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGING_INSTALL_PREFIX /ScarletNX.app/Contents)
    set(CPACK_DMG_VOLUME_NAME ScarletNX)
    set(CPACK_PACKAGE_FILE_NAME ScarletNX-macos-arm64)
endif()
include(CPack)
