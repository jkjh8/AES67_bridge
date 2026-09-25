set(APP_SOURCES
  common/Config.cpp
  common/Log.cpp
  common/NetIf.cpp
  ptp/PtpClient.cpp
  media/TxStream.cpp
  media/RxStream.cpp
  media/AsioLink.cpp
  media/AudioEngine.cpp
  dsp/Asrc.cpp
  sap/SapAnnouncer.cpp
  sap/SapListener.cpp
  wasapi/DeviceEnum.cpp
  wasapi/WasapiCaptureSource.cpp
  wasapi/WasapiRenderSink.cpp
  ui/TrayApp.cpp
  ui/WebUi.cpp
  ui/app.rc
  app/Controller.cpp
  app/main.cpp)

add_executable(aes67bridge WIN32 ${APP_SOURCES})

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
               ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
target_include_directories(aes67bridge PRIVATE ${CMAKE_BINARY_DIR}/generated)

set_source_files_properties(ui/app.rc PROPERTIES
  OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/web/index.html)

target_include_directories(aes67bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(aes67bridge PRIVATE
  nlohmann_json
  webview2
  samplerate
  user32 shell32 comctl32 ole32 shlwapi
  ws2_32 iphlpapi winmm avrt version)

set_target_properties(aes67bridge PROPERTIES
  VS_DPI_AWARENESS "PerMonitor"
  OUTPUT_NAME "AES67Bridge")
