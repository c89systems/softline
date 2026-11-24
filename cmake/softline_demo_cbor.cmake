include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/softline_verified_archive.cmake")

# Cache released demo SDKs independently of the Softline SDK and its consumers.
function(softline_demo_cbor_sdk repository stem version package digest output)
  set(name "${stem}-${version}-${SL_TARGET_ID}")
  softline_verified_archive(
    "${repository} TCP demo SDK"
    "https://github.com/c89systems/${repository}/releases/download/v${version}/${name}.tar.gz"
    "${digest}" "${name}.tar.gz" archive)

  set(directory "${PROJECT_SOURCE_DIR}/.cache/deps/${SL_TARGET_ID}/tcp-demo/${stem}-${digest}")
  set(config "${directory}/${name}/lib/cmake/${package}")
  softline_check_generated_path("${PROJECT_SOURCE_DIR}" "${directory}")
  if(NOT EXISTS "${directory}/.ready" OR NOT EXISTS "${config}/${package}Config.cmake")
    file(REMOVE_RECURSE "${directory}")
    file(MAKE_DIRECTORY "${directory}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${directory}")
    if(NOT EXISTS "${config}/${package}Config.cmake")
      message(FATAL_ERROR "${name}: SDK archive is missing its ${package} CMake config")
    endif()
    file(TOUCH "${directory}/.ready")
  endif()
  set(${output} "${directory}/${name}" PARENT_SCOPE)
endfunction()

function(softline_find_demo_cbor)
  set(cbor_version "0.2.0")
  set(SL_CBOR_SDK_PREFIXES "" CACHE STRING "Override the TCP demo C89Cbor, lonejson and cpkt SDK prefixes")
  set(prefixes ${SL_CBOR_SDK_PREFIXES})
  if(NOT prefixes AND NOT C89Cbor_DIR AND "$ENV{C89Cbor_DIR}" STREQUAL "")
    # Each row pins the published libcbor 0.2.0, lonejson 0.50.0 and cpkt 0.8.0
    # archives for one target, in that order.
    set(digests_x86_64-linux-gnu
      136ba69b0765654cc0fbf7e42db2128f45b1dfe9b6fa5ca6b6307a0092281b74
      c0b855e40cc21a39a3d4bb5366f0e7f3629e53c956a0e675dea69791a7d7b936
      7a9f2c1de42beec5fb324ac9c8b5a8005c17fb98003d9625eb5ad5ad228ba941)
    set(digests_x86_64-linux-musl
      d671237d7fa13fea23e2dd9e55fca20d0cfc43890bc37e0113e5ccd5f0c90780
      e736ab07a9694558fab51d1b7403a2d0ce8884900be9d0bcee7ca1903489ef12
      9f768f99fa1933adfa0ae5fa8d8f13d1b0c9136047adc72e36a1970d7932772e)
    set(digests_aarch64-linux-gnu
      840e216b2ecd07edaed31e8af05205cbd0b7d2016095354dbb075a82a2a3534c
      0bcc6d753b65bf122d7941fca0e4671f0dfdb508c4cfff968fe628357db7b04e
      fd34aaefe2642f548990c7c53d2463e8104004d8e55e38bc2ba63399b28fee43)
    set(digests_aarch64-linux-musl
      eaf93027ef05df7efafd03837b6c1db787eada66ac7672485c7a0469ea885128
      d18d291a2c6a731aa54205673633221e388e4fbea3e5e2ee15caf8a0c653b04b
      533e75c266416a721bad6dd50126d25575c776e663b8d32686f491fdff80451b)
    set(digests_armhf-linux-gnu
      4276041efdc4d94e7d0de58ccb53497282ce7be4ca3e15e900b24e236c573618
      5ea12a2dc327811101bad13dff23182c54a2bfa7133e87352442147706e6b094
      4066a6fba75cc48baead943431be0b6fb55a290a48ed8b1405f9e068de5e10d0)
    set(digests_armhf-linux-musl
      a5403cf3ee5072e017c8f8198f11320ea5e2cbd92856da7fd67c88a851209544
      d11abea1a1e0e387426b230fb19c7ef35086b351b40245a10efb12784ee64797
      f0412151082a3e99f141dd19f0a8040ba1c507695f6df270e72aa4b62e2c58c5)
    set(digests_arm64-apple-darwin
      8c77f1274e5d9c5aa7b29d6c823aa0531166d09cd0d6837c322cfbfc4b2bf2a1
      efc25f93d2768fb57f89d54025711ae3f82f7feeba1c776b25108b43356d8031
      25e912cf05614810de3c4d8afbde0bad9dd9033a87443256aef6073f1a432b62)
    set(digests ${digests_${SL_TARGET_ID}})
    if(digests)
      list(POP_FRONT digests cbor_digest json_digest cpkt_digest)
      softline_demo_cbor_sdk(libcbor libcbor "${cbor_version}" C89Cbor "${cbor_digest}" cbor)
      softline_demo_cbor_sdk(lonejson liblonejson "0.50.0" lonejson "${json_digest}" json)
      softline_demo_cbor_sdk(cpkt cpkt "0.8.0" CpktCore "${cpkt_digest}" cpkt)
      set(prefixes "${cbor}" "${json}" "${cpkt}")
      set(C89Cbor_DIR "${cbor}/lib/cmake/C89Cbor")
      set(lonejson_DIR "${json}/lib/cmake/lonejson")
      set(CpktCore_DIR "${cpkt}/lib/cmake/CpktCore")
    endif()
  endif()
  list(PREPEND CMAKE_PREFIX_PATH ${prefixes})
  list(PREPEND CMAKE_FIND_ROOT_PATH ${prefixes})
  find_package(C89Cbor ${cbor_version} CONFIG REQUIRED)
endfunction()
