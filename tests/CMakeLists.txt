# Unit tests of the library
add_executable(unit-tests unit/unit-tests.cpp)
target_link_libraries(unit-tests PRIVATE tinymodem-desktop)
target_compile_definitions(unit-tests PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

set(UNIT_TESTS
    config-modes baudot-codec callerid-mdmf callerid-sdmf uic-codec
    binary-codec bit-helpers encoder-signal custom-generator loopback-modes
    loopback-rate-perfect loopback-options loopback-sample-rates rate-slop
    amplitude noise auto-carrier chunk-sizes input-formats audio-info-change
    multiple-transmissions output-filters callerid-rx uic-rx stream-api
    audiotools-pipeline mode-switch config-reuse baudot-transmissions
    stream-modes stream-no-loss stream-rtos packet-codec packet-modem
    mfsk-config mfsk-loopback mfsk-robust mfsk-formats mfsk-stream
    ax25-frame ax25-loopback ax25-kiss ax25-stream fx25-codec
    il2p-codec fx25-loopback psk-varicode psk-loopback
    psk-robust psk-modes qpsk-loopback qpsk-robust psk-stream ofdm-config ofdm-loopback ofdm-robust ofdm-stream ggwave-config ggwave-rs
    ggwave-signal ggwave-loopback ggwave-robust ggwave-stream dtmf-config
    dtmf-loopback dtmf-robust dtmf-stream olivia-config olivia-loopback
    olivia-robust olivia-stream css-config css-loopback css-robust css-stream
    rx-one)
foreach(test ${UNIT_TESTS})
    add_test(NAME unit-${test} COMMAND unit-tests ${test})
endforeach()

if(NOT TINYMODEM_BUILD_TOOLS)
    return()
endif()

# Command line tests: tinymodem is used to transmit and receive
set(CLI_TESTS
    self-test-1200 self-test-300 self-test-rtty self-test-0.5 self-test-12000
    self-test-8000hz self-test-22050hz verify-perfect rate-slop amplitude
    noise noise-purefreqs multibyte callerid-mdmf callerid-sdmf same ascii7
    tdd baudot-300 v21 msb-first invert-start-stop inverted start-stop-bits
    sync-byte auto-carrier wav-formats)
set(CLI_TX_TESTS verify-tx-consistent tx-wav-header)
# extensions which are not supported by the original minimodem
set(CLI_OWN_TESTS
    mfsk4 mfsk8 mfsk16 mfsk32 mfsk64 mfsk-noise mfsk-clock mfsk-base-freq
    mfsk-tx-carrier packets-fsk packets-mfsk packets-binary ax25 ax25-22050hz
    ax25-noise ax25-kiss fx25 fx25-64 fx25-noise il2p il2p-noise fx25-rx-ax25
    psk31 psk125 psk500 psk-noise psk-offset packets-psk qpsk31 qpsk250 qpsk-noise 8psk125 8psk1000
   
    ofdm16 ofdm32 ofdm64 ofdm-noise packets-ofdm ggwave-fast ggwave-fastest
    ggwave-u-normal ggwave-dt-fast ggwave-auto-speed ggwave-noise ggwave-44100hz
    packets-ggwave dtmf dtmf-8000hz dtmf-noise olivia olivia-8-250 olivia-noise
    olivia-offset contestia css7 css5 css-noise css-offset packets-css)

set(CLI_TEST ${CMAKE_CURRENT_SOURCE_DIR}/cli-test.sh)
foreach(test ${CLI_TESTS} ${CLI_TX_TESTS} ${CLI_OWN_TESTS})
    add_test(NAME cli-${test}
        COMMAND bash ${CLI_TEST} ${test} $<TARGET_FILE:tinymodem> $<TARGET_FILE:tinymodem>)
endforeach()

# Interoperability tests with the original minimodem (if available):
# -DMINIMODEM_REFERENCE=/path/to/minimodem
find_program(MINIMODEM_REFERENCE minimodem NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH)
if(MINIMODEM_REFERENCE)
    message(STATUS "minimodem reference for interop tests: ${MINIMODEM_REFERENCE}")
    foreach(test ${CLI_TESTS})
        # we transmit, the original minimodem receives
        add_test(NAME interop-tx-${test}
            COMMAND bash ${CLI_TEST} ${test} $<TARGET_FILE:tinymodem> ${MINIMODEM_REFERENCE})
        # the original minimodem transmits, we receive
        add_test(NAME interop-rx-${test}
            COMMAND bash ${CLI_TEST} ${test} ${MINIMODEM_REFERENCE} $<TARGET_FILE:tinymodem>)
    endforeach()
else()
    message(STATUS "minimodem reference not found: interop tests are skipped")
endif()

# AX.25 interoperability tests with direwolf (if available): atest decodes
# and gen_packets generates AX.25 audio. DIREWOLF_LIBRARY_PATH can define the
# location of the shared libraries of an unpacked direwolf package.
find_program(DIREWOLF_ATEST atest)
find_program(DIREWOLF_GEN_PACKETS gen_packets)
if(DIREWOLF_ATEST AND DIREWOLF_GEN_PACKETS)
    message(STATUS "direwolf for AX.25 interop tests: ${DIREWOLF_ATEST}")
    set(AX25_INTEROP ${CMAKE_CURRENT_SOURCE_DIR}/ax25-interop.sh)
    foreach(test tx rx noise fx25-tx fx25-rx il2p-tx il2p-rx)
        add_test(NAME interop-ax25-${test}
            COMMAND bash ${AX25_INTEROP} ${test} $<TARGET_FILE:tinymodem>
                ${DIREWOLF_ATEST} ${DIREWOLF_GEN_PACKETS})
        if(DIREWOLF_LIBRARY_PATH)
            set_tests_properties(interop-ax25-${test} PROPERTIES
                ENVIRONMENT "LD_LIBRARY_PATH=${DIREWOLF_LIBRARY_PATH}")
        endif()
    endforeach()
else()
    message(STATUS "direwolf (atest, gen_packets) not found: no AX.25 interop tests")
endif()

# ggwave interoperability tests (if available): ggwave-to-file generates and
# ggwave-from-file decodes ggwave audio (examples of the ggwave project)
find_program(GGWAVE_TO_FILE ggwave-to-file)
find_program(GGWAVE_FROM_FILE ggwave-from-file)
if(GGWAVE_TO_FILE AND GGWAVE_FROM_FILE)
    message(STATUS "ggwave for interop tests: ${GGWAVE_TO_FILE}")
    set(GGWAVE_INTEROP ${CMAKE_CURRENT_SOURCE_DIR}/ggwave-interop.sh)
    foreach(test tx rx)
        add_test(NAME interop-ggwave-${test}
            COMMAND bash ${GGWAVE_INTEROP} ${test} $<TARGET_FILE:tinymodem>
                ${GGWAVE_TO_FILE} ${GGWAVE_FROM_FILE})
    endforeach()
else()
    message(STATUS "ggwave (ggwave-to-file, ggwave-from-file) not found: no ggwave interop tests")
endif()
